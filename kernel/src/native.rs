use crate::ffi;
use kernmini::{Error, ErrorKind, ExecuteOutcome, ExecutionContext, LanguageError, LanguageEvent, Result};
use serde_json::{Value, json};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::{Arc, Mutex};

pub type KernelContext = Arc<Mutex<Option<ExecutionContext>>>;

pub struct Options {
    pub import_paths: Vec<Vec<u8>>,
    pub target_accelerator: Vec<u8>,
}

impl Options {
    pub unsafe fn copy(options: *const ffi::SessionOptions) -> Self {
        if options.is_null() {
            return Self { import_paths: vec![], target_accelerator: vec![] };
        }
        let options = unsafe { &*options };
        let paths = if options.import_path_count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(options.import_paths, options.import_path_count) }
        };
        Self {
            import_paths: paths.iter().map(|path| unsafe { path.bytes() }.to_vec()).collect(),
            target_accelerator: unsafe { options.target_accelerator.bytes() }.to_vec(),
        }
    }
}

struct Output {
    context: Option<ExecutionContext>,
    kernel_context: KernelContext,
    failure: Option<Error>,
    diagnostics: Vec<String>,
    runtime_error: Option<LanguageError>,
}

impl Output {
    fn emit(&mut self, event: LanguageEvent) {
        if self.failure.is_some() {
            return;
        }
        let context = self.context.clone().or_else(|| self.kernel_context.lock().unwrap().clone());
        if let Some(context) = context {
            self.failure = context.emit_blocking(event).err();
        }
    }

    fn emit_diagnostics(&mut self) {
        for text in std::mem::take(&mut self.diagnostics) {
            self.emit(LanguageEvent::Stream { name: "stderr".into(), text });
        }
    }
}

// Callback state is boxed at a stable address and accessed only on the session's owning thread.
unsafe extern "C" fn output(context: *mut c_void, stream: u32, text: ffi::Span) {
    let output = unsafe { &mut *context.cast::<Output>() };
    output.emit(LanguageEvent::Stream { name: if stream == 0 { "stdout" } else { "stderr" }.into(), text: unsafe { text.text() } });
}

unsafe extern "C" fn display(context: *mut c_void, kind: u32, items: *const ffi::MimeData, size: usize) {
    let output = unsafe { &mut *context.cast::<Output>() };
    let items = if size == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(items, size) } };
    let data: serde_json::Map<String, Value> = items.iter().map(|item| unsafe { (item.mime_type.text(), Value::String(item.data.text())) }).collect();
    let event = if kind == 1 {
        let count = output.context.as_ref().map(ExecutionContext::execution_count).unwrap_or(0);
        LanguageEvent::Message {
            msg_type: "execute_result".into(),
            content: json!({"execution_count": count, "data": data, "metadata": {}}),
            metadata: json!({}),
            identity: None,
            buffers: vec![],
        }
    } else {
        LanguageEvent::Display { event: json!({"data": data}), buffers: vec![] }
    };
    output.emit(event);
}

unsafe extern "C" fn diagnostic(context: *mut c_void, _severity: u32, message: ffi::Span) {
    unsafe { &mut *context.cast::<Output>() }.diagnostics.push(unsafe { message.text() });
}

unsafe extern "C" fn runtime_error(context: *mut c_void, message: ffi::Span, stack: ffi::Span) {
    let message = unsafe { message.text() };
    let stack = unsafe { stack.text() };
    let mut traceback = vec![format!("MojoError: {message}")];
    if !stack.is_empty() {
        traceback.push(stack);
    }
    unsafe { &mut *context.cast::<Output>() }.runtime_error = Some(LanguageError { ename: "MojoError".into(), evalue: message, traceback });
}

fn check(error: *mut ffi::Error, kind: ErrorKind) -> Result<()> {
    if error.is_null() {
        return Ok(());
    }
    let message = unsafe { ffi::xmojo_error_message(error).text() };
    unsafe {
        ffi::xmojo_error_free(error);
    }
    Err(Error::new(kind, message))
}

