mod ffi;
mod native;

use kernmini::{
    CompleteRequest, Error, ErrorKind, ExecuteOutcome, ExecuteRequest, ExecutionContext, InspectRequest, KernelInfo, LanguageSession, Result,
    ThreadWorker,
};
use native::{KernelContext, NativeSession, Options};
use serde_json::{Value, json};
use std::ffi::{CStr, c_char};
use std::sync::{Arc, Mutex};
use std::thread::Builder;

#[derive(Clone)]
struct MojoSession {
    worker: ThreadWorker<NativeSession>,
    kernel_context: KernelContext,
    version: String,
}

impl MojoSession {
    async fn start(options: Options, version: String) -> Result<Self> {
        let kernel_context = Arc::new(Mutex::new(None));
        let output_context = kernel_context.clone();
        let worker = ThreadWorker::start(Builder::new().name("xmojo-session".into()), move || NativeSession::create(options, output_context)).await?;
        Ok(Self { worker, kernel_context, version })
    }
}

#[async_trait::async_trait]
impl LanguageSession for MojoSession {
    fn supported_features(&self) -> Vec<String> {
        vec!["interrupt requires restart".into()]
    }

    fn kernel_info(&self) -> Result<KernelInfo> {
        Ok(KernelInfo {
            implementation: "xmojo".into(),
            implementation_version: self.version.clone(),
            banner: "xmojo ORC Mojo kernel".into(),
            language_info: json!({"name": "mojo", "version": "nightly", "mimetype": "text/x-mojo", "file_extension": ".mojo",
                "pygments_lexer": "mojo", "codemirror_mode": "text/x-mojo"}),
        })
    }

    fn set_kernel_context(&self, context: ExecutionContext) -> Result<()> {
        *self.kernel_context.lock().unwrap() = Some(context);
        Ok(())
    }

    async fn execute(&self, request: ExecuteRequest, context: ExecutionContext) -> Result<ExecuteOutcome> {
        self.worker.call(move |session| session.execute(&request.code, context)).await?
    }

    async fn complete(&self, request: CompleteRequest) -> Result<Value> {
        self.worker.call(move |session| session.complete(&request.code, request.cursor_pos)).await?
    }

    async fn inspect(&self, request: InspectRequest) -> Result<Value> {
        self.worker.call(move |session| session.inspect(&request.code, request.cursor_pos, request.detail_level)).await?
    }

    async fn is_complete(&self, code: String) -> Result<Value> {
        self.worker.call(move |session| session.is_complete(&code)).await?
    }

    async fn shutdown(&self) -> Result<()> {
        self.worker.shutdown().await
    }
}

/// Called by the existing C++ CLI; Rust supplies the session callbacks.
///
/// # Safety
/// Strings must be NUL-terminated. Non-NULL options and their spans must satisfy SessionC.h for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn xmojo_run_kernel(connection_file: *const c_char, options: *const ffi::SessionOptions, version: *const c_char) -> i32 {
    let run = || -> Result<()> {
        if connection_file.is_null() || version.is_null() {
            return Err(Error::new(ErrorKind::InvalidInput, "NULL kernel connection file or version"));
        }
        let file = unsafe { CStr::from_ptr(connection_file) }.to_string_lossy().into_owned();
        let version = unsafe { CStr::from_ptr(version) }.to_string_lossy().into_owned();
        let options = unsafe { Options::copy(options) };
        kernmini::run_kernel_blocking(file, MojoSession::start(options, version))
    };
    match run() {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("xmojo: {error}");
            1
        }
    }
}
