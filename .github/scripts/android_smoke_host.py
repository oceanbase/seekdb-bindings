#!/usr/bin/env python3
"""Probe the Android runtime capabilities of an ephemeral GitHub-hosted runner."""

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-acceleration", action="store_true")
    args = parser.parse_args()
    report = {"architecture": platform.machine(), "platform": platform.platform()}
    if platform.system() == "Linux":
        import fcntl
        report["kvm"] = {"exists": Path("/dev/kvm").exists()}
        try:
            fd = os.open("/dev/kvm", os.O_RDWR)
            try:
                report["kvm"]["api"] = fcntl.ioctl(fd, 0xAE00)
                vm = fcntl.ioctl(fd, 0xAE01, 0)
                os.close(vm)
            finally:
                os.close(fd)
        except OSError as error:
            report["kvm"]["error"] = str(error)
        for name, command in {
            "binder_module": ["modinfo", "binder_linux"],
            "binder_filesystem": ["sh", "-c", "cat /proc/filesystems | sed -n '/binder/p'"],
            "kernel_headers": ["readlink", "-f", "/lib/modules/" + platform.release() + "/build"],
        }.items():
            result = subprocess.run(command, text=True, capture_output=True)
            report[name] = {"exit": result.returncode, "output": result.stdout + result.stderr}
    else:
        result = subprocess.run(["sysctl", "hw.model", "hw.ncpu", "kern.hv_support"], text=True, capture_output=True)
        report["hardware"] = result.stdout + result.stderr
        emulator = Path(os.environ["ANDROID_HOME"]) / "emulator/emulator"
        result = subprocess.run([emulator, "-accel-check"], text=True, capture_output=True)
        report["acceleration"] = {"exit": result.returncode, "output": result.stdout + result.stderr}
    output = Path("build/android-host.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(output.read_text(), flush=True)
    if args.require_acceleration and report["acceleration"]["exit"]:
        raise RuntimeError("This GitHub-hosted macOS runner cannot accelerate the Android emulator")


if __name__ == "__main__":
    main()
