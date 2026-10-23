//! Start a local seekdb instance and obtain its MySQL-protocol endpoint.
//! SQL connections are managed by the caller's chosen MySQL client.

use libloading::Library;
use std::collections::BTreeMap;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr, CString};
use std::fmt;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::{Mutex, OnceLock};

type Handle = *mut c_void;
type OpenFn = unsafe extern "C" fn(*const c_char, *const *const c_char, *mut Handle) -> c_int;
type LastOpenErrorFn = unsafe extern "C" fn() -> *const c_char;
type CloseFn = unsafe extern "C" fn(Handle) -> c_int;
type SetBinaryPathFn = unsafe extern "C" fn(*const c_char) -> c_int;
type ConnectionOptionsFn = unsafe extern "C" fn(Handle, *mut RawConnectionOptions) -> c_int;

#[repr(C)]
#[derive(Default)]
struct RawConnectionOptions {
    transport: *const c_char,
    port: c_uint,
    host: *const c_char,
    unix_socket: *const c_char,
    named_pipe: *const c_char,
    user: *const c_char,
}

struct Native {
    _library: Library,
    open: OpenFn,
    last_open_error: LastOpenErrorFn,
    close: CloseFn,
    connection_options: ConnectionOptionsFn,
}

static NATIVE: OnceLock<Native> = OnceLock::new();
static NATIVE_INIT: Mutex<()> = Mutex::new(());

impl Native {
    fn get() -> Result<&'static Self, Error> {
        if let Some(native) = NATIVE.get() {
            return Ok(native);
        }
        let _guard = NATIVE_INIT
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(native) = NATIVE.get() {
            return Ok(native);
        }
        let native = Self::load().map_err(Error::Setup)?;
        // The mutex ensures only this thread can initialize NATIVE. Failed
        // loads are left uncached so a corrected runtime path can be retried.
        let _ = NATIVE.set(native);
        Ok(NATIVE.get().expect("native library was initialized"))
    }

    fn directory(library_name: &str) -> Result<PathBuf, String> {
        if let Some(directory) = std::env::var_os("SEEKDB_NATIVE_DIR") {
            return Self::validate_directory(Path::new(&directory), library_name);
        }
        if let Ok(executable) = std::env::current_exe() {
            if let Some(parent) = executable.parent() {
                let staged = parent.join("lib");
                if staged.join(library_name).exists() || staged.join("seekdb").exists() {
                    return Self::validate_directory(&staged, library_name);
                }
            }
        }
        Self::validate_directory(Path::new(env!("SEEKDB_BUILD_NATIVE_DIR")), library_name)
    }

    fn validate_directory(directory: &Path, library_name: &str) -> Result<PathBuf, String> {
        let directory = directory
            .canonicalize()
            .map_err(|error| format!("resolve {}: {error}", directory.display()))?;
        for name in [library_name, "seekdb"] {
            if !directory.join(name).is_file() {
                return Err(format!("{} is missing", directory.join(name).display()));
            }
        }
        Ok(directory)
    }

    fn load() -> Result<Self, String> {
        let library_name = if cfg!(target_os = "macos") {
            "libseekdb.dylib"
        } else {
            "libseekdb.so"
        };
        let directory = Self::directory(library_name)?;
        let library_path = directory.join(library_name);
        let binary_path = directory.join("seekdb");
        let binary = CString::new(binary_path.as_os_str().as_bytes())
            .map_err(|_| "seekdb executable path contains NUL".to_string())?;
        // SAFETY: The library remains owned by Native for as long as the copied
        // function pointers can be called. Function signatures match seekdb.h.
        unsafe {
            let library = Library::new(&library_path)
                .map_err(|error| format!("load {}: {error}", library_path.display()))?;
            let open = *library
                .get::<OpenFn>(b"seekdb_open\0")
                .map_err(|error| error.to_string())?;
            let last_open_error = *library
                .get::<LastOpenErrorFn>(b"seekdb_last_open_error\0")
                .map_err(|error| error.to_string())?;
            let close = *library
                .get::<CloseFn>(b"seekdb_close\0")
                .map_err(|error| error.to_string())?;
            let set_binary_path = *library
                .get::<SetBinaryPathFn>(b"seekdb_set_binary_path\0")
                .map_err(|error| error.to_string())?;
            let connection_options = *library
                .get::<ConnectionOptionsFn>(b"seekdb_connection_options\0")
                .map_err(|error| error.to_string())?;
            if set_binary_path(binary.as_ptr()) != 0 {
                return Err(format!(
                    "seekdb_set_binary_path rejected {}",
                    binary_path.display()
                ));
            }
            Ok(Self {
                _library: library,
                open,
                last_open_error,
                close,
                connection_options,
            })
        }
    }
}

/// Parameters passed to seekdb when it initializes a database directory.
#[derive(Debug, Default, Clone)]
pub struct OpenOptions {
    pub parameters: BTreeMap<String, String>,
}

/// Connection details for a caller-owned MySQL-protocol client.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectionOptions {
    pub transport: String,
    pub host: Option<String>,
    pub port: Option<u16>,
    pub unix_socket: Option<String>,
    pub named_pipe: Option<String>,
    pub user: String,
}

#[derive(Debug)]
pub enum Error {
    InvalidInput(String),
    Setup(String),
    Native { code: i32, message: String },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidInput(message) | Self::Setup(message) => formatter.write_str(message),
            Self::Native { code, message } => write!(formatter, "{message} (seekdb code {code})"),
        }
    }
}

impl std::error::Error for Error {}

/// An open local seekdb lifecycle handle. Close MySQL clients before closing it.
pub struct Seekdb {
    handle: Option<NonNull<c_void>>,
    native: &'static Native,
    directory: PathBuf,
}

