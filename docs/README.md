# SySeBa Technical Documentation

[Italiano](README.it.md) | [Project README](../README.md) |
[Complete manual](../ReadmeAI.en.md)

This index covers the native C implementation, the retained Python line,
published artifacts, administration, migration, API, security, testing, and
release engineering. Every technical document has an Italian counterpart.

## Current Status

| Item | Current value |
|---|---|
| Native release | [SySeBa 2.0.0](https://github.com/okno/SySeBa/releases/tag/v2.0.0) |
| Legacy release | [SySeBa 1.0.0 Python](https://github.com/okno/SySeBa/releases/tag/v1.0.0-python) |
| Development branch | `main` |
| Python maintenance branch | `legacy/python` |
| Public OCI package | [`ghcr.io/okno/syseba-packages`](https://github.com/okno/SySeBa/pkgs/container/syseba-packages) |
| Package tags | `2.0.0`, `latest` |
| Package digest | `sha256:823bfa56d87f2ed3deb817c4483cfe4e5951139e4820bae4a69473f0790173f8` |

GitHub Releases is the primary direct-download channel. GitHub Packages is a
public static OCI mirror that carries all release files under `/packages`; it
is not a runnable SySeBa service image.

## Documentation Map

| Topic | English | Italiano |
|---|---|---|
| Architecture and runtime | [Architecture](ARCHITECTURE.md) | [Architettura](ARCHITECTURE.it.md) |
| Build and release engineering | [Build](BUILD.md) | [Build](BUILD.it.md) |
| Packages and installers | [Packaging](PACKAGING.md) | [Packaging](PACKAGING.it.md) |
| Installation migration and rollback | [Migration](MIGRATION.md) | [Migrazione](MIGRATION.it.md) |
| Service operation and observability | [Operations](OPERATIONS.md) | [Operatività](OPERATIONS.it.md) |
| HTTP/Web API | [API](API.md) | [API](API.it.md) |
| Threat model and hardening | [Security](SECURITY.md) | [Sicurezza](SECURITY.it.md) |
| Automated and manual testing | [Testing](TESTING.md) | [Test](TESTING.it.md) |
| Native C release notes | [2.0.0](releases/v2.0.0.md) | [2.0.0](releases/v2.0.0.it.md) |
| Python legacy release notes | [1.0.0 Python](releases/v1.0.0-python.md) | [1.0.0 Python](releases/v1.0.0-python.it.md) |
| Change history | [Changelog](../CHANGELOG.md) | [Changelog](../CHANGELOG.it.md) |
| Third-party summary | [Authoritative notices](../THIRD_PARTY_NOTICES.md) | [Riepilogo](../THIRD_PARTY_NOTICES.it.md) |

## Distribution Verification

After downloading a Release:

```bash
sha256sum -c SHA256SUMS
```

From GitHub Packages:

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
cd syseba-packages
sha256sum -c SHA256SUMS
```

The Windows executables are currently unsigned. The macOS DMG is unsigned,
not notarized, and structurally cross-verified rather than executed on Apple
hardware. Review [Security](SECURITY.md), [Testing](TESTING.md), and
[Packaging](PACKAGING.md) before production deployment.

## Documentation Policy

- Commands are written for SySeBa 2.0.0 unless a section explicitly says
  Python Legacy.
- Runtime paths are defaults; package and migration layouts can preserve
  historical `/opt/syseba` state.
- Examples use port `8765`, but the port remains configurable.
- New fields may be added to JSON APIs; clients must ignore unknown fields.
- English and Italian documents should be changed together.
