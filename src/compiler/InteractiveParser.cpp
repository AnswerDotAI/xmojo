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

#include "KGEN/LITDialect/LITOps.h"
#include "KGEN/MojoParser/ASTDecl.h"
#include "KGEN/MojoParser/DeclResolver.h"
#include "KGEN/MojoParser/EntryPoint.h"
#include "KGEN/MojoParser/SharedState.h"
#include "KGEN/MojoTooling/ParserDriver.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/ScopeExit.h"
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
using M::KGEN::LIT::FnOp;
using M::KGEN::LIT::SharedState;
using mlir::FileLineColLoc;
using mlir::MLIRContext;

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
  CellSourceMap map;
};

WrappedCell wrapCell(StringRef source, StringRef functionName) {
  SmallVector<StringRef> topLevel;
  SmallVector<StringRef> body;
  partitionCell(source, topLevel, body);

  WrappedCell wrapped;
  llvm::raw_string_ostream output(wrapped.source);
  output << "def " << functionName << "():\n"
         << "  try:\n";
  if (body.empty()) {
    output << "    pass\n";
  } else {
    for (StringRef line : body) {
      output << "    ";
      wrapped.map.add(line, output.str().size());
      output << line << '\n';
    }
  }
  output << "  except error:\n"
         << "    print(\"Error:\", error)\n\n";

  for (StringRef line : topLevel) {
    wrapped.map.add(line, output.str().size());
    output << line << '\n';
  }
  output.flush();
  return wrapped;
}

ASTDecl &buildModule(const llvm::MemoryBuffer *sourceBuffer,
                     StringRef moduleName, SharedState &state,
                     M::MojoASTDeclRef previous) {
  MLIRContext *context = state.getContext();
  auto location =
      FileLineColLoc::get(context, sourceBuffer->getBufferIdentifier(), 1, 1);
  ASTDecl &module = state.createModule(moduleName, sourceBuffer, location);
  (void)state.declResolver->resolveBody(module, module.getLoc());

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

} // namespace

class xmojo::InteractiveParser::Impl {
public:
  Impl(MLIRContext &context, const M::KGEN::CompilationOptions &options,
       std::vector<std::string> importPaths)
      : parserConfig(&context, options),
        parserContext(sourceManager, parserConfig) {
    sourceManager.setIncludeDirs(importPaths);
  }

  std::optional<Cell> parse(StringRef source, StringRef moduleName,
                            StringRef functionName,
                            std::vector<Diagnostic> &diagnostics) {
    unsigned sourceID = sourceManager.AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(source, moduleName),
        llvm::SMLoc());
    const llvm::MemoryBuffer *sourceBuffer =
        sourceManager.getMemoryBuffer(sourceID);

    WrappedCell wrapped = wrapCell(sourceBuffer->getBuffer(), functionName);
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
          llvm::SMDiagnostic mapped =
              handler.parser->maps.back()->mapDiagnostic(
                  diagnostic, handler.parser->sourceManager);
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

    SharedState &state = parserContext.getSharedState();
    ASTDecl &module = buildModule(buffer, moduleName, state, committedModule);
    state.diags.clear();
    if (emittedError)
      return std::nullopt;

    ASTDecl *entryPoint = lookupSingleDeclaration(module, functionName);
    if (!entryPoint) {
      diagnostics.push_back({DiagnosticSeverity::Error,
                             "interactive cell entry point was not produced"});
      return std::nullopt;
    }
    return Cell{M::MojoASTDeclRef(&module), M::MojoASTDeclRef(entryPoint)};
  }

  llvm::SourceMgr sourceManager;
  M::KGEN::LIT::ParserConfig parserConfig;
  M::MojoParserContext parserContext;
  M::MojoASTDeclRef committedModule;
  std::vector<std::unique_ptr<CellSourceMap>> maps;
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
}

llvm::SourceMgr &xmojo::InteractiveParser::getSourceManager() {
  return impl->sourceManager;
}
