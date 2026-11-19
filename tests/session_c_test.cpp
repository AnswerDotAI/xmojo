//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "xmojo/SessionC.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

xmojo_string span(std::string_view text) { return {text.data(), text.size()}; }

std::string copy(xmojo_string text) {
  return std::string(text.data ? text.data : "", text.size);
}

testing::AssertionResult noError(xmojo_error *error) {
  std::unique_ptr<xmojo_error, decltype(&xmojo_error_free)> owned(
      error, xmojo_error_free);
  if (error)
    return testing::AssertionFailure() << copy(xmojo_error_message(error));
  return testing::AssertionSuccess();
}

TEST(InteractiveSessionCTest, PreservesNativeEventsAndBorrowedToolingResults) {
  struct Events {
    std::vector<std::string> ordered;
    std::string diagnostic;
    std::string runtimeError;
    std::string stackTrace;
  } events;
  xmojo_session_options options{};
  options.callbacks = {
      &events,
      [](void *context, xmojo_output_stream stream, xmojo_string text) {
        static_cast<Events *>(context)->ordered.push_back(
            (stream == XMOJO_STDOUT ? "stdout:" : "stderr:") + copy(text));
      },
      [](void *context, xmojo_display_kind kind, const xmojo_mime_data *data,
         size_t size) {
        ASSERT_EQ(size, 1u);
        EXPECT_EQ(copy(data[0].mime_type), "text/plain");
        static_cast<Events *>(context)->ordered.push_back(
            (kind == XMOJO_DISPLAY_DATA ? "display:" : "result:") +
            copy(data[0].data));
      },
      [](void *context, xmojo_diagnostic_severity severity,
         xmojo_string message) {
        EXPECT_EQ(severity, XMOJO_ERROR);
        static_cast<Events *>(context)->diagnostic += copy(message);
      },
      [](void *context, xmojo_string message, xmojo_string stackTrace) {
        auto &events = *static_cast<Events *>(context);
        events.runtimeError = copy(message);
        events.stackTrace = copy(stackTrace);
      }};
  xmojo_session *raw = nullptr;
  ASSERT_TRUE(noError(xmojo_session_create(&options, &raw)));
  auto destroy = [](xmojo_session *session) {
    EXPECT_TRUE(noError(xmojo_session_destroy(session)));
  };
  std::unique_ptr<xmojo_session, decltype(destroy)> session(raw, destroy);
  xmojo_execution_status status;
  ASSERT_TRUE(noError(xmojo_session_execute(session.get(), span(R"(
from xmojo import display
from std.sys import stderr
var owned = String("kept")
print("before")
display(42)
print("after", file=stderr)
43
)"), &status)));
  EXPECT_EQ(status, XMOJO_EXECUTION_SUCCEEDED);
  EXPECT_EQ(events.ordered, (std::vector<std::string>{
                                "stdout:before\n", "display:Int(42)",
                                "stderr:after\n", "result:Int(43)"}));

  struct Completion {
    std::string signature;
    std::string documentation;
  } completion;
  ASSERT_TRUE(noError(xmojo_session_complete(
      session.get(), span("# é\nown"), 8,
      [](void *context, const xmojo_completion_result *result) {
        EXPECT_EQ(result->cursor_start, 5u);
        EXPECT_EQ(result->cursor_end, 8u);
        for (size_t index = 0; index < result->size; ++index) {
          const auto &item = result->items[index];
          if (copy(item.label) != "owned")
            continue;
          EXPECT_EQ(item.kind, XMOJO_COMPLETION_VARIABLE);
          auto &completion = *static_cast<Completion *>(context);
          completion.signature = copy(item.signature);
          completion.documentation = copy(item.documentation);
        }
      },
      &completion)));
  EXPECT_EQ(completion.signature, "var owned: String");
  EXPECT_NE(completion.documentation.find(completion.signature),
            std::string::npos);
  std::string markdown;
  ASSERT_TRUE(noError(xmojo_session_inspect(
      session.get(), span("owned"), 3, 0,
      [](void *context, const xmojo_inspection_result *result) {
        EXPECT_TRUE(result->found);
        *static_cast<std::string *>(context) = copy(result->markdown);
      },
      &markdown)));
  EXPECT_NE(markdown.find("owned"), std::string::npos) << markdown;
  std::string indent;
  ASSERT_TRUE(noError(xmojo_session_is_complete(
      session.get(), span("def unfinished() -> Int:\n"),
      [](void *context, const xmojo_completeness_result *result) {
        EXPECT_EQ(result->status, XMOJO_INCOMPLETE);
        *static_cast<std::string *>(context) = copy(result->indent);
      },
      &indent)));
  EXPECT_FALSE(indent.empty());

  ASSERT_TRUE(noError(xmojo_session_execute(
      session.get(), span("unknown_name\n"), &status)));
  EXPECT_EQ(status, XMOJO_EXECUTION_COMPILE_ERROR);
  EXPECT_NE(events.diagnostic.find("unknown_name"), std::string::npos);
  ASSERT_TRUE(noError(xmojo_session_execute(
      session.get(), span("raise Error(\"kaboom\")\n"), &status)));
  EXPECT_EQ(status, XMOJO_EXECUTION_RUNTIME_ERROR);
  EXPECT_EQ(events.runtimeError, "kaboom");

  xmojo_error *wrongThread = nullptr;
  std::thread other([&] { wrongThread = xmojo_session_destroy(session.get()); });
  other.join();
  ASSERT_NE(wrongThread, nullptr);
  EXPECT_NE(copy(xmojo_error_message(wrongThread)).find("creating OS thread"),
            std::string::npos);
  xmojo_error_free(wrongThread);
  ASSERT_TRUE(noError(xmojo_session_execute(session.get(), span("owned\n"),
                                            &status)));
  EXPECT_EQ(status, XMOJO_EXECUTION_SUCCEEDED);
  EXPECT_EQ(events.ordered.back(), "result:'kept'");
}

} // namespace
