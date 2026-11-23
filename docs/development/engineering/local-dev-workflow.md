<p align="right">
  <a href="local-dev-workflow.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Local Dev Workflow: Commit, Build, Flash, Log

Mandatory local workflow for this branch's application development. Every approved
change set follows these steps in order; do not skip any of them.

## 1. Commit after every completed change set

- Commit on the active feature branch immediately after a change set is finished
  and validated. Never let finished work sit uncommitted.
- Commit messages in English, conventional style (`feat(scope): ...`, `fix(scope): ...`).
- Never commit `build/`, `sdkconfig`, `managed_components/`, `.cache/`,
  `__pycache__/` (already in `.gitignore`).

## 2. Build the firmware after committing

The toolchain lives outside the default `python3` probe path, so the Python env
must be pinned explicitly:

```bash
export IDF_PYTHON_ENV_PATH=/home/apps/.espressif/python_env/idf5.5_py3.11_env
source /home/apps/esp/esp-idf/export.sh
cd /workspace/apps/empty-GPWS/ai-passport
idf.py build          # or: ./tools/validate.sh --firmware for the full gate
```

## 3. Report firmware path and flash command

After every build, report to the user:

- **Firmware path**: `build/FoloToy-AI-Passport-full.bin` (verified merged
  image: bootloader + partition table + app; produced by
  `./tools/validate.sh --firmware`)
- **Flash command** (pick the connected port, usually `/dev/ttyACM0`,
  `/dev/ttyUSB0`, or on macOS `/dev/cu.usbmodem*`):

```bash
# Preferred: single verified merged image at 0x0 (see environment-setup.md)
python3 -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  --port <PORT> \
  write_flash 0x0 build/FoloToy-AI-Passport-full.bin

# Incremental development flash only (preserves existing NVS):
idf.py -p <PORT> flash
```

Rules:

- Prefer the merged image: one write, no manual per-image offsets, and the
  image is the one the firmware gate verified. Segmented three-image esptool
  commands re-state offsets by hand and flash unverified intermediate
  artifacts; do not use them as the default.
- The merged image can reset NVS/PHY gaps between images; when existing NVS
  state must be preserved, use the segmented `idf.py flash` instead.
- Flashing still requires the user's explicit approval per the repository
  AGENTS.md on-device testing policy.
- Never change the baud rate or drop `--before default_reset --after
  hard_reset` on your own: 460800 is the verified rate for the USB-Serial/JTAG
  port; higher rates (e.g. 921600) drop the connection mid-write.
- **Stop the log capture (section 4) before flashing.** A concurrent serial
  reader eats esptool's SLIP responses and the write aborts mid-image with
  `chip stopped responding` (the device stays recoverable by re-flashing).

## 4. Capture device logs with `tools/bootlog.py`

The standard log capture tool, versioned in the repository so it cannot be
lost. Runs on the host next to the device (macOS or Linux); needs only
`pyserial`, which `esptool` already pulls in.

```bash
cd /workspace/apps/empty-GPWS/ai-passport   # or a copy of this repo on the Mac

# Start (foreground; Ctrl+C to stop)
python3 tools/bootlog.py                     # default: /dev/cu.usbmodem* -> boot.log
python3 tools/bootlog.py -p /dev/ttyUSB0     # Linux or an exact port
python3 tools/bootlog.py -p '/dev/cu.usbmodem*' -o run.log

# Background variant
nohup python3 tools/bootlog.py >/dev/null 2>&1 &
pkill -f tools/bootlog.py                    # stop it
```

Behavior and rules:

- Auto-reconnect: after every deep sleep or reboot the device re-enumerates
  under a possibly different USB name, so pass a glob pattern, never a fixed
  port name. The tool rescans until the device reappears.
- Attach/detach timestamps are written into the log file. Keep the file for
  the whole reproduction window: it holds the crash output and the boot-time
  reset-reason line even when the terminal missed them.
- Flash and capture are mutually exclusive: `pkill -f tools/bootlog.py`
  first, flash, then start the capture again.

## 5. Order of operations per change set

1. Audit plan approved by the user (no code changes before approval).
2. Implement the minimal change set.
3. `./tools/validate.sh --static` (host tests) and `idf.py build` (firmware) pass.
4. Commit.
5. Report: changed files, validation results, firmware path, flash command.
