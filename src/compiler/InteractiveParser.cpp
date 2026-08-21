//===----------------------------------------------------------------------===//
// Copyright (c) 2026, Modular Inc. All rights reserved.
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//
//
// The cell partitioning, diagnostic remapping, and chained declaration import
// logic are adapted from Modular's ParserDriverREPL.cpp. The execution and
// persistence policies from that implementation are intentionally not used.
//
//===----------------------------------------------------------------------===//

#include "InteractiveParser.h"
#include "xmojo/InteractiveSession.h"

#include "KGEN/KGENDialect/KGENDialect.h"
#include "KGEN/LITDialect/LITAttrs.h"
#include "KGEN/LITDialect/LITOps.h"
#include "KGEN/MojoParser/ASTDecl.h"
#include "KGEN/MojoParser/DeclResolver.h"
#include "KGEN/MojoParser/EntryPoint.h"
#include "KGEN/MojoParser/ExprNode.h"
#include "KGEN/MojoParser/IRValues.h"
#include "KGEN/MojoParser/Lexer.h"
#include "KGEN/MojoParser/SharedState.h"
#include "KGEN/MojoTooling/CodeComplete.h"
#include "KGEN/MojoTooling/ParserDriver.h"
#include "KGEN/MojoTooling/PublicASTDecl.h"
#include "KGEN/lib/MojoParser/ParserBase.h"
#include "Support/Compiler/Diags.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>

using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::SmallVectorImpl;
using llvm::StringRef;
using M::KGEN::LIT::ASTDecl;
using M::KGEN::LIT::ExprNode;
using M::KGEN::LIT::FnOp;
using M::KGEN::LIT::Lexer;
using M::KGEN::LIT::LexerCursor;
using M::KGEN::LIT::ParserBase;
using M::KGEN::LIT::SharedState;
using M::KGEN::LIT::Token;
using mlir::FileLineColLoc;
using mlir::MLIRContext;
using xmojo::CompletenessResult;
using xmojo::CompletenessStatus;
using xmojo::CompletionKind;
using xmojo::CompletionResult;
using xmojo::InspectionResult;

namespace {

xmojo::DiagnosticSeverity convertSeverity(llvm::SourceMgr::DiagKind kind) {
  switch (kind) {
  case llvm::SourceMgr::DK_Error:
    return xmojo::DiagnosticSeverity::Error;
  case llvm::SourceMgr::DK_Warning:
    return xmojo::DiagnosticSeverity::Warning;
  case llvm::SourceMgr::DK_Remark:
  case llvm::SourceMgr::DK_Note:
    return xmojo::DiagnosticSeverity::Note;
  }
  llvm_unreachable("unknown SourceMgr diagnostic kind");
}

class CellSourceMap {
public:
  void add(StringRef inputText, size_t wrappedOffset) {
    mappings.push_back({inputText.data(), inputText.size(), wrappedOffset});
  }

  void setWrapped(StringRef value) { wrapped = value; }

  llvm::SMLoc mapLocation(llvm::SMLoc location) const {
    if (!location.isValid() || wrapped.empty())
      return {};
    const char *pointer = location.getPointer();
    if (pointer < wrapped.begin() || pointer > wrapped.end())
      return {};
    size_t offset = pointer - wrapped.begin();
    for (const Mapping &mapping : mappings) {
      if (offset >= mapping.wrappedOffset &&
          offset <= mapping.wrappedOffset + mapping.size)
        return llvm::SMLoc::getFromPointer(mapping.input + offset -
                                           mapping.wrappedOffset);
    }
    return {};
  }

  llvm::SMRange mapRange(llvm::SMRange range) const {
    llvm::SMLoc start = mapLocation(range.Start);
    llvm::SMLoc end = mapLocation(range.End);
    if (start.isValid() && !end.isValid() && range.End.getPointer()) {
      end =
          mapLocation(llvm::SMLoc::getFromPointer(range.End.getPointer() - 1));
      if (end.isValid())
        end = llvm::SMLoc::getFromPointer(end.getPointer() + 1);
    }
    return {start, end};
  }

  llvm::SMDiagnostic mapDiagnostic(const llvm::SMDiagnostic &diagnostic,
                                   llvm::SourceMgr &sourceManager) const {
    llvm::SMLoc location = mapLocation(diagnostic.getLoc());
    if (!location.isValid())
      return diagnostic;

    auto [line, column] = sourceManager.getLineAndColumn(location);
    --column;
    int columnDifference = diagnostic.getColumnNo() - column;

    SmallVector<std::pair<unsigned, unsigned>> ranges(diagnostic.getRanges());
    std::string lineContents = diagnostic.getLineContents().str();
    if (columnDifference) {
      for (auto &range : ranges) {
        range.first -= columnDifference;
        range.second -= columnDifference;
      }
      if (!lineContents.empty()) {
        if (columnDifference > 0)
          lineContents.erase(0, columnDifference);
        else
          lineContents.insert(0, -columnDifference, ' ');
      }
    }

    SmallVector<llvm::SMFixIt> fixIts;
    for (const llvm::SMFixIt &fixIt : diagnostic.getFixIts()) {
      llvm::SMRange range = mapRange(fixIt.getRange());
      if (range.isValid())
        fixIts.emplace_back(range, fixIt.getText());
    }

    unsigned bufferID = sourceManager.FindBufferContainingLoc(location);
    StringRef filename =
        sourceManager.getMemoryBuffer(bufferID)->getBufferIdentifier();
    return llvm::SMDiagnostic(sourceManager, location, filename, line, column,
                              diagnostic.getKind(), diagnostic.getMessage(),
                              lineContents, ranges, fixIts);
  }

private:
  struct Mapping {
    const char *input;
    size_t size;
    size_t wrappedOffset;
  };

