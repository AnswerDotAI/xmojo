//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "MojoInterpreter.h"
#include "xmojo/InteractiveSession.h"

#include "xeus/xhelper.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace xmojo;

namespace {

std::vector<std::string> diagnosticMessages(const ExecutionResult &result) {
  std::vector<std::string> messages;
  messages.reserve(result.diagnostics.size());
  for (const Diagnostic &diagnostic : result.diagnostics)
    messages.push_back(diagnostic.message);
  return messages;
}

std::string joinMessages(const std::vector<std::string> &messages) {
  std::string text;
  for (const std::string &message : messages)
    text += message;
  return text;
}

} // namespace

M::ErrorOr<std::unique_ptr<MojoInterpreter>> MojoInterpreter::create() {
  auto interpreter = std::unique_ptr<MojoInterpreter>(new MojoInterpreter());

  SessionOptions options;
  options.output = [instance = interpreter.get()](OutputStream stream,
                                                  llvm::StringRef text) {
    instance->emitOutput(stream, text);
  };
  auto sessionOr = InteractiveSession::create(std::move(options));
  if (sessionOr.isError())
    return sessionOr.takeError();
  interpreter->session = sessionOr.takeValue();
  return std::move(interpreter);
}

void MojoInterpreter::configure_impl() {}

void MojoInterpreter::execute_request_impl(send_reply_callback callback, int,
                                           const std::string &code,
                                           xeus::execute_request_config config,
                                           nl::json) {
  outputEnabled = !config.silent;
  auto resultOr = session->execute(code);
  outputEnabled = true;

  if (resultOr.isError()) {
    std::string message = resultOr.getError();
    std::vector<std::string> traceback{message};
    if (!config.silent)
      publish_execution_error("MojoInfrastructureError", message, traceback);
    callback(xeus::create_error_reply("MojoInfrastructureError", message,
                                      traceback));
    return;
  }

  std::vector<std::string> messages = diagnosticMessages(*resultOr);
  if (!resultOr->succeeded) {
    std::string message = joinMessages(messages);
    if (!config.silent)
      publish_execution_error("MojoError", message, messages);
    callback(xeus::create_error_reply("MojoError", message, messages));
    return;
  }

  if (!config.silent) {
    for (const std::string &message : messages)
      publish_stream("stderr", message);
  }
  callback(xeus::create_successful_reply());
}

nl::json MojoInterpreter::complete_request_impl(const std::string &,
                                                int cursorPosition) {
  return xeus::create_complete_reply({}, cursorPosition, cursorPosition);
}

nl::json MojoInterpreter::inspect_request_impl(const std::string &, int, int) {
  return xeus::create_inspect_reply();
}

nl::json MojoInterpreter::is_complete_request_impl(const std::string &) {
  return xeus::create_is_complete_reply("complete");
}

nl::json MojoInterpreter::kernel_info_request_impl() {
  return xeus::create_info_reply(
      "xmojo", "0.1.0", "mojo", "nightly", "text/x-mojo", ".mojo", "mojo",
      std::string("text/x-mojo"), {}, "xmojo ORC Mojo kernel");
}

nl::json MojoInterpreter::shutdown_request_impl(bool restart) {
  return xeus::create_shutdown_reply(restart);
}

nl::json MojoInterpreter::interrupt_request_impl() {
  return xeus::create_interrupt_reply();
}

void MojoInterpreter::emitOutput(OutputStream stream, llvm::StringRef text) {
  if (outputEnabled)
    publish_stream(stream == OutputStream::Stdout ? "stdout" : "stderr",
                   text.str());
}
