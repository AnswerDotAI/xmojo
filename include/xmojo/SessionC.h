//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions:
//===----------------------------------------------------------------------===//

#ifndef XMOJO_SESSIONC_H
#define XMOJO_SESSIONC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define XMOJO_C_NOEXCEPT noexcept
#else
#define XMOJO_C_NOEXCEPT
#endif

#define XMOJO_C_EXPORT __attribute__((visibility("default")))

typedef struct xmojo_session xmojo_session;
typedef struct xmojo_error xmojo_error;

/// A byte span, not a NUL-terminated string. An empty span may have NULL data.
/// Input spans are borrowed for the duration of the call. All callback spans,
/// arrays, and result pointers are borrowed only until that callback returns.
typedef struct xmojo_string {
  const char *data;
  size_t size;
} xmojo_string;

typedef uint32_t xmojo_output_stream;
enum { XMOJO_STDOUT = 0, XMOJO_STDERR = 1 };

typedef uint32_t xmojo_display_kind;
enum { XMOJO_DISPLAY_DATA = 0, XMOJO_EXECUTE_RESULT = 1 };

typedef uint32_t xmojo_diagnostic_severity;
enum { XMOJO_NOTE = 0, XMOJO_WARNING = 1, XMOJO_ERROR = 2 };

typedef uint32_t xmojo_completion_kind;
enum {
  XMOJO_COMPLETION_UNKNOWN = 0,
  XMOJO_COMPLETION_PACKAGE = 1,
  XMOJO_COMPLETION_MODULE = 2,
  XMOJO_COMPLETION_STRUCT = 3,
  XMOJO_COMPLETION_FUNCTION = 4,
  XMOJO_COMPLETION_FIELD = 5,
  XMOJO_COMPLETION_VARIABLE = 6,
  XMOJO_COMPLETION_TRAIT = 7
};

typedef uint32_t xmojo_execution_status;
enum {
  XMOJO_EXECUTION_SUCCEEDED = 0,
  XMOJO_EXECUTION_COMPILE_ERROR = 1,
  XMOJO_EXECUTION_RUNTIME_ERROR = 2
};

typedef uint32_t xmojo_completeness_status;
enum {
  XMOJO_COMPLETE = 0,
  XMOJO_INCOMPLETE = 1,
  XMOJO_INVALID = 2
};

typedef struct xmojo_mime_data {
  xmojo_string mime_type;
  xmojo_string data;
} xmojo_mime_data;

typedef struct xmojo_completion_item {
  xmojo_string label;
  xmojo_string documentation;
  xmojo_completion_kind kind;
  xmojo_string signature;
} xmojo_completion_item;

typedef struct xmojo_completion_result {
  const xmojo_completion_item *items;
  size_t size;
  size_t cursor_start;
  size_t cursor_end;
} xmojo_completion_result;

/// Parameter ranges are UTF-8 byte offsets in their signature's full label.
typedef struct xmojo_signature_parameter {
  size_t label_start;
  size_t label_end;
  xmojo_string documentation;
} xmojo_signature_parameter;

typedef struct xmojo_signature {
  xmojo_string label;
  xmojo_string documentation;
  const xmojo_signature_parameter *parameters;
  size_t parameter_count;
} xmojo_signature;

typedef struct xmojo_inspection_result {
  uint32_t found;
  xmojo_string markdown;
  const xmojo_signature *signatures;
  size_t signature_count;
  size_t active_signature;
  size_t active_parameter;
} xmojo_inspection_result;

typedef struct xmojo_completeness_result {
  xmojo_completeness_status status;
  xmojo_string indent;
} xmojo_completeness_result;

typedef void (*xmojo_output_callback)(void *context, xmojo_output_stream stream,
                                      xmojo_string text);
typedef void (*xmojo_display_callback)(void *context, xmojo_display_kind kind,
                                       const xmojo_mime_data *data, size_t size);
typedef void (*xmojo_diagnostic_callback)(
    void *context, xmojo_diagnostic_severity severity, xmojo_string message);
