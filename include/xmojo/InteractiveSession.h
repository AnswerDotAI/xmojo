//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_INTERACTIVESESSION_H
#define XMOJO_INTERACTIVESESSION_H

#include "Support/ErrorOr.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xmojo {

enum class DiagnosticSeverity { Note, Warning, Error };
enum class OutputStream { Stdout, Stderr };
enum class DisplayKind { DisplayData, ExecuteResult };

/// Receives one complete CPU print emission during execute(). The callback is
/// synchronous and must copy text that it needs after returning.
using OutputCallback =
    llvm::unique_function<void(OutputStream, llvm::StringRef)>;

struct MimeData {
  std::string mimeType;
  std::string data;
};

struct DisplayEvent {
  DisplayKind kind;
  std::vector<MimeData> data;
};

/// Receives one complete MIME bundle during execute(). The callback is
/// synchronous and owns the supplied event after returning.
using DisplayCallback = llvm::unique_function<void(DisplayEvent)>;

struct Diagnostic {
  DiagnosticSeverity severity;
  std::string message;
};

struct RuntimeError {
  std::string message;
  std::string stackTrace;
};

struct ExecutionResult {
  bool succeeded = false;
  std::vector<Diagnostic> diagnostics;
  std::optional<RuntimeError> runtimeError;
};

enum class CompletionKind {
  Unknown,
  Package,
  Module,
  Struct,
  Function,
  Field,
  Variable,
  Trait,
};

struct CompletionItem {
  std::string label;
  std::string documentation;
  CompletionKind kind = CompletionKind::Unknown;
};

struct CompletionResult {
  std::vector<CompletionItem> items;
  size_t cursorStart = 0;
  size_t cursorEnd = 0;
};

struct InspectionResult {
  bool found = false;
  std::string markdown;
};

enum class CompletenessStatus { Complete, Incomplete, Invalid };

struct CompletenessResult {
  CompletenessStatus status = CompletenessStatus::Complete;
  std::string indent;
};

struct SessionOptions {
  std::vector<std::string> importPaths;
  OutputCallback output;
  DisplayCallback display;
};

/// A persistent, in-process Mojo compilation and execution session.
///
/// Each successfully loaded cell contributes declarations to subsequent cells
/// and remains in a shared ORC execution engine, even if its execution raises.
/// User compilation and runtime failures are returned as ExecutionResult
/// values; failures to construct or safely continue operating the compiler are
/// returned as Error values.
class InteractiveSession {
public:
  static M::ErrorOr<std::unique_ptr<InteractiveSession>>
  create(SessionOptions options = {});

  ~InteractiveSession();

  InteractiveSession(const InteractiveSession &) = delete;
  InteractiveSession &operator=(const InteractiveSession &) = delete;

  M::ErrorOr<ExecutionResult> execute(llvm::StringRef source);
  CompletionResult complete(llvm::StringRef source, size_t cursorPosition);
  InspectionResult inspect(llvm::StringRef source, size_t cursorPosition);
  CompletenessResult isComplete(llvm::StringRef source);

private:
  class Impl;

  explicit InteractiveSession(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl;
};

} // namespace xmojo

#endif // XMOJO_INTERACTIVESESSION_H
