//! Native release configuration, target selection and mirror URL handling.
use serde::Deserialize;
use sha2::{Digest, Sha256};
use url::Url;

pub const PACKAGE_TOML: &str = include_str!("Cargo.toml");
pub const MAX_MANIFEST_BYTES: u64 = 1024 * 1024;
const DEFAULT_BASE_URL: &str =
    "https://obbusiness-private.oss-cn-shanghai.aliyuncs.com/download-center/opensource/libseekdb/";

#[derive(Clone, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub struct Config {
    pub native_version: String,
    pub manifest_sha256: String,
}

#[derive(Deserialize)]
struct PackageDocument {
    package: Package,
}
#[derive(Deserialize)]
struct Package {
    metadata: Metadata,
}
#[derive(Deserialize)]
struct Metadata {
    seekdb: Config,
}

fn sha256_string(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

impl Config {
    pub fn from_package(toml: &str) -> Result<Self, String> {
        let document: PackageDocument =
            toml::from_str(toml).map_err(|e| format!("read [package.metadata.seekdb]: {e}"))?;
        let config = document.package.metadata.seekdb;
        if !config
            .native_version
            .starts_with(|c: char| c.is_ascii_digit())
            || !config
                .native_version
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b".-_".contains(&b))
            || !sha256_string(&config.manifest_sha256)
        {
            return Err(
                "invalid native-version or manifest-sha256 in [package.metadata.seekdb]".into(),
            );
        }
        Ok(config)
    }

    pub fn manifest_name(&self) -> String {
        format!("manifest-{}.json", self.native_version)
    }

    pub fn verify_manifest(&self, bytes: &[u8]) -> Result<(), String> {
        if bytes.len() as u64 > MAX_MANIFEST_BYTES {
            return Err("native manifest exceeds the 1 MiB limit".into());
        }
        let actual = format!("{:x}", Sha256::digest(bytes));
        if actual != self.manifest_sha256 {
            return Err(format!(
                "native manifest SHA-256 mismatch: expected {}, got {actual}",
                self.manifest_sha256
            ));
        }
        self.parse_manifest(bytes).map(|_| ())
    }

    fn parse_manifest(&self, bytes: &[u8]) -> Result<Manifest, String> {
        let manifest: Manifest =
            serde_json::from_slice(bytes).map_err(|e| format!("parse native manifest: {e}"))?;
        if manifest.schema_version != "1.0"
            || manifest.package != "libseekdb"
            || manifest.package_version != self.native_version
        {
            return Err(
                "native manifest identity/schema does not match the pinned configuration".into(),
            );
        }
        Ok(manifest)
    }

    pub fn asset(&self, bytes: &[u8], target: &str, base: &DownloadBase) -> Result<Asset, String> {
        let (os, architecture, library) = target_platform(target)?;
        self.verify_manifest(bytes)?;
        let manifest = self.parse_manifest(bytes)?;
        let mut matches = manifest
            .artifacts
            .into_iter()
            .filter(|a| a.os == os && a.architecture == architecture);
        let runtime = matches
            .next()
            .ok_or("native manifest has no matching asset")?
            .runtime;
        if matches.next().is_some()
            || !runtime
                .file
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b".-_".contains(&b))
            || !runtime
                .file
                .starts_with(&format!("libseekdb-{}-", self.native_version))
            || !runtime.file.ends_with(".tar.gz")
            || runtime.size_bytes == 0
            || !sha256_string(&runtime.sha256)
        {
            return Err("invalid or ambiguous native manifest asset".into());
        }
        Ok(Asset {
            url: base.file_url(&runtime.file),
            name: runtime.file,
            sha256: runtime.sha256,
            size_bytes: runtime.size_bytes,
            version: manifest.package_version,
            library,
        })
    }
}

pub struct DownloadBase(Url);

impl DownloadBase {
    pub fn new(value: Option<&str>) -> Result<Self, String> {
        let mut url = Url::parse(value.unwrap_or(DEFAULT_BASE_URL))
            .map_err(|_| "SEEKDB_DOWNLOAD_BASE_URL must be an absolute HTTPS directory URL")?;
        if url.scheme() != "https"
            || url.host_str().is_none()
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
        {
            return Err("SEEKDB_DOWNLOAD_BASE_URL must be an HTTPS directory URL without credentials, query or fragment".into());
        }
        if !url.path().ends_with('/') {
            let path = format!("{}/", url.path());
            url.set_path(&path);
        }
        Ok(Self(url))
    }

    /// Call with a validated manifest or asset basename; no remote URL is accepted.
    pub fn file_url(&self, name: &str) -> String {
        self.0.join(name).expect("validated native basename").into()
    }
}

pub fn target_platform(target: &str) -> Result<(&'static str, &'static str, &'static str), String> {
    // ABI and architecture mapping only; release identities live in metadata/JSON.
    match target {
        "x86_64-unknown-linux-gnu" => Ok(("linux", "x86_64", "libseekdb.so")),
        "aarch64-unknown-linux-gnu" => Ok(("linux", "aarch64", "libseekdb.so")),
        "x86_64-apple-darwin" => Ok(("macos", "x86_64", "libseekdb.dylib")),
        "aarch64-apple-darwin" => Ok(("macos", "arm64", "libseekdb.dylib")),
        "x86_64-pc-windows-msvc" => Ok(("windows", "x86_64", "seekdb.dll")),
        _ => Err(format!("unsupported seekdb target {target}; the release supports GNU/Linux x86_64/aarch64, macOS x86_64/arm64, and Windows x86_64 MSVC; musl, Windows GNU/ARM64 and other targets are unavailable")),
    }
}

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
