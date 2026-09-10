# abp — ADB Backup Program

`abp` is a Linux command-line tool for making full backups of Android
devices over ADB and restoring them, including installed apps, their
private data, and shared storage (photos, downloads, etc). It works on
stock, non-rooted devices via ADB's own backup mechanism, and on rooted
devices via a more complete `tar`-over-ADB pipeline.

It is written in modern C++17, built with CMake, and has no third-party
runtime dependencies beyond a working `adb` (and, for root-mode backups,
root access on the device).

## Why

Android's built-in backup story is fragmented: `adb backup` is deprecated,
silently skips any app with `android:allowBackup="false"` (the default for
apps targeting recent SDKs), and can't capture split APKs or arbitrary
files. `abp` gives you a single tool that:

- Uses the best available mechanism automatically (root if present,
  otherwise the standard ADB flow), or lets you force one.
- On rooted devices, captures the **entire** private data directory of
  every app (not just what the app opted into), plus its full APK set
  (base + split APKs), plus all of shared storage.
- Writes a single, versioned, human-readable `manifest.json` describing
  exactly what was captured, with SHA-256 checksums so a restore can
  detect a corrupted or truncated archive before touching the device.
- Restores app data with correct ownership (UID/GID) and SELinux context,
  not just a raw file dump.

## Features

- **Device discovery**: `abp devices`, `abp info` (model, Android version,
  root status).
- **Package enumeration**: `abp list-packages`, with JSON output for
  scripting.
- **Full backup**: APKs (including split APKs), per-app private data, and
  shared storage (`/sdcard`), selectable independently.
- **Full restore**: reinstalls APKs, restores app data with UID
  remapping and SELinux relabeling, restores shared storage.
- **Two backends**, chosen automatically or forced explicitly:
  - **Root mode** — streams `tar` archives of each app's data directory
    (and of `/sdcard`) directly over `adb exec-out` / `adb shell`, without
    ever buffering large data in the host process's memory.
  - **Standard mode** — uses the public `adb backup`/`adb restore` flow
    for app data and `adb pull`/`adb push` for shared storage. Works on
    any device with USB debugging enabled, no root required.
- **Selective backup/restore**: `--only`, `--exclude`, `--no-apks`,
  `--no-data`, `--no-shared`, `--system`.
- **Integrity checking**: every archive is SHA-256 checksummed at backup
  time and verified before it is written back to the device at restore
  time.
- **No shell injection surface**: every command sent to `adb`/the device
  shell is built from validated package names and paths, never from raw
  string concatenation of untrusted input.

See [docs/ROOT_BACKUP.md](docs/ROOT_BACKUP.md) for exactly what root mode
does on-device, and [docs/MANIFEST.md](docs/MANIFEST.md) for the backup
directory layout and manifest schema.

## Requirements

- A C++17 compiler (GCC ≥ 9 or Clang ≥ 10) and CMake ≥ 3.16.
- `adb` (Android Platform Tools) installed and on your `PATH`, or pointed
  to explicitly with `--adb-path` / `ABP_ADB_PATH`.
- USB debugging enabled on the target device, and the host authorized
  (`adb devices` should show it as `device`, not `unauthorized`).
- For root-mode backups: either a `su` binary reachable from the adb
  shell user (Magisk, KernelSU, etc.) or a userdebug/eng build where
  `adbd` already runs as root.

## Building

```sh
git clone https://github.com/guillaume-behr/abp.git
cd abp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # optional: run the unit tests
```

This produces `build/src/abp`. To install it system-wide:

```sh
sudo cmake --install build
```

Useful CMake options:

| Option                      | Default | Description                                   |
|------------------------------|---------|------------------------------------------------|
| `ABP_BUILD_TESTS`            | `ON`    | Build the `abp_tests` unit test binary.        |
| `ABP_WARNINGS_AS_ERRORS`     | `OFF`   | Treat compiler warnings as errors (used in CI).|

## Usage

```text
abp devices                                  # list connected/authorized devices
abp info [-s SERIAL]                         # model, Android version, root status
abp list-packages [-s SERIAL] [--system]     # installed packages

abp backup  -o ./my-backup [options]         # back up
abp restore -i ./my-backup [options]         # restore
```

### Backing up

```sh
# Back up everything abp can reach, auto-detecting root:
abp backup -o ~/backups/pixel-2026-01-01

# Force standard (non-root) mode, apps only, skip shared storage:
abp backup -o ~/backups/quick --standard --no-shared

# Only two specific apps, including their private data:
abp backup -o ~/backups/subset --only com.example.one,com.example.two
```

Standard-mode app-data backups use the legacy `adb backup` mechanism,
which requires you to unlock the device and tap **"Back up my data"**
when prompted; `abp` waits for that confirmation. It also only captures
apps with `android:allowBackup="true"` — most modern apps opt out of
this. Root mode has no such limitation and needs no on-device
interaction.

### Restoring

```sh
abp restore -i ~/backups/pixel-2026-01-01
```

The backup's own `manifest.json` records whether it was captured in root
or standard mode, and the restore uses the matching mechanism
automatically — you don't need to specify it again. Restoring into a
non-rooted device from a root-mode backup will still reinstall APKs, but
app data and shared storage cannot be restored without root.

Run `abp --help`, or see [docs/USAGE.md](docs/USAGE.md), for the full
option reference and more examples.

## Limitations

This is not a substitute for verified, tested backup software for
anything you cannot afford to lose:

- Standard mode's app-data capture depends entirely on `adb backup`,
  which Google has deprecated and which most modern apps opt out of.
  Treat it as "some data if you're lucky," not a full backup.
- Root-mode UID remapping assumes the app has already been (re)installed
  with a clean data directory before its archive is extracted; restoring
  onto a device where the package was never freshly installed may leave
  incorrect file ownership until the app is relaunched.
- System apps and OS-level state (Wi-Fi credentials, accounts, device
  settings) are out of scope; `abp` backs up app data and media, not the
  whole device image.
- No encryption is applied to backup archives. If your backups may
  contain sensitive data, store them on encrypted media.

## Documentation

- [docs/USAGE.md](docs/USAGE.md) — full command/option reference, examples, troubleshooting.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the code is layered.
- [docs/ROOT_BACKUP.md](docs/ROOT_BACKUP.md) — exactly what root mode does on-device.
- [docs/MANIFEST.md](docs/MANIFEST.md) — backup directory layout and `manifest.json` schema.

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
