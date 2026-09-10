# 🧾 Backup directory layout and manifest schema

A backup produced by `abp backup -o DIR` looks like:

```
DIR/
  manifest.json
  apks/
    <package>/
      base.apk
      split_config.arm64_v8a.apk   (if present)
  data/
    <package>.tar.gz               (root mode only)
  shared_storage.tar                (root mode; single tar of /sdcard)
  shared_storage/                   (standard mode; plain directory tree, pulled via adb pull)
  legacy_backup.ab                  (standard mode; raw `adb backup` archive)
```

Every path referenced from `manifest.json` is relative to `DIR`, so a
backup directory is self-contained and can be moved/copied as a whole.

## `manifest.json`

```jsonc
{
  "format_version": 1,
  "abp_version": "1.0.0",
  "created_at_utc": "2026-01-01T12:00:00Z",
  "mode": "root",                    // or "standard"
  "device": {
    "serial": "ABC123",
    "model": "Pixel 8",
    "manufacturer": "Google",
    "android_release": "15",
    "sdk_int": 35,
    "rooted": true,
    "root_method": "su_binary"       // "adbd_root" | "su_binary" | "none"
  },
  "shared_storage_included": true,
  "shared_storage_is_directory": false, // true in standard mode (a pulled dir tree, not a tar)
  "shared_storage_archive": "shared_storage.tar",
  "shared_storage_archive_bytes": 123456789,
  "shared_storage_archive_sha256": "…",  // empty when shared_storage_is_directory is true

  "legacy_adb_backup_file": "",       // set to "legacy_backup.ab" in standard mode

  "packages": [
    {
      "name": "com.example.app",
      "system_app": false,

      "apk_included": true,
      "apk_files": ["apks/com.example.app/base.apk"],

      "data_included": true,
      "data_archive": "data/com.example.app.tar.gz",
      "data_archive_bytes": 45678,
      "data_archive_sha256": "…",

      "external_data_included": false,
      "external_data_archive": "",
      "external_data_archive_bytes": 0,
      "external_data_archive_sha256": "",

      "error": ""                     // non-empty if this package failed
    }
  ]
}
```

Notes:

- Object key order in the file is stable (insertion order, as written
  above) — `abp`'s JSON writer preserves it, which keeps manifests
  diff-friendly across repeated backups.
- `error` on a package entry does not mean the whole backup failed —
  `abp backup`'s exit code reflects fatal errors (couldn't connect,
  couldn't create the output directory, ...); per-package errors are
  reported in the summary and left in the manifest for inspection.
- `data_included: false` with an empty `error` means there was simply
  nothing to capture (e.g. the app has no data directory yet), not a
  failure.
- `external_data_included`/`external_data_archive*` fields are reserved
  for a future capture of `/sdcard/Android/data/<pkg>` (per-app external
  storage); they are always empty/false today.
- `format_version` will be bumped if the schema changes in a
  backwards-incompatible way; `abp` refuses to guess at unknown
  versions rather than silently misinterpreting a newer manifest.

See [ROOT_BACKUP.md](ROOT_BACKUP.md) for what actually produces the
`data/*.tar.gz` and `shared_storage.tar` archives, and the top-level
[README.md](../README.md) for the CLI that reads/writes this format.
