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

// Validate the Windows path spelling independently of the host OS, so the
// drive/UNC/device-path rules can also be covered by non-Windows unit tests.
#[cfg(any(windows, test))]
fn windows_drive_path(text: &str) -> Result<&str, Error> {
    let text = text.strip_prefix(r"\\?\").unwrap_or(text);
    let bytes = text.as_bytes();
    if bytes.len() < 3
        || !bytes[0].is_ascii_alphabetic()
        || bytes[1] != b':'
        || !matches!(bytes[2], b'\\' | b'/')
    {
        return Err(Error::InvalidInput(
            "the r2 Windows driver requires an absolute local drive path".into(),
        ));
    }
    Ok(text)
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
    let text = windows_drive_path(text)?;
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
    #[test]
    fn windows_path_validation_accepts_only_drive_rooted_local_paths() {
        for (input, expected) in [
            (r"C:\data with spaces\db", r"C:\data with spaces\db"),
            ("c:/data/db", "c:/data/db"),
            (r"\\?\C:\data\db", r"C:\data\db"),
        ] {
            assert_eq!(windows_drive_path(input).unwrap(), expected);
        }
        for input in [
            "",
            "relative",
            r"C:relative",
            r"\root-relative",
            "9:/invalid-drive",
            r"\\server\share\db",
            r"\\?\UNC\server\share\db",
            r"\\?\unc\server\share\db",
            r"\\?\UnC\server\share\db",
            r"\\?\Volume{12345678-1234-1234-1234-123456789abc}\data",
            r"\\?\GLOBALROOT\Device\HarddiskVolume1\data",
            r"\\.\C:\data",
        ] {
            assert!(windows_drive_path(input).is_err(), "{input}");
        }
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