  StringRef wrapped;
  std::vector<Mapping> mappings;
};

size_t indentation(StringRef line) { return line.size() - line.ltrim().size(); }

bool startsWithAny(StringRef line, std::initializer_list<StringRef> prefixes) {
  return llvm::any_of(
      prefixes, [&](StringRef prefix) { return line.starts_with(prefix); });
}

bool isDeclaration(StringRef line) {
  return startsWithAny(line, {"fn ", "def ", "struct ", "trait "});
}

bool isIndented(StringRef line) { return startsWithAny(line, {" ", "\t"}); }

bool takeTopLevelStatement(unsigned &line, ArrayRef<StringRef> lines,
                           SmallVectorImpl<StringRef> &topLevel) {
  StringRef current = lines[line];
  if (current.starts_with("@")) {
    topLevel.push_back(current);
    ++line;
    return true;
  }

  bool declaration = isDeclaration(current);
  bool topLevelStatement = declaration || current.starts_with("from ") ||
                           current.starts_with("alias ") ||
                           current.starts_with("comptime");
  if (!topLevelStatement)
    return false;

  bool requiresColon = declaration;
  for (size_t openings = 0; line < lines.size();) {
    topLevel.push_back(lines[line++]);
    for (char character : topLevel.back()) {
      if (character == '#')
        break;
      if (character == '(' || character == '[')
        ++openings;
      else if (character == ')' || character == ']')
        --openings;
      else if (character == ':' && openings == 0) {
        requiresColon = false;
        break;
      }
    }
    if (openings == 0 && !requiresColon)
      break;
  }

  if (declaration) {
    while (line < lines.size()) {
      StringRef bodyLine = lines[line];
      if (!bodyLine.empty() && !isIndented(bodyLine) &&
          !bodyLine.starts_with("#"))
        break;
      topLevel.push_back(bodyLine);
      ++line;
    }
  }
  return true;
}

void partitionCell(StringRef source, SmallVectorImpl<StringRef> &topLevel,
                   SmallVectorImpl<StringRef> &body) {
  SmallVector<StringRef> lines;
  source.split(lines, '\n');

  size_t minimumIndent = std::numeric_limits<size_t>::max();
  for (StringRef &line : lines) {
    line = line.trim("\r");
    if (!line.empty())
      minimumIndent = std::min(minimumIndent, indentation(line));
  }
  if (minimumIndent == std::numeric_limits<size_t>::max())
    minimumIndent = 0;

  for (StringRef &line : lines) {
    if (!line.empty() && minimumIndent)
      line = line.drop_front(minimumIndent);
  }

  for (unsigned line = 0; line < lines.size();) {
    if (takeTopLevelStatement(line, lines, topLevel))
      continue;
    if (lines[line].starts_with("import "))
      topLevel.push_back(lines[line]);
    else if (!lines[line].empty())
      body.push_back(lines[line]);
    ++line;
  }
}

struct WrappedCell {
  std::string source;
  std::string declarations;
  CellSourceMap map;
};

struct TrailingExpression {
  const char *start;
  const char *end;
};

void updateBrackets(const Token &token, SmallVectorImpl<Token::Kind> &open) {
  auto close = [&](Token::Kind expected) {
    if (!open.empty() && open.back() == expected)
      open.pop_back();
    else
      open.clear();
  };
  switch (token.getKind()) {
  case Token::l_paren:
  case Token::l_square:
  case Token::l_brace:
    open.push_back(token.getKind());
    break;
  case Token::r_paren:
    close(Token::l_paren);
    break;
  case Token::r_square:
    close(Token::l_square);
    break;
  case Token::r_brace:
    close(Token::l_brace);
    break;
  default:
    break;
  }
}

std::optional<LexerCursor>
findTrailingStatement(SharedState &state, const llvm::MemoryBuffer *buf) {
  size_t baseIndent = std::numeric_limits<size_t>::max();
  for (Lexer lexer(state.diags, buf); !lexer.getToken().is(Token::eof);
       lexer.lexToken()) {
    if (auto indent = lexer.getToken().getIndentation())
      baseIndent = std::min(baseIndent, *indent);
  }
  if (baseIndent == std::numeric_limits<size_t>::max())
    return std::nullopt;

  std::optional<LexerCursor> trailing;
  SmallVector<Token::Kind> openBrackets;
  Lexer lexer(state.diags, buf);
  while (!lexer.getToken().is(Token::eof)) {
    const Token &token = lexer.getToken();
    if (openBrackets.empty()) {
      if (token.getIndentation() == baseIndent)
        trailing = lexer.getCursor();
      if (token.is(Token::semi)) {
        lexer.lexToken();
        if (!lexer.getToken().is(Token::eof) &&
            !lexer.getToken().isStartOfLine())
          trailing = lexer.getCursor();
        continue;
      }
    }
    updateBrackets(token, openBrackets);
    lexer.lexToken();
  }
  return trailing;
}

bool requiresFollowingToken(Token::Kind kind) {
  switch (kind) {
  case Token::comma:
  case Token::dot:
  case Token::colon:
  case Token::equal:
  case Token::plus:
  case Token::minus:
  case Token::star:
  case Token::star_star:
  case Token::slash:
  case Token::slash_slash:
  case Token::percent:
  case Token::at:
  case Token::amp:
  case Token::pipe:
  case Token::caret:
  case Token::less:
  case Token::greater:
  case Token::less_equal:
  case Token::greater_equal:
  case Token::equal_equal:
  case Token::exclaim_equal:
  case Token::plus_equal:
  case Token::minus_equal:
  case Token::star_equal:
  case Token::slash_equal:
  case Token::kw_and:
  case Token::kw_or:
  case Token::kw_not:
    return true;
  default:
    return false;
  }
}

CompletenessResult checkCompleteness(SharedState &state,
                                     llvm::SourceMgr &sourceManager,
                                     StringRef source) {
  unsigned id = sourceManager.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy(source, "Mojo completeness"),
      llvm::SMLoc());
  const llvm::MemoryBuffer *buffer = sourceManager.getMemoryBuffer(id);
  auto oldHandler = sourceManager.getDiagHandler();
  void *oldContext = sourceManager.getDiagContext();
  sourceManager.setDiagHandler([](const llvm::SMDiagnostic &, void *) {},
                               nullptr);
  auto restoreHandler = llvm::scope_exit(
      [&] { sourceManager.setDiagHandler(oldHandler, oldContext); });

