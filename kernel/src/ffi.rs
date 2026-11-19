//! The language-only C ABI declared in include/xmojo/SessionC.h.

use std::ffi::{c_char, c_void};

#[repr(C)]
pub struct Session {
    _private: [u8; 0],
}
#[repr(C)]
pub struct Error {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Span {
    pub data: *const c_char,
    pub size: usize,
}

impl Span {
    pub fn new(bytes: &[u8]) -> Self {
        Self { data: bytes.as_ptr().cast(), size: bytes.len() }
    }

    /// The C ABI borrows valid spans only until the current call/callback returns.
    pub unsafe fn bytes<'a>(self) -> &'a [u8] {
        if self.size == 0 { &[] } else { unsafe { std::slice::from_raw_parts(self.data.cast(), self.size) } }
    }

    pub unsafe fn text(self) -> String {
        String::from_utf8_lossy(unsafe { self.bytes() }).into_owned()
    }
}

#[repr(C)]
pub struct MimeData {
    pub mime_type: Span,
    pub data: Span,
}

#[repr(C)]
pub struct CompletionItem {
    pub label: Span,
    pub documentation: Span,
    pub kind: u32,
    pub signature: Span,
}

#[repr(C)]
pub struct CompletionResult {
    pub items: *const CompletionItem,
    pub size: usize,
    pub cursor_start: usize,
    pub cursor_end: usize,
}

#[repr(C)]
pub struct SignatureParameter {
    pub label_start: usize,
    pub label_end: usize,
    pub documentation: Span,
}

#[repr(C)]
pub struct Signature {
    pub label: Span,
    pub documentation: Span,
    pub parameters: *const SignatureParameter,
    pub parameter_count: usize,
}

#[repr(C)]
pub struct InspectionResult {
    pub found: u32,
    pub markdown: Span,
    pub signatures: *const Signature,
    pub signature_count: usize,
    pub active_signature: usize,
    pub active_parameter: usize,
}

#[repr(C)]
pub struct CompletenessResult {
    pub status: u32,
    pub indent: Span,
}

#[repr(C)]
pub struct Callbacks {
    pub context: *mut c_void,
    pub output: Option<unsafe extern "C" fn(*mut c_void, u32, Span)>,
    pub display: Option<unsafe extern "C" fn(*mut c_void, u32, *const MimeData, usize)>,
    pub diagnostic: Option<unsafe extern "C" fn(*mut c_void, u32, Span)>,
    pub runtime_error: Option<unsafe extern "C" fn(*mut c_void, Span, Span)>,
}

#[repr(C)]
pub struct SessionOptions {
    pub import_paths: *const Span,
    pub import_path_count: usize,
    pub target_accelerator: Span,
    pub callbacks: Callbacks,
}

unsafe extern "C" {
    pub fn xmojo_session_create(options: *const SessionOptions, session: *mut *mut Session) -> *mut Error;
    pub fn xmojo_session_destroy(session: *mut Session) -> *mut Error;
    pub fn xmojo_session_execute(session: *mut Session, source: Span, status: *mut u32) -> *mut Error;
    pub fn xmojo_session_complete(
        session: *mut Session,
        source: Span,
        cursor: usize,
        callback: unsafe extern "C" fn(*mut c_void, *const CompletionResult),
        context: *mut c_void,
    ) -> *mut Error;
    pub fn xmojo_session_inspect(
        session: *mut Session,
        source: Span,
        cursor: usize,
        detail_level: u32,
        callback: unsafe extern "C" fn(*mut c_void, *const InspectionResult),
        context: *mut c_void,
    ) -> *mut Error;
    pub fn xmojo_session_is_complete(
        session: *mut Session,
        source: Span,
        callback: unsafe extern "C" fn(*mut c_void, *const CompletenessResult),
        context: *mut c_void,
    ) -> *mut Error;
    pub fn xmojo_error_message(error: *const Error) -> Span;
    pub fn xmojo_error_free(error: *mut Error);
}
