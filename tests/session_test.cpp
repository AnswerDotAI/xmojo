//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>

using xmojo::DiagnosticSeverity;
using xmojo::ExecutionResult;
using xmojo::InteractiveSession;

namespace {

std::unique_ptr<InteractiveSession> createSession() {
  auto sessionOr = InteractiveSession::create();
  if (!sessionOr.isError())
    return sessionOr.takeValue();
  ADD_FAILURE() << sessionOr.getError();
  return nullptr;
}

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

TEST(InteractiveSessionTest, RunsInteractiveSession) {
  auto session = createSession();
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
  return parameterized[42]()
)"));
  ASSERT_TRUE(executionSucceeds(*session, "print(answer())\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(parameterized[7]())\n"));
  ASSERT_TRUE(executionSucceeds(*session, "print(add(Pair(3, 4).left, 5))\n"));

  ASSERT_TRUE(executionFailsWith(*session, R"(
def rejected() -> Int:
  return 7

var first = 1
var second = unknown_name
)",
                                 "var second = unknown_name"));
  ASSERT_TRUE(executionFailsWith(*session, "print(rejected())\n", "rejected"));

  EXPECT_TRUE(executionSucceeds(*session, "print(answer())\n"));
}

TEST(InteractiveSessionTest, IsolatesSessions) {
  auto first = createSession();
  auto second = createSession();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  ASSERT_TRUE(executionSucceeds(*first, R"(
def only_in_first() -> Int:
  return 1
)"));
  EXPECT_TRUE(
      executionFailsWith(*second, "print(only_in_first())\n", "only_in_first"));
}

} // namespace