  SmallVector<Token::Kind> openBrackets;
  Token::Kind lastKind = Token::eof;
  size_t lineIndent = 0;
  bool invalid = false;
  bool lexicalError = false;
  auto close = [&](Token::Kind expected) {
    if (openBrackets.empty() || openBrackets.back() != expected) {
      invalid = true;
      return;
    }
    openBrackets.pop_back();
  };
  for (Lexer lexer(state.diags, buffer); !lexer.getToken().is(Token::eof);
       lexer.lexToken()) {
    const Token &token = lexer.getToken();
    lastKind = token.getKind();
    lexicalError |= token.is(Token::error);
    if (auto indent = token.getIndentation())
      lineIndent = *indent;
    switch (token.getKind()) {
    case Token::l_paren:
    case Token::l_square:
    case Token::l_brace:
      openBrackets.push_back(token.getKind());
      break;
    case Token::r_paren:
      close(Token::l_paren);
      break;
    case Token::r_square:
      close(Token::l_square);
      break;
    case Token::r_brace:
      close(Token::l_brace);
      break;
    default:
      break;
    }
  }
  state.diags.clear();

  if (invalid)
    return {CompletenessStatus::Invalid, {}};
  if (lexicalError || !openBrackets.empty() || requiresFollowingToken(lastKind))
    return {CompletenessStatus::Incomplete, std::string(lineIndent + 2, ' ')};
  return {};
}

bool canStartTrailingExpression(Token::Kind kind) {
  if (!ParserBase::isPrimaryExprStart(kind))
    return false;
  return kind != Token::kw_async && kind != Token::kw_comptime &&
         kind != Token::kw_def && kind != Token::kw_fn &&
         kind != Token::dot_dot_dot;
}

std::optional<TrailingExpression>
findTrailingExpression(SharedState &state, const llvm::MemoryBuffer *buffer) {
  std::optional<LexerCursor> cursor = findTrailingStatement(state, buffer);
  if (!cursor || !canStartTrailingExpression(cursor->getToken().getKind()))
    return std::nullopt;

  Lexer lexer(state.diags, *cursor);
  ExprNode *expression = nullptr;
  size_t indentation = cursor->getToken().getIndentation().value_or(0);
  if (failed(ParserBase(state, lexer)
                 .parseSimpleStmtExprs(expression, indentation)) ||
      !expression || !lexer.getToken().is(Token::eof))
    return std::nullopt;
  if (expression->kind == ExprNode::kTypePattern ||
      (expression->kind >= ExprNode::kFirstAssignStmt &&
       expression->kind <= ExprNode::kLastAssignStmt))
    return std::nullopt;

  llvm::SMRange range = state.diags.convertToSMRange(expression->getRange());
  if (!range.isValid())
    return std::nullopt;
  return TrailingExpression{range.Start.getPointer(), range.End.getPointer()};
}

