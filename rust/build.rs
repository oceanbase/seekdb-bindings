use sha2::{Digest, Sha256};
use std::env;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::Command;

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

fn run(command: &mut Command) -> Result<(), String> {
    let description = format!("{command:?}");
    let status = command
        .status()
        .map_err(|error| format!("run {description}: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{description} failed with {status}"))
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
    if let Some(directory) = env::var_os("SEEKDB_NATIVE_DIR") {
        return validate_native_dir(Path::new(&directory), asset.library);
    }
    if asset.url.is_empty() {
        return Err(format!(
            "native bundle URL is not configured for {target}; set SEEKDB_NATIVE_DIR to a compatible lib/ directory"
        ));
    }

    let package = out_dir.join(asset.name);
    if !package.exists() || sha256(&package).map_err(|error| error.to_string())? != asset.sha256 {
        let temporary = package.with_extension("download");
        run(Command::new("curl")
            .args([
                "--fail",
                "--location",
                "--retry",
                "3",
                "--proto",
                "=https",
                "--proto-redir",
                "=https",
                "--silent",
                "--show-error",
                "--output",
            ])
            .arg(&temporary)
            .arg(asset.url))?;
        let actual = sha256(&temporary).map_err(|error| error.to_string())?;
        if actual != asset.sha256 {
            return Err(format!(
                "SHA-256 mismatch for {}: expected {}, got {actual}",
                asset.url, asset.sha256
            ));
        }
        fs::rename(&temporary, &package).map_err(|error| error.to_string())?;
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
    fs::create_dir_all(&expanded).map_err(|error| error.to_string())?;
    run(Command::new("tar")
        .arg("-xzf")
        .arg(&package)
        .arg("-C")
        .arg(&expanded))?;
    let directory = find_native_dir(&expanded, asset.library)?;
    fs::write(&marker, b"complete\n").map_err(|error| error.to_string())?;
    Ok(directory)
}

fn main() {
    if let Err(error) = build() {
        panic!("seekdb Rust binding build failed: {error}");
    }
}

fn build() -> Result<(), String> {
    println!("cargo:rerun-if-env-changed=SEEKDB_NATIVE_DIR");
    let target = env::var("TARGET").map_err(|error| error.to_string())?;
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is missing")?);
    let native_dir = get_native_dir(&out_dir, &target)?;
    println!(
        "cargo:rustc-env=SEEKDB_BUILD_NATIVE_DIR={}",
        native_dir.display()
    );
    Ok(())
}
