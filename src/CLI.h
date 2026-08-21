//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_CLI_H
#define XMOJO_CLI_H

#include "xmojo/InteractiveSession.h"

namespace xmojo {

int runKernel(int argc, char **argv, SessionOptions options);
int runREPL(int argc, char **argv, SessionOptions options);

} // namespace xmojo

#endif // XMOJO_CLI_H
