//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"
#include "InteractiveParser.h"

#include "AsyncRT/CompilerSupport/Context.h"
#include "AsyncRT/Runtime/CPUDevice.h"
#include "Init/Init.h"
#include "KGEN/Compiler/KGENCompiler.h"
#include "KGEN/Compiler/ObjectCompiler.h"
#include "KGEN/ExecutionEngine/JIT/StaticArchiveLayer.h"
#include "KGEN/KGENDialect/KGENDialect.h"
#include "KGEN/KGENDialect/KGENUtils.h"
#include "KGEN/LITDialect/LITOps.h"
#include "KGEN/MojoParser/EntryPoint.h"
#include "KGEN/Support/Configuration.h"
#include "KGEN/ToolCommon/InitAllDialects.h"
#include "KGEN/TransformUtils/SlicingUtils.h"
#include "Support/MDialect/MAttrs.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unistd.h>

using namespace M;
using namespace M::KGEN;
using namespace mlir;
using namespace xmojo;

namespace {

thread_local OutputCallback *activeOutput = nullptr;
thread_local DisplayCallback *activeDisplay = nullptr;
thread_local std::optional<DisplayEvent> pendingDisplay;
thread_local std::optional<std::string> pendingError;

void writeAll(int fileDescriptor, const char *data, size_t size) {
  while (size != 0) {
    ssize_t written = ::write(fileDescriptor, data, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return;
    data += written;
    size -= written;
  }
}

extern "C" void xmojo_emit_print(const char *data, size_t size,
                                 int32_t fileDescriptor) {
  if (activeOutput &&
      (fileDescriptor == STDOUT_FILENO || fileDescriptor == STDERR_FILENO)) {
    (*activeOutput)(fileDescriptor == STDOUT_FILENO ? OutputStream::Stdout
                                                    : OutputStream::Stderr,
                    StringRef(data, size));
    return;
  }
  writeAll(fileDescriptor, data, size);
}

// Set when a cell raises: the wrapper's except clause routes the error here
// instead of printing it, so execute() can report a failed cell.
extern "C" void xmojo_emit_error(const char *data, size_t size) {
  pendingError.emplace(data, size);
}

extern "C" void xmojo_display_begin(int32_t kind) {
  pendingDisplay.emplace(DisplayEvent{
      kind == 0 ? DisplayKind::DisplayData : DisplayKind::ExecuteResult, {}});
}

extern "C" void xmojo_display_add(const char *mimeType, size_t mimeTypeSize,
                                  const char *data, size_t dataSize) {
  if (!pendingDisplay)
    return;
  pendingDisplay->data.push_back(
      {std::string(mimeType, mimeTypeSize), std::string(data, dataSize)});
}

extern "C" void xmojo_display_end() {
  if (!pendingDisplay)
    return;
  if (activeDisplay) {
    (*activeDisplay)(std::move(*pendingDisplay));
  } else {
    for (const MimeData &item : pendingDisplay->data) {
      if (item.mimeType != "text/plain")
        continue;
      if (activeOutput)
        (*activeOutput)(OutputStream::Stdout, item.data);
      else
        writeAll(STDOUT_FILENO, item.data.data(), item.data.size());
      if (item.data.empty() || item.data.back() != '\n') {
        if (activeOutput)
          (*activeOutput)(OutputStream::Stdout, "\n");
        else
          writeAll(STDOUT_FILENO, "\n", 1);
      }
      break;
    }
  }
  pendingDisplay.reset();
}

class RuntimeCallbackScope {
public:
  explicit RuntimeCallbackScope(SessionOptions &options)
      : previousOutput(activeOutput), previousDisplay(activeDisplay),
        previousPending(std::move(pendingDisplay)),
        previousError(std::move(pendingError)) {
    OutputCallback &output = options.output;
    activeOutput = output ? &output : nullptr;
    DisplayCallback &display = options.display;
    activeDisplay = display ? &display : nullptr;
    pendingDisplay.reset();
    pendingError.reset();
  }

  ~RuntimeCallbackScope() {
    pendingError = std::move(previousError);
    pendingDisplay = std::move(previousPending);
    activeDisplay = previousDisplay;
    activeOutput = previousOutput;
  }

private:
  OutputCallback *previousOutput;
  DisplayCallback *previousDisplay;
  std::optional<DisplayEvent> previousPending;
  std::optional<std::string> previousError;
};

void initializeTargets() {
  static std::once_flag once;
  std::call_once(once, [] {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
  });
}

} // namespace

class InteractiveSession::Impl {
public:
  static ErrorOr<std::unique_ptr<Impl>> create(SessionOptions options) {
    initializeTargets();

    ErrorOr<ContextRef> runtimeContextOr = Init::getOrCreateContext(
        "xmojo",
        Init::Options().withCPUDeviceOptions(AsyncRT::CPUDeviceOptions()),
        "interactive");
    if (runtimeContextOr.isError())
      return runtimeContextOr.takeError();

    auto impl = std::unique_ptr<Impl>(
        new Impl(std::move(*runtimeContextOr), std::move(options)));
    if (ErrorOrSuccess error = impl->initialize())
      return error.takeError();
    return std::move(impl);
  }

