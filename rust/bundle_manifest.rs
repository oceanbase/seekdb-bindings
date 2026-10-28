//! The checked-in release manifest is the source of archive identities.
use serde::Deserialize;

pub const MANIFEST: &str = include_str!("bundles/manifest-1.4.0.0-r2.json");
const BASE_URL: &str =
    "https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/";

#[derive(Deserialize)]
struct Manifest {
    schema_version: String,
    package: String,
    package_version: String,
    artifacts: Vec<Artifact>,
}
#[derive(Deserialize)]
struct Artifact {
    os: String,
    architecture: String,
    runtime: Runtime,
}
#[derive(Deserialize)]
struct Runtime {
    file: String,
    sha256: String,
    size_bytes: u64,
}

pub struct Asset {
    pub url: String,
    pub name: String,
    pub sha256: String,
    pub version: String,
    pub size_bytes: u64,
    pub library: &'static str,
}

pub fn asset(target: &str) -> Result<Asset, String> {
    select(MANIFEST, target)
}

fn select(json: &str, target: &str) -> Result<Asset, String> {
    // ABI and architecture mapping only: filenames and hashes belong to the manifest.
    let (os, architecture, library) = match target {
        "x86_64-unknown-linux-gnu" => ("linux", "x86_64", "libseekdb.so"),
        "aarch64-unknown-linux-gnu" => ("linux", "aarch64", "libseekdb.so"),
        "x86_64-apple-darwin" => ("macos", "x86_64", "libseekdb.dylib"),
        "aarch64-apple-darwin" => ("macos", "arm64", "libseekdb.dylib"),
        "x86_64-pc-windows-msvc" => ("windows", "x86_64", "seekdb.dll"),
        _ => return Err(format!("unsupported seekdb target {target}; the pinned release has GNU/Linux x86_64/aarch64, macOS x86_64/arm64, and Windows x86_64 MSVC bundles; musl, Windows GNU/ARM64 and other targets are unavailable")),
    };
    let manifest: Manifest =
        serde_json::from_str(json).map_err(|e| format!("parse native manifest: {e}"))?;
    if manifest.schema_version != "1.0"
        || manifest.package != "libseekdb"
        || manifest.package_version != "1.4.0.0-r2"
    {
        return Err("unsupported native manifest identity/schema".into());
    }
    let mut matches = manifest
        .artifacts
        .into_iter()
        .filter(|a| a.os == os && a.architecture == architecture);
    let runtime = matches
        .next()
        .ok_or("native manifest has no matching asset")?
        .runtime;
    if matches.next().is_some()
        || runtime.file.contains(['/', '\\'])
        || !runtime
            .file
            .starts_with(&format!("libseekdb-{}-", manifest.package_version))
        || !runtime.file.ends_with(".tar.gz")
        || runtime.size_bytes == 0
        || runtime.sha256.len() != 64
        || !runtime
            .sha256
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err("invalid or ambiguous native manifest asset".into());
    }
    Ok(Asset {
        url: format!("{BASE_URL}{}", runtime.file),
        name: runtime.file,
        sha256: runtime.sha256,
        size_bytes: runtime.size_bytes,
        version: manifest.package_version,
        library,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selects_exact_target_abi_and_release() {
        for (target, platform, library) in [
            (
                "x86_64-unknown-linux-gnu",
                "linux-x86_64-glibc2.28",
                "libseekdb.so",
            ),
            (
                "aarch64-unknown-linux-gnu",
                "linux-aarch64-glibc2.28",
                "libseekdb.so",
            ),
            ("x86_64-apple-darwin", "macos15.0-x86_64", "libseekdb.dylib"),
            ("aarch64-apple-darwin", "macos15.0-arm64", "libseekdb.dylib"),
            ("x86_64-pc-windows-msvc", "windows-x86_64", "seekdb.dll"),
        ] {
            let asset = asset(target).unwrap();
            assert!(asset.name.contains(platform));
            assert_eq!(asset.library, library);
            assert_eq!(asset.version, "1.4.0.0-r2");
        }
        for target in [
            "x86_64-unknown-linux-musl",
            "x86_64-pc-windows-gnu",
            "aarch64-pc-windows-msvc",
        ] {
            assert!(asset(target).is_err());
        }
    }
    #[test]
    fn rejects_unsafe_or_ambiguous_metadata() {
        let mut m: serde_json::Value = serde_json::from_str(MANIFEST).unwrap();
        m["artifacts"][0]["runtime"]["file"] = "../bundle.tar.gz".into();
        assert!(select(&m.to_string(), "x86_64-unknown-linux-gnu").is_err());
        let mut m: serde_json::Value = serde_json::from_str(MANIFEST).unwrap();
        let duplicate = m["artifacts"][0].clone();
        m["artifacts"].as_array_mut().unwrap().push(duplicate);
        assert!(select(&m.to_string(), "x86_64-unknown-linux-gnu").is_err());
    }
}
