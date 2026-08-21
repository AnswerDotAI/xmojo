//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "Build/mojo-build.h"
#include "Precompile/mojo-precompile.h"

#include "CLI.h"
#include "SessionOptionsCLI.h"

#include "KGEN/Support/CLOptionUtils.h"
#include "KGEN/Support/ForceLinkMLIRC.h"
#include "KGEN/ToolCommon/OOMHandler.h"
#include "Support/Driver/DriverSupport.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/InitLLVM.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

void printUsage() {
  std::cout << "usage: xmojo [build|precompile|kernel] [options]\n"
               "       xmojo [-e CODE | FILE]\n";
}

int runCompilerCommand(const std::string &command, int argc, char **argv) {
  const char *stdlibFile = std::getenv("XMOJO_COMPILER_STDLIB_PATH");
  const char *compilerRTFile =
      std::getenv("MODULAR_MOJO_MAX_COMPILERRT_PATH");
  if (!stdlibFile) {
    std::cerr << "xmojo: XMOJO_COMPILER_STDLIB_PATH is not configured\n";
    return 1;
  }
  if (!compilerRTFile) {
    std::cerr << "xmojo: MODULAR_MOJO_MAX_COMPILERRT_PATH is not configured\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::path stdlibPath =
      std::filesystem::canonical(stdlibFile, error);
  if (error) {
    std::cerr << "xmojo: cannot locate compiler stdlib '" << stdlibFile
              << "': " << error.message() << '\n';
    return 1;
  }
  std::filesystem::path compilerRTPath =
      std::filesystem::canonical(compilerRTFile, error);
  if (error) {
    std::cerr << "xmojo: cannot locate CompilerRT '" << compilerRTFile
              << "': " << error.message() << '\n';
    return 1;
  }
  if (setenv("MODULAR_MOJO_MAX_IMPORT_PATH",
             stdlibPath.parent_path().c_str(), 1) != 0) {
    std::cerr << "xmojo: cannot configure compiler import path\n";
    return 1;
  }
  std::string sharedLibraries =
      "-Xlinker,-rpath,-Xlinker," + compilerRTPath.parent_path().string();
  if (setenv("MODULAR_MOJO_MAX_SHARED_LIBS", sharedLibraries.c_str(), 1) !=
      0) {
    std::cerr << "xmojo: cannot configure CompilerRT loader path\n";
    return 1;
  }

  M::KGEN::forceLinkMLIRC();
  M::registerCommandFlags();
  llvm::InitLLVM initLLVM(argc, argv);
  M::KGEN::installOOMHandler();

  M::SubcommandRegistry registry;
  if (command == "build")
    M::registerBuildSubcommand(registry);
  else
    M::registerPrecompileSubcommand(registry);

  auto callback = registry.getCallback(command);
  if (callback.isError()) {
    std::cerr << "xmojo: " << callback.getError() << '\n';
    return 1;
  }
  llvm::ArrayRef<const char *> arguments(argv + 2, argc - 2);
  return callback.get()(M::State(argv[0], command.c_str(), arguments));
}

} // namespace

int main(int argc, char **argv) {
  xmojo::SessionOptions options;
  if (auto error = xmojo::extractSessionOptions(argc, argv, options)) {
    std::cerr << "xmojo: " << *error << '\n';
    return 2;
  }

  if (argc == 1)
    return xmojo::runREPL(argc, argv, std::move(options));

  std::string command = argv[1];
  if (command == "--help") {
    printUsage();
    return 0;
  }
  if (command == "--version") {
    std::cout << "xmojo " XMOJO_VERSION "\n";
    return 0;
  }
  if (command == "kernel")
    return xmojo::runKernel(argc - 1, argv + 1, std::move(options));
  if (command == "build" || command == "precompile") {
    if (!options.targetAccelerator.empty()) {
      std::cerr << "xmojo: --modular-gpu is only supported by interactive "
                   "sessions\n";
      return 2;
    }
    return runCompilerCommand(command, argc, argv);
  }
  if (command == "-e" || command.front() != '-')
    return xmojo::runREPL(argc, argv, std::move(options));

  printUsage();
  return 2;
}