pub struct NativeSession {
    session: NonNull<ffi::Session>,
    output: Box<Output>,
}

impl NativeSession {
    pub fn create(options: Options, kernel_context: KernelContext) -> Result<Self> {
        let mut sink = Box::new(Output { context: None, kernel_context, failure: None, diagnostics: vec![], runtime_error: None });
        let paths: Vec<_> = options.import_paths.iter().map(|path| ffi::Span::new(path)).collect();
        let options = ffi::SessionOptions {
            import_paths: paths.as_ptr(),
            import_path_count: paths.len(),
            target_accelerator: ffi::Span::new(&options.target_accelerator),
            callbacks: ffi::Callbacks {
                context: (&mut *sink as *mut Output).cast(),
                output: Some(output),
                display: Some(display),
                diagnostic: Some(diagnostic),
                runtime_error: Some(runtime_error),
            },
        };
        let mut session = std::ptr::null_mut();
        check(unsafe { ffi::xmojo_session_create(&options, &mut session) }, ErrorKind::Adapter)?;
        let session = NonNull::new(session).ok_or_else(|| Error::new(ErrorKind::Adapter, "native session creation returned NULL"))?;
        Ok(Self { session, output: sink })
    }

    pub fn execute(&mut self, code: &str, context: ExecutionContext) -> Result<ExecuteOutcome> {
        self.output.context = Some(context);
        self.output.failure = None;
        self.output.diagnostics.clear();
        self.output.runtime_error = None;
        let mut status = 0;
        let native =
            check(unsafe { ffi::xmojo_session_execute(self.session.as_ptr(), ffi::Span::new(code.as_bytes()), &mut status) }, ErrorKind::Closed);
        if let Err(error) = native {
            self.output.context = None;
            return Err(error);
        }
        let error = match status {
            0 => {
                self.output.emit_diagnostics();
                None
            }
            1 => Some(LanguageError {
                ename: "MojoError".into(),
                evalue: self.output.diagnostics.concat(),
                traceback: std::mem::take(&mut self.output.diagnostics),
            }),
            2 => {
                self.output.emit_diagnostics();
                self.output.runtime_error.take()
            }
            _ => {
                self.output.context = None;
                return Err(Error::new(ErrorKind::Adapter, "invalid native execution status"));
            }
        };
        self.output.context = None;
        if let Some(failure) = self.output.failure.take() {
            return Err(failure);
        }
        if status == 2 && error.is_none() {
            return Err(Error::new(ErrorKind::Adapter, "native runtime error callback was not delivered"));
        }
        Ok(ExecuteOutcome { error, ..Default::default() })
    }

    pub fn complete(&mut self, code: &str, cursor: u64) -> Result<Value> {
        let mut reply = json!({});
        check(
            unsafe {
                ffi::xmojo_session_complete(
                    self.session.as_ptr(),
                    ffi::Span::new(code.as_bytes()),
                    byte_offset(code, cursor),
                    completion,
                    (&mut reply as *mut Value).cast(),
                )
            },
            ErrorKind::Adapter,
        )?;
        for name in ["cursor_start", "cursor_end"] {
            reply[name] = json!(character_offset(code, reply[name].as_u64().unwrap_or(0) as usize));
        }
        let start = reply["cursor_start"].clone();
        let end = reply["cursor_end"].clone();
        for item in reply["metadata"]["_jupyter_types_experimental"].as_array_mut().unwrap() {
            item["start"] = start.clone();
            item["end"] = end.clone();
        }
        Ok(reply)
    }

    pub fn inspect(&mut self, code: &str, cursor: u64, detail_level: u64) -> Result<Value> {
        let mut reply = json!({});
        check(
            unsafe {
                ffi::xmojo_session_inspect(
                    self.session.as_ptr(),
                    ffi::Span::new(code.as_bytes()),
                    byte_offset(code, cursor),
                    u32::from(detail_level != 0),
                    inspection,
                    (&mut reply as *mut Value).cast(),
                )
            },
            ErrorKind::Adapter,
        )?;
        Ok(reply)
    }

