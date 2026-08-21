//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "MojoInterpreter.h"
#include "xmojo/InteractiveSession.h"

#include "xeus/xhelper.hpp"

#include <algorithm>
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

std::string completionKindName(CompletionKind kind) {
  switch (kind) {
  case CompletionKind::Package:
    return "package";
  case CompletionKind::Module:
    return "module";
  case CompletionKind::Struct:
    return "class";
  case CompletionKind::Function:
    return "function";
  case CompletionKind::Field:
    return "property";
  case CompletionKind::Variable:
    return "instance";
  case CompletionKind::Trait:
    return "class";
  case CompletionKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

size_t utf8ByteOffset(llvm::StringRef text, int characterOffset) {
  if (characterOffset <= 0)
    return 0;
  size_t byteOffset = 0;
  for (int character = 0;
       byteOffset < text.size() && character < characterOffset; ++character) {
    ++byteOffset;
    while (byteOffset < text.size() &&
           (static_cast<unsigned char>(text[byteOffset]) & 0xc0) == 0x80)
      ++byteOffset;
  }
  return byteOffset;
}

size_t utf8CharacterOffset(llvm::StringRef text, size_t byteOffset) {
  byteOffset = std::min(byteOffset, text.size());
  size_t characters = 0;
  for (size_t byte = 0; byte < byteOffset; ++byte)
    if ((static_cast<unsigned char>(text[byte]) & 0xc0) != 0x80)
      ++characters;
  return characters;
}

} // namespace

M::ErrorOr<std::unique_ptr<MojoInterpreter>>
MojoInterpreter::create(SessionOptions options) {
  auto interpreter = std::unique_ptr<MojoInterpreter>(new MojoInterpreter());

  options.output = [instance = interpreter.get()](OutputStream stream,
                                                  llvm::StringRef text) {
    instance->emitOutput(stream, text);
  };
  options.display = [instance = interpreter.get()](DisplayEvent event) {
    instance->emitDisplay(std::move(event));
  };
  auto sessionOr = InteractiveSession::create(std::move(options));
  if (sessionOr.isError())
    return sessionOr.takeError();
  interpreter->session = sessionOr.takeValue();
  return std::move(interpreter);
}

void MojoInterpreter::configure_impl() {}

void MojoInterpreter::execute_request_impl(send_reply_callback callback,
                                           int executionCounter,
                                           const std::string &code,
                                           xeus::execute_request_config config,
                                           nl::json) {
  outputEnabled = !config.silent;
  currentExecutionCounter = executionCounter;
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
  if (resultOr->runtimeError) {
    const RuntimeError &error = *resultOr->runtimeError;
    std::vector<std::string> traceback{"MojoError: " + error.message};
    if (!error.stackTrace.empty())
      traceback.push_back(error.stackTrace);
    if (!config.silent) {
      for (const std::string &diagnostic : messages)
        publish_stream("stderr", diagnostic);
      publish_execution_error("MojoError", error.message, traceback);
    }
    callback(xeus::create_error_reply("MojoError", error.message, traceback));
    return;
  }

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

nl::json MojoInterpreter::complete_request_impl(const std::string &code,
                                                int cursorPosition) {
  CompletionResult result =
      session->complete(code, utf8ByteOffset(code, cursorPosition));
  size_t cursorStart = utf8CharacterOffset(code, result.cursorStart);
  size_t cursorEnd = utf8CharacterOffset(code, result.cursorEnd);
  nl::json matches = nl::json::array();
  nl::json detailed = nl::json::array();
  for (const CompletionItem &item : result.items) {
    matches.push_back(item.label);
    detailed.push_back({{"start", cursorStart},
                        {"end", cursorEnd},
                        {"text", item.label},
                        {"type", completionKindName(item.kind)},
                        {"signature", item.signature}});
  }
  nl::json metadata = {{"_jupyter_types_experimental", std::move(detailed)}};
  return xeus::create_complete_reply(matches, cursorStart, cursorEnd, metadata);
}

nl::json MojoInterpreter::inspect_request_impl(const std::string &code,
                                               int cursorPosition, int) {
  InspectionResult result =
      session->inspect(code, utf8ByteOffset(code, cursorPosition));
  if (!result.found)
    return xeus::create_inspect_reply();
  return xeus::create_inspect_reply(
      true, {{"text/markdown", std::move(result.markdown)}});
}

nl::json MojoInterpreter::is_complete_request_impl(const std::string &code) {
  CompletenessResult result = session->isComplete(code);
  switch (result.status) {
  case CompletenessStatus::Complete:
    return xeus::create_is_complete_reply("complete");
  case CompletenessStatus::Incomplete:
    return xeus::create_is_complete_reply("incomplete", result.indent);
  case CompletenessStatus::Invalid:
    return xeus::create_is_complete_reply("invalid");
  }
  return xeus::create_is_complete_reply("unknown");
}

nl::json MojoInterpreter::kernel_info_request_impl() {
  return xeus::create_info_reply(
      "xmojo", XMOJO_VERSION, "mojo", "nightly", "text/x-mojo", ".mojo", "mojo",
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

void MojoInterpreter::emitDisplay(DisplayEvent event) {
  if (!outputEnabled)
    return;
  nl::json data = nl::json::object();
  for (MimeData &item : event.data)
    data[std::move(item.mimeType)] = std::move(item.data);
  if (event.kind == DisplayKind::ExecuteResult)
    publish_execution_result(currentExecutionCounter, std::move(data),
                             nl::json::object());
  else
    display_data(std::move(data), nl::json::object(), nl::json::object());
}
