//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "CLI.h"
#include "Kernel.h"

#include <iostream>
#include <string>
#include <vector>

int xmojo::runKernel(int argc, char **argv, SessionOptions options) {
  const char *connectionFile = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--version") {
      std::cout << "xmojo " XMOJO_VERSION "\n";
      return 0;
    }
    if (argument != "-f" || ++i == argc || connectionFile) {
      std::cerr << "usage: xmojo kernel -f CONNECTION_FILE\n";
      return 2;
    }
    connectionFile = argv[i];
  }

  if (!connectionFile) {
    std::cerr << "usage: xmojo kernel -f CONNECTION_FILE\n";
    return 2;
  }

  std::vector<xmojo_string> importPaths;
  for (const std::string &path : options.importPaths)
    importPaths.push_back({path.data(), path.size()});
  xmojo_session_options nativeOptions{};
  nativeOptions.import_paths = importPaths.data();
  nativeOptions.import_path_count = importPaths.size();
  nativeOptions.target_accelerator = {options.targetAccelerator.data(),
                                     options.targetAccelerator.size()};
  return xmojo_run_kernel(connectionFile, &nativeOptions, XMOJO_VERSION);
}
