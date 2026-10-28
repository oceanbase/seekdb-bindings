//! Build-time metadata cache. Runtime code never downloads a manifest.
use crate::bundle_manifest::Config;
use crate::native_cache::private_directory;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

pub fn cache_path(root: &Path, config: &Config) -> PathBuf {
    root.join("manifests")
        .join(&config.native_version)
        .join(&config.manifest_sha256)
        .join("manifest.json")
}

fn verified(path: &Path, config: &Config) -> Option<Vec<u8>> {
    // Bound reads even for a damaged or manually modified metadata cache.
    use std::io::Read;
    let mut bytes = Vec::new();
    fs::File::open(path)
        .ok()?
        .take(crate::bundle_manifest::MAX_MANIFEST_BYTES + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    config.verify_manifest(&bytes).ok()?;
    Some(bytes)
}

fn publish(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path.parent().unwrap();
    let mut temporary = tempfile::NamedTempFile::new_in(parent)
        .map_err(|e| format!("stage native manifest: {e}"))?;
    temporary
        .write_all(bytes)
        .map_err(|e| format!("write native manifest: {e}"))?;
    temporary
        .persist(path)
        .map_err(|e| format!("publish native manifest at {}: {e}", path.display()))?;
    Ok(())
}

/// Prefer verified local copies, otherwise fetch the exact pinned revision.
/// Transport failures can use the packaged snapshot. A downloaded manifest
/// with a wrong digest/schema/identity is an error, never a silent fallback.
pub fn prepare_manifest(
    out_dir: &Path,
    root: Option<&Path>,
    config: &Config,
    bundled: &Path,
    fetch: impl FnOnce() -> Result<Vec<u8>, String>,
) -> Result<Vec<u8>, String> {
    let local = out_dir.join(config.manifest_name());
    let shared = root.map(|root| cache_path(root, config));
    let bytes = if let Some(bytes) =
        verified(&local, config).or_else(|| shared.as_ref().and_then(|p| verified(p, config)))
    {
        bytes
    } else {
        match fetch() {
            Ok(bytes) => {
                config.verify_manifest(&bytes)?;
                bytes
            }
            Err(error) => {
                let bytes = verified(bundled, config).ok_or_else(|| {
                    format!(
                        "{error}; no verified packaged manifest at {}",
                        bundled.display()
                    )
                })?;
                println!("cargo:warning={error}; using the verified packaged native manifest");
                bytes
            }
        }
    };
    // Keep a verified build-local copy independently of user-cache cleanup.
    if verified(&local, config).is_none() {
        publish(&local, &bytes)?;
    }
    if let (Some(root), Some(shared)) = (root, &shared) {
        let result = private_directory(root)
            .map_err(|e| e.to_string())
            .and_then(|()| private_directory(shared.parent().unwrap()).map_err(|e| e.to_string()))
            .and_then(|()| {
                if verified(shared, config).is_none() {
                    publish(shared, &bytes)?;
                }
                Ok(())
            });
        if let Err(error) = result {
            println!("cargo:warning=cannot cache native manifest: {error}; using OUT_DIR");
        }
    }
    Ok(bytes)
}