  ErrorOr<ExecutionResult> execute(StringRef source) {
    ExecutionResult result;

    std::string compilerDiagnosticText;
    llvm::raw_string_ostream compilerDiagnosticStream(compilerDiagnosticText);
    SourceMgrDiagnosticHandler compilerDiagnosticHandler(
        interactiveParser->getSourceManager(), &mlirContext,
        compilerDiagnosticStream);

    size_t cellID = nextCellID++;
    std::string moduleName = ("Mojo cell " + Twine(cellID)).str();
    std::string functionName =
        ("__mojo_interactive_cell_" + Twine(cellID)).str();
    std::optional<InteractiveParser::Cell> cell = interactiveParser->parse(
        source, moduleName, functionName, result.diagnostics);
    if (!cell)
      return result;

    auto expressionFunction =
        cast<LIT::FnOp>(cell->entryPointDecl.getIfOperation());
    expressionFunction.setLinkageNameAttr(
        LinkageNameAttr::get(expressionFunction->getContext(), functionName));

    IRMapping mapping;
    OwningOpRef<ModuleOp> module =
        LIT::cloneDeclModuleForCompilation(*cell->moduleDecl, mapping);
    auto clonedFunction = cast<LIT::FnOp>(mapping.lookup(&*expressionFunction));
    clonedFunction.setExported();

    auto signature = clonedFunction.getFuncTypeGenerator();
    auto body = signature.getBody();
    clonedFunction.setFuncTypeGenerator(LIT::FnTypeGeneratorType::get(
        signature.getInputParamTypes(),
        body.getWithFnEffects(body.getFnEffects().setCABI(true)),
        signature.getParamListAttrs()));

    (*module)->setAttr(EnvAttr::getEnvAttrName(), compilationEnvironment);
    extendWithModularEnvAttr(*module, nullptr);

    KGENCompiler compiler(mlirContext, compilationOptions);
    if (ErrorOrSuccess error = compiler.runKGENPipeline(*module, targetInfo)) {
      appendCompilerDiagnostic(result, compilerDiagnosticStream,
                               error.getError());
      return result;
    }

    SymbolTable symbols(*module);
    ExportMap exports;
    exports.insert(
        {StringAttr::get(&mlirContext, functionName), ExportKind::Exported});
    OwningOpRef<ModuleOp> slice = produceStandaloneModule(symbols, exports);
    // Later cells reuse specializations materialized by earlier ones. Keep
    // every reachable definition visible across object additions to the
    // session's JITDylib instead of recompiling the complete history.
    slice->walk([](ExportInterface symbol) {
      symbol.setExportKind(ExportKind::Exported);
    });

    ErrorOr<BufferRef> archiveOr =
        objectCompiler->emitArchive(std::move(slice));
    if (archiveOr.isError()) {
      appendCompilerDiagnostic(result, compilerDiagnosticStream,
                               archiveOr.getError());
      return result;
    }

    constexpr StringLiteral libraryName = "mojo-interactive-session";
    if (ErrorOrSuccess error = executionEngine->add<StaticArchiveLayer>(
            libraryName, archiveOr.takeValue())) {
      return error.takeError();
    }
    if (!runtimeSymbolsDefined) {
      if (ErrorOrSuccess error = defineRuntimeSymbols(libraryName))
        return error.takeError();
      runtimeSymbolsDefined = true;
    }

    ErrorOr<CompiledFunc> functionOr =
        executionEngine->lookup(libraryName, functionName);
    if (functionOr.isError())
      return functionOr.takeError();

    llvm::CrashRecoveryContext crashRecovery;
    crashRecovery.Enable();
    RuntimeCallbackScope callbackScope(options);
    bool executed =
        crashRecovery.RunSafely([&] { functionOr->invoke<void>(); });
    if (!executed) {
      result.diagnostics.push_back(
          {xmojo::DiagnosticSeverity::Error, "Mojo cell execution crashed"});
      return result;
    }

    // A cell that raised at runtime routed its error here: report a failed
    // execution and do not commit the cell to visible history.
    if (pendingError) {
      result.diagnostics.push_back(
          {xmojo::DiagnosticSeverity::Error, std::move(*pendingError)});
      return result;
    }

    interactiveParser->commit(*cell);
    result.succeeded = true;
    return result;
  }

