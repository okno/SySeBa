# SySeBa 2

![SySeBa logo](SySeBa_Logo.webp)

[Italiano](README.it.md) | [Complete English guide](ReadmeAI.en.md) |
[Guida completa italiana](ReadmeAI.md) |
[Technical documentation](docs/README.md)

SySeBa is a native C11 continuous backup service for Windows, Linux, and
macOS. It performs a parallel initial synchronization, watches the source
tree for changes, maintains the current copy in `backup`, and moves deleted
items into a browsable `restore` area.

Version 2 has no Python runtime dependency. One executable provides the
filesystem engine, bilingual console dashboard, CLI, SQLite audit log,
token-protected Web UI, restore browser, and native service integration.

## Release Lines

- **[SySeBa 2.0.0 (native C)](https://github.com/okno/SySeBa/releases/tag/v2.0.0)**
  is the current published release, with ready-to-install artifacts for Linux,
  Windows, and macOS. Development continues on `main`.
- **SySeBa 1.x (Python Legacy)** remains available from the
  [`legacy/python`](https://github.com/okno/SySeBa/tree/legacy/python) branch
  and the
  [GitHub Releases](https://github.com/okno/SySeBa/releases/tag/v1.0.0-python)
  page for compatibility and exact rollback.

Both implementations remain downloadable. New platform support and feature
development target the native C line; the Python line is retained for
existing installations and critical compatibility or security corrections.

The source used for the 2.0.0 binaries is tagged `v2.0.0`. Documentation and
package-publication automation may advance on `main` without changing those
immutable release assets.

## Highlights

- Native watchers: inotify on Linux, `ReadDirectoryChangesW` on Windows, and
  a portable polling fallback.
- Bounded worker model with a dedicated serialized log writer.
- Atomic file replacement, source stability checks, retry handling, and
  single-instance kernel locking.
- Automatic migration of every historical SQLite `logs` schema, including
  databases that do not yet contain the `level` column.
- Italian and English CLI, console dashboard, Web UI, messages, and guides.
- Web status, process and disk metrics, live logs, validated configuration
  editing, service restart, restore search, preview, download, and recovery.
- Hardened systemd unit, Windows Service, and macOS LaunchDaemon support.
- Consistent stopped-state snapshots, exact-version rollback, checksums, and
  automatic rollback after a failed upgrade.

## Release Artifacts

Download published versions from
[GitHub Releases](https://github.com/okno/SySeBa/releases). The local release
builder produces:

| Platform | Published file |
|---|---|
| Linux x86_64 | `syseba-2.0.0-linux-x86_64.tar.gz` |
| Debian/Ubuntu x86_64 | `syseba_2.0.0_amd64.deb` |
| RPM Linux x86_64 | `syseba-2.0.0-1.x86_64.rpm` |
| Windows 11/Server x86_64 | `SySeBa-2.0.0-windows-x86_64.zip` and `SySeBa-2.0.0-windows-x86_64-setup.exe` |
| macOS 11+ Intel/Apple Silicon | `SySeBa-2.0.0-macos-universal.dmg` |
| Source | `syseba-2.0.0-source.tar.gz` with vendored dependencies |

Run `scripts/build-release.sh` under Linux/WSL, or
`scripts/build-release.ps1` from Windows. Artifacts are written to `dist/`;
the scripts never publish or push anything.

Every release includes `SHA256SUMS` and `release-manifest.txt`. Verify them
before installation:

```bash
sha256sum -c SHA256SUMS
```

### GitHub Packages

The same verified artifacts are mirrored in the
[`ghcr.io/okno/syseba-packages`](https://github.com/okno/SySeBa/pkgs/container/syseba-packages)
OCI package. It is a static artifact carrier, not a runtime container:

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
```

Validate the extracted files with `sha256sum -c syseba-packages/SHA256SUMS`.
The public package currently exposes `2.0.0` and `latest`; both resolve to
digest
`sha256:823bfa56d87f2ed3deb817c4483cfe4e5951139e4820bae4a69473f0790173f8`.
The manual
[`publish-packages.yml`](.github/workflows/publish-packages.yml) workflow
downloads the GitHub Release, verifies all expected files and hashes, and
publishes it with a repository-scoped `GITHUB_TOKEN`.

## Linux Install

Package install:

```bash
sudo apt install ./syseba_2.0.0_amd64.deb
sudoedit /etc/syseba/syseba.conf
sudo systemctl enable --now syseba.service
```

Source build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo syseba service-install --config /etc/syseba/syseba.conf --lang en
```

Default Linux configuration:

```ini
[SETTINGS]
source = /srv/syseba/source
backup = /srv/syseba/backup
restore = /srv/syseba/restore
log = /var/log/syseba/syseba.log
threads = 4
```

## Upgrade From the Python Release

Do not overwrite `/opt/syseba` manually. From a separate checkout:

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

The migration tool builds and tests the C candidate while the old daemon is
still running, stops the service, creates a checksummed snapshot in the
current directory, preserves configuration/database/log/token state, switches
the installation atomically, starts the Web-enabled service, and performs a
health check. A failed health check restores the previous installation
automatically.

Exact rollback:

```bash
sudo ./scripts/syseba-maintenance.sh rollback
sudo ./scripts/syseba-maintenance.sh rollback pre-update
```

The interactive command lists the available software snapshots before asking
for confirmation. Source, backup, and restore data trees are never included
in or changed by software snapshots.

## Web UI and Logs

The installed service starts the Web UI automatically on port `8765`:

```text
http://SERVER_IP:8765
```

Linux token and logs:

```bash
sudo cat /etc/syseba/syseba_web.token
sudo journalctl -fu syseba.service -o short-iso-precise
sudo tail -n 200 -F /var/log/syseba/syseba.log
```

The built-in server is HTTP. Restrict port `8765` to a trusted LAN/VPN or put
it behind an authenticated TLS reverse proxy. It is not intended for direct
Internet exposure.

## CLI

```text
syseba run
syseba status [--json]
syseba logs --lines 200
syseba config-check
syseba restore-list [--search TEXT] [--json]
syseba restore-copy --path RELATIVE [--rename|--overwrite]
syseba restore-browser
syseba service-install
```

Run `syseba --help --lang en` for every option.

## Documentation

- [Bilingual technical documentation index](docs/README.md)
- [Complete English guide](ReadmeAI.en.md)
- [Guida completa italiana](ReadmeAI.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Build and release engineering](docs/BUILD.md)
- [Security model](docs/SECURITY.md)
- [Operations and observability](docs/OPERATIONS.md)
- [Migration and rollback](docs/MIGRATION.md)
- [HTTP API](docs/API.md)
- [Testing](docs/TESTING.md)
- [Packaging](docs/PACKAGING.md)
- [2.0.0 release notes](docs/releases/v2.0.0.md)
- [Changelog](CHANGELOG.md)

Every technical guide has an Italian counterpart linked from the
documentation index.

## License

SySeBa is released under the MIT License. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
