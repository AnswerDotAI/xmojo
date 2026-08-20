//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

using xmojo::DiagnosticSeverity;
using xmojo::DisplayEvent;
using xmojo::DisplayKind;
using xmojo::ExecutionResult;
using xmojo::InteractiveSession;
using xmojo::OutputStream;
using xmojo::SessionOptions;

namespace {

std::unique_ptr<InteractiveSession> createSession(SessionOptions options = {}) {
  auto sessionOr = InteractiveSession::create(std::move(options));
  if (!sessionOr.isError())
    return sessionOr.takeValue();
  ADD_FAILURE() << sessionOr.getError();
  return nullptr;
}

struct CapturedOutput {
  std::string standardOutput;
  std::string standardError;
  std::vector<DisplayEvent> displays;

  SessionOptions sessionOptions() {
    SessionOptions options;
    options.output = [this](OutputStream stream, llvm::StringRef text) {
      std::string &destination =
          stream == OutputStream::Stdout ? standardOutput : standardError;
      destination.append(text.data(), text.size());
    };
    options.display = [this](DisplayEvent event) {
      displays.push_back(std::move(event));
    };
    return options;
  }
};

std::string diagnosticsText(const ExecutionResult &result) {
  std::string text;
  for (const auto &diagnostic : result.diagnostics)
    text += diagnostic.message;
  return text;
}

testing::AssertionResult executionSucceeds(InteractiveSession &session,
                                           const std::string &source) {
  auto resultOr = session.execute(source);
  if (resultOr.isError())
    return testing::AssertionFailure()
           << "compiler infrastructure failed: " << resultOr.getError();
  if (!resultOr->succeeded)
    return testing::AssertionFailure() << "cell was rejected:\n"
                                       << diagnosticsText(*resultOr);
  return testing::AssertionSuccess();
}

testing::AssertionResult executionFailsWith(InteractiveSession &session,
                                            const std::string &source,
                                            const std::string &expected) {
  auto resultOr = session.execute(source);
  if (resultOr.isError())
    return testing::AssertionFailure()
           << "compiler infrastructure failed: " << resultOr.getError();
  if (resultOr->succeeded)
    return testing::AssertionFailure() << "invalid cell unexpectedly succeeded";

  std::string text = diagnosticsText(*resultOr);
  bool hasError = false;
  for (const auto &diagnostic : resultOr->diagnostics)
    hasError |= diagnostic.severity == DiagnosticSeverity::Error;
  if (!hasError)
    return testing::AssertionFailure() << "cell produced no error diagnostic:\n"
                                       << text;
  if (text.find(expected) == std::string::npos)
    return testing::AssertionFailure()
           << "expected diagnostic containing '" << expected << "', got:\n"
           << text;
  return testing::AssertionSuccess();
}

testing::AssertionResult runtimeFailsWith(InteractiveSession &session,
                                          const std::string &source,
                                          const std::string &expected) {
  auto resultOr = session.execute(source);
  if (resultOr.isError())
    return testing::AssertionFailure()
           << "compiler infrastructure failed: " << resultOr.getError();
  if (resultOr->succeeded)
    return testing::AssertionFailure() << "raising cell unexpectedly succeeded";
  if (!resultOr->runtimeError)
    return testing::AssertionFailure()
           << "raising cell produced no runtime error";
  if (resultOr->runtimeError->message.find(expected) == std::string::npos)
    return testing::AssertionFailure()
           << "expected runtime error containing '" << expected << "', got:\n"
           << resultOr->runtimeError->message;
  return testing::AssertionSuccess();
}

TEST(InteractiveSessionTest, RunsInteractiveSession) {
  CapturedOutput output;
  auto session = createSession(output.sessionOptions());
  ASSERT_NE(session, nullptr);

  ASSERT_TRUE(executionSucceeds(*session, R"(
from std.collections import List
alias Count = Int

@fieldwise_init
struct Pair:
  var left: Int
  var right: Int

def add(
  left: Int,
  right: Int,
) -> Int:
  return left + right

var total = 0
for value in range(4):
  total += value
print(total)
)"));

  ASSERT_TRUE(executionSucceeds(*session, R"(
def parameterized[value: Int]() -> Int:
  return value

def accepts_list(value: List[Count]):
  pass
)"));
  ASSERT_TRUE(executionSucceeds(*session, R"(
def answer() -> Int:
  """Return the answer used by this session story."""
  return parameterized[42]()
)"));

  auto completion = session->complete("ans", 3);
  EXPECT_EQ(completion.cursorStart, 0u);
  EXPECT_EQ(completion.cursorEnd, 3u);
  auto answerCompletion =
      std::find_if(completion.items.begin(), completion.items.end(),
                   [](const auto &item) { return item.label == "answer"; });
  ASSERT_NE(answerCompletion, completion.items.end());
  EXPECT_EQ(answerCompletion->kind, xmojo::CompletionKind::Function);

  auto memberCompletion = session->complete("Pair(1, 2).le", 13);
  EXPECT_EQ(memberCompletion.cursorStart, 11u);
  EXPECT_NE(std::find_if(memberCompletion.items.begin(),
                         memberCompletion.items.end(),
                         [](const auto &item) { return item.label == "left"; }),
            memberCompletion.items.end());

  auto inspection = session->inspect("answer()", 3);
  ASSERT_TRUE(inspection.found);
  EXPECT_NE(inspection.markdown.find("def answer"), std::string::npos);
  EXPECT_NE(inspection.markdown.find("Return the answer used"),
            std::string::npos);
  ASSERT_TRUE(executionSucceeds(*session, "print(answer())\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(parameterized[7]())\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(add(Pair(3, 4).left, 5))\n"));
  EXPECT_EQ(output.standardOutput, "6\n42\n7\n8\n");

  ASSERT_TRUE(executionSucceeds(*session, R"(
from std.sys import stderr
print("warning", file=stderr)
)"));
  EXPECT_EQ(output.standardError, "warning\n");

  std::string longText(4096, 'x');
  ASSERT_TRUE(executionSucceeds(*session, "print(\"" + longText + "\")\n"));
  EXPECT_EQ(output.standardOutput, "6\n42\n7\n8\n" + longText + "\n");

  ASSERT_TRUE(executionSucceeds(*session, R"(
from xmojo import HTMLRepr, MarkdownRepr, display

@fieldwise_init
struct RichValue(HTMLRepr, MarkdownRepr, Writable):
  var value: Int

  def _repr_html_(self) -> String:
    return String("<b>", self.value, "</b>")

  def _repr_markdown_(self) -> String:
    return String("**", self.value, "**")

display(RichValue(42))
)"));
  ASSERT_EQ(output.displays.size(), 1u);
  EXPECT_EQ(output.displays[0].kind, DisplayKind::DisplayData);
  ASSERT_EQ(output.displays[0].data.size(), 3u);
  EXPECT_EQ(output.displays[0].data[0].mimeType, "text/html");
  EXPECT_EQ(output.displays[0].data[0].data, "<b>42</b>");
  EXPECT_EQ(output.displays[0].data[1].mimeType, "text/markdown");
  EXPECT_EQ(output.displays[0].data[1].data, "**42**");
  EXPECT_EQ(output.displays[0].data[2].mimeType, "text/plain");
  EXPECT_EQ(output.displays[0].data[2].data, "RichValue(value=Int(42))");

  ASSERT_TRUE(executionSucceeds(*session, R"(
RichValue(
  7
)
)"));
  ASSERT_EQ(output.displays.size(), 2u);
  EXPECT_EQ(output.displays[1].kind, DisplayKind::ExecuteResult);
  ASSERT_EQ(output.displays[1].data.size(), 3u);
  EXPECT_EQ(output.displays[1].data[0].data, "<b>7</b>");

  ASSERT_TRUE(executionSucceeds(*session, "display(RichValue(8))\n"));
  ASSERT_EQ(output.displays.size(), 3u);
  EXPECT_EQ(output.displays[2].kind, DisplayKind::DisplayData);
  EXPECT_EQ(output.displays[2].data[0].data, "<b>8</b>");

  ASSERT_TRUE(executionSucceeds(*session, "42; 43\n"));
  ASSERT_EQ(output.displays.size(), 4u);
  EXPECT_EQ(output.displays[3].kind, DisplayKind::ExecuteResult);
  ASSERT_EQ(output.displays[3].data.size(), 1u);
  EXPECT_EQ(output.displays[3].data[0].mimeType, "text/plain");
  EXPECT_EQ(output.displays[3].data[0].data, "Int(43)");

  ASSERT_TRUE(executionSucceeds(*session, R"mojo(
from xmojo import MIMEBundle, MIMEBundleRepr, display

struct Bundled(MIMEBundleRepr):
  def __init__(out self):
    pass

  def _repr_mimebundle_(self) -> MIMEBundle:
    var bundle = MIMEBundle()
    bundle.add("application/x-xmojo-test", "structured")
    bundle.add("text/plain", "Bundled()")
    return bundle^

display(Bundled())
)mojo"));
  ASSERT_EQ(output.displays.size(), 5u);
  EXPECT_EQ(output.displays[4].kind, DisplayKind::DisplayData);
  ASSERT_EQ(output.displays[4].data.size(), 2u);
  EXPECT_EQ(output.displays[4].data[0].mimeType, "application/x-xmojo-test");
  EXPECT_EQ(output.displays[4].data[0].data, "structured");
  EXPECT_EQ(output.displays[4].data[1].mimeType, "text/plain");
  EXPECT_EQ(output.displays[4].data[1].data, "Bundled()");

  ASSERT_TRUE(executionSucceeds(*session, R"(
@fieldwise_init
struct MoveOnly(Movable, Writable):
  var value: Int

MoveOnly(9)
)"));
  ASSERT_EQ(output.displays.size(), 6u);
  EXPECT_EQ(output.displays[5].kind, DisplayKind::ExecuteResult);
  EXPECT_EQ(output.displays[5].data[0].data, "MoveOnly(value=Int(9))");

  ASSERT_TRUE(executionFailsWith(*session, R"(
def rejected() -> Int:
  return 7

var first = 1
var second = unknown_name
)",
                                 "var second = unknown_name"));
  ASSERT_TRUE(executionFailsWith(*session, "print(rejected())\n", "rejected"));

  ASSERT_TRUE(runtimeFailsWith(*session, R"(
def survives_raise() -> Int:
  return 123

print("before raise")
raise Error("kaboom")
)",
                               "kaboom"));
  ASSERT_TRUE(executionSucceeds(*session, "print(survives_raise())\n"));

  ASSERT_TRUE(executionSucceeds(*session, R"(
try:
  raise Error("caught")
except error:
  print(error)
)"));

  EXPECT_TRUE(executionSucceeds(*session, "print(answer())\n"));
  EXPECT_EQ(output.standardOutput,
            "6\n42\n7\n8\n" + longText + "\nbefore raise\n123\ncaught\n42\n");
  EXPECT_EQ(output.standardError, "warning\n");

  output.standardOutput.clear();
  ASSERT_TRUE(executionSucceeds(*session, R"(
struct Tracked(Movable):
  var label: String

  def __init__(out self, label: String):
    self.label = label

  def __deinit__(deinit self):
    print("destroyed", self.label)

var x = 1
var y = 1
var owned = String("kept")
var tracked = Tracked("value")
var rich = RichValue(12)
var multiline = String(
  "multi"
)
print(x, y, owned, multiline)
)"));

  auto variableCompletion = session->complete("own", 3);
  auto ownedCompletion = std::find_if(
      variableCompletion.items.begin(), variableCompletion.items.end(),
      [](const auto &item) { return item.label == "owned"; });
  ASSERT_NE(ownedCompletion, variableCompletion.items.end());
  EXPECT_EQ(ownedCompletion->kind, xmojo::CompletionKind::Variable);
  EXPECT_EQ(ownedCompletion->signature, "var owned: String");
  EXPECT_NE(ownedCompletion->documentation.find("var owned: String"),
            std::string::npos);

  auto persistentMemberCompletion = session->complete("owned.by", 8);
  EXPECT_NE(std::find_if(
                persistentMemberCompletion.items.begin(),
                persistentMemberCompletion.items.end(),
                [](const auto &item) { return item.label == "byte_length"; }),
            persistentMemberCompletion.items.end());

  auto variableInspection = session->inspect("owned", 3);
  ASSERT_TRUE(variableInspection.found);
  EXPECT_NE(variableInspection.markdown.find("var owned: String"),
            std::string::npos);

  auto persistentMemberInspection =
      session->inspect("owned.byte_length()", 10);
  ASSERT_TRUE(persistentMemberInspection.found);
  EXPECT_NE(persistentMemberInspection.markdown.find("byte_length"),
            std::string::npos);

  size_t displayCount = output.displays.size();
  ASSERT_TRUE(executionSucceeds(*session, "rich\n"));
  ASSERT_EQ(output.displays.size(), displayCount + 1);
  EXPECT_EQ(output.displays.back().kind, DisplayKind::ExecuteResult);
  ASSERT_EQ(output.displays.back().data.size(), 3u);
  EXPECT_EQ(output.displays.back().data[0].mimeType, "text/html");
  EXPECT_EQ(output.displays.back().data[0].data, "<b>12</b>");

  ASSERT_TRUE(executionSucceeds(*session, "owned += String(\"!\")\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(owned)\n"));

  ASSERT_TRUE(runtimeFailsWith(*session, R"(
x += 1
raise Error("mutation completed")
)",
                               "mutation completed"));
  ASSERT_TRUE(executionSucceeds(*session, "print(x, y)\n"));
  EXPECT_EQ(output.standardOutput, "1 1 kept multi\nkept!\n2 1\n");

  ASSERT_TRUE(
      executionSucceeds(*session, "var owner = String(\"borrowed\")\n"));
  EXPECT_TRUE(executionFailsWith(*session, "var view = StringSlice(owner)\n",
                                 "cannot persist borrowed value"));
  ASSERT_TRUE(
      executionSucceeds(*session, "var copied = String(StringSlice(owner))\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(copied)\n"));
  ASSERT_TRUE(executionSucceeds(
      *session, "var static_view: StaticString = \"static\"\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(static_view)\n"));

  EXPECT_TRUE(executionFailsWith(*session, R"(
var same_cell_owner = String("local")
var same_cell_view = StringSlice(same_cell_owner)
)",
                                 "cannot persist borrowed value"));

  ASSERT_TRUE(executionSucceeds(*session, R"(
if True:
  var nested = 7
  print(nested)
)"));
  EXPECT_TRUE(executionFailsWith(*session, "print(nested)\n", "nested"));

  EXPECT_TRUE(
      executionFailsWith(*session, "x = String(\"wrong\")\n", "String"));
  EXPECT_TRUE(executionFailsWith(*session, "var x = 3\n", "already declared"));
  EXPECT_TRUE(executionFailsWith(*session, R"(
def cannot_capture_x() -> Int:
  return x

print(cannot_capture_x())
)",
                                 "x"));

  ASSERT_TRUE(runtimeFailsWith(*session, R"(
var lost = 3
raise Error("new variable failed")
)",
                               "new variable failed"));
  EXPECT_TRUE(executionFailsWith(*session, "print(lost)\n", "lost"));

  session.reset();
  EXPECT_EQ(
      output.standardOutput,
      "1 1 kept multi\nkept!\n2 1\nborrowed\nstatic\n7\ndestroyed value\n");
}

TEST(InteractiveSessionTest, PreventsConsumingPersistentStorage) {
  auto session = createSession();
  ASSERT_NE(session, nullptr);

  ASSERT_TRUE(executionSucceeds(*session, R"(
struct MoveOnly(Movable):
  var value: Int

  def __init__(out self, value: Int):
    self.value = value

def consume_move_only(var value: MoveOnly):
  pass

var movable = MoveOnly(5)
)"));
  EXPECT_TRUE(executionFailsWith(*session, "consume_move_only(movable^)\n",
                                 "does not designate a value with an origin"));
}

TEST(InteractiveSessionTest, CompletesSeveralPersistentUserTypes) {
  auto session = createSession();
  ASSERT_NE(session, nullptr);

  ASSERT_TRUE(executionSucceeds(*session, R"(
struct First(Movable):
  var value: String

  def __init__(out self, value: String):
    self.value = value
)"));
  auto initialCompletion = session->complete("Fir", 3);
  EXPECT_NE(std::find_if(
                initialCompletion.items.begin(), initialCompletion.items.end(),
                [](const auto &item) { return item.label == "First"; }),
            initialCompletion.items.end());

  ASSERT_TRUE(executionSucceeds(*session, R"(
struct Second(Movable):
  var value: Int

  def __init__(out self, value: Int):
    self.value = value

def consume_second(var value: Second):
  pass

var first = First("one")
var second = Second(2)
var owned = String("value")
)"));

  auto completion = session->complete("own", 3);
  EXPECT_NE(
      std::find_if(completion.items.begin(), completion.items.end(),
                   [](const auto &item) { return item.label == "owned"; }),
      completion.items.end());

  auto memberCompletion = session->complete("owned.by", 8);
  auto byteLength = std::find_if(
      memberCompletion.items.begin(), memberCompletion.items.end(),
      [](const auto &item) { return item.label == "byte_length"; });
  ASSERT_NE(byteLength, memberCompletion.items.end());
  EXPECT_FALSE(byteLength->documentation.empty());
}

TEST(InteractiveSessionTest, RejectsPersistentBorrowedTemporary) {
  auto session = createSession();
  ASSERT_NE(session, nullptr);

  EXPECT_TRUE(executionFailsWith(
      *session, "var dangling = StringSlice(String(\"temporary\"))\n",
      "cannot persist borrowed value"));
}

TEST(InteractiveSessionTest, IsolatesSessions) {
  CapturedOutput firstOutput;
  CapturedOutput secondOutput;
  auto first = createSession(firstOutput.sessionOptions());
  auto second = createSession(secondOutput.sessionOptions());
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  const char *sharedCell = R"(
def shared_value() -> Int:
  return 1
)";
  ASSERT_TRUE(executionSucceeds(*first, sharedCell));
  ASSERT_TRUE(executionSucceeds(*second, sharedCell));
  ASSERT_TRUE(executionSucceeds(*first, "print(shared_value())\n"));
  ASSERT_TRUE(executionSucceeds(*second, "print(shared_value())\n"));
  EXPECT_EQ(firstOutput.standardOutput, "1\n");
  EXPECT_EQ(secondOutput.standardOutput, "1\n");

  ASSERT_TRUE(executionSucceeds(*first, R"(
def only_in_first() -> Int:
  return 2
)"));
  EXPECT_TRUE(
      executionFailsWith(*second, "print(only_in_first())\n", "only_in_first"));
}

TEST(InteractiveSessionTest, NativeCrashPoisonsSession) {
  auto session = createSession();
  ASSERT_NE(session, nullptr);

  auto crash = session->execute("from std.os import abort\nabort()\n");
  ASSERT_TRUE(crash.isError());
  EXPECT_NE(std::string(crash.getError()).find("restart"), std::string::npos);

  auto retry = session->execute("42\n");
  ASSERT_TRUE(retry.isError());
  EXPECT_NE(std::string(retry.getError()).find("unusable"), std::string::npos);
}

} // namespace
