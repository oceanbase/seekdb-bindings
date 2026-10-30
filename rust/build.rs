mod build_cache;
mod bundle_manifest;
mod manifest_cache;
mod native_cache;

use build_cache::{prepare_archive, sha256};
use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;
use tempfile::NamedTempFile;

const DOWNLOAD_RETRIES: u32 = 3;
use bundle_manifest::{Asset, Config, DownloadBase, MAX_MANIFEST_BYTES};

fn agent() -> ureq::Agent {
    // The default agent honors proxy environment variables, including NO_PROXY.
    ureq::Agent::config_builder()
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
        .new_agent()
}

fn download(asset: &Asset, out_dir: &Path, package: &Path) -> Result<(), String> {
    let agent = agent();
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
                native_cache::publish_file(temporary, package)
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

fn download_manifest(url: &str) -> Result<Vec<u8>, String> {
    let agent = agent();
    for attempt in 0..=DOWNLOAD_RETRIES {
        let response = agent.get(url).call();
        let retryable = !matches!(&response,
            Err(ureq::Error::StatusCode(code)) if *code != 408 && *code != 429 && *code < 500);
        let result = response
            .map_err(|e| format!("download {url}: {e}"))
            .and_then(|mut response| {
                let mut bytes = Vec::new();
                response
                    .body_mut()
                    .as_reader()
                    .take(MAX_MANIFEST_BYTES + 1)
                    .read_to_end(&mut bytes)
                    .map_err(|e| format!("read {url}: {e}"))?;
                Ok(bytes)
            });
        match result {
            Ok(bytes) => return Ok(bytes),
            Err(error) if retryable && attempt < DOWNLOAD_RETRIES => {
                println!(
                    "cargo:warning={error}; retrying ({}/{DOWNLOAD_RETRIES})",
                    attempt + 1
                );
                thread::sleep(Duration::from_secs(1 << attempt));
            }
            Err(error) => return Err(error),
        }
    }
    unreachable!()
}

fn prepare_bundle(out_dir: &Path, target: &str) -> Result<(), String> {
    // Reject unsupported ABIs before requesting any remote metadata.
    bundle_manifest::target_platform(target)?;
    let config = Config::from_package(bundle_manifest::PACKAGE_TOML)?;
    let bundled =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("CARGO_MANIFEST_DIR is missing")?)
            .join("bundles")
            .join(config.manifest_name());
    println!("cargo:rerun-if-changed={}", bundled.display());
    if env::var_os("DOCS_RS").is_some() {
        // docs.rs disables network access. Rustdoc only needs the public API,
        // not a runnable native payload. Still verify the packaged release pin.
        let manifest =
            fs::read(&bundled).map_err(|error| format!("read documentation manifest: {error}"))?;
        let asset = config.asset(&manifest, target, &DownloadBase::new(None)?)?;
        return write_bundle_metadata(out_dir, target, &asset, "&[]");
    }
    let prefix = env::var("SEEKDB_DOWNLOAD_BASE_URL")
        .map(Some)
        .or_else(|e| match e {
            env::VarError::NotPresent => Ok(None),
            env::VarError::NotUnicode(_) => {
                Err("SEEKDB_DOWNLOAD_BASE_URL must be UTF-8".to_string())
            }
        })?;
    let base = DownloadBase::new(prefix.as_deref())?;
    let root = env::var_os("SEEKDB_CACHE_DIR")
        .map(PathBuf::from)
        .or_else(native_cache::default_root);
    if root.as_ref().is_some_and(|path| !path.is_absolute()) {
        return Err("SEEKDB_CACHE_DIR must be a non-empty absolute path".into());
    }
    let manifest =
        manifest_cache::prepare_manifest(out_dir, root.as_deref(), &config, &bundled, || {
            download_manifest(&base.file_url(&config.manifest_name()))
        })?;
    let asset = config.asset(&manifest, target, &base)?;
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
    let archive = format!(
        "include_bytes!(concat!(env!(\"OUT_DIR\"), \"/{}\"))",
        asset.name
    );
    write_bundle_metadata(out_dir, target, &asset, &archive)
}

fn write_bundle_metadata(
    out_dir: &Path,
    target: &str,
    asset: &Asset,
    archive: &str,
) -> Result<(), String> {
    let source = format!(
        "pub const ARCHIVE: &[u8] = {archive};\n\
         pub const TARGET: &str = {target:?};\n\
         pub const VERSION: &str = {version:?};\n\
         pub const SHA256: &str = {hash:?};\n\
         pub const LIBRARY: &str = {library:?};\n",
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
    println!("cargo:rerun-if-changed=manifest_cache.rs");
    println!("cargo:rerun-if-changed=Cargo.toml");
    println!("cargo:rerun-if-changed=build_cache.rs");
    println!("cargo:rerun-if-changed=native_cache.rs");
    for variable in [
        "DOCS_RS",
        "SEEKDB_CACHE_DIR",
        "SEEKDB_DOWNLOAD_BASE_URL",
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