void emitMappedLine(llvm::raw_string_ostream &output, CellSourceMap &map,
                    StringRef line,
                    std::optional<TrailingExpression> expression) {
  const char *position = line.begin();
  const char *lineEnd = line.end();
  auto emitSource = [&](const char *end) {
    if (end == position)
      return;
    StringRef text(position, end - position);
    map.add(text, output.str().size());
    output << text;
    position = end;
  };
  if (expression && expression->start >= line.begin() &&
      expression->start <= lineEnd) {
    emitSource(expression->start);
    output << "__xmojo_cell_display(";
  }
  if (expression && expression->end >= line.begin() &&
      expression->end <= lineEnd) {
    emitSource(expression->end);
    output << ')';
  }
  emitSource(lineEnd);
}

SmallVector<std::string> findTopLevelVariables(SharedState &state,
                                               const llvm::MemoryBuffer *buf) {
  size_t baseIndent = std::numeric_limits<size_t>::max();
  for (Lexer lexer(state.diags, buf); !lexer.getToken().is(Token::eof);
       lexer.lexToken()) {
    if (auto indent = lexer.getToken().getIndentation())
      baseIndent = std::min(baseIndent, *indent);
  }
  if (baseIndent == std::numeric_limits<size_t>::max())
    return {};

  SmallVector<std::string> variables;
  SmallVector<Token::Kind> openBrackets;
  bool statementAtBase = false;
  bool afterSemicolon = false;
  bool expectName = false;
  for (Lexer lexer(state.diags, buf); !lexer.getToken().is(Token::eof);
       lexer.lexToken()) {
    const Token &token = lexer.getToken();
    if (token.isStartOfLine())
      statementAtBase = token.getIndentation() == baseIndent;

    bool statementStart = token.isStartOfLine() || afterSemicolon;
    afterSemicolon = false;
    if (expectName) {
      if (token.isIdentifier() &&
          !llvm::is_contained(variables, token.getSpelling()))
        variables.push_back(token.getSpelling().str());
      expectName = false;
    } else if (statementStart && statementAtBase && openBrackets.empty() &&
               token.is(Token::kw_var)) {
      expectName = true;
    }

    updateBrackets(token, openBrackets);
    if (openBrackets.empty() && token.is(Token::semi))
      afterSemicolon = true;
  }
  return variables;
}

std::string persistentTypeName(size_t index) {
  return ("__xmojo_var_type_" + llvm::Twine(index)).str();
}

WrappedCell
wrapCell(StringRef source, const llvm::MemoryBuffer *sourceBuffer,
         SharedState &state, StringRef functionName,
         ArrayRef<xmojo::InteractiveParser::PersistentVar> sessionVars,
         ArrayRef<std::string> declaredVars) {
  SmallVector<StringRef> topLevel;
  SmallVector<StringRef> body;
  partitionCell(source, topLevel, body);
  std::optional<TrailingExpression> trailing =
      findTrailingExpression(state, sourceBuffer);

  WrappedCell wrapped;
  llvm::raw_string_ostream declarations(wrapped.declarations);
  for (StringRef line : topLevel)
    declarations << line << '\n';
  declarations.flush();

  llvm::raw_string_ostream output(wrapped.source);
  output << "def " << functionName << "(__xmojo_slots: __xmojo_cell_slots):\n";
  for (size_t index = 0; index < sessionVars.size(); ++index) {
    output << "  ref `" << sessionVars[index].name
           << "` = __xmojo_slots[unsafe_offset=" << index << "].unsafe_bitcast["
           << persistentTypeName(index) << "]()[]\n";
  }
  output << "  try:\n";
  if (body.empty()) {
    output << "    pass\n";
  } else {
    for (StringRef line : body) {
      output << "    ";
      emitMappedLine(output, wrapped.map, line, trailing);
      output << '\n';
    }
  }
  for (size_t index = 0; index < declaredVars.size(); ++index) {
    output << "    __xmojo_cell_persist(__xmojo_slots, "
           << (sessionVars.size() + index) << ", `" << declaredVars[index]
           << "`^)\n";
  }
  output << "  except error:\n"
         << "    __xmojo_cell_error(error)\n\n";

  for (StringRef line : topLevel) {
    wrapped.map.add(line, output.str().size());
    output << line << '\n';
  }
  if (trailing)
    output << "from xmojo import __xmojo_display as "
              "__xmojo_cell_display\n";
  if (!declaredVars.empty())
    output << "from xmojo import __xmojo_persist as "
              "__xmojo_cell_persist\n";
  output << "from xmojo import __xmojo_slot_array as __xmojo_cell_slots\n";
  output << "from xmojo import __xmojo_error as __xmojo_cell_error\n";
  output.flush();
  return wrapped;
}

