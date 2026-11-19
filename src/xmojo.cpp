//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "Build/mojo-build.h"
#include "Precompile/mojo-precompile.h"

#include "CLI.h"
#include "SessionOptionsCLI.h"

#include "Mojo/Support/CLOptionUtils.h"
#include "Mojo/Support/ForceLinkMLIRC.h"
#include "Mojo/ToolCommon/OOMHandler.h"
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
  if (!stdlibFile) {
    std::cerr << "xmojo: XMOJO_COMPILER_STDLIB_PATH is not configured\n";
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
  std::string importPaths = stdlibPath.parent_path().string();
  if (const char *configured = std::getenv("MODULAR_MOJO_MAX_IMPORT_PATH"))
    importPaths += "," + std::string(configured);
  if (setenv("MODULAR_MOJO_MAX_IMPORT_PATH", importPaths.c_str(), 1) != 0) {
    std::cerr << "xmojo: cannot configure compiler import path\n";
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
  if (argc > 1) {
    std::string command = argv[1];
    if (command == "build" || command == "precompile")
      return runCompilerCommand(command, argc, argv);
  }
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
  if (command == "-e" || command.front() != '-')
    return xmojo::runREPL(argc, argv, std::move(options));

  printUsage();
  return 2;
}
