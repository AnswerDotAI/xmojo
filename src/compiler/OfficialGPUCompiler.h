//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#ifndef XMOJO_OFFICIALGPUCOMPILER_H
#define XMOJO_OFFICIALGPUCOMPILER_H

#include "Support/ErrorOr.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace xmojo {

struct GPUArtifact {
  std::string functionName;
  std::string object;
};

/// Compiles one resolved, noncapturing Mojo function with Modular's official
/// compiler and copies the resulting accelerator object out of a temporary
/// metadata library.
class OfficialGPUCompiler {
public:
  OfficialGPUCompiler(std::string compilerPath, std::string modularHome,
                      std::vector<std::string> importPaths,
                      std::string targetAccelerator,
                      std::string cacheDirectory);

  M::ErrorOr<GPUArtifact> compile(llvm::StringRef declarationSource,
                                  llvm::StringRef functionName) const;

private:
  M::ErrorOr<std::string>
  getToolchainFingerprint(llvm::StringRef compilerPath) const;

  std::string compilerPath;
  std::string modularHome;
  std::vector<std::string> importPaths;
  std::string targetAccelerator;
  std::string cacheDirectory;
};

} // namespace xmojo

#endif // XMOJO_OFFICIALGPUCOMPILER_H