ASTDecl &
buildModule(const llvm::MemoryBuffer *sourceBuffer, StringRef moduleName,
            SharedState &state, M::MojoASTDeclRef previous,
            ArrayRef<xmojo::InteractiveParser::PersistentVar> sessionVars) {
  MLIRContext *context = state.getContext();
  auto location =
      FileLineColLoc::get(context, sourceBuffer->getBufferIdentifier(), 1, 1);
  ASTDecl &module = state.createModule(moduleName, sourceBuffer, location);
  (void)state.declResolver->resolveBody(module, module.getLoc());

  for (size_t index = 0; index < sessionVars.size(); ++index) {
    const auto &sessionVar = sessionVars[index];
    (void)state.resolveDeclReferencesIn(llvm::SMLoc(), sessionVar.type);
    M::KGEN::LIT::PValue typeValue(sessionVar.type);
    mlir::OpBuilder builder = module.getDeclEndBuilder();
    auto typeDecl = M::KGEN::LIT::AliasDeclOp::create(
        builder, builder.getUnknownLoc(),
        M::KGEN::ParamDeclAttr::get(persistentTypeName(index),
                                    typeValue.getType()),
        typeValue.get());
    state.declResolver->addFullyResolvedDecl(
        &*typeDecl, persistentTypeName(index), llvm::SMLoc(), &module);
  }

  if (previous) {
    SmallVector<
        std::pair<mlir::StringAttr, const llvm::TinyPtrVector<ASTDecl *>>>
        overlappingFunctions;
    for (auto &[name, declarations] : previous->getDeclsInScope()) {
      auto existing = module.lookupInCurrentScope(name);
      if (existing.empty()) {
        state.declResolver->aliasDecls(declarations, name, llvm::SMLoc(),
                                       module);
      } else if (llvm::isa_and_nonnull<FnOp>(
                     existing.front()->getIfOperation()) &&
                 llvm::isa_and_nonnull<FnOp>(
                     declarations.front()->getIfOperation())) {
        overlappingFunctions.push_back({name, declarations});
      }
    }
    for (auto &[name, declarations] : overlappingFunctions)
      (void)state.declResolver->tryAliasDecls(declarations, name, llvm::SMLoc(),
                                              module);
  }

  state.declResolver->resolveAllReferencedFrom(module,
                                               /*eraseUnparsedDecls=*/false);
  (void)state.declResolver->resolveAllWildcardImports(module);
  return module;
}

ASTDecl *lookupSingleDeclaration(ASTDecl &scope, StringRef name) {
  auto declarations = scope.lookupInCurrentScope(name);
  return declarations.size() == 1 ? declarations.front() : nullptr;
}

bool collectDeclaredVarTypes(
    ASTDecl &entryPoint, ArrayRef<std::string> declaredVars,
    std::vector<xmojo::InteractiveParser::PersistentVar> &newVars) {
  auto entryFunction =
      llvm::dyn_cast_or_null<FnOp>(entryPoint.getIfOperation());
  if (!entryFunction)
    return false;

  llvm::StringMap<mlir::Type> resolved;
  entryFunction->walk([&](M::KGEN::LIT::VarDeclOp varOp) {
    StringRef name = varOp.getNameAttr().strref();
    if (llvm::is_contained(declaredVars, name))
      resolved.try_emplace(name, varOp.getType().getElementType());
  });
  for (const std::string &name : declaredVars) {
    auto entry = resolved.find(name);
    if (entry == resolved.end())
      return false;
    newVars.push_back({name, entry->second});
  }
  return true;
}

bool isStaticOrigin(mlir::TypedAttr origin) {
  using namespace M::KGEN::LIT;
  origin = OriginType::stripMutCastAndRebind(origin);
  if (M::KGEN::sugarIsa<StaticOriginAttr>(origin))
    return true;
  if (auto field = M::KGEN::sugarDynCast<OriginFieldAttr>(origin))
    return isStaticOrigin(field.getBase());
  if (auto interior = M::KGEN::sugarDynCast<InteriorOriginAttr>(origin))
    return isStaticOrigin(interior.getBase());
  if (auto subtree = M::KGEN::sugarDynCast<OriginSubtreeAttr>(origin))
    return isStaticOrigin(subtree.getBase());
  if (auto originUnion = M::KGEN::sugarDynCast<OriginUnionAttr>(origin))
    return !originUnion.getOperands().empty() &&
           llvm::all_of(originUnion.getOperands(), isStaticOrigin);
  return false;
}

bool hasNonStaticOrigin(mlir::Type type) {
  bool result = false;
  type.walk([&](mlir::TypedAttr attribute) {
    if (mlir::isa<M::KGEN::LIT::OriginType>(attribute.getType()) &&
        !isStaticOrigin(attribute))
      result = true;
  });
  return result;
}

std::string persistentVariableDeclaration(
    const xmojo::InteractiveParser::PersistentVar &variable,
    SharedState &state) {
  return "var " + variable.name + ": " +
         M::MojoASTTypeRef(variable.type).getAsString(state);
}

