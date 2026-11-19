//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "xmojo/SessionC.h"
#include "xmojo/InteractiveSession.h"

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct xmojo_session {
  std::unique_ptr<xmojo::InteractiveSession> native;
  xmojo_callbacks callbacks;
  std::thread::id thread;
};

struct xmojo_error {
  std::string message;
};

namespace {

xmojo_error *error(const char *message) { return new xmojo_error{message}; }

bool valid(xmojo_string text) { return text.data || text.size == 0; }

llvm::StringRef ref(xmojo_string text) {
  return llvm::StringRef(text.data ? text.data : "", text.size);
}

xmojo_string span(llvm::StringRef text) { return {text.data(), text.size()}; }

xmojo_error *checkSession(xmojo_session *session) {
  if (!session)
    return error("session is NULL");
  if (session->thread != std::this_thread::get_id())
    return error("session must be used and destroyed on its creating OS thread");
  return nullptr;
}

xmojo_completion_kind completionKind(xmojo::CompletionKind kind) {
  switch (kind) {
  case xmojo::CompletionKind::Unknown:
    return XMOJO_COMPLETION_UNKNOWN;
  case xmojo::CompletionKind::Package:
    return XMOJO_COMPLETION_PACKAGE;
  case xmojo::CompletionKind::Module:
    return XMOJO_COMPLETION_MODULE;
  case xmojo::CompletionKind::Struct:
    return XMOJO_COMPLETION_STRUCT;
  case xmojo::CompletionKind::Function:
    return XMOJO_COMPLETION_FUNCTION;
  case xmojo::CompletionKind::Field:
    return XMOJO_COMPLETION_FIELD;
  case xmojo::CompletionKind::Variable:
    return XMOJO_COMPLETION_VARIABLE;
  case xmojo::CompletionKind::Trait:
    return XMOJO_COMPLETION_TRAIT;
  }
  llvm_unreachable("unknown completion kind");
}

xmojo_diagnostic_severity severity(xmojo::DiagnosticSeverity value) {
  switch (value) {
  case xmojo::DiagnosticSeverity::Note:
    return XMOJO_NOTE;
  case xmojo::DiagnosticSeverity::Warning:
    return XMOJO_WARNING;
  case xmojo::DiagnosticSeverity::Error:
    return XMOJO_ERROR;
  }
  llvm_unreachable("unknown diagnostic severity");
}

} // namespace

xmojo_error *xmojo_session_create(const xmojo_session_options *options,
                                 xmojo_session **session) noexcept {
  if (!session)
    return error("session output is NULL");
  *session = nullptr;
  xmojo::SessionOptions nativeOptions;
  xmojo_callbacks callbacks{};
  if (options) {
    if ((!options->import_paths && options->import_path_count != 0) ||
        !valid(options->target_accelerator))
      return error("invalid session options span");
    for (size_t index = 0; index < options->import_path_count; ++index) {
      if (!valid(options->import_paths[index]))
        return error("invalid import path span");
      nativeOptions.importPaths.push_back(ref(options->import_paths[index]).str());
    }
    nativeOptions.targetAccelerator = ref(options->target_accelerator).str();
    callbacks = options->callbacks;
  }
  if (callbacks.output) {
    nativeOptions.output = [callbacks](xmojo::OutputStream stream,
                                        llvm::StringRef text) {
      callbacks.output(callbacks.context,
                       stream == xmojo::OutputStream::Stdout ? XMOJO_STDOUT
                                                            : XMOJO_STDERR,
                       span(text));
    };
  }
  if (callbacks.display) {
    nativeOptions.display = [callbacks](xmojo::DisplayEvent event) {
      std::vector<xmojo_mime_data> data;
      data.reserve(event.data.size());
      for (const auto &item : event.data)
        data.push_back({span(item.mimeType), span(item.data)});
      callbacks.display(callbacks.context,
                        event.kind == xmojo::DisplayKind::DisplayData
                            ? XMOJO_DISPLAY_DATA
                            : XMOJO_EXECUTE_RESULT,
                        data.data(), data.size());
    };
  }
  auto nativeOr = xmojo::InteractiveSession::create(std::move(nativeOptions));
  if (nativeOr.isError())
    return error(nativeOr.getError());
  *session = new xmojo_session{nativeOr.takeValue(), callbacks,
                                std::this_thread::get_id()};
  return nullptr;
}

xmojo_error *xmojo_session_destroy(xmojo_session *session) noexcept {
  if (!session)
    return nullptr;
  if (auto *failure = checkSession(session))
    return failure;
  delete session;
  return nullptr;
}

