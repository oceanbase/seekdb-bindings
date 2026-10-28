//! Encode paths for the pinned C interface, without silently replacing characters.
use crate::Error;
use std::ffi::CString;
use std::path::Path;

#[cfg(unix)]
pub(crate) fn c_path(path: &Path) -> Result<CString, Error> {
    use std::os::unix::ffi::OsStrExt;
    CString::new(path.as_os_str().as_bytes())
        .map_err(|_| Error::InvalidInput("native path contains NUL".into()))
}

#[cfg(windows)]
pub(crate) fn c_path(path: &Path) -> Result<CString, Error> {
    use std::os::windows::ffi::OsStrExt;
    // r2's C driver calls the Windows A APIs. Its strings use the process ANSI
    // code page, not necessarily UTF-8. Reject a lossy conversion explicitly.
    #[link(name = "kernel32")]
    extern "system" {
        fn GetACP() -> u32;
        fn WideCharToMultiByte(
            code_page: u32,
            flags: u32,
            wide: *const u16,
            count: i32,
            bytes: *mut u8,
            capacity: i32,
            default_char: *const u8,
            used_default: *mut i32,
        ) -> i32;
    }
    let text = path
        .to_str()
        .ok_or_else(|| Error::InvalidInput("native path contains invalid Unicode".into()))?;
    let text = text.strip_prefix(r"\\?\").unwrap_or(text);
    if text.starts_with("UNC\\") || text.starts_with(r"\\") {
        return Err(Error::InvalidInput(
            "the r2 Windows driver requires a local drive path".into(),
        ));
    }
    let wide: Vec<u16> = std::ffi::OsStr::new(text).encode_wide().collect();
    if wide.contains(&0) || wide.len() >= 240 {
        return Err(Error::InvalidInput(
            "the r2 Windows driver requires a path shorter than 240 UTF-16 units without NUL"
                .into(),
        ));
    }
    let mut used_default = 0;
    let mut bytes = vec![0u8; wide.len() * 4 + 1];
    // SAFETY: Both buffers are valid for their supplied lengths. UTF-8 ACP
    // forbids lpUsedDefaultChar and WC_NO_BEST_FIT_CHARS; other ACPs use them
    // to detect characters that the native A APIs cannot represent.
    let count = unsafe {
        let utf8 = GetACP() == 65001;
        WideCharToMultiByte(
            0,
            if utf8 { 0x80 } else { 0x400 },
            wide.as_ptr(),
            wide.len() as i32,
            bytes.as_mut_ptr(),
            bytes.len() as i32,
            std::ptr::null(),
            if utf8 {
                std::ptr::null_mut()
            } else {
                &mut used_default
            },
        )
    };
    if count == 0 || used_default != 0 {
        return Err(Error::InvalidInput("native path cannot be represented by the Windows ANSI code page used by the r2 driver; choose another path or enable the system UTF-8 code page".into()));
    }
    bytes.truncate(count as usize);
    CString::new(bytes).map_err(|_| Error::InvalidInput("native path contains NUL".into()))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_nul_in_native_path() {
        assert!(c_path(Path::new("bad\0path")).is_err());
    }
    #[cfg(windows)]
    #[test]
    fn windows_normalizes_verbatim_paths_and_checks_native_limits() {
        assert_eq!(
            c_path(Path::new(r"\\?\C:\data with spaces\db"))
                .unwrap()
                .to_bytes(),
            br"C:\data with spaces\db"
        );
        assert!(c_path(Path::new(&format!("C:\\{}", "a".repeat(240)))).is_err());
        assert!(c_path(Path::new(r"\\server\share\db")).is_err());
    }
}
