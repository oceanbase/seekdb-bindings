mod build_cache;

use build_cache::{prepare_archive, sha256};
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;
use tempfile::NamedTempFile;

const DOWNLOAD_RETRIES: u32 = 3;
const NATIVE_VERSION: &str = "1.4.0.0-r1";

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

fn prepare_bundle(out_dir: &Path, target: &str) -> Result<(), String> {
    let asset = asset(target).ok_or_else(|| format!("unsupported seekdb target {target}"))?;
    if asset.url.is_empty() {
        return Err(format!(
            "native bundle URL is not configured for {target}; automatic builds currently require Linux x86_64 with glibc 2.28 or newer"
        ));
    }
    let shared = dirs::cache_dir().map(|root| {
        let root = root.join("seekdb");
        let root = if cfg!(windows) {
            root.join("cache")
        } else {
            root
        };
        root.join("downloads")
            .join(target)
            .join(NATIVE_VERSION)
            .join(asset.sha256)
    });
    prepare_archive(
        out_dir,
        shared.as_deref(),
        asset.name,
        asset.sha256,
        |directory, package| download(&asset, directory, package),
    )?;
    let source = format!(
        "pub const ARCHIVE: &[u8] = include_bytes!(concat!(env!(\"OUT_DIR\"), \"/{name}\"));\n\
         pub const TARGET: &str = {target:?};\n\
         pub const VERSION: &str = {version:?};\n\
         pub const SHA256: &str = {hash:?};\n\
         pub const LIBRARY: &str = {library:?};\n",
        name = asset.name,
        version = NATIVE_VERSION,
        hash = asset.sha256,
        library = asset.library,
    );
    fs::write(out_dir.join("native_bundle.rs"), source)
        .map_err(|error| format!("generate embedded bundle metadata: {error}"))
}

fn main() {
    if let Err(error) = build() {
        panic!("seekdb Rust binding build failed: {error}");
    }
}

fn build() -> Result<(), String> {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=build_cache.rs");
    for variable in ["XDG_CACHE_HOME", "HOME", "LOCALAPPDATA", "USERPROFILE"] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    let target = env::var("TARGET").map_err(|error| error.to_string())?;
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is missing")?);
    prepare_bundle(&out_dir, &target)
}
