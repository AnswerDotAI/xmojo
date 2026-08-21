#include "KGEN/Compiler/Target/TargetBackend.h"
#include "KGEN/KGENDialect/KGENDType.h"
#include "KGEN/KGENDialect/KGENOps.h"
#include "KGEN/KGENDialect/KGENTypes.h"
#include "Target/TargetLowering.h"
#include "Target/TargetTraits.h"

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

using namespace M;
using namespace M::KGEN;
using namespace mlir;

namespace xmojo {
namespace {

constexpr llvm::StringLiteral kABIAttr = "xmojo.abi";
constexpr llvm::StringLiteral kABIErrorAttr = "xmojo.abi.error";
constexpr llvm::StringLiteral kBindingAttr = "llvm.xmojo.binding";

bool isSupportedType(llvm::StringRef type) {
  return type == "f32" || type == "si32" || type == "ui32";
}

class SPIRVTraits final : public TargetTraits {
public:
  llvm::StringRef name() const override { return "xmojo-spirv"; }
  bool matches(const llvm::Triple &triple) const override {
    return triple.isSPIRV();
  }
  bool isGPU() const override { return true; }
  llvm::StringRef getAsmExtension() const override { return ".spvasm"; }
  llvm::StringRef getLLVMExtension() const override { return ".spirv.ll"; }
  llvm::StringRef getObjectExtension() const override { return ".spv"; }

  static const SPIRVTraits &get() {
    static const SPIRVTraits instance;
    return instance;
  }

protected:
  bool isBaseTarget() const override { return true; }
};

class SPIRVLowering final : public TargetLowering {
public:
  const TargetTraits *traits() const override { return &SPIRVTraits::get(); }

protected:
  bool isBaseTarget() const override { return true; }
};

struct ABIArgument {
  enum class Kind { Buffer, Scalar } kind;
  std::string type;
  bool readOnly = false;
};

struct KernelABI {
  llvm::SmallVector<ABIArgument> arguments;

  std::string encode() const {
    std::string result = "v1";
    for (const ABIArgument &argument : arguments) {
      result += ";";
      if (argument.kind == ABIArgument::Kind::Buffer)
        result += argument.readOnly ? "b:ro:" : "b:rw:";
      else
        result += "s:";
      result += argument.type;
    }
    return result;
  }

