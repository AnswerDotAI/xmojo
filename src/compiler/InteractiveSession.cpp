//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"
#include "InteractiveParser.h"
#include "OfficialGPUCompiler.h"

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
#include "llvm/ADT/SmallString.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include <cerrno>
#include <cstdint>
#include <dlfcn.h>
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
thread_local std::optional<GPUArtifact> pendingGPUArtifact;
thread_local std::string pendingGPUError;
using GPUCompileCallback =
    llvm::unique_function<ErrorOr<GPUArtifact>(llvm::StringRef)>;
thread_local GPUCompileCallback *activeGPUCompiler = nullptr;
using PersistentDestroy = void (*)(void *);
thread_local std::vector<PersistentDestroy> *activePersistentDestroyers =
    nullptr;

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

extern "C" void xmojo_register_persistent(size_t index,
                                          PersistentDestroy destroy) {
  if (activePersistentDestroyers && index < activePersistentDestroyers->size())
    (*activePersistentDestroyers)[index] = destroy;
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

extern "C" int32_t xmojo_compile_gpu(const char *name, size_t nameSize) {
  pendingGPUArtifact.reset();
  pendingGPUError.clear();
  if (!activeGPUCompiler) {
    pendingGPUError = "GPU compilation is unavailable outside cell execution";
    return 1;
  }
  ErrorOr<GPUArtifact> artifactOr =
      (*activeGPUCompiler)(StringRef(name, nameSize));
  if (artifactOr.isError()) {
    pendingGPUError = artifactOr.getError();
    return 1;
  }
  pendingGPUArtifact.emplace(artifactOr.takeValue());
  return 0;
}

extern "C" const char *xmojo_gpu_object_data(size_t *size) {
  if (!pendingGPUArtifact) {
    *size = 0;
    return nullptr;
  }
  *size = pendingGPUArtifact->object.size();
  return pendingGPUArtifact->object.data();
}

extern "C" const char *xmojo_gpu_function_name(size_t *size) {
  if (!pendingGPUArtifact) {
    *size = 0;
    return nullptr;
  }
  *size = pendingGPUArtifact->functionName.size();
  return pendingGPUArtifact->functionName.data();
}

extern "C" const char *xmojo_gpu_error(size_t *size) {
  *size = pendingGPUError.size();
  return pendingGPUError.data();
}

class RuntimeCallbackScope {
public:
  explicit RuntimeCallbackScope(
      SessionOptions &options,
      std::vector<PersistentDestroy> *persistentDestroyers = nullptr)
      : previousOutput(activeOutput), previousDisplay(activeDisplay),
        previousPendingDisplay(std::move(pendingDisplay)),
        previousRuntimeError(std::move(pendingRuntimeError)),
        previousGPUCompiler(activeGPUCompiler),
        previousGPUArtifact(std::move(pendingGPUArtifact)),
        previousGPUError(std::move(pendingGPUError)),
        previousPersistentDestroyers(activePersistentDestroyers) {
    OutputCallback &output = options.output;
    activeOutput = output ? &output : nullptr;
    DisplayCallback &display = options.display;
    activeDisplay = display ? &display : nullptr;
    activePersistentDestroyers = persistentDestroyers;
    pendingDisplay.reset();
    pendingRuntimeError.reset();
    pendingGPUArtifact.reset();
    pendingGPUError.clear();
  }

  void setGPUCompiler(GPUCompileCallback &compiler) {
    activeGPUCompiler = &compiler;
  }

  ~RuntimeCallbackScope() {
    activePersistentDestroyers = previousPersistentDestroyers;
    pendingRuntimeError = std::move(previousRuntimeError);
    pendingDisplay = std::move(previousPendingDisplay);
    pendingGPUError = std::move(previousGPUError);
    pendingGPUArtifact = std::move(previousGPUArtifact);
    activeGPUCompiler = previousGPUCompiler;
    activeDisplay = previousDisplay;
    activeOutput = previousOutput;
  }

private:
  OutputCallback *previousOutput;
  DisplayCallback *previousDisplay;
  std::optional<DisplayEvent> previousPendingDisplay;
  std::optional<RuntimeError> previousRuntimeError;
  GPUCompileCallback *previousGPUCompiler;
  std::optional<GPUArtifact> previousGPUArtifact;
  std::string previousGPUError;
  std::vector<PersistentDestroy> *previousPersistentDestroyers;
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

ErrorOrSuccess loadGPURuntime(StringRef requestedPath) {
  SmallString<256> canonicalPath;
  if (std::error_code error =
          llvm::sys::fs::real_path(requestedPath, canonicalPath))
    return Error((Twine("could not resolve the GPU runtime library '") +
                  requestedPath + "': " + error.message())
                     .str());

  static std::mutex mutex;
  static std::string loadedPath;
  static void *handle = nullptr;
  std::lock_guard lock(mutex);
  if (handle) {
    if (canonicalPath != loadedPath)
      return Error((Twine("this process already loaded the GPU runtime '") +
                    loadedPath + "' and cannot also load '" + canonicalPath +
                    "'")
                       .str());
    return M::success();
  }

  handle = dlopen(canonicalPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle)
    return Error(
        ("could not load the GPU runtime library: " + std::string(dlerror()))
            .c_str());
  loadedPath = canonicalPath.str().str();
  return M::success();
}

} // namespace

class InteractiveSession::Impl {
public:
  static ErrorOr<std::unique_ptr<Impl>> create(SessionOptions options) {
    initializeTargets();

    if (!options.targetAccelerator.empty()) {
      if (options.gpuRuntimeLibrary.empty())
        return Error("GPU sessions require SessionOptions.gpuRuntimeLibrary");
      if (ErrorOrSuccess error = loadGPURuntime(options.gpuRuntimeLibrary))
        return error.takeError();
    }

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

  ~Impl() { destroyVariables(); }

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
    gpuDeclarationSource += cell->declarationSource;

    std::vector<void *> slots;
    slots.reserve(sessionVars.size() + cell->newVars.size());
    for (const SessionVar &variable : sessionVars)
      slots.push_back(variable.storage);
    slots.resize(sessionVars.size() + cell->newVars.size(), nullptr);
    std::vector<PersistentDestroy> destroyers(slots.size(), nullptr);

    llvm::CrashRecoveryContext crashRecovery;
    crashRecovery.Enable();
    RuntimeCallbackScope callbackScope(options, &destroyers);
    GPUCompileCallback gpuCompile = [this](StringRef functionName) {
      return officialGPUCompiler->compile(gpuDeclarationSource, functionName);
    };
    callbackScope.setGPUCompiler(gpuCompile);
    bool executed = crashRecovery.RunSafely(
        [&] { functionOr->invoke<void, void *>(slots.data()); });
    if (!executed) {
      usable = false;
      return Error("Mojo cell execution crashed; restart the interactive "
                   "session");
    }

    if (pendingRuntimeError) {
      result.runtimeError = std::move(*pendingRuntimeError);
      return result;
    }

    size_t firstNewSlot = sessionVars.size();
    for (size_t index = 0; index < cell->newVars.size(); ++index) {
      size_t slotIndex = firstNewSlot + index;
      if (!slots[slotIndex] || !destroyers[slotIndex]) {
        usable = false;
        return Error(("persistent variable '" + cell->newVars[index].name +
                      "' did not initialize its " +
                      (!slots[slotIndex] ? "storage" : "destructor") +
                      "; restart the interactive session")
                         .c_str());
      }
      const auto &variable = cell->newVars[index];
      sessionVars.push_back({variable.name, variable.type, slots[slotIndex],
                             destroyers[slotIndex]});
    }
    interactiveParser->activateVariables(*cell);
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
  struct SessionVar {
    std::string name;
    mlir::Type type;
    void *storage;
    PersistentDestroy destroy;
  };

  Impl(ContextRef runtimeContext, SessionOptions options)
      : runtimeContext(std::move(runtimeContext)), options(std::move(options)),
        mlirContext(MLIRContext::Threading::DISABLED) {
    compilationOptions.targetAccelerator = this->options.targetAccelerator;
  }

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
    std::vector<std::string> officialImportPaths = importPaths;
    interactiveParser = std::make_unique<InteractiveParser>(
        mlirContext, compilationOptions, std::move(importPaths));
    officialGPUCompiler = std::make_unique<OfficialGPUCompiler>(
        options.officialMojoCompiler, options.officialModularHome,
        std::move(officialImportPaths), options.targetAccelerator,
        options.gpuCacheDirectory);

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

  void destroyVariables() {
    RuntimeCallbackScope callbackScope(options);
    for (SessionVar &variable : llvm::reverse(sessionVars)) {
      llvm::CrashRecoveryContext crashRecovery;
      crashRecovery.Enable();
      (void)crashRecovery.RunSafely(
          [&] { variable.destroy(variable.storage); });
    }
    sessionVars.clear();
  }

  ErrorOrSuccess defineRuntimeSymbols(StringRef libraryName) {
    llvm::orc::ExecutionSession &session =
        executionEngine->getExecutionSession();
    llvm::orc::JITDylib *dylib = session.getJITDylibByName(libraryName);
    if (!dylib)
      return Error("could not find interactive session JITDylib");

    auto processSymbols =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            executionEngine->getDataLayout().getGlobalPrefix());
    if (!processSymbols)
      return Error(llvm::toString(processSymbols.takeError()));
    dylib->addGenerator(std::move(*processSymbols));

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
    addSymbol("xmojo_register_persistent", &xmojo_register_persistent);
    addSymbol("xmojo_display_begin", &xmojo_display_begin);
    addSymbol("xmojo_display_add", &xmojo_display_add);
    addSymbol("xmojo_display_end", &xmojo_display_end);
    addSymbol("xmojo_compile_gpu", &xmojo_compile_gpu);
    addSymbol("xmojo_gpu_object_data", &xmojo_gpu_object_data);
    addSymbol("xmojo_gpu_function_name", &xmojo_gpu_function_name);
    addSymbol("xmojo_gpu_error", &xmojo_gpu_error);
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
  std::unique_ptr<OfficialGPUCompiler> officialGPUCompiler;
  std::string gpuDeclarationSource;
  TargetInfoAttr targetInfo;
  // Declared before its users so it is removed after the engine and compiler.
  std::optional<TempDir> objectCache;
  std::unique_ptr<ObjectCompiler> objectCompiler;
  std::unique_ptr<ExecutionEngine> executionEngine;
  std::vector<SessionVar> sessionVars;
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
