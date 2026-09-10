# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.0.0] - 2026-09-10

### Added

- Initial release.
- `abp devices`, `abp info`, `abp list-packages` commands.
- `abp backup` / `abp restore` with automatic root/standard backend
  selection (`--root` / `--standard` to force one).
- Root-mode backend: per-app `tar` capture of `/data/data/<pkg>` and of
  `/sdcard`, streamed over `adb exec-out`/`adb shell`; UID remapping and
  `restorecon` on restore.
- Standard-mode backend: legacy `adb backup`/`adb restore` for app data,
  `adb pull`/`adb push` for shared storage.
- Versioned `manifest.json` with per-archive SHA-256 checksums, verified
  before restore.
- Selective backup/restore via `--only`, `--exclude`, `--no-apks`,
  `--no-data`, `--no-shared`, `--system`.
- Dependency-free implementation: custom subprocess wrapper, JSON
  parser/serializer, and SHA-256, all in-tree.