typedef void (*xmojo_runtime_error_callback)(void *context, xmojo_string message,
                                             xmojo_string stack_trace);
typedef void (*xmojo_completion_callback)(
    void *context, const xmojo_completion_result *result);
typedef void (*xmojo_inspection_callback)(
    void *context, const xmojo_inspection_result *result);
typedef void (*xmojo_completeness_callback)(
    void *context, const xmojo_completeness_result *result);

typedef struct xmojo_callbacks {
  void *context;
  xmojo_output_callback output;
  xmojo_display_callback display;
  xmojo_diagnostic_callback diagnostic;
  xmojo_runtime_error_callback runtime_error;
} xmojo_callbacks;

typedef struct xmojo_session_options {
  const xmojo_string *import_paths;
  size_t import_path_count;
  xmojo_string target_accelerator;
  xmojo_callbacks callbacks;
} xmojo_session_options;

/// Session calls, including destruction, must run on the OS thread that
/// created the session. Calls must not overlap or reenter from callbacks.
/// Callbacks are synchronous on that thread and must not throw or unwind.
/// Options and callback pointers are copied; callback context must remain valid
/// until destruction returns, since persistent-value destructors can emit output.
/// NULL options selects defaults. All callbacks in options are optional.
/// Output and MIME callbacks run live, in native emission order; final-expression
/// results arrive as XMOJO_EXECUTE_RESULT, never as a second returned result.
/// Native fatal errors and allocation failures terminate the process; C++
/// exceptions never unwind across this ABI.
XMOJO_C_EXPORT xmojo_error *
xmojo_session_create(const xmojo_session_options *options,
                     xmojo_session **session) XMOJO_C_NOEXCEPT;

/// NULL is a no-op. A wrong-thread error leaves the session alive.
XMOJO_C_EXPORT xmojo_error *
xmojo_session_destroy(xmojo_session *session) XMOJO_C_NOEXCEPT;

/// NULL error means the call completed, including ordinary Mojo failures.
/// In that case status is set and diagnostics/runtime errors have been delivered
/// through the callbacks. Compiler diagnostics are delivered after execution,
/// followed by the runtime error if present. A non-NULL error reports an API or
/// compiler-infrastructure failure, and status must be ignored.
XMOJO_C_EXPORT xmojo_error *
xmojo_session_execute(xmojo_session *session, xmojo_string source,
                      xmojo_execution_status *status) XMOJO_C_NOEXCEPT;

/// Tooling callbacks are required and run exactly once on success. All cursor
/// positions are UTF-8 byte offsets, without ranking or character conversion.
XMOJO_C_EXPORT xmojo_error *
xmojo_session_complete(xmojo_session *session, xmojo_string source,
                       size_t cursor_position, xmojo_completion_callback callback,
                       void *context) XMOJO_C_NOEXCEPT;
/// detail_level 0 returns documentation; 1 also includes retained source when
/// available. Signature-help arrays are independent of the documentation text.
XMOJO_C_EXPORT xmojo_error *
xmojo_session_inspect(xmojo_session *session, xmojo_string source,
                      size_t cursor_position, uint32_t detail_level,
                      xmojo_inspection_callback callback,
                      void *context) XMOJO_C_NOEXCEPT;
XMOJO_C_EXPORT xmojo_error *
xmojo_session_is_complete(xmojo_session *session, xmojo_string source,
                          xmojo_completeness_callback callback,
                          void *context) XMOJO_C_NOEXCEPT;

/// Errors are caller-owned and may be read/freed on any thread. The message is
/// borrowed until error_free. NULL returns an empty span; freeing NULL is safe.
XMOJO_C_EXPORT xmojo_string
xmojo_error_message(const xmojo_error *error) XMOJO_C_NOEXCEPT;
XMOJO_C_EXPORT void xmojo_error_free(xmojo_error *error) XMOJO_C_NOEXCEPT;

#undef XMOJO_C_EXPORT
#undef XMOJO_C_NOEXCEPT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XMOJO_SESSIONC_H