impl Seekdb {
    /// Open or reuse the seekdb instance rooted at `directory`.
    pub fn open(directory: impl AsRef<Path>) -> Result<Self, Error> {
        Self::open_with_options(directory, OpenOptions::default())
    }

    /// Open with seekdb server parameters. Ordinary parameters apply on first initialization.
    pub fn open_with_options(
        directory: impl AsRef<Path>,
        options: OpenOptions,
    ) -> Result<Self, Error> {
        let directory = directory.as_ref();
        if directory.as_os_str().is_empty() {
            return Err(Error::InvalidInput(
                "database directory must not be empty".into(),
            ));
        }
        std::fs::create_dir_all(directory).map_err(|error| {
            Error::InvalidInput(format!("create {}: {error}", directory.display()))
        })?;
        let directory = directory.canonicalize().map_err(|error| {
            Error::InvalidInput(format!("resolve {}: {error}", directory.display()))
        })?;
        let c_directory = CString::new(directory.as_os_str().as_bytes())
            .map_err(|_| Error::InvalidInput("database directory contains NUL".into()))?;
        let mut strings = Vec::with_capacity(options.parameters.len() * 2);
        for (key, value) in &options.parameters {
            if key.is_empty() {
                return Err(Error::InvalidInput(
                    "parameter name must not be empty".into(),
                ));
            }
            strings.push(
                CString::new(key.as_bytes())
                    .map_err(|_| Error::InvalidInput("parameter name contains NUL".into()))?,
            );
            strings.push(
                CString::new(value.as_bytes())
                    .map_err(|_| Error::InvalidInput("parameter value contains NUL".into()))?,
            );
        }
        let mut pointers: Vec<*const c_char> = strings.iter().map(|value| value.as_ptr()).collect();
        pointers.push(std::ptr::null());
        let native = Native::get()?;
        let mut handle = std::ptr::null_mut();
        // SAFETY: All C strings and the pointer array live through the call.
        let code = unsafe { (native.open)(c_directory.as_ptr(), pointers.as_ptr(), &mut handle) };
        if code != 0 {
            // SAFETY: seekdb_last_open_error returns a thread-local borrowed string.
            // Copy it immediately before another open on this thread.
            let message = unsafe { optional_string((native.last_open_error)()) }
                .unwrap_or_else(|| "seekdb_open failed".into());
            if !handle.is_null() {
                unsafe { (native.close)(handle) };
            }
            return Err(Error::Native { code, message });
        }
        let handle = NonNull::new(handle)
            .ok_or_else(|| Error::Setup("seekdb_open returned a null handle".into()))?;
        Ok(Self {
            handle: Some(handle),
            native,
            directory,
        })
    }

    pub fn directory(&self) -> &Path {
        &self.directory
    }

    pub fn connection_options(&self) -> Result<ConnectionOptions, Error> {
        let handle = self
            .handle
            .expect("Seekdb handle is available until close consumes it");
        let mut raw = RawConnectionOptions::default();
        // SAFETY: The handle remains open. Returned strings are copied before
        // this method returns, while the handle is still alive.
        let code = unsafe { (self.native.connection_options)(handle.as_ptr(), &mut raw) };
        if code != 0 {
            return Err(Error::Native {
                code,
                message: "seekdb_connection_options failed".into(),
            });
        }
        // SAFETY: seekdb_connection_options returned strings borrowed from the
        // live handle, or null for fields unused by this transport.
        unsafe {
            let port = if raw.port == 0 {
                None
            } else {
                Some(u16::try_from(raw.port).map_err(|_| Error::Setup("port exceeds u16".into()))?)
            };
            Ok(ConnectionOptions {
                transport: optional_string(raw.transport).unwrap_or_default(),
                host: optional_string(raw.host),
                port,
                unix_socket: optional_string(raw.unix_socket),
                named_pipe: optional_string(raw.named_pipe),
                user: optional_string(raw.user).unwrap_or_default(),
            })
        }
    }

    /// Release the lifecycle handle without deleting database data.
    pub fn close(mut self) -> Result<(), Error> {
        let handle = self
            .handle
            .take()
            .expect("Seekdb handle is available until close consumes it");
        // SAFETY: This call consumes the only owned handle.
        let code = unsafe { (self.native.close)(handle.as_ptr()) };
        if code == 0 {
            Ok(())
        } else {
            Err(Error::Native {
                code,
                message: "seekdb_close failed".into(),
            })
        }
    }
}

impl Drop for Seekdb {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            // SAFETY: Drop consumes the still-owned handle. It cannot report errors.
            unsafe { (self.native.close)(handle.as_ptr()) };
        }
    }
}

unsafe fn optional_string(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        None
    } else {
        Some(CStr::from_ptr(pointer).to_string_lossy().into_owned())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_load_can_retry_after_missing_directory() {
        let temporary = tempfile::tempdir().unwrap();
        let database_dir = temporary.path().join("database");
        let previous = std::env::var_os("SEEKDB_NATIVE_DIR");

        std::env::set_var("SEEKDB_NATIVE_DIR", temporary.path().join("missing"));
        assert!(matches!(Seekdb::open(&database_dir), Err(Error::Setup(_))));

        std::env::set_var("SEEKDB_NATIVE_DIR", env!("SEEKDB_BUILD_NATIVE_DIR"));
        let database = Seekdb::open(&database_dir).unwrap();
        database.close().unwrap();

        if let Some(directory) = previous {
            std::env::set_var("SEEKDB_NATIVE_DIR", directory);
        } else {
            std::env::remove_var("SEEKDB_NATIVE_DIR");
        }
    }
}
