//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"

#include <dlfcn.h>

#include <iostream>
#include <string>
#include <utility>

using xmojo::ExecutionResult;
using xmojo::InteractiveSession;
using xmojo::SessionOptions;

namespace {

std::string diagnosticsText(const ExecutionResult &result) {
  std::string text;
  for (const auto &diagnostic : result.diagnostics)
    text += diagnostic.message;
  return text;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gpu_shared_library_test LIBRARY\n";
    return 2;
  }

  void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    std::cerr << "could not load GPU launcher: " << dlerror() << '\n';
    return 1;
  }

  SessionOptions options;
  options.targetAccelerator = "apple-m5-metal4";
  auto sessionOr = InteractiveSession::create(std::move(options));
  if (sessionOr.isError()) {
    std::cerr << "could not create session: " << sessionOr.getError() << '\n';
    return 1;
  }
  auto session = sessionOr.takeValue();

  auto resultOr = session->execute(R"mojo(
from std.ffi import c_int, external_call
from max.gpu.host import DeviceContext

for iteration in range(4):
  with DeviceContext() as context:
    var buffer = context.enqueue_create_buffer[DType.float32](16)
    buffer.enqueue_fill(Float32(iteration))

    var status = external_call["xmojo_test_launch", c_int](
      Pointer(to=context),
      Pointer(to=buffer),
      c_int(16),
      c_int(1),
      c_int(16),
    )
    if status != 0:
      raise Error("GPU launcher failed")

    with buffer.map_to_host() as values:
      for i in range(16):
        if values[i] != Float32(iteration + 1):
          raise Error("wrong GPU result at ", i, ": ", values[i])
)mojo");

  if (resultOr.isError()) {
    std::cerr << "session infrastructure failed: " << resultOr.getError()
              << '\n';
    return 1;
  }
  if (!resultOr->succeeded) {
    if (resultOr->runtimeError)
      std::cerr << resultOr->runtimeError->message << '\n';
    std::cerr << diagnosticsText(*resultOr);
    return 1;
  }

  std::cout << "source-built xmojo launched an official-Mojo Metal kernel\n";
  session.reset();
  dlclose(library);
  return 0;
}
