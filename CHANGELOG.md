# Changelog

[Italiano](CHANGELOG.it.md) | [Documentation index](docs/README.md)

## Unreleased

### Documentation and Distribution

- Added complete English/Italian technical documentation and navigation.
- Published all verified 2.0.0 artifacts in the public, repository-linked
  `ghcr.io/okno/syseba-packages` OCI package.
- Added a least-privilege manual GitHub Actions workflow that revalidates a
  tagged Release before publishing version and `latest` package tags.
- Documented current package digest, anonymous extraction verification, and
  unsigned Windows/macOS release status.

## 2.0.0

### Added

- Complete C11 runtime for Linux, Windows, and macOS.
- Native Linux and Windows filesystem watchers with polling fallback.
- Parallel initial synchronization and bounded worker queues.
- Responsive Italian/English console dashboard.
- Embedded token-protected Web UI and JSON API.
- Interactive CLI restore browser and non-interactive restore commands.
- Native systemd, Windows Service, and launchd integration.
- DEB, RPM, Windows ZIP/NSIS, macOS Universal DMG, portable Linux, and source
  packaging.
- Automated stopped-state migration, update, snapshot, health check, and exact
  rollback tool.

### Changed

- Python, watchdog, psutil, and pip are no longer runtime dependencies.
- File replacement is temporary-file based, source-verified, durable, and
  atomic.
- Service Web UI starts automatically on port 8765 with persistent token
  authentication.
- Console and Web layouts were redesigned for clearer status and restore
  workflows.
- Systemd service receives explicit writable roots and additional sandboxing.

### Fixed

- Historical SQLite databases missing `level` or other audit columns are
  migrated transactionally before logging starts.
- Single-instance status uses the kernel lock and is not confused by stale
  files or PID reuse.
- Restore traversal, symlink/reparse-point escape, token-file link attacks,
  partial copies, retrodated same-size files, and configuration write races
  are rejected or handled safely.
- Restore POST no longer retains a JSON-owned strategy pointer after deleting
  the request object.

### Compatibility

- Existing `[SETTINGS]` INI configuration is accepted.
- Legacy `/opt/syseba` config, database, token, log, and service state can be
  migrated and rolled back by `scripts/syseba-maintenance.sh`.
