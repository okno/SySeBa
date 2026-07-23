# Third-Party Notices

SySeBa includes the following source dependencies in its executable.

## SQLite 3.53.3

Upstream: <https://www.sqlite.org/>

Files: `third_party/sqlite/sqlite3.c`, `sqlite3.h`

SQLite is in the public domain. See the upstream copyright page:
<https://www.sqlite.org/copyright.html>.

## cJSON 1.7.19

Upstream: <https://github.com/DaveGamble/cJSON>

Files: `third_party/cjson/`

License: MIT. The complete license text is in
`third_party/cjson/LICENSE`.

## CivetWeb 1.16

Upstream: <https://github.com/civetweb/civetweb>

Files: `third_party/civetweb/`

License: MIT. The complete license text is in
`third_party/civetweb/LICENSE.md`.

## Release Build Tools

Zig and `libdmg-hfsplus` may be used by the local cross-platform release
builder. They are build tools and are not linked into or distributed as part
of the SySeBa runtime.

- Zig: <https://ziglang.org/>, MIT license.
- libdmg-hfsplus: <https://github.com/mozilla/libdmg-hfsplus>, GPLv3.
- NSIS: <https://nsis.sourceforge.io/>, zlib/libpng license.
- CMake/CPack: <https://cmake.org/>, BSD-3-Clause.

Review upstream notices when redistributing tool binaries rather than only
the artifacts they produce.
