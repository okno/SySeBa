# Packaging

## Artifact Set

The release directory contains:

```text
syseba-2.0.0-linux-x86_64.tar.gz
syseba_2.0.0_amd64.deb
syseba-2.0.0-1.x86_64.rpm
SySeBa-2.0.0-windows-x86_64.zip
SySeBa-2.0.0-windows-x86_64-setup.exe
SySeBa-2.0.0-macos-universal.dmg
syseba-2.0.0-source.tar.gz
release-manifest.txt
SHA256SUMS
```

Names can contain CPack platform suffix differences; the manifest records the
actual final names, sizes, hashes, compiler identity, and test status.

## GitHub Packages OCI Bundle

GitHub Packages does not provide a generic binary registry. The complete
release directory is therefore mirrored as a `scratch` OCI image at
`ghcr.io/okno/syseba-packages`. The image contains no operating system or
runtime process; every release asset is stored below `/packages`.

Build from an already verified release directory:

```bash
docker build \
  --file packaging/oci/Dockerfile.packages \
  --build-arg SYSEBA_VERSION=2.0.0 \
  --build-arg SYSEBA_REVISION="$(git rev-parse HEAD)" \
  --tag ghcr.io/okno/syseba-packages:2.0.0 \
  dist/syseba-2.0.0
docker tag ghcr.io/okno/syseba-packages:2.0.0 \
  ghcr.io/okno/syseba-packages:latest
```

The `org.opencontainers.image.source` label associates the package with this
repository. Publish only after `sha256sum -c SHA256SUMS` succeeds.

The manual `.github/workflows/publish-packages.yml` workflow performs this
operation with the repository-scoped `GITHUB_TOKEN`. It downloads the selected
GitHub Release, validates the version, verifies every checksum and expected
filename, and publishes both the immutable version tag and `latest`.

Extract without running the image:

```bash
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
```

## Linux Portable Bundle

The executable is cross-compiled against a glibc 2.17 ABI target. The bundle
contains config template, service unit, maintenance tool, license, notices,
and documentation. It is not a package-manager installation; administrators
must place files and run `service-install`.

## DEB

The Debian package uses the same glibc 2.17-targeted executable as the
portable bundle and declares `libc6 (>= 2.17)`. It installs:

- `/usr/bin/syseba`;
- `/usr/sbin/syseba-maintenance`;
- `/etc/syseba/syseba.conf`;
- `/usr/lib/systemd/system/syseba.service`;
- documentation under `/usr/share/doc/syseba`.

Maintainer scripts create state directories, reload systemd, preserve
configuration, and avoid deleting user data on uninstall.

Inspect:

```bash
dpkg-deb --info package.deb
dpkg-deb --contents package.deb
```

## RPM

The RPM carries the equivalent filesystem layout and lifecycle scripts. Its
generated symbol requirements do not exceed `GLIBC_2.17`. Inspect:

```bash
rpm -qip package.rpm
rpm -qlp package.rpm
rpm -qp --scripts package.rpm
```

Package removal must not remove source, backup, restore, config, DB, token, or
logs.

## Windows

The ZIP is portable and includes service install/uninstall PowerShell scripts.
The NSIS executable installs the same layout. Service registration is
explicit so configuration can be reviewed before LocalSystem begins watching
data.

Before public release:

1. sign `SySeBa.exe` and setup with Authenticode;
2. timestamp signatures;
3. scan exact hashes with organizational malware controls;
4. test upgrade/uninstall on Windows 11 and supported Server editions.

## macOS DMG

The builder cross-compiles x86_64 and arm64 slices, creates one Universal 2
Mach-O, stages config/plist/install scripts, writes an HFS+ image, and
compresses it as DMG.

Structural verification extracts the DMG and confirms the universal binary.
Before public release, sign executable and scripts as applicable, notarize the
DMG, staple the ticket, and test on real Intel and Apple Silicon hosts.

The open-source `libdmg-hfsplus` build tool is external to the distributed
SySeBa runtime. See its upstream GPLv3 license when reproducing the DMG build.

## Source Archive

The source tarball includes CMake, first-party C, tests, scripts, Web assets,
packaging metadata, docs, and vendored dependency sources/licenses. It excludes
VCS metadata and all runtime state.

## Integrity

Verify:

```bash
sha256sum -c SHA256SUMS
```

Local SHA-256 proves transfer integrity against the local manifest. Public
authenticity requires a detached signature or signed release channel.
