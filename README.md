# OBS Sound Trigger

A lightweight macOS OBS plugin that starts recording when it recognizes a specific sound. Includes a model for Animal Crossing: New Horizons shooting-star sounds.

Clean sound examples are prepared into a reusable model ahead of time. The plugin listens to your selected OBS audio track and records until **X seconds after the latest match**. Further matches extend the recording.

## Setup

Requires OBS, Python 3.14, ffmpeg, CMake, Ninja, and the Xcode command-line tools.

From the repository root:

```sh
python3.14 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
python3.14 tools/build_macos.py
python3.14 tools/install_macos.py
```

Restart OBS after installation. To update an existing installation, add `--replace` to the install command.

## Use

1. Open **Tools → Sound Trigger…** in OBS.
2. Choose a sound model, audio track, and recording duration.
3. Check **Enable detection**, leave **Dry run** on, and click **Apply** to test detection.
4. Turn **Dry run** off and click **Apply** to allow automatic recording.

The plugin uses your normal OBS recording settings. Detection starts disabled after each OBS restart. Manually started recordings remain under your control. Disabling detection leaves any ongoing recording running until you stop it manually.

## Custom sounds

Use `tools/train.py` to prepare a model from clean WAV examples, then select that model in OBS. Python is only needed for preparation and development; detection runs natively inside OBS.
