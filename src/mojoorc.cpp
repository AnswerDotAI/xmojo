//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

using xmojo::Diagnostic;
using xmojo::ExecutionResult;
using xmojo::InteractiveSession;

namespace {

enum class ExecutionStatus { Success, UserError, InfrastructureError };

void printDiagnostics(const ExecutionResult &result) {
  for (const Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.message;
    if (diagnostic.message.empty() || diagnostic.message.back() != '\n')
      std::cerr << '\n';
  }
}

ExecutionStatus execute(InteractiveSession &session,
                        const std::string &source) {
  auto resultOr = session.execute(source);
  if (resultOr.isError()) {
    std::cerr << "mojoorc: " << resultOr.getError() << '\n';
    return ExecutionStatus::InfrastructureError;
  }

  printDiagnostics(*resultOr);
  if (resultOr->runtimeError) {
    const auto &error = *resultOr->runtimeError;
    std::cerr << "Error: " << error.message << '\n';
    if (!error.stackTrace.empty()) {
      std::cerr << error.stackTrace;
      if (error.stackTrace.back() != '\n')
        std::cerr << '\n';
    }
  }
  return resultOr->succeeded ? ExecutionStatus::Success
                             : ExecutionStatus::UserError;
}

std::optional<std::string> readCell(size_t cellNumber) {
  std::string cell;
  std::string line;
  bool firstLine = true;

  while (true) {
    std::cout << (firstLine ? std::to_string(cellNumber) + "> " : ".. ")
              << std::flush;
    if (!std::getline(std::cin, line)) {
      if (cell.empty())
        return std::nullopt;
      return cell;
    }

    if (line.empty()) {
      if (!cell.empty())
        return cell;
      continue;
    }

    cell += line;
    cell += '\n';
    firstLine = false;
  }
}

int runInteractive(InteractiveSession &session) {
  std::cout << "Mojo ORC REPL\n"
               "Expressions are delimited by a blank line. :quit exits.\n\n";

  for (size_t cellNumber = 1;; ++cellNumber) {
    std::optional<std::string> cell = readCell(cellNumber);
    if (!cell || *cell == ":quit\n")
      return 0;
    if (execute(session, *cell) == ExecutionStatus::InfrastructureError)
      return 1;
  }
}

std::optional<std::string> readFile(const char *path) {
  std::ifstream input(path);
  if (!input)
    return std::nullopt;
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void printUsage(const char *program) {
  std::cout << "usage: " << program << " [-e CODE | FILE]\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    printUsage(argv[0]);
    return 0;
  }

  bool interactive = argc == 1;
  std::optional<std::string> source;
  if (argc == 3 && std::string(argv[1]) == "-e") {
    source = argv[2];
  } else if (argc == 2 && argv[1][0] != '-') {
    source = readFile(argv[1]);
    if (!source) {
      std::cerr << "mojoorc: unable to read '" << argv[1] << "'\n";
      return 1;
    }
  } else if (!interactive) {
    printUsage(argv[0]);
    return 2;
  }

  auto sessionOr = InteractiveSession::create();
  if (sessionOr.isError()) {
    std::cerr << "mojoorc: " << sessionOr.getError() << '\n';
    return 1;
  }
  std::unique_ptr<InteractiveSession> session = sessionOr.takeValue();

  if (interactive)
    return runInteractive(*session);
  return execute(*session, *source) == ExecutionStatus::Success ? 0 : 1;
}