    pub fn is_complete(&mut self, code: &str) -> Result<Value> {
        let mut reply = json!({});
        check(
            unsafe {
                ffi::xmojo_session_is_complete(
                    self.session.as_ptr(),
                    ffi::Span::new(code.as_bytes()),
                    completeness,
                    (&mut reply as *mut Value).cast(),
                )
            },
            ErrorKind::Adapter,
        )?;
        Ok(reply)
    }
}

impl Drop for NativeSession {
    fn drop(&mut self) {
        if let Err(error) = check(unsafe { ffi::xmojo_session_destroy(self.session.as_ptr()) }, ErrorKind::Adapter) {
            eprintln!("xmojo: {error}");
        }
    }
}

fn byte_offset(code: &str, cursor: u64) -> usize {
    code.char_indices().nth(cursor as usize).map(|(index, _)| index).unwrap_or(code.len())
}
fn character_offset(code: &str, cursor: usize) -> usize {
    code.char_indices().take_while(|(index, _)| *index < cursor).count()
}

unsafe extern "C" fn completion(context: *mut c_void, result: *const ffi::CompletionResult) {
    let result = unsafe { &*result };
    let items = if result.size == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(result.items, result.size) } };
    let mut matches = vec![];
    let mut detailed = vec![];
    for item in items {
        let text = unsafe { item.label.text() };
        let kind = match item.kind {
            1 => "package",
            2 => "module",
            3 | 7 => "class",
            4 => "function",
            5 => "property",
            6 => "instance",
            _ => "unknown",
        };
        detailed.push(json!({"text": text, "type": kind, "signature": unsafe { item.signature.text() }, "rank": 0}));
        matches.push(text);
    }
    let reply = unsafe { &mut *context.cast::<Value>() };
    *reply = json!({"status": "ok", "matches": matches, "cursor_start": result.cursor_start, "cursor_end": result.cursor_end,
        "metadata": {"_jupyter_types_experimental": detailed}});
}

unsafe extern "C" fn inspection(context: *mut c_void, result: *const ffi::InspectionResult) {
    let result = unsafe { &*result };
    let mut data = json!({});
    let markdown = unsafe { result.markdown.text() };
    if !markdown.is_empty() {
        data["text/markdown"] = json!(markdown);
    }
    if result.signature_count != 0 {
        let signatures = unsafe { std::slice::from_raw_parts(result.signatures, result.signature_count) };
        let signatures: Vec<_> = signatures
            .iter()
            .map(|signature| {
                let label = unsafe { signature.label.text() };
                let parameters = if signature.parameter_count == 0 {
                    &[][..]
                } else {
                    unsafe { std::slice::from_raw_parts(signature.parameters, signature.parameter_count) }
                };
                let parameters: Vec<_> = parameters
                    .iter()
                    .map(|parameter| {
                        json!({
                            "label": [label[..parameter.label_start].encode_utf16().count(), label[..parameter.label_end].encode_utf16().count()],
                            "documentation": {"value": unsafe { parameter.documentation.text() }},
                        })
                    })
                    .collect();
                json!({"label": label, "documentation": {"value": unsafe { signature.documentation.text() }}, "parameters": parameters})
            })
            .collect();
        data["application/json"] =
            json!({"signatures": signatures, "activeSignature": result.active_signature, "activeParameter": result.active_parameter});
    }
    let reply = unsafe { &mut *context.cast::<Value>() };
    *reply = json!({"status": "ok", "found": result.found != 0, "data": data, "metadata": {}});
}

unsafe extern "C" fn completeness(context: *mut c_void, result: *const ffi::CompletenessResult) {
    let result = unsafe { &*result };
    let reply = unsafe { &mut *context.cast::<Value>() };
    *reply = match result.status {
        0 => json!({"status": "complete"}),
        1 => json!({"status": "incomplete", "indent": unsafe { result.indent.text() }}),
        2 => json!({"status": "invalid"}),
        _ => json!({"status": "unknown"}),
    };
}