std::string persistentVariableMarkdown(
    const xmojo::InteractiveParser::PersistentVar &variable,
    SharedState &state) {
  return "### variable `" + variable.name + "`\n\n---\n\n###\n```mojo\n" +
         persistentVariableDeclaration(variable, state) + "\n```";
}

class REPLListener final : public M::MojoParserREPLListener {
public:
  void notifyWrappedExpr(StringRef) override {}
  void notifyFixedExpr(StringRef) override {}
  void notifyDiagnostics(ArrayRef<llvm::SMDiagnostic>) override {}
  bool shouldPersistVariable(StringRef, mlir::Type) override { return false; }
};

class ReferenceCollector final : public M::KGEN::LIT::ParserListener {
public:
  struct Reference {
    SmallVector<M::MojoASTDeclRef> declarations;
    llvm::SMRange range;
  };

  bool isInterestedInLoc(llvm::SMLoc) override { return collecting; }

  void onRef(ArrayRef<ASTDecl *> declarations, StringRef,
             llvm::SMRange range) override {
    if (!collecting)
      return;
    Reference &reference = references.emplace_back();
    reference.range = range;
    llvm::transform(declarations, std::back_inserter(reference.declarations),
                    [](ASTDecl *decl) { return M::MojoASTDeclRef(decl); });
  }

  bool collecting = false;
  std::vector<Reference> references;
};

CompletionKind
convertCompletionKind(M::KGEN::Mojo::CodeCompletionResult::Kind kind) {
  using Kind = M::KGEN::Mojo::CodeCompletionResult::Kind;
  switch (kind) {
  case Kind::kPackage:
    return CompletionKind::Package;
  case Kind::kModule:
    return CompletionKind::Module;
  case Kind::kStruct:
    return CompletionKind::Struct;
  case Kind::kFunction:
    return CompletionKind::Function;
  case Kind::kField:
    return CompletionKind::Field;
  case Kind::kVariable:
    return CompletionKind::Variable;
  case Kind::kTrait:
    return CompletionKind::Trait;
  case Kind::kUnknown:
    return CompletionKind::Unknown;
  }
  llvm_unreachable("unknown Mojo completion kind");
}

class ToolingState {
public:
  ToolingState(MLIRContext &context, const M::KGEN::CompilationOptions &options,
               ArrayRef<std::string> importPaths)
      : parserConfig(&context, options) {
    sourceManager.setIncludeDirs(importPaths);
    parserConfig.parserListener = &references;
    parserContext =
        std::make_unique<M::MojoParserContext>(sourceManager, parserConfig);
  }

  void record(StringRef source, StringRef moduleName,
              ArrayRef<xmojo::InteractiveParser::PersistentVar> variables) {
    unsigned id = sourceManager.AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(source, moduleName),
        llvm::SMLoc());
    auto types = variableTypes(variables);
    (void)parserContext->parseREPLExpression(replListener, id,
                                             "__xmojo_tooling_cell", types);
  }

  CompletionResult
  complete(StringRef source, size_t cursorPosition,
           ArrayRef<xmojo::InteractiveParser::PersistentVar> variables) {
    cursorPosition = std::min(cursorPosition, source.size());
    CompletionResult result;
    result.cursorEnd = cursorPosition;
    result.cursorStart = cursorPosition;
    while (result.cursorStart &&
           isCompletionCharacter(source[result.cursorStart - 1]))
      --result.cursorStart;

    auto types = variableTypes(variables);
    auto completions = parserContext->codeCompleteREPLExpression(
        source, cursorPosition, types);
    result.items.reserve(completions.size());
    for (auto &completion : completions) {
      result.items.push_back({std::move(completion.label),
                              std::move(completion.documentation),
                              convertCompletionKind(completion.kind)});
    }
    return result;
  }

  InspectionResult
  inspect(StringRef source, size_t cursorPosition,
          ArrayRef<xmojo::InteractiveParser::PersistentVar> variables) {
    InspectionResult result;
    if (cursorPosition > source.size())
      return result;

    references.references.clear();
    references.collecting = true;
    unsigned id = sourceManager.AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(source, "Mojo inspection"),
        llvm::SMLoc());
    const llvm::MemoryBuffer *buffer = sourceManager.getMemoryBuffer(id);
    auto types = variableTypes(variables);
    auto parsed = parserContext->parseREPLExpression(
        replListener, id, "__xmojo_inspection_cell", types);
    references.collecting = false;
    if (!parsed.isValid())
      return inspectPersistent(source, cursorPosition, variables);
    parserContext->removeLastREPLExpression();

    const char *cursor = buffer->getBufferStart() + cursorPosition;
    for (const ReferenceCollector::Reference &reference :
         references.references) {
      llvm::SMRange range =
          parserContext->getREPLLocMapper().mapRange(reference.range);
      if (!range.isValid() || cursor < range.Start.getPointer() ||
          cursor > range.End.getPointer())
        continue;
      for (M::MojoASTDeclRef declaration : reference.declarations) {
        std::unique_ptr<M::PublicDecl> view = declaration.getDecl();
        if (!view)
          continue;
        if (!result.markdown.empty())
          result.markdown += "\n---\n\n";
        result.markdown += view->getFullMarkdownString(*parserContext);
      }
      result.found = !result.markdown.empty();
      if (result.found)
        return result;
      break;
    }
    return inspectPersistent(source, cursorPosition, variables);
  }

