//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_SESSIONOPTIONSCLI_H
#define XMOJO_SESSIONOPTIONSCLI_H

#include "xmojo/InteractiveSession.h"

#include <optional>
#include <string>

namespace xmojo {

inline std::optional<std::string>
extractSessionOptions(int &argc, char **argv, SessionOptions &options) {
  int writeIndex = 1;
  for (int readIndex = 1; readIndex < argc;) {
    std::string argument = argv[readIndex];
    std::string *destination = nullptr;
    if (argument == "--target-accelerator")
      destination = &options.targetAccelerator;
    else if (argument == "--gpu-runtime-library")
      destination = &options.gpuRuntimeLibrary;
    else if (argument == "--gpu-cache-directory")
      destination = &options.gpuCacheDirectory;
    else if (argument == "--mojo-compiler")
      destination = &options.officialMojoCompiler;
    else if (argument == "--modular-home")
      destination = &options.officialModularHome;

    if (!destination) {
      argv[writeIndex++] = argv[readIndex++];
      continue;
    }
    if (++readIndex == argc)
      return "missing value after " + argument;
    *destination = argv[readIndex++];
  }
  argc = writeIndex;
  return std::nullopt;
}

} // namespace xmojo

#endif // XMOJO_SESSIONOPTIONSCLI_H