  CompletionResult complete(StringRef source, size_t cursorPosition) {
    return interactiveParser->complete(source, cursorPosition);
  }

  InspectionResult inspect(StringRef source, size_t cursorPosition) {
    return interactiveParser->inspect(source, cursorPosition);
  }

  CompletenessResult isComplete(StringRef source) {
    return interactiveParser->isComplete(source);
  }

  ~Impl() {
    if (ownedObjectCache.empty())
      return;
    // Drop the compiler before removing the cache directory it writes into.
    objectCompiler.reset();
    llvm::sys::fs::remove_directories(ownedObjectCache);
  }

private:
  Impl(ContextRef runtimeContext, SessionOptions options)
      : runtimeContext(std::move(runtimeContext)), options(std::move(options)),
        mlirContext(MLIRContext::Threading::DISABLED) {}

  ErrorOrSuccess initialize() {
    DialectRegistry registry;
    registerAllKGENDialects(registry);
    registerKGENToLLVMTranslation(registry);
    registerContext(registry, runtimeContext);
    mlirContext.appendDialectRegistry(registry);
    mlirContext.loadDialect<KGENDialect>();

    auto compilationEnvironmentOr = compilationOptions.parseDefinesWithDefaults(
        &mlirContext, {"HEAP_BUFFER_BYTES=131072"});
    if (compilationEnvironmentOr.isError())
      return compilationEnvironmentOr.takeError();
    compilationEnvironment = *compilationEnvironmentOr;

    MojoConfig mojoConfig = MojoConfig::fromContext(runtimeContext);
    SmallVector<StringRef> configuredImportPaths;
    mojoConfig.getParserImportPaths(configuredImportPaths);

    std::vector<std::string> importPaths;
    importPaths.reserve(configuredImportPaths.size() +
                        options.importPaths.size());
    for (StringRef path : configuredImportPaths)
      importPaths.push_back(path.str());
    llvm::append_range(importPaths, options.importPaths);
    interactiveParser = std::make_unique<InteractiveParser>(
        mlirContext, compilationOptions, std::move(importPaths));

    auto targetInfoOr = getTargetInfoFor(
        &mlirContext, compilationOptions.targetTriple,
        compilationOptions.targetCpu, compilationOptions.targetFeatures,
        /*tuneCpu=*/"", compilationOptions.targetAccelerator,
        compilationOptions.relocModel, compilationOptions.targetAbi);
    if (targetInfoOr.isError())
      return targetInfoOr.takeError();
    targetInfo = *targetInfoOr;

    std::string objectCache = options.objectCacheDir;
    if (objectCache.empty()) {
      llvm::SmallString<128> temporaryCache;
      if (std::error_code error = llvm::sys::fs::createUniqueDirectory(
              "xmojo-session", temporaryCache))
        return Error("could not create session cache directory: " +
                     error.message());
      ownedObjectCache = temporaryCache.str().str();
      objectCache = ownedObjectCache;
    }

    auto objectCompilerOr = ObjectCompiler::create(
        objectCache, compilationOptions, /*isJIT=*/true, mlirContext);
    if (objectCompilerOr.isError())
      return objectCompilerOr.takeError();
    objectCompiler = objectCompilerOr.takeValue();

    ExecutionEngineOptions executionOptions;
    auto executionEngineOr = initializeExecutionEngine(
        mlirContext, compilationOptions, std::move(executionOptions),
        /*isJIT=*/true);
    if (executionEngineOr.isError())
      return executionEngineOr.takeError();
    executionEngine = executionEngineOr.takeValue();
    return M::success();
  }

