/// Open and close a database through a transitive seekdb dependency.
///
/// # Safety
/// `directory` must point to a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn seekdb_consumer_probe(directory: *const std::ffi::c_char) -> i32 {
    if directory.is_null() {
        return -1;
    }
    let Ok(directory) = std::ffi::CStr::from_ptr(directory).to_str() else {
        return -1;
    };
    match seekdb_consumer_b::open(std::path::Path::new(directory))
        .and_then(|database| database.close())
    {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("{error}");
            -1
        }
    }
}
