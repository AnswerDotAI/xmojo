//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_KERNEL_H
#define XMOJO_KERNEL_H

#include "xmojo/SessionC.h"

/// Runs the kernmini frontend until shutdown. Arguments are borrowed for the
/// duration of the call; the frontend installs its own session callbacks.
extern "C" int xmojo_run_kernel(const char *connection_file,
                                const xmojo_session_options *options,
                                const char *version);

#endif // XMOJO_KERNEL_H
