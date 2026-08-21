#include "xmojo/InteractiveSession.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>

using xmojo::InteractiveSession;
using xmojo::OutputStream;
using xmojo::SessionOptions;

TEST(SPIRVTargetTest, CompilesTypedKernelABI) {
  std::string output;
  SessionOptions options;
  options.output = [&](OutputStream stream, llvm::StringRef text) {
    if (stream == OutputStream::Stdout)
      output.append(text.data(), text.size());
  };
  auto sessionOr = InteractiveSession::create(std::move(options));
  ASSERT_FALSE(sessionOr.isError()) << sessionOr.getError();
  std::unique_ptr<InteractiveSession> session = sessionOr.takeValue();

  auto declarationOr = session->execute(R"mojo(
@__llvm_arg_metadata(
  source, `llvm.xmojo.binding`=__mlir_attr.`"read_only:f32"`
)
@__llvm_arg_metadata(
  destination, `llvm.xmojo.binding`=__mlir_attr.`"read_write:f32"`
)
def typed_kernel(
  source: Pointer[Float32, ImmutAnyOrigin],
  destination: Pointer[Float32, MutAnyOrigin],
  count: Int32,
  scale: Float32,
):
  destination[] = source[] * scale + Float32(count)

def missing_binding(pointer: Pointer[Float32, MutAnyOrigin]):
  pointer[] = 0
)mojo");
  ASSERT_FALSE(declarationOr.isError()) << declarationOr.getError();
  std::string diagnostics;
  for (const auto &diagnostic : declarationOr->diagnostics)
    diagnostics += diagnostic.message + "\n";
  ASSERT_TRUE(declarationOr->succeeded) << diagnostics;

  auto resultOr = session->execute(R"mojo(
from std.compile import compile_info
from std.testing import assert_equal, assert_true
from xmojo.spirv import _target

var object = compile_info[
  typed_kernel, target=_target, emission_kind="object"
]().asm
assert_true(object.byte_length() > 20)
var bytes = object.as_bytes()
assert_equal(bytes[0], 0x03)
assert_equal(bytes[1], 0x02)
assert_equal(bytes[2], 0x23)
assert_equal(bytes[3], 0x07)

print(compile_info[typed_kernel, target=_target]().asm)
)mojo");
  ASSERT_FALSE(resultOr.isError()) << resultOr.getError();
  diagnostics.clear();
  for (const auto &diagnostic : resultOr->diagnostics)
    diagnostics += diagnostic.message + "\n";
  ASSERT_TRUE(resultOr->succeeded) << diagnostics;
  EXPECT_NE(output.find("OpEntryPoint GLCompute"), std::string::npos);
  EXPECT_NE(output.find("DescriptorSet 0"), std::string::npos);
  EXPECT_NE(output.find("Binding 0"), std::string::npos);
  EXPECT_NE(output.find("Binding 1"), std::string::npos);
  EXPECT_NE(output.find("NonWritable"), std::string::npos);
  EXPECT_NE(output.find("PushConstant"), std::string::npos);

  auto rejectedOr = session->execute(R"mojo(
from std.compile import compile_info
from xmojo.spirv import _target

_ = compile_info[missing_binding, target=_target]()
)mojo");
  ASSERT_FALSE(rejectedOr.isError()) << rejectedOr.getError();
  diagnostics.clear();
  for (const auto &diagnostic : rejectedOr->diagnostics)
    diagnostics += diagnostic.message + "\n";
  EXPECT_FALSE(rejectedOr->succeeded);
  EXPECT_NE(diagnostics.find("kernel pointer arguments require "
                             "@__llvm_arg_metadata"),
            std::string::npos)
      << diagnostics;
}
