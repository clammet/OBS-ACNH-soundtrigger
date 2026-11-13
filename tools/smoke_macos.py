"""Verify native dependencies and OBS module exports without opening any UI."""
import argparse
import ctypes
from pathlib import Path
import plistlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-app", type=Path, default=Path(Path.home().anchor) / "Applications/OBS.app")
    parser.add_argument("--bundle", type=Path, default=Path("build/obs-soundtrigger.plugin"))
    args = parser.parse_args()
    subprocess.run(["codesign", "--verify", "--strict", str(args.bundle)], check=True)
    frameworks = args.obs_app / "Contents/Frameworks"
    obs = ctypes.CDLL(str(frameworks / "libobs.framework/Versions/A/libobs"), mode=ctypes.RTLD_GLOBAL)
    ctypes.CDLL(str(frameworks / "obs-frontend-api.dylib"), mode=ctypes.RTLD_GLOBAL)
    plugin = ctypes.CDLL(str((args.bundle / "Contents/MacOS/obs-soundtrigger").resolve()))
    plugin.obs_module_name.restype = ctypes.c_char_p
    plugin.obs_module_ver.restype = ctypes.c_uint32
    obs.obs_get_version.restype = ctypes.c_uint32
    if plugin.obs_module_ver() >> 24 != obs.obs_get_version() >> 24:
        raise RuntimeError("Plugin and installed OBS API major versions differ")
    for name in ["obs_module_load", "obs_module_unload", "obs_module_set_pointer"]:
        getattr(plugin, name)
    with (args.bundle / "Contents/Info.plist").open("rb") as source:
        assert plistlib.load(source)["CFBundleExecutable"] == "obs-soundtrigger"
    print(f"Dynamic loading, signature, and module exports passed: {plugin.obs_module_name().decode()}")
    print("This does not initialize the UI or exercise recording inside OBS.")


if __name__ == "__main__":
    main()
