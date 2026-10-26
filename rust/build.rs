use flate2::read::GzDecoder;
use sha2::{Digest, Sha256};
use std::env;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;
use tempfile::{Builder, NamedTempFile};

const DOWNLOAD_RETRIES: u32 = 3;

struct Asset {
    url: &'static str,
    name: &'static str,
    sha256: &'static str,
    library: &'static str,
}

fn asset(target: &str) -> Option<Asset> {
    match target {
        "aarch64-apple-darwin" => Some(Asset {
            url: "",
            name: "libseekdb-1.4.0.0-r1-macos15.6-arm64-sdb596e4a5c9809-bndcdb303fbbd5e.tar.gz",
            sha256: "347db30614ff0e90f35277aeaa1f7091f6bf7223ed7c3c717d68d6b5f0731a8a",
            library: "libseekdb.dylib",
        }),
        "x86_64-unknown-linux-gnu" => Some(Asset {
            url: "https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/libseekdb-1.4.0.0-r1-linux-x86_64-glibc2.28-sdb596e4a5c9809-bndcdb303fbbd5e.tar.gz",
            name: "libseekdb-1.4.0.0-r1-linux-x86_64-glibc2.28-sdb596e4a5c9809-bndcdb303fbbd5e.tar.gz",
            sha256: "90f0d3e39fa53a23331fd432fa7f6c1bfe2016cb784f659ff6cb3252890f17cd",
            library: "libseekdb.so",
        }),
        "aarch64-unknown-linux-gnu" => Some(Asset {
            url: "",
            name: "libseekdb-1.4.0.0-r1-linux-aarch64-glibc2.28-sdb596e4a5c9809-bndcdb303fbbd5e.tar.gz",
            sha256: "7860c9f7a8ce381d076bb27cd92b1c892587232733842bf57ff81375adf5a883",
            library: "libseekdb.so",
        }),
        _ => None,
    }
}

fn sha256(path: &Path) -> io::Result<String> {
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

fn download(asset: &Asset, out_dir: &Path, package: &Path) -> Result<(), String> {
    // The default agent honors proxy environment variables, including NO_PROXY.
    let agent = ureq::Agent::config_builder()
        .https_only(true)
        .max_redirects(10)
        .timeout_connect(Some(Duration::from_secs(20)))
        .timeout_global(Some(Duration::from_secs(300)))
        .build()
        .new_agent();

    for attempt in 0..=DOWNLOAD_RETRIES {
        let mut temporary = NamedTempFile::new_in(out_dir)
            .map_err(|error| format!("create temporary download: {error}"))?;
        let result = agent.get(asset.url).call().map_err(|error| {
            let retryable = match error {
                ureq::Error::StatusCode(code) => code == 408 || code == 429 || code >= 500,
                ureq::Error::RequireHttpsOnly(_) => false,
                _ => true,
            };
            (format!("download {}: {error}", asset.url), retryable)
        });
        let result = result.and_then(|mut response| {
            io::copy(
                &mut response.body_mut().as_reader(),
                temporary.as_file_mut(),
            )
            .map_err(|error| (format!("read {}: {error}", asset.url), true))?;
            let actual = sha256(temporary.path())
                .map_err(|error| (format!("hash temporary download: {error}"), false))?;
            if actual != asset.sha256 {
                return Err((
                    format!(
                        "SHA-256 mismatch for {}: expected {}, got {actual}",
                        asset.url, asset.sha256
                    ),
                    true,
                ));
            }
            Ok(())
        });
        match result {
            Ok(()) => {
                temporary
                    .persist(package)
                    .map_err(|error| format!("cache {}: {error}", package.display()))?;
                return Ok(());
            }
            Err((message, retryable)) if retryable && attempt < DOWNLOAD_RETRIES => {
                println!(
                    "cargo:warning={message}; retrying ({}/{DOWNLOAD_RETRIES})",
                    attempt + 1
                );
                thread::sleep(Duration::from_secs(1 << attempt));
            }
            Err((message, _)) => return Err(message),
        }
    }
    unreachable!()
}

fn validate_native_dir(directory: &Path, library: &str) -> Result<PathBuf, String> {
    let directory = fs::canonicalize(directory)
        .map_err(|error| format!("resolve {}: {error}", directory.display()))?;
    for name in [library, "seekdb"] {
        if !directory.join(name).is_file() {
            return Err(format!("{} is missing", directory.join(name).display()));
        }
    }
    Ok(directory)
}

fn find_native_dir(root: &Path, library: &str) -> Result<PathBuf, String> {
    let mut matches = Vec::new();
    if let Ok(directory) = validate_native_dir(&root.join("lib"), library) {
        matches.push(directory);
    }
    for entry in fs::read_dir(root).map_err(|error| format!("read {}: {error}", root.display()))? {
        let entry = entry.map_err(|error| error.to_string())?;
        if entry
            .file_type()
            .map_err(|error| error.to_string())?
            .is_dir()
        {
            if let Ok(directory) = validate_native_dir(&entry.path().join("lib"), library) {
                matches.push(directory);
            }
        }
    }
    if matches.len() == 1 {
        Ok(matches.remove(0))
    } else {
        Err(format!(
            "expected one lib/ directory containing {library} and seekdb, found {}",
            matches.len()
        ))
    }
}

fn get_native_dir(out_dir: &Path, target: &str) -> Result<PathBuf, String> {
    let asset = asset(target).ok_or_else(|| format!("unsupported seekdb target {target}"))?;
    if asset.url.is_empty() {
        return Err(format!(
            "native bundle URL is not configured for {target}; automatic builds currently require Linux x86_64 with glibc 2.28 or newer"
        ));
    }

    let package = out_dir.join(asset.name);
    if !package.exists() || sha256(&package).map_err(|error| error.to_string())? != asset.sha256 {
        download(&asset, out_dir, &package)?;
    }

    let expanded = out_dir.join(format!("native-{}", &asset.sha256[..12]));
    let marker = expanded.join(".complete");
    if expanded.exists() {
        if marker.is_file() {
            if let Ok(directory) = find_native_dir(&expanded, asset.library) {
                return Ok(directory);
            }
        }
        fs::remove_dir_all(&expanded).map_err(|error| error.to_string())?;
    }
    let staging = Builder::new()
        .prefix("native-extract-")
        .tempdir_in(out_dir)
        .map_err(|error| format!("create temporary extraction directory: {error}"))?;
    let archive = GzDecoder::new(File::open(&package).map_err(|error| error.to_string())?);
    tar::Archive::new(archive)
        .unpack(staging.path())
        .map_err(|error| format!("extract {}: {error}", package.display()))?;
    find_native_dir(staging.path(), asset.library)?;
    fs::write(staging.path().join(".complete"), b"complete\n")
        .map_err(|error| error.to_string())?;
    fs::rename(staging.path(), &expanded).map_err(|error| error.to_string())?;
    find_native_dir(&expanded, asset.library)
}

fn main() {
    if let Err(error) = build() {
        panic!("seekdb Rust binding build failed: {error}");
    }
}

fn build() -> Result<(), String> {
    println!("cargo:rerun-if-changed=build.rs");
    let target = env::var("TARGET").map_err(|error| error.to_string())?;
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is missing")?);
    let native_dir = get_native_dir(&out_dir, &target)?;
    // Cargo adds native search paths inside its target directory to the loader
    // path for `cargo run` and `cargo test`, including a custom target directory.
    // Keep build artifacts in OUT_DIR; deployed apps carry their own lib/.
    println!("cargo:rustc-link-search=native={}", native_dir.display());
    Ok(())
}