  static ErrorOr<KernelABI> decode(llvm::StringRef encoded) {
    KernelABI abi;
    llvm::SmallVector<llvm::StringRef> fields;
    encoded.split(fields, ';');
    if (fields.empty() || fields.front() != "v1")
      return Error("unsupported xmojo kernel ABI");
    for (llvm::StringRef field : llvm::ArrayRef(fields).drop_front()) {
      llvm::SmallVector<llvm::StringRef> parts;
      field.split(parts, ':');
      if (parts.size() == 2 && parts[0] == "s" &&
          isSupportedType(parts[1])) {
        abi.arguments.push_back({ABIArgument::Kind::Scalar, parts[1].str()});
        continue;
      }
      if (parts.size() == 3 && parts[0] == "b" &&
          (parts[1] == "ro" || parts[1] == "rw") &&
          isSupportedType(parts[2])) {
        abi.arguments.push_back({ABIArgument::Kind::Buffer, parts[2].str(),
                                 parts[1] == "ro"});
        continue;
      }
      return Error("malformed xmojo kernel ABI");
    }
    return abi;
  }
};

ErrorOr<std::string> scalarName(Type type) {
  auto scalar = dyn_cast<SIMDType>(type);
  if (!scalar || scalar.getResolvedSize() != 1 ||
      !scalar.getResolvedDType())
    return Error("kernel scalar arguments must be resolved scalar values");
  KGENDType dtype = *scalar.getResolvedDType();
  if (dtype == KGENDType::f32)
    return std::string("f32");
  if (dtype == KGENDType::si32)
    return std::string("si32");
  if (dtype == KGENDType::ui32)
    return std::string("ui32");
  return Error("kernel scalar arguments currently support only Float32, "
               "Int32, and UInt32");
}

ErrorOr<KernelABI> analyzeKernel(FuncOp func) {
  if (func.getFunctionType().getNumResults() != 0)
    return Error("xmojo kernels cannot return values");

  KernelABI abi;
  ArrayAttr metadata = func.getLLVMArgMetadata();
  for (auto [index, type] :
       llvm::enumerate(func.getFunctionType().getInputs())) {
    DictionaryAttr argumentMetadata = metadata.empty()
                                          ? DictionaryAttr()
                                          : cast<DictionaryAttr>(metadata[index]);
    Attribute binding =
        argumentMetadata ? argumentMetadata.get(kBindingAttr) : Attribute();

    if (isa<PointerType>(type)) {
      auto value = dyn_cast_or_null<StringAttr>(binding);
      if (!value)
        return Error("kernel pointer arguments require @__llvm_arg_metadata "
                     "with llvm.xmojo.binding");
      llvm::StringRef descriptor = value.getValue();
      bool readOnly;
      if (descriptor.consume_front("read_only:"))
        readOnly = true;
      else if (descriptor.consume_front("read_write:"))
        readOnly = false;
      else
        return Error("llvm.xmojo.binding must start with read_only: or "
                     "read_write:");
      if (!isSupportedType(descriptor))
        return Error(
            "kernel buffers currently support only f32, si32, and ui32");
      abi.arguments.push_back(
          {ABIArgument::Kind::Buffer, descriptor.str(), readOnly});
      continue;
    }

    if (binding)
      return Error("llvm.xmojo.binding is valid only on pointer arguments");
    ErrorOr<std::string> name = scalarName(type);
    if (name.isError())
      return name.takeError();
    abi.arguments.push_back({ABIArgument::Kind::Scalar, name.takeValue()});
  }
  return abi;
}

void setMetadata(FuncOp func, llvm::StringRef name, llvm::StringRef value) {
  NamedAttrList metadata(func.getLLVMMetadataAttr());
  metadata.set(name, StringAttr::get(func.getContext(), value));
  func.setLLVMMetadataAttr(DictionaryAttr::get(func.getContext(), metadata));
}

llvm::Type *llvmType(llvm::LLVMContext &context, llvm::StringRef name) {
  if (name == "f32")
    return llvm::Type::getFloatTy(context);
  return llvm::Type::getInt32Ty(context);
}

ErrorOrSuccess addKernelWrapper(llvm::Function &body) {
  ErrorOr<KernelABI> abiOr =
      KernelABI::decode(body.getFnAttribute(kABIAttr).getValueAsString());
  if (abiOr.isError())
    return abiOr.takeError();
  KernelABI abi = abiOr.takeValue();
  if (body.arg_size() != abi.arguments.size())
    return Error(
        "xmojo kernel ABI does not match the lowered function signature");

  llvm::Module &module = *body.getParent();
  llvm::LLVMContext &context = module.getContext();
  std::string entryName = body.getName().str();
  body.setName(entryName + ".xmojo_body");
  body.setLinkage(llvm::GlobalValue::InternalLinkage);
  body.removeFnAttr(kABIAttr);

  auto *wrapperType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto *wrapper = llvm::Function::Create(
      wrapperType, llvm::GlobalValue::ExternalLinkage, entryName, module);
  wrapper->addFnAttr("hlsl.shader", "compute");
  wrapper->addFnAttr("hlsl.numthreads", "1,1,1");

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", wrapper);
  llvm::IRBuilder<> builder(entry);
  llvm::SmallVector<llvm::Value *> callArguments;
  llvm::SmallVector<llvm::Type *> scalarTypes;
  unsigned binding = 0;

  for (const ABIArgument &argument : abi.arguments)
    if (argument.kind == ABIArgument::Kind::Scalar)
      scalarTypes.push_back(llvmType(context, argument.type));

  llvm::GlobalVariable *pushConstants = nullptr;
  llvm::StructType *pushConstantType = nullptr;
  if (!scalarTypes.empty()) {
    pushConstantType = llvm::StructType::create(
        context, scalarTypes, "xmojo.push_constants");
    pushConstants = new llvm::GlobalVariable(
        module, pushConstantType, false, llvm::GlobalValue::ExternalLinkage,
        nullptr, "xmojo.push_constants", nullptr,
        llvm::GlobalValue::NotThreadLocal, 13, true);
    pushConstants->setVisibility(llvm::GlobalValue::HiddenVisibility);
  }

  unsigned scalar = 0;
  for (const ABIArgument &argument : abi.arguments) {
    if (argument.kind == ABIArgument::Kind::Scalar) {
      llvm::Value *pointer =
          builder.CreateStructGEP(pushConstantType, pushConstants, scalar);
      callArguments.push_back(builder.CreateLoad(scalarTypes[scalar], pointer));
      ++scalar;
      continue;
    }

    llvm::Type *elementType = llvmType(context, argument.type);
    auto *runtimeArray = llvm::ArrayType::get(elementType, 0);
    llvm::Type *resourceType = llvm::TargetExtType::get(
        context, "spirv.VulkanBuffer", {runtimeArray},
        {12, argument.readOnly ? 0U : 1U});
    llvm::Function *handleFromBinding =
        llvm::Intrinsic::getOrInsertDeclaration(
            &module, llvm::Intrinsic::spv_resource_handlefrombinding,
            {resourceType});
    llvm::Value *name = builder.CreateGlobalString(
        argument.readOnly ? "buffer" : "rw_buffer");
    llvm::Value *handle = builder.CreateCall(
        handleFromBinding,
        {builder.getInt32(0), builder.getInt32(binding++), builder.getInt32(1),
         builder.getInt32(0), name});
    llvm::Type *storagePointer = llvm::PointerType::get(context, 11);
    llvm::Function *getPointer = llvm::Intrinsic::getOrInsertDeclaration(
        &module, llvm::Intrinsic::spv_resource_getpointer,
        {storagePointer, resourceType, builder.getInt32Ty()});
    llvm::Value *pointer =
        builder.CreateCall(getPointer, {handle, builder.getInt32(0)});
    callArguments.push_back(builder.CreateAddrSpaceCast(
        pointer, body.getFunctionType()->getParamType(callArguments.size())));
  }

  builder.CreateCall(&body, callArguments);
  builder.CreateRetVoid();
  return {};
}

class SPIRVBackend final : public TargetBackend {
public:
  const TargetTraits *traits() const override { return &SPIRVTraits::get(); }
  SplitStrategy splitStrategy(const CompilationOptions &) const override {
    return SplitStrategy::PerExported;
  }
  bool isOffload() const override { return true; }