private:
  static SmallVector<std::pair<StringRef, mlir::Type>>
  variableTypes(ArrayRef<xmojo::InteractiveParser::PersistentVar> variables) {
    SmallVector<std::pair<StringRef, mlir::Type>> result;
    result.reserve(variables.size());
    for (const auto &variable : variables)
      result.push_back({variable.name, variable.type});
    return result;
  }

  static bool isCompletionCharacter(char character) {
    return llvm::isAlnum(static_cast<unsigned char>(character)) ||
           character == '_' || character == '$';
  }

  InspectionResult inspectPersistent(
      StringRef source, size_t cursorPosition,
      ArrayRef<xmojo::InteractiveParser::PersistentVar> variables) {
    size_t start = cursorPosition;
    while (start && isCompletionCharacter(source[start - 1]))
      --start;
    size_t end = cursorPosition;
    while (end < source.size() && isCompletionCharacter(source[end]))
      ++end;
    StringRef name = source.slice(start, end);
    if (name.empty())
      return {};

    if (start && source[start - 1] == '.') {
      CompletionResult completions = complete(source, end, variables);
      InspectionResult result;
      for (const auto &item : completions.items) {
        if (item.label != name || item.documentation.empty())
          continue;
        if (!result.markdown.empty())
          result.markdown += "\n---\n\n";
        result.markdown += item.documentation;
      }
      result.found = !result.markdown.empty();
      return result;
    }

    auto variable = llvm::find_if(variables, [&](const auto &candidate) {
      return candidate.name == name;
    });
    if (variable == variables.end())
      return {};
    return {true, persistentVariableMarkdown(*variable,
                                             parserContext->getSharedState())};
  }

  llvm::SourceMgr sourceManager;
  ReferenceCollector references;
  M::KGEN::LIT::ParserConfig parserConfig;
  std::unique_ptr<M::MojoParserContext> parserContext;
  REPLListener replListener;
};

} // namespace

class xmojo::InteractiveParser::Impl {
public:
  Impl(MLIRContext &context, const M::KGEN::CompilationOptions &options,
       std::vector<std::string> importPaths)
      : parserConfig(&context, options),
        parserContext(sourceManager, parserConfig) {
    sourceManager.setIncludeDirs(importPaths);
    tooling = std::make_unique<ToolingState>(context, options, importPaths);
  }

  std::optional<Cell> parse(StringRef source, StringRef moduleName,
                            StringRef functionName,
                            std::vector<Diagnostic> &diagnostics) {
    unsigned sourceID = sourceManager.AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(source, moduleName),
        llvm::SMLoc());
    const llvm::MemoryBuffer *sourceBuffer =
        sourceManager.getMemoryBuffer(sourceID);

    SharedState &state = parserContext.getSharedState();
    SmallVector<std::string> declaredVars =
        findTopLevelVariables(state, sourceBuffer);
    for (const std::string &declared : declaredVars) {
      if (llvm::none_of(persistentVars, [&](const PersistentVar &variable) {
            return variable.name == declared;
          }))
        continue;
      diagnostics.push_back(
          {DiagnosticSeverity::Error,
           ("persistent variable '" + declared +
            "' is already declared; assign to it without 'var'")});
      return std::nullopt;
    }

    WrappedCell wrapped =
        wrapCell(sourceBuffer->getBuffer(), sourceBuffer, state, functionName,
                 persistentVars, declaredVars);
    auto wrappedBuffer = llvm::MemoryBuffer::getMemBufferCopy(
        wrapped.source, (moduleName + " wrapper").str());
    unsigned wrappedID = sourceManager.AddNewSourceBuffer(
        std::move(wrappedBuffer), llvm::SMLoc());
    const llvm::MemoryBuffer *buffer = sourceManager.getMemoryBuffer(wrappedID);
    auto sourceMap = std::make_unique<CellSourceMap>(std::move(wrapped.map));
    sourceMap->setWrapped(buffer->getBuffer());
    maps.push_back(std::move(sourceMap));
    parserContext.getSharedState().registerWrapperBuffer(wrappedID, moduleName);

