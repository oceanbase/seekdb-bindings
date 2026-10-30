use crate::native_cache::{publish_file, NativeCache};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use tempfile::NamedTempFile;

pub(crate) fn sha256(path: &Path) -> io::Result<String> {
    let mut file = File::open(path)?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn valid(path: &Path, expected: &str) -> bool {
    sha256(path).is_ok_and(|actual| actual == expected)
}

fn copy_verified(source: &Path, destination: &Path, expected: &str) -> Result<(), String> {
    let temporary = NamedTempFile::new_in(destination.parent().unwrap()).map_err(|error| {
        format!(
            "create temporary copy of {}: {error}",
            destination.display()
        )
    })?;
    fs::copy(source, temporary.path()).map_err(|error| {
        format!(
            "copy {} to {}: {error}",
            source.display(),
            destination.display()
        )
    })?;
    if !valid(temporary.path(), expected) {
        return Err(format!(
            "SHA-256 mismatch while copying {}",
            source.display()
        ));
    }
    publish_file(temporary, destination)
        .map_err(|error| format!("publish {}: {error}", destination.display()))?;
    Ok(())
}

/// Share verified downloads while keeping include_bytes! inputs in Cargo's OUT_DIR.
pub(crate) fn prepare_archive(
    out_dir: &Path,
    shared_cache: Option<&NativeCache<'_>>,
    name: &str,
    expected: &str,
    download: impl Fn(&Path, &Path) -> Result<(), String>,
) -> Result<PathBuf, String> {
    let package = out_dir.join(name);
    let local_valid = valid(&package, expected);
    if let Some(cache) = shared_cache {
        match cache.prepare_directory() {
            Ok(()) => {
                let shared = cache.archive_path();
                if !local_valid {
                    if valid(&shared, expected) {
                        copy_verified(&shared, &package, expected)?;
                    } else {
                        download(out_dir, &package)?;
                        if !valid(&package, expected) {
                            return Err(format!("SHA-256 mismatch for {}", package.display()));
                        }
                    }
                }
                let bytes = fs::read(&package)
                    .map_err(|error| format!("read {}: {error}", package.display()))?;
                match cache.ensure(&bytes) {
                    Ok(_) => {
                        if !valid(&shared, expected) {
                            if let Err(error) = cache.store_archive(&cache.entry(), &bytes) {
                                println!("cargo:warning=cannot cache seekdb archive: {error}; using OUT_DIR");
                            }
                        }
                    }
                    Err(error) => println!("cargo:warning=cannot prepare shared seekdb native cache: {error}; using OUT_DIR"),
                }
                // Build and runtime publish independently and reuse the winning
                // complete entry. Cargo embeds an independent OUT_DIR snapshot.
                return Ok(package);
            }
            Err(error) => println!(
                "cargo:warning=cannot use shared seekdb native cache: {error}; using OUT_DIR"
            ),
        }
    }
    if !local_valid {
        download(out_dir, &package)?;
        if !valid(&package, expected) {
            return Err(format!("SHA-256 mismatch for {}", package.display()));
        }
    }
    Ok(package)
}