xmojo_error *xmojo_session_execute(xmojo_session *session, xmojo_string source,
                                  xmojo_execution_status *status) noexcept {
  if (auto *failure = checkSession(session))
    return failure;
  if (!valid(source) || !status)
    return error("invalid execution source span or NULL status output");
  auto resultOr = session->native->execute(ref(source));
  if (resultOr.isError())
    return error(resultOr.getError());
  const auto &callbacks = session->callbacks;
  if (callbacks.diagnostic) {
    for (const auto &diagnostic : resultOr->diagnostics)
      callbacks.diagnostic(callbacks.context, severity(diagnostic.severity),
                           span(diagnostic.message));
  }
  if (resultOr->runtimeError) {
    const auto &runtimeError = *resultOr->runtimeError;
    if (callbacks.runtime_error)
      callbacks.runtime_error(callbacks.context, span(runtimeError.message),
                              span(runtimeError.stackTrace));
    *status = XMOJO_EXECUTION_RUNTIME_ERROR;
  } else {
    *status = resultOr->succeeded ? XMOJO_EXECUTION_SUCCEEDED
                                  : XMOJO_EXECUTION_COMPILE_ERROR;
  }
  return nullptr;
}

xmojo_error *xmojo_session_complete(xmojo_session *session, xmojo_string source,
                                   size_t cursorPosition,
                                   xmojo_completion_callback callback,
                                   void *context) noexcept {
  if (auto *failure = checkSession(session))
    return failure;
  if (!valid(source) || cursorPosition > source.size || !callback)
    return error("invalid completion source span, byte offset, or callback");
  auto result = session->native->complete(ref(source), cursorPosition);
  std::vector<xmojo_completion_item> items;
  items.reserve(result.items.size());
  for (const auto &item : result.items)
    items.push_back({span(item.label), span(item.documentation),
                     completionKind(item.kind), span(item.signature)});
  xmojo_completion_result borrowed{items.data(), items.size(), result.cursorStart,
                                    result.cursorEnd};
  callback(context, &borrowed);
  return nullptr;
}

xmojo_error *xmojo_session_inspect(xmojo_session *session, xmojo_string source,
                                  size_t cursorPosition,
                                  uint32_t detailLevel,
                                  xmojo_inspection_callback callback,
                                  void *context) noexcept {
  if (auto *failure = checkSession(session))
    return failure;
  if (!valid(source) || cursorPosition > source.size || !callback)
    return error("invalid inspection source span, byte offset, or callback");
  auto result = session->native->inspect(ref(source), cursorPosition, detailLevel);
  std::vector<std::vector<xmojo_signature_parameter>> parameters;
  std::vector<xmojo_signature> signatures;
  for (const auto &signature : result.signatureHelp.signatures) {
    auto &items = parameters.emplace_back();
    for (const auto &parameter : signature.parameters)
      items.push_back({parameter.labelStart, parameter.labelEnd,
                        span(parameter.documentation)});
    signatures.push_back({span(signature.label), span(signature.documentation),
                           items.data(), items.size()});
  }
  xmojo_inspection_result borrowed{
      result.found, span(result.markdown), signatures.data(), signatures.size(),
      result.signatureHelp.activeSignature, result.signatureHelp.activeParameter};
  callback(context, &borrowed);
  return nullptr;
}

xmojo_error *xmojo_session_is_complete(xmojo_session *session, xmojo_string source,
                                      xmojo_completeness_callback callback,
                                      void *context) noexcept {
  if (auto *failure = checkSession(session))
    return failure;
  if (!valid(source) || !callback)
    return error("invalid completeness source span or callback");
  auto result = session->native->isComplete(ref(source));
  xmojo_completeness_status status;
  switch (result.status) {
  case xmojo::CompletenessStatus::Complete:
    status = XMOJO_COMPLETE;
    break;
  case xmojo::CompletenessStatus::Incomplete:
    status = XMOJO_INCOMPLETE;
    break;
  case xmojo::CompletenessStatus::Invalid:
    status = XMOJO_INVALID;
    break;
  }
  xmojo_completeness_result borrowed{status, span(result.indent)};
  callback(context, &borrowed);
  return nullptr;
}

xmojo_string xmojo_error_message(const xmojo_error *error) noexcept {
  return error ? span(error->message) : xmojo_string{nullptr, 0};
}

void xmojo_error_free(xmojo_error *error) noexcept { delete error; }
