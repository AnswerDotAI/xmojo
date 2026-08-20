//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_MOJOINTERPRETER_H
#define XMOJO_MOJOINTERPRETER_H

#include "xeus/xinterpreter.hpp"
#include "xmojo/InteractiveSession.h"

#include <memory>

namespace xmojo {

class MojoInterpreter final : public xeus::xinterpreter {
public:
  static M::ErrorOr<std::unique_ptr<MojoInterpreter>> create();

private:
  MojoInterpreter() = default;

  void configure_impl() override;
  void execute_request_impl(send_reply_callback callback, int executionCounter,
                            const std::string &code,
                            xeus::execute_request_config config,
                            nl::json userExpressions) override;
  nl::json complete_request_impl(const std::string &code,
                                 int cursorPosition) override;
  nl::json inspect_request_impl(const std::string &code, int cursorPosition,
                                int detailLevel) override;
  nl::json is_complete_request_impl(const std::string &code) override;
  nl::json kernel_info_request_impl() override;
  nl::json shutdown_request_impl(bool restart) override;
  nl::json interrupt_request_impl() override;

  void emitOutput(OutputStream stream, llvm::StringRef text);
  void emitDisplay(DisplayEvent event);

  std::unique_ptr<InteractiveSession> session;
  bool outputEnabled = true;
  int currentExecutionCounter = 0;
};

} // namespace xmojo

#endif // XMOJO_MOJOINTERPRETER_H
