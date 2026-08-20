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
#include "KGEN/KGENDialect/KGENUtils.h"
#include "KGEN/LITDialect/LITOps.h"
#include "KGEN/MojoParser/EntryPoint.h"
#include "KGEN/Support/Configuration.h"
#include "KGEN/Support/Constants.h"
#include "KGEN/ToolCommon/InitAllDialects.h"
#include "KGEN/TransformUtils/SlicingUtils.h"
#include "Support/MDialect/MAttrs.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include <mutex>

using namespace M;
using namespace M::KGEN;
using namespace mlir;
using namespace xmojo;

namespace {

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

    ErrorOr<CompiledFunc> functionOr =
        executionEngine->lookup(libraryName, functionName);
    if (functionOr.isError())
      return functionOr.takeError();

    llvm::CrashRecoveryContext crashRecovery;
    crashRecovery.Enable();
    bool executed =
        crashRecovery.RunSafely([&] { functionOr->invoke<void>(); });
    if (!executed) {
      result.diagnostics.push_back(
          {xmojo::DiagnosticSeverity::Error, "Mojo cell execution crashed"});
      return result;
    }

    interactiveParser->commit(*cell);
    result.succeeded = true;
    return result;
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

    auto objectCompilerOr = ObjectCompiler::create(
        kMojoCacheBaseDirName, compilationOptions, /*isJIT=*/true, mlirContext);
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

  ContextRef runtimeContext;
  SessionOptions options;
  CompilationOptions compilationOptions;
  MLIRContext mlirContext;
  std::unique_ptr<InteractiveParser> interactiveParser;
  TargetInfoAttr targetInfo;
  std::unique_ptr<ObjectCompiler> objectCompiler;
  std::unique_ptr<ExecutionEngine> executionEngine;
  size_t nextCellID = 1;
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