  void prepareModuleForLowering(Operation *operation,
                                const CompilationOptions &) const override {
    operation->walk([](FuncOp func) {
      if (!func.isExported())
        return;
      ErrorOr<KernelABI> abi = analyzeKernel(func);
      if (abi.isError()) {
        Error error = abi.takeError();
        std::string name = ("llvm." + kABIErrorAttr).str();
        setMetadata(func, name, error.get());
        return;
      }
      std::string name = ("llvm." + kABIAttr).str();
      setMetadata(func, name, abi->encode());
    });
  }

  void finalizeModuleForTarget(llvm::Module &module, llvm::TargetMachine &,
                               llvm::StringRef) const override {
    llvm::SmallVector<llvm::Function *> kernels;
    for (llvm::Function &function : module)
      if (!function.isDeclaration() && function.hasFnAttribute(kABIAttr))
        kernels.push_back(&function);
    for (llvm::Function *kernel : kernels)
      if (ErrorOrSuccess error = addKernelWrapper(*kernel))
        kernel->addFnAttr(kABIErrorAttr, error.getError());
  }

  ErrorOr<BufferRef> emitAssembly(llvm::Module &module,
                                  EmitContext &ctx) const override {
    return emit(module, ctx, false);
  }
  ErrorOr<BufferRef> emitObject(llvm::Module &module,
                                EmitContext &ctx) const override {
    return emit(module, ctx, true);
  }
  ErrorOr<BufferRef> createArchive(llvm::MutableArrayRef<BufferRef> objects,
                                   llvm::StringRef,
                                   EmitContext &) const override {
    if (objects.size() != 1)
      return Error("xmojo SPIR-V compilation expects one kernel per artifact");
    return objects.front();
  }

protected:
  bool isBaseTarget() const override { return true; }

private:
  ErrorOr<BufferRef> emit(llvm::Module &module, EmitContext &ctx,
                          bool object) const {
    for (llvm::Function &function : module)
      if (function.hasFnAttribute(kABIErrorAttr))
        return Error(
            function.getFnAttribute(kABIErrorAttr).getValueAsString());
    WriteableBufferRef buffer = WriteableBuffer::get();
    if (ErrorOrSuccess error = ctx.runLlc(module, *buffer, object))
      return error.takeError();
    return BufferRef::take(buffer.release());
  }
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wglobal-constructors"
RegisterTargetTraits<SPIRVTraits> registerTraits;
RegisterTargetLowering<SPIRVLowering> registerLowering;
RegisterTargetBackend<SPIRVBackend> registerBackend;
#pragma GCC diagnostic pop

} // namespace
} // namespace xmojo
