//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#include "src/kernel/MojoInterpreter.h"

#include "gtest/gtest.h"

#include <algorithm>
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

  nl::json complete(const std::string &code, int cursor) {
    return interpreter->complete_request(code, cursor);
  }

  nl::json inspect(const std::string &code, int cursor) {
    return interpreter->inspect_request(code, cursor, 0);
  }

  nl::json isComplete(const std::string &code) {
    return interpreter->is_complete_request(code);
  }

  std::string stream(const std::string &name) const {
    std::string text;
    for (const PublishedMessage &message : published) {
      if (message.type == "stream" && message.content["name"] == name)
        text += message.content["text"].get<std::string>();
    }
    return text;
  }

  std::vector<const PublishedMessage *> messages(const std::string &type) {
    std::vector<const PublishedMessage *> result;
    for (const PublishedMessage &message : published)
      if (message.type == type)
        result.push_back(&message);
    return result;
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

  nl::json completion = story.complete("ans", 3);
  EXPECT_EQ(completion["status"], "ok");
  EXPECT_EQ(completion["cursor_start"], 0);
  EXPECT_NE(std::find(completion["matches"].begin(),
                      completion["matches"].end(), "answer"),
            completion["matches"].end());
  EXPECT_TRUE(completion["metadata"].contains(
      "_jupyter_types_experimental"));

  completion = story.complete("# é\nans", 7);
  EXPECT_EQ(completion["cursor_start"], 4);
  EXPECT_EQ(completion["cursor_end"], 7);
  EXPECT_NE(std::find(completion["matches"].begin(),
                      completion["matches"].end(), "answer"),
            completion["matches"].end());

  nl::json inspection = story.inspect("# é\nanswer()", 7);
  EXPECT_EQ(inspection["status"], "ok");
  EXPECT_TRUE(inspection["found"]);
  EXPECT_NE(inspection["data"]["text/markdown"]
                .get<std::string>()
                .find("def answer"),
            std::string::npos);

  EXPECT_EQ(story.isComplete("def unfinished() -> Int:\n")["status"],
            "incomplete");
  EXPECT_EQ(story.isComplete("def finished() -> Int:\n  return 1\n")["status"],
            "complete");
  EXPECT_EQ(story.isComplete("print((1 + 2]\n")["status"], "invalid");

  story.published.clear();
  EXPECT_EQ(story.execute("answer()\n")["status"], "ok");
  auto results = story.messages("execute_result");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]->content["execution_count"], 2);
  EXPECT_EQ(results[0]->content["data"]["text/plain"], "Int(42)");

  story.published.clear();
  EXPECT_EQ(story.execute(R"(
from xmojo import HTMLRepr, display

@fieldwise_init
struct HTML(HTMLRepr):
  var value: Int

  def _repr_html_(self) -> String:
    return String("<i>", self.value, "</i>")

display(HTML(7))
)" )["status"],
            "ok");
  auto displays = story.messages("display_data");
  ASSERT_EQ(displays.size(), 1u);
  EXPECT_EQ(displays[0]->content["data"]["text/html"], "<i>7</i>");

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
