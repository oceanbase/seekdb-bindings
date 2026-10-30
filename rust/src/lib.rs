//! Start a local seekdb instance and obtain its MySQL-protocol endpoint.
//! SQL connections are managed by the caller's chosen MySQL client.
//!
//! The native runtime is downloaded and verified at build time, embedded in the
//! application, and restored from its bytes at runtime without network access.
//! See the [usage and platform requirements](https://github.com/oceanbase/seekdb-bindings/tree/main/rust)
//! and the [release guide](https://github.com/oceanbase/seekdb-bindings/blob/main/rust/docs/releasing.md).

#[path = "../native_cache.rs"]
mod native_cache;
mod native_path;
mod runtime;

use libloading::Library;
use std::collections::BTreeMap;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr, CString};
use std::fmt;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::{Mutex, OnceLock};

type Handle = *mut c_void;
type OpenFn = unsafe extern "C" fn(*const c_char, *const *const c_char, *mut Handle) -> c_int;
type LastOpenErrorFn = unsafe extern "C" fn() -> *const c_char;
type CloseFn = unsafe extern "C" fn(Handle) -> c_int;
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
    bundle: runtime::Bundle<'static>,
    open: OpenFn,
    last_open_error: LastOpenErrorFn,
    close: CloseFn,
    connection_options: ConnectionOptionsFn,
    #[cfg(windows)]
    set_binary_path: unsafe extern "C" fn(*const c_char) -> c_int,
}

static NATIVE: OnceLock<Native> = OnceLock::new();
static NATIVE_INIT: Mutex<Option<PathBuf>> = Mutex::new(None);

/// Override the extraction root for this copy of the seekdb dependency.
///
/// Call before the first successful native initialization. The absolute path
/// takes precedence over SEEKDB_CACHE_DIR and the platform's default cache.
/// The directory must be user-owned, writable, and permit native execution.
pub fn set_runtime_cache_dir(directory: impl AsRef<Path>) -> Result<(), Error> {
    let directory = runtime::absolute_root(directory.as_ref())?;
    let mut configured = NATIVE_INIT
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    if NATIVE.get().is_some() {
        return Err(Error::InvalidInput(
            "seekdb runtime is already initialized; configure its cache directory before the first Seekdb::open".into(),
        ));
    }
    *configured = Some(directory);
    Ok(())
}

impl Native {
    fn get() -> Result<&'static Self, Error> {
        let configured = NATIVE_INIT
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(native) = NATIVE.get() {
            // Cached function pointers survive cache cleanup. The server and
            // dependencies must still be restored before another open.
            let path = native.bundle.ensure()?;
            #[cfg(windows)]
            native.configure_server(&path)?;
            #[cfg(not(windows))]
            let _ = path;
            return Ok(native);
        }
        let bundle = runtime::Bundle::embedded(configured.as_deref())?;
        let library_path = bundle.ensure()?;
        let native = Self::load(bundle, &library_path).map_err(Error::Setup)?;
        #[cfg(windows)]
        native.configure_server(&library_path)?;
        let _ = NATIVE.set(native);
        Ok(NATIVE.get().expect("native library was initialized"))
    }

    #[cfg(windows)]
    fn configure_server(&self, library_path: &Path) -> Result<(), Error> {
        // The old DLL can stay mapped while a repaired generation supplies a
        // new server. All its dependencies are colocated with this executable.
        let executable = native_path::c_path(&library_path.with_file_name("seekdb.exe"))?;
        // SAFETY: the C API copies this string and serializes its configuration.
        let code = unsafe { (self.set_binary_path)(executable.as_ptr()) };
        if code == 0 {
            Ok(())
        } else {
            Err(Error::Native {
                code,
                message: "configure bundled Windows server".into(),
            })
        }
    }

    fn load(bundle: runtime::Bundle<'static>, library_path: &Path) -> Result<Self, String> {
        // SAFETY: The library remains owned by Native for as long as the copied
        // function pointers can be called. Function signatures match seekdb.h.
        unsafe {
            #[cfg(not(windows))]
            let loaded = Library::new(library_path);
            #[cfg(windows)]
            let loaded = libloading::os::windows::Library::load_with_flags(
                library_path,
                libloading::os::windows::LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                    | libloading::os::windows::LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            )
            .map(Library::from);
            let library = loaded.map_err(|error| {
                format!(
                    "load {}: {error}; verify cache permissions and execution policy, or select another cache with SEEKDB_CACHE_DIR or set_runtime_cache_dir",
                    library_path.display()
                )
            })?;
            let open = *library
                .get::<OpenFn>(b"seekdb_open\0")
                .map_err(|error| error.to_string())?;
            let last_open_error = *library
                .get::<LastOpenErrorFn>(b"seekdb_last_open_error\0")
                .map_err(|error| error.to_string())?;
            let close = *library
                .get::<CloseFn>(b"seekdb_close\0")
                .map_err(|error| error.to_string())?;
            let connection_options = *library
                .get::<ConnectionOptionsFn>(b"seekdb_connection_options\0")
                .map_err(|error| error.to_string())?;
            #[cfg(windows)]
            let set_binary_path = *library
                .get::<unsafe extern "C" fn(*const c_char) -> c_int>(b"seekdb_set_binary_path\0")
                .map_err(|error| error.to_string())?;
            // libseekdb locates its server executable beside the loaded library.
            Ok(Self {
                _library: library,
                bundle,
                open,
                last_open_error,
                close,
                connection_options,
                #[cfg(windows)]
                set_binary_path,
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
    RuntimeCache {
        path: PathBuf,
        operation: &'static str,
        source: std::io::Error,
    },
    RuntimeCacheNoExec {
        path: PathBuf,
    },
    Native {
        code: i32,
        message: String,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidInput(message) | Self::Setup(message) => formatter.write_str(message),
            Self::RuntimeCache { path, operation, source } => write!(formatter,
                "seekdb runtime cache: {operation} at {}: {source}; configure another directory with SEEKDB_CACHE_DIR or set_runtime_cache_dir", path.display()),
            Self::RuntimeCacheNoExec { path } => write!(formatter,
                "seekdb runtime cache {} is on a filesystem mounted with noexec; configure a directory that permits native library loading and server execution with SEEKDB_CACHE_DIR or set_runtime_cache_dir", path.display()),
            Self::Native { code, message } => write!(formatter, "{message} (seekdb code {code})"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::RuntimeCache { source, .. } => Some(source),
            _ => None,
        }
    }
}

/// An open local seekdb lifecycle handle. Close MySQL clients before closing it.
///
/// Opening waits synchronously for server readiness. This handle is neither
/// Send nor Sync: keep it on its owning thread. Async applications can open,
/// use, and close it within a blocking task while SQL runs on their async
/// runtime; see examples/async_query.rs.
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
        let c_directory = native_path::c_path(&directory)?;
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
