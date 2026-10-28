mod build_cache;
mod bundle_manifest;
mod native_cache;

use build_cache::{prepare_archive, sha256};
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;
use tempfile::NamedTempFile;

const DOWNLOAD_RETRIES: u32 = 3;
use bundle_manifest::{asset, Asset};

fn download(asset: &Asset, out_dir: &Path, package: &Path) -> Result<(), String> {
    // The default agent honors proxy environment variables, including NO_PROXY.
    let agent = ureq::Agent::config_builder()
        .https_only(true)
        .tls_config(
            ureq::tls::TlsConfig::builder()
                .root_certs(ureq::tls::RootCerts::PlatformVerifier)
                .build(),
        )
        .max_redirects(10)
        .timeout_connect(Some(Duration::from_secs(20)))
        .timeout_global(Some(Duration::from_secs(300)))
        .build()
        .new_agent();

    for attempt in 0..=DOWNLOAD_RETRIES {
        let mut temporary = NamedTempFile::new_in(out_dir)
            .map_err(|error| format!("create temporary download: {error}"))?;
        let result = agent.get(&asset.url).call().map_err(|error| {
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
            if temporary
                .as_file()
                .metadata()
                .map_err(|e| (e.to_string(), false))?
                .len()
                != asset.size_bytes
            {
                return Err(("native archive size mismatch".into(), true));
            }
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
    let asset = asset(target)?;
    let root = env::var_os("SEEKDB_CACHE_DIR")
        .map(PathBuf::from)
        .or_else(native_cache::default_root);
    if root.as_ref().is_some_and(|path| !path.is_absolute()) {
        return Err("SEEKDB_CACHE_DIR must be a non-empty absolute path".into());
    }
    let cache = root.map(|root| native_cache::NativeCache {
        root,
        target,
        version: &asset.version,
        hash: &asset.sha256,
        library: asset.library,
    });
    prepare_archive(
        out_dir,
        cache.as_ref(),
        &asset.name,
        &asset.sha256,
        |directory, package| download(&asset, directory, package),
    )?;
    let source = format!(
        "pub const ARCHIVE: &[u8] = include_bytes!(concat!(env!(\"OUT_DIR\"), \"/{name}\"));\n\
         pub const TARGET: &str = {target:?};\n\
         pub const VERSION: &str = {version:?};\n\
         pub const SHA256: &str = {hash:?};\n\
         pub const LIBRARY: &str = {library:?};\n",
        name = asset.name,
        version = asset.version,
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
    println!("cargo:rerun-if-changed=bundle_manifest.rs");
    println!("cargo:rerun-if-changed=bundles/manifest-1.4.0.0-r2.json");
    println!("cargo:rerun-if-changed=build_cache.rs");
    println!("cargo:rerun-if-changed=native_cache.rs");
    for variable in [
        "SEEKDB_CACHE_DIR",
        "XDG_CACHE_HOME",
        "HOME",
        "LOCALAPPDATA",
        "USERPROFILE",
    ] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    let target = env::var("TARGET").map_err(|error| error.to_string())?;
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is missing")?);
    prepare_bundle(&out_dir, &target)
}
