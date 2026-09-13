# 📜 Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- `abp gui`: a local web GUI, served from the `abp` binary itself, for
  backing up and restoring devices and for exploring backups already on
  disk (manifest details, per-package archive sizes/checksums/errors, and
  a file browser over the backup directory).
- JSON API behind the GUI (`/api/devices`, `/api/backups`,
  `/api/jobs/backup`, `/api/job`, ...), usable on its own for scripting.
- `BackupStore`: read-only discovery, summarizing and sandboxed browsing
  of backup directories.
- `Logger` sink hook, used to stream a running job's progress to the
  browser while still printing it to the terminal.

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
