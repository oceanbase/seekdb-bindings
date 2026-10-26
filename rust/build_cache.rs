use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
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

fn lock_cache(directory: &Path) -> io::Result<File> {
    let mut builder = fs::DirBuilder::new();
    builder.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder.create(directory)?;
    let mut options = OpenOptions::new();
    options.read(true).write(true).create(true).truncate(false);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let lock = options.open(directory.join(".download.lock"))?;
    lock.lock_exclusive()?;
    Ok(lock)
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
    temporary
        .persist(destination)
        .map_err(|error| format!("publish {}: {error}", destination.display()))?;
    Ok(())
}

/// Share verified downloads while keeping include_bytes! inputs in Cargo's OUT_DIR.
pub(crate) fn prepare_archive(
    out_dir: &Path,
    shared_directory: Option<&Path>,
    name: &str,
    expected: &str,
    download: impl Fn(&Path, &Path) -> Result<(), String>,
) -> Result<PathBuf, String> {
    let package = out_dir.join(name);
    let local_valid = valid(&package, expected);
    if let Some(directory) = shared_directory {
        match lock_cache(directory) {
            Ok(_lock) => {
                let shared = directory.join(name);
                if !valid(&shared, expected) {
                    if local_valid {
                        // A build predating the shared cache can seed it offline.
                        if let Err(error) = copy_verified(&package, &shared, expected) {
                            println!("cargo:warning=cannot populate shared seekdb download cache: {error}; using OUT_DIR");
                        }
                        return Ok(package);
                    }
                    download(directory, &shared)?;
                    if !valid(&shared, expected) {
                        return Err(format!("SHA-256 mismatch for {}", shared.display()));
                    }
                }
                if !local_valid {
                    copy_verified(&shared, &package, expected)?;
                }
                // The file handle releases the lock on return. OUT_DIR gets a
                // copy, so later cache cleanup cannot remove a compiler input.
                return Ok(package);
            }
            Err(error) => println!(
                "cargo:warning=cannot use shared seekdb download cache {}: {error}; using OUT_DIR",
                directory.display()
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
