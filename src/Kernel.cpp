//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "CLI.h"
#include "kernel/MojoInterpreter.h"

#include "xeus-zmq/xserver_zmq.hpp"
#include "xeus-zmq/xzmq_context.hpp"
#include "xeus/xhelper.hpp"
#include "xeus/xkernel.hpp"
#include "xeus/xkernel_configuration.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

int xmojo::runKernel(int argc, char **argv, SessionOptions options) {
  if (xeus::should_print_version(argc, argv)) {
    std::cout << "xmojo " XMOJO_VERSION "\n";
    return 0;
  }

  std::string connectionFile = xeus::extract_filename(argc, argv);
  if (connectionFile.empty()) {
    std::cerr << "usage: xmojo kernel -f CONNECTION_FILE\n";
    return 2;
  }

  auto interpreterOr = MojoInterpreter::create(std::move(options));
  if (interpreterOr.isError()) {
    std::cerr << "xmojo: " << interpreterOr.getError() << '\n';
    return 1;
  }

  xeus::xkernel kernel(xeus::load_configuration(connectionFile),
                       xeus::get_user_name(), xeus::make_zmq_context(),
                       interpreterOr.takeValue(), xeus::make_xserver_default);
  kernel.start();
  return 0;
}
