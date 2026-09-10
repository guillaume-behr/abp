# 🚀 Usage reference

## Global options

These may appear anywhere on the command line, before or after the
subcommand:

| Option              | Description                                                        |
|----------------------|---------------------------------------------------------------------|
| `--adb-path PATH`    | Use this `adb` executable instead of the one on `PATH`.            |
| `-v, --verbose`      | Print debug-level log messages.                                    |
| `--no-color`         | Disable coloured output.                                           |

`--adb-path` can also be set via the `ABP_ADB_PATH` environment variable,
which `--adb-path` overrides if both are given.

Colour is enabled automatically when the relevant stream is a terminal
and disabled when it is redirected. Setting the `NO_COLOR` environment
variable to any non-empty value turns it off, as does `--no-color`.

`abp -V` / `abp --version` prints the version. (`-v` is *verbose*, as it
is in most CLIs; use the capital `-V` for the version.)

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

### What gets captured

Standard (non-root) mode captures debuggable apps completely via `run-as`
and falls back to legacy `adb backup` only for the rest, so coverage is
per app rather than all-or-nothing. See the
[coverage table](../README.md#-what-each-mode-can-save) and
[NON_ROOT_BACKUP.md](NON_ROOT_BACKUP.md).

The backup summary reports the split, for example:

```
Backup complete (standard mode).
  Packages:        48
  With app data:   41
    via run-as:     12 (complete per-app archives)
    via adb backup: 29 (partial; apps may have opted out)
  Errors:          0
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

There is no `--root`/`--standard` flag for restore: the backup's own
`manifest.json` records which mode produced it, and that dictates how
its app data must be restored.

**Per-package filtering (`--only`/`--exclude`) works for app data that was
captured into a per-package archive** — that is, everything in a root-mode
backup, and every debuggable app in a standard-mode backup (captured via
`run-as`). Check `data_capture_method` in `manifest.json`: `root_tar` and
`run_as_tar` filter per package; `legacy_adb_backup` does not.

Packages captured into `legacy_backup.ab` by `adb backup` share one opaque
archive that `adb restore` can only write back as a whole. `abp` only
invokes that restore if at least one selected package needs it, and warns
when doing so will also restore packages you deselected.

APK installation always respects the filters. Shared storage has no
per-package concept, so `--only`/`--exclude` don't apply to it at all —
use `--no-shared` to skip it entirely.

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