    bool emittedError = false;
    auto oldHandler = sourceManager.getDiagHandler();
    void *oldContext = sourceManager.getDiagContext();
    struct DiagnosticHandlerContext {
      Impl *parser;
      std::vector<Diagnostic> *diagnostics;
      bool *emittedError;
    } handlerContext{this, &diagnostics, &emittedError};
    sourceManager.setDiagHandler(
        [](const llvm::SMDiagnostic &diagnostic, void *context) {
          auto &handler = *static_cast<DiagnosticHandlerContext *>(context);
          const CellSourceMap &map = *handler.parser->maps.back();
          if (diagnostic.getKind() != llvm::SourceMgr::DK_Error &&
              !map.mapLocation(diagnostic.getLoc()).isValid())
            return;
          llvm::SMDiagnostic mapped =
              map.mapDiagnostic(diagnostic, handler.parser->sourceManager);
          std::string message;
          llvm::raw_string_ostream stream(message);
          mapped.print("xmojo", stream, /*ShowColors=*/false,
                       /*ShowKindLabel=*/true);
          handler.diagnostics->push_back(
              {convertSeverity(mapped.getKind()), std::move(message)});
          *handler.emittedError |=
              mapped.getKind() == llvm::SourceMgr::DK_Error;
        },
        &handlerContext);
    auto restoreHandler = llvm::scope_exit(
        [&] { sourceManager.setDiagHandler(oldHandler, oldContext); });

    ASTDecl &module =
        buildModule(buffer, moduleName, state, committedModule, persistentVars);
    state.diags.clear();
    if (emittedError)
      return std::nullopt;

    ASTDecl *entryPoint = lookupSingleDeclaration(module, functionName);
    if (!entryPoint) {
      diagnostics.push_back({DiagnosticSeverity::Error,
                             "interactive cell entry point was not produced"});
      return std::nullopt;
    }
    Cell cell{M::MojoASTDeclRef(&module),
              M::MojoASTDeclRef(entryPoint),
              source.str(),
              std::move(wrapped.declarations),
              moduleName.str(),
              {}};
    if (!collectDeclaredVarTypes(*entryPoint, declaredVars, cell.newVars)) {
      diagnostics.push_back({DiagnosticSeverity::Error,
                             "interactive cell declared a variable "
                             "the compiler did not resolve"});
      return std::nullopt;
    }
    for (const PersistentVar &variable : cell.newVars) {
      if (!hasNonStaticOrigin(variable.type))
        continue;
      diagnostics.push_back({DiagnosticSeverity::Error,
                             "cannot persist borrowed value in variable '" +
                                 variable.name +
                                 "'; persist an owned copy instead"});
      return std::nullopt;
    }
    return cell;
  }

  CompletionResult complete(StringRef source, size_t cursorPosition) {
    CompletionResult result =
        tooling->complete(source, cursorPosition, persistentVars);
    StringRef prefix = source.slice(result.cursorStart, result.cursorEnd);
    for (const PersistentVar &variable : persistentVars) {
      if (!StringRef(variable.name).starts_with(prefix) ||
          llvm::any_of(result.items, [&](const auto &item) {
            return item.label == variable.name;
          }))
        continue;
      std::string signature = persistentVariableDeclaration(
          variable, parserContext.getSharedState());
      result.items.push_back(
          {variable.name,
           persistentVariableMarkdown(variable, parserContext.getSharedState()),
           CompletionKind::Variable, std::move(signature)});
    }
    return result;
  }

  InspectionResult inspect(StringRef source, size_t cursorPosition) {
    return tooling->inspect(source, cursorPosition, persistentVars);
  }

  CompletenessResult isComplete(StringRef source) {
    return checkCompleteness(parserContext.getSharedState(), sourceManager,
                             source);
  }

  llvm::SourceMgr sourceManager;
  M::KGEN::LIT::ParserConfig parserConfig;
  M::MojoParserContext parserContext;
  M::MojoASTDeclRef committedModule;
  std::vector<PersistentVar> persistentVars;
  std::vector<std::unique_ptr<CellSourceMap>> maps;
  std::unique_ptr<ToolingState> tooling;
};

xmojo::InteractiveParser::InteractiveParser(
    MLIRContext &context, const M::KGEN::CompilationOptions &options,
    std::vector<std::string> importPaths)
    : impl(std::make_unique<Impl>(context, options, std::move(importPaths))) {}

xmojo::InteractiveParser::~InteractiveParser() = default;

std::optional<xmojo::InteractiveParser::Cell>
xmojo::InteractiveParser::parse(StringRef source, StringRef moduleName,
                                StringRef functionName,
                                std::vector<Diagnostic> &diagnostics) {
  return impl->parse(source, moduleName, functionName, diagnostics);
}

void xmojo::InteractiveParser::commit(const Cell &cell) {
  impl->committedModule = cell.moduleDecl;
  impl->tooling->record(cell.source, cell.moduleName, impl->persistentVars);
}

void xmojo::InteractiveParser::activateVariables(const Cell &cell) {
  llvm::append_range(impl->persistentVars, cell.newVars);
}

CompletionResult xmojo::InteractiveParser::complete(StringRef source,
                                                    size_t cursorPosition) {
  return impl->complete(source, cursorPosition);
}

InspectionResult xmojo::InteractiveParser::inspect(StringRef source,
                                                   size_t cursorPosition) {
  return impl->inspect(source, cursorPosition);
}

CompletenessResult xmojo::InteractiveParser::isComplete(StringRef source) {
  return impl->isComplete(source);
}

llvm::SourceMgr &xmojo::InteractiveParser::getSourceManager() {
  return impl->sourceManager;
}
