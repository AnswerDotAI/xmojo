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
#include "KGEN/Support/Constants.h"
#include "KGEN/ToolCommon/InitAllDialects.h"
#include "KGEN/TransformUtils/SlicingUtils.h"
#include "Support/FileSystemExtras.h"
#include "Support/MDialect/MAttrs.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
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
thread_local std::optional<RuntimeError> pendingRuntimeError;

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

extern "C" void xmojo_emit_error(const char *message, size_t messageSize,
                                 const char *stackTrace,
                                 size_t stackTraceSize) {
  pendingRuntimeError.emplace(RuntimeError{
      std::string(message, messageSize),
      stackTraceSize == 0 ? std::string()
                          : std::string(stackTrace, stackTraceSize)});
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
        previousPendingDisplay(std::move(pendingDisplay)),
        previousRuntimeError(std::move(pendingRuntimeError)) {
    OutputCallback &output = options.output;
    activeOutput = output ? &output : nullptr;
    DisplayCallback &display = options.display;
    activeDisplay = display ? &display : nullptr;
    pendingDisplay.reset();
    pendingRuntimeError.reset();
  }

  ~RuntimeCallbackScope() {
    pendingRuntimeError = std::move(previousRuntimeError);
    pendingDisplay = std::move(previousPendingDisplay);
    activeDisplay = previousDisplay;
    activeOutput = previousOutput;
  }

private:
  OutputCallback *previousOutput;
  DisplayCallback *previousDisplay;
  std::optional<DisplayEvent> previousPendingDisplay;
  std::optional<RuntimeError> previousRuntimeError;
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
    if (!usable)
      return Error("interactive session is unusable after a native JIT "
                   "failure; restart it");

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
      usable = false;
      return error.takeError();
    }
    if (!runtimeSymbolsDefined) {
      if (ErrorOrSuccess error = defineRuntimeSymbols(libraryName)) {
        usable = false;
        return error.takeError();
      }
      runtimeSymbolsDefined = true;
    }

    ErrorOr<CompiledFunc> functionOr =
        executionEngine->lookup(libraryName, functionName);
    if (functionOr.isError()) {
      usable = false;
      return functionOr.takeError();
    }

    // Static declarations are already resident in ORC and cannot be removed
    // per cell through Modular's execution-engine API. Make the parser history
    // match that state before running executable statements, which may raise.
    interactiveParser->commit(*cell);

    llvm::CrashRecoveryContext crashRecovery;
    crashRecovery.Enable();
    RuntimeCallbackScope callbackScope(options);
    bool executed =
        crashRecovery.RunSafely([&] { functionOr->invoke<void>(); });
    if (!executed) {
      usable = false;
      return Error("Mojo cell execution crashed; restart the interactive "
                   "session");
    }

    if (pendingRuntimeError) {
      result.runtimeError = std::move(*pendingRuntimeError);
      return result;
    }

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

    auto objectCacheOr = TempDir::create("xmojo-session.%%%%%%");
    if (objectCacheOr.isError())
      return objectCacheOr.takeError();
    objectCache.emplace(std::move(*objectCacheOr));

    auto objectCompilerOr =
        ObjectCompiler::create(objectCache->getPath().string(),
                               compilationOptions, /*isJIT=*/true, mlirContext);
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
    addSymbol("xmojo_emit_error", &xmojo_emit_error);
    addSymbol("xmojo_display_begin", &xmojo_display_begin);
    addSymbol("xmojo_display_add", &xmojo_display_add);
    addSymbol("xmojo_display_end", &xmojo_display_end);
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
  // Declared before its users so it is removed after the engine and compiler.
  std::optional<TempDir> objectCache;
  std::unique_ptr<ObjectCompiler> objectCompiler;
  std::unique_ptr<ExecutionEngine> executionEngine;
  size_t nextCellID = 1;
  bool runtimeSymbolsDefined = false;
  bool usable = true;
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
