"""Fetch build headers matching installed OBS and build the macOS plugin."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def fetch(url, destination, digest=None):
    if not destination.exists():
        print(f"Downloading {url}", flush=True)
        urllib.request.urlretrieve(url, destination)
    if digest and hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"Checksum mismatch: {destination.name}; remove it and retry")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-app", type=Path, help="Path to OBS.app if installed outside Applications")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    candidates = [Path(Path.home().anchor) / "Applications/OBS.app", Path.home() / "Applications/OBS.app"]
    app = args.obs_app or next((p for p in candidates if p.exists()), None)
    if not app:
        parser.error("Could not find OBS.app; pass --obs-app")
    app = app.resolve()
    with (app / "Contents/Info.plist").open("rb") as source:
        version = plistlib.load(source)["CFBundleShortVersionString"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        parser.error("A stable OBS release is required for automatic header download")
    deps = ROOT / ".deps"
    deps.mkdir(exist_ok=True)
    packages = [
        (f"obs-studio-{version}", f"https://github.com/obsproject/obs-studio/archive/refs/tags/{version}.tar.gz",
         "9174ee0e3677c92d0d060bdce4c5d44dfee9fd3afddfb6333f098fd029b8de2b" if version == "33.0.1" else None),
        ("simde-0.8.2", "https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz",
         "ed2a3268658f2f2a9b5367628a85ccd4cf9516460ed8604eed369653d49b25fb"),
    ]
    for directory, url, digest in packages:
        archive = deps / f"{directory}.tar.gz"
        fetch(url, archive, digest)
        if not (deps / directory).exists():
            with tarfile.open(archive) as source:
                source.extractall(deps, filter="data")
    build = args.build_dir.resolve()
    subprocess.run([
        "cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_OBS_PLUGIN=ON", f"-DOBS_APP={app}", f"-DOBS_SOURCE_DIR={deps / f'obs-studio-{version}'}",
        f"-DSIMDE_INCLUDE_DIR={deps / 'simde-0.8.2'}",
    ], check=True)
    subprocess.run(["cmake", "--build", str(build)], check=True)
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"], check=True)
    print(f"Built {build / 'obs-soundtrigger.plugin'} (OBS {version})")


if __name__ == "__main__":
    main()
