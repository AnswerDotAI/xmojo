//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "src/kernel/MojoInterpreter.h"

#include "gtest/gtest.h"

#include <string>
#include <utility>
#include <vector>

namespace {

struct PublishedMessage {
  std::string type;
  nl::json content;
};

class InterpreterStory {
public:
  InterpreterStory() {
    auto interpreterOr = xmojo::MojoInterpreter::create();
    if (interpreterOr.isError()) {
      ADD_FAILURE() << interpreterOr.getError();
      return;
    }
    interpreter = interpreterOr.takeValue();
    interpreter->register_publisher(
        [this](xeus::xrequest_context, const std::string &type, nl::json,
               nl::json content, xeus::buffer_sequence) {
          published.push_back({type, std::move(content)});
        });
  }

  nl::json execute(const std::string &code, bool silent = false) {
    nl::json reply;
    interpreter->execute_request(
        {}, [&](nl::json value) { reply = std::move(value); }, code,
        {.silent = silent, .store_history = !silent, .allow_stdin = false},
        nl::json::object());
    return reply;
  }

  std::string stream(const std::string &name) const {
    std::string text;
    for (const PublishedMessage &message : published) {
      if (message.type == "stream" && message.content["name"] == name)
        text += message.content["text"].get<std::string>();
    }
    return text;
  }

  std::unique_ptr<xmojo::MojoInterpreter> interpreter;
  std::vector<PublishedMessage> published;
};

TEST(MojoInterpreterTest, RunsAJupyterExecutionStory) {
  InterpreterStory story;
  ASSERT_NE(story.interpreter, nullptr);

  EXPECT_EQ(story.execute(R"(
def answer() -> Int:
  return 42

print("ready")
)")["status"],
            "ok");
  EXPECT_EQ(story.stream("stdout"), "ready\n");

  story.published.clear();
  EXPECT_EQ(story.execute("print(answer())\n")["status"], "ok");
  EXPECT_EQ(story.stream("stdout"), "42\n");

  story.published.clear();
  EXPECT_EQ(story.execute(R"(
from std.sys import stderr
print("warning", file=stderr)
)")["status"],
            "ok");
  EXPECT_EQ(story.stream("stderr"), "warning\n");

  story.published.clear();
  nl::json failure = story.execute("print(unknown_name)\n");
  EXPECT_EQ(failure["status"], "error");
  EXPECT_EQ(failure["ename"], "MojoError");
  ASSERT_FALSE(story.published.empty());
  EXPECT_EQ(story.published.back().type, "error");
  EXPECT_NE(story.published.back().content["evalue"].get<std::string>().find(
                "unknown_name"),
            std::string::npos);

  story.published.clear();
  EXPECT_EQ(story.execute("print(99)\n", /*silent=*/true)["status"], "ok");
  EXPECT_TRUE(story.published.empty());
}

} // namespace
