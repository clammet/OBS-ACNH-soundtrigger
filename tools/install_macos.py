"""Install a built plugin for the current user; OBS loads it on its next restart."""
import argparse
from datetime import datetime
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path("build/obs-soundtrigger.plugin"))
    parser.add_argument("--replace", action="store_true", help="Keep a backup and replace an installed version")
    args = parser.parse_args()
    if not (args.bundle / "Contents/MacOS/obs-soundtrigger").is_file():
        parser.error("Build the plugin first")
    subprocess.run(["codesign", "--verify", "--strict", str(args.bundle)], check=True)
    parent = Path.home() / "Library/Application Support/obs-studio/plugins"
    destination = parent / args.bundle.name
    if destination.exists() and not args.replace:
        parser.error("Plugin already installed; pass --replace to back it up and update it")
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="soundtrigger-install-", dir=parent) as temporary:
        staged = Path(temporary) / args.bundle.name
        shutil.copytree(args.bundle, staged)
        if destination.exists():
            backup_dir = parent.parent / "soundtrigger-backups" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            backup_dir.mkdir(parents=True)
            destination.rename(backup_dir / destination.name)
            print(f"Backed up previous plugin to {backup_dir}")
        staged.rename(destination)
    print(f"Installed {destination}\nRestart OBS and open Tools > Sound Trigger. It starts disarmed.")


if __name__ == "__main__":
    main()
