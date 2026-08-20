//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#ifndef XMOJO_INTERACTIVEPARSER_H
#define XMOJO_INTERACTIVEPARSER_H

#include "KGEN/MojoTooling/PublicASTDecl.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class SourceMgr;
}

namespace mlir {
class MLIRContext;
}

namespace M::KGEN {
class CompilationOptions;
}

namespace xmojo {

struct Diagnostic;
struct CompletionResult;
struct InspectionResult;
struct CompletenessResult;

/// Parses interactive cells against an explicitly committed declaration
/// history. Parsing a cell never changes the history until commit is called.
class InteractiveParser {
public:
  /// One top-level `var` kept alive across cells: its name, and the compiler's
  /// handle for its type, valid while this parser lives.
  struct PersistentVar {
    std::string name;
    mlir::Type type;
  };

  struct Cell {
    M::MojoASTDeclRef moduleDecl;
    M::MojoASTDeclRef entryPointDecl;
    std::string source;
    std::string moduleName;
    /// Top-level `var`s this cell declares, in the order the wrapper persists
    /// them, continuing the session's argument order.
    std::vector<PersistentVar> newVars;
  };

  InteractiveParser(mlir::MLIRContext &context,
                    const M::KGEN::CompilationOptions &options,
                    std::vector<std::string> importPaths);
  ~InteractiveParser();

  InteractiveParser(const InteractiveParser &) = delete;
  InteractiveParser &operator=(const InteractiveParser &) = delete;

  std::optional<Cell> parse(llvm::StringRef source, llvm::StringRef moduleName,
                            llvm::StringRef functionName,
                            llvm::ArrayRef<PersistentVar> sessionVars,
                            std::vector<Diagnostic> &diagnostics);
  void commit(const Cell &cell);

  CompletionResult complete(llvm::StringRef source, size_t cursorPosition);
  InspectionResult inspect(llvm::StringRef source, size_t cursorPosition);
  CompletenessResult isComplete(llvm::StringRef source);

  llvm::SourceMgr &getSourceManager();

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace xmojo

#endif // XMOJO_INTERACTIVEPARSER_H
