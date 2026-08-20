//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_INTERACTIVESESSION_H
#define XMOJO_INTERACTIVESESSION_H

#include "Support/ErrorOr.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace xmojo {

enum class DiagnosticSeverity { Note, Warning, Error };

struct Diagnostic {
  DiagnosticSeverity severity;
  std::string message;
};

struct ExecutionResult {
  bool succeeded = false;
  std::vector<Diagnostic> diagnostics;
};

struct SessionOptions {
  std::vector<std::string> importPaths;
};

/// A persistent, in-process Mojo compilation and execution session.
///
/// Each successfully compiled cell contributes declarations to subsequent
/// cells and remains loaded in a shared ORC execution engine. User compilation
/// failures are returned as ExecutionResult values; failures to construct or
/// operate the compiler itself are returned as Error values.
class InteractiveSession {
public:
  static M::ErrorOr<std::unique_ptr<InteractiveSession>>
  create(SessionOptions options = {});

  ~InteractiveSession();

  InteractiveSession(const InteractiveSession &) = delete;
  InteractiveSession &operator=(const InteractiveSession &) = delete;

  M::ErrorOr<ExecutionResult> execute(llvm::StringRef source);

private:
  class Impl;

  explicit InteractiveSession(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl;
};

} // namespace xmojo

#endif // XMOJO_INTERACTIVESESSION_H
