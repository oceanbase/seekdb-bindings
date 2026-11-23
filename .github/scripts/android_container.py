#!/usr/bin/env python3
"""Run native arm64 Android using the binder module on a GitHub Linux arm64 runner."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import time

import package

NAME = "seekdb-android-smoke"
IMAGE = "redroid/redroid:12.0.0_64only-latest"
SERIAL = "127.0.0.1:5555"
WORK = package.ROOT / "build/android-runtime"


def run(arguments, **kwargs):
    return package.run(arguments, timeout=300, **kwargs)


def start():
    if platform.machine() != "aarch64":
        raise RuntimeError("The Android container requires a native Linux arm64 runner")
    WORK.mkdir(parents=True)
    run(["sudo", "modprobe", "binder_linux", "devices=binder,hwbinder,vndbinder"])
    run(["sudo", "apt-get", "update", "-qq"])
    run(["sudo", "apt-get", "install", "-y", "adb"])
    run(["docker", "pull", IMAGE])
    image = json.loads(subprocess.check_output(["docker", "image", "inspect", IMAGE], text=True))[0]
    package.write_json(WORK / "host.json", {"kernel": platform.release(), "architecture": platform.machine(),
        "image": image["RepoDigests"], "image_architecture": image["Architecture"]})
    run(["docker", "run", "--detach", "--privileged", "--name", NAME,
         "--publish", "127.0.0.1:5555:5555", "--volume", str(WORK / "data") + ":/data", IMAGE,
         "androidboot.use_memfd=true", "androidboot.redroid_gpu_mode=guest"])
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        subprocess.run(["adb", "connect", SERIAL], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        state = subprocess.run(["adb", "-s", SERIAL, "shell", "getprop", "sys.boot_completed"],
                               capture_output=True, text=True, timeout=10)
        if state.returncode == 0 and state.stdout.strip() == "1":
            run(["adb", "-s", SERIAL, "shell", "getprop", "ro.product.cpu.abi"])
            print("Native arm64 Android booted on the GitHub-hosted runner", flush=True)
            return
        time.sleep(2)
    raise TimeoutError("The native Android container did not finish booting")


def stop():
    WORK.mkdir(parents=True, exist_ok=True)
    for name, command in {
        "container": ["docker", "logs", NAME],
        "logcat": ["docker", "exec", NAME, "logcat", "-d"],
        "kernel": ["sudo", "dmesg", "--ctime"],
    }.items():
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (WORK / (name + ".log")).write_text(result.stdout + result.stderr)
    subprocess.run(["docker", "rm", "--force", NAME], timeout=30)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("start", "stop"))
    {"start": start, "stop": stop}[parser.parse_args().command]()