  static void appendCompilerDiagnostic(ExecutionResult &result,
                                       llvm::raw_string_ostream &stream,
                                       StringRef fallback) {
    stream.flush();
    if (!stream.str().empty()) {
      result.diagnostics.push_back(
          {xmojo::DiagnosticSeverity::Error, stream.str()});
    } else if (!fallback.empty()) {
      result.diagnostics.push_back(
          {xmojo::DiagnosticSeverity::Error, fallback.str()});
    }
  }

  ErrorOrSuccess defineRuntimeSymbols(StringRef libraryName) {
    llvm::orc::ExecutionSession &session =
        executionEngine->getExecutionSession();
    llvm::orc::JITDylib *dylib = session.getJITDylibByName(libraryName);
    if (!dylib)
      return Error("could not find interactive session JITDylib");

    llvm::orc::SymbolMap symbols;
    auto addSymbol = [&](StringRef name, auto *function) {
      std::string mangledName;
      llvm::raw_string_ostream stream(mangledName);
      llvm::Mangler::getNameWithPrefix(stream, name,
                                       executionEngine->getDataLayout());
      symbols[session.intern(mangledName)] = llvm::orc::ExecutorSymbolDef(
          llvm::orc::ExecutorAddr::fromPtr(function),
          llvm::JITSymbolFlags::Exported);
    };
    addSymbol("xmojo_emit_print", &xmojo_emit_print);
    addSymbol("xmojo_display_begin", &xmojo_display_begin);
    addSymbol("xmojo_display_add", &xmojo_display_add);
    addSymbol("xmojo_display_end", &xmojo_display_end);
    addSymbol("xmojo_emit_error", &xmojo_emit_error);
    if (llvm::Error error =
            dylib->define(llvm::orc::absoluteSymbols(std::move(symbols))))
      return Error(llvm::toString(std::move(error)));
    return M::success();
  }

  ContextRef runtimeContext;
  SessionOptions options;
  CompilationOptions compilationOptions;
  EnvAttr compilationEnvironment;
  MLIRContext mlirContext;
  std::unique_ptr<InteractiveParser> interactiveParser;
  TargetInfoAttr targetInfo;
  std::string ownedObjectCache;
  std::unique_ptr<ObjectCompiler> objectCompiler;
  std::unique_ptr<ExecutionEngine> executionEngine;
  size_t nextCellID = 1;
  bool runtimeSymbolsDefined = false;
};

ErrorOr<std::unique_ptr<InteractiveSession>>
InteractiveSession::create(SessionOptions options) {
  auto implOr = Impl::create(std::move(options));
  if (implOr.isError())
    return implOr.takeError();
  return std::unique_ptr<InteractiveSession>(
      new InteractiveSession(implOr.takeValue()));
}

InteractiveSession::InteractiveSession(std::unique_ptr<Impl> impl)
    : impl(std::move(impl)) {}

InteractiveSession::~InteractiveSession() = default;

ErrorOr<ExecutionResult> InteractiveSession::execute(StringRef source) {
  return impl->execute(source);
}

CompletionResult InteractiveSession::complete(StringRef source,
                                              size_t cursorPosition) {
  return impl->complete(source, cursorPosition);
}

InspectionResult InteractiveSession::inspect(StringRef source,
                                             size_t cursorPosition) {
  return impl->inspect(source, cursorPosition);
}

CompletenessResult InteractiveSession::isComplete(StringRef source) {
  return impl->isComplete(source);
}
