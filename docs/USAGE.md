# Usage reference

## Global options

These may appear before the subcommand:

| Option              | Description                                                        |
|----------------------|---------------------------------------------------------------------|
| `--adb-path PATH`    | Use this `adb` executable instead of the one on `PATH`.            |

The same can be set via the `ABP_ADB_PATH` environment variable, which
`--adb-path` overrides if both are given.

## `abp devices`

Lists devices visible to `adb`, including unauthorized/offline ones:

```
$ abp devices
ABC123          device       Pixel_8
DEF456          unauthorized
```

If nothing is listed, check that USB debugging is enabled and that
you've accepted the "Allow USB debugging?" prompt on the device.

## `abp info [-s SERIAL]`

```
$ abp info
Serial:        ABC123
Manufacturer:  Google
Model:         Pixel 8
Android:       15 (SDK 35)
Root access:   yes (su binary available)
```

`-s SERIAL` targets a specific device when more than one is connected;
otherwise `abp` uses whichever device `adb` picks by default (fails if
more than one is connected and no serial is given).

## `abp list-packages [-s SERIAL] [--system] [--json]`

Lists installed packages. By default only third-party (user-installed)
apps are shown; `--system` includes system apps too. `--json` prints a
JSON array (`name`, `system_app`, `apk_count`) instead of plain text —
useful for scripting `--only`/`--exclude` lists.

## `abp backup -o DIR [options]`

| Option                | Description |
|------------------------|-------------|
| `-s, --serial SERIAL`  | Target a specific device. |
| `-o, --output DIR`     | **Required.** Directory to write the backup into (created if missing). |
| `--system`             | Include system apps (default: third-party only). |
| `--no-apks`            | Skip extracting APK files. |
| `--no-data`            | Skip app data. |
| `--no-shared`          | Skip `/sdcard`. |
| `--only PKGS`          | Comma-separated package names; only these are backed up. |
| `--exclude PKGS`       | Comma-separated package names to skip. |
| `--root`               | Require root; fail immediately if unavailable. |
| `--standard`           | Force standard (non-root) mode even if root is available. |
| `-y, --yes`            | Skip the confirmation prompt. |
| `-v, --verbose`        | Print debug-level log messages. |

Examples:

```sh
# Everything abp can reach, auto-detecting root:
abp backup -o ~/backups/full

# Apps + their data, no photos/videos, forcing root mode:
abp backup -o ~/backups/apps-only --no-shared --root

# Just two apps' data (not their APKs):
abp backup -o ~/backups/two-apps --no-apks --only com.example.one,com.example.two

# Everything except a noisy app:
abp backup -o ~/backups/most --exclude com.chatty.app
```

## `abp restore -i DIR [options]`

| Option                | Description |
|------------------------|-------------|
| `-s, --serial SERIAL`  | Target a specific device. |
| `-i, --input DIR`      | **Required.** Backup directory produced by `abp backup`. |
| `--no-apks`            | Don't reinstall APKs. |
| `--no-data`            | Don't restore app data. |
| `--no-shared`          | Don't restore shared storage. |
| `--only PKGS`          | Comma-separated package names to restore (root mode only — see below). |
| `--exclude PKGS`       | Comma-separated package names to skip. |
| `-y, --yes`            | Skip the confirmation prompt. |
| `-v, --verbose`        | Print debug-level log messages. |

There is no `--root`/`--standard` flag for restore: the backup's own
`manifest.json` records which mode produced it, and that dictates how
its app data must be restored.

**Per-package filtering (`--only`/`--exclude`) only works for app data in
root-mode backups**, because root mode captures one archive per package.
Standard-mode app data lives in a single `legacy_backup.ab` file produced
by `adb backup`, which can only be restored as a whole — `abp` will warn
and restore all of it regardless of `--only`/`--exclude`. APK
installation and shared storage restoration always respect the filters
(shared storage has no per-package concept, so `--only`/`--exclude` don't
apply to it at all — use `--no-shared` to skip it entirely).

Examples:

```sh
# Restore everything:
abp restore -i ~/backups/full

# Just reinstall APKs, skip data and media:
abp restore -i ~/backups/full --no-data --no-shared

# Restore one app's data only (root-mode backup):
abp restore -i ~/backups/full --only com.example.one --no-shared
```

## Exit codes

| Code | Meaning |
|------|---------|
| 0    | Success. |
| 1    | Operation ran but failed (no device, bad manifest, per-package restore failures, user declined confirmation). |
| 2    | Usage error (bad flags, missing required option, unknown subcommand). |

## Troubleshooting

- **`Could not run 'adb'`** — install Android Platform Tools and ensure
  `adb` is on `PATH`, or pass `--adb-path`.
- **`No connected and authorized device found`** — run `adb devices`
  yourself; if the device shows `unauthorized`, accept the RSA key
  prompt on the device screen and try again.
- **Root mode falls back to standard unexpectedly** — run `abp info` to
  see the detected root method; if it says `none` but you believe the
  device is rooted, check that your root manager grants access to the
  `adb shell` (not just `su` from a terminal app), and watch the device
  screen for a superuser grant prompt during the first backup attempt.
- **Standard-mode backup produces no data for an app** — that app almost
  certainly has `android:allowBackup="false"`, which is the default for
  apps targeting recent Android SDKs. This is a hard limitation of
  `adb backup`, not a bug in `abp`; root mode has no such restriction.
