# Build e release engineering

[English](BUILD.md) | [Indice documentazione](README.it.md)

## Target supportati

| Target | Compilatore/toolchain | Runtime minimo previsto |
|---|---|---|
| Linux x86_64 nativo | GCC/Clang | Distribuzione host di build |
| Linux x86_64 portabile | Zig cc, target glibc 2.17 | Distribuzioni glibc moderne |
| Windows x86_64 | MinGW-w64 o MSVC | Windows 11 e Server supportati |
| macOS x86_64 | Zig cc | macOS 11 |
| macOS arm64 | Zig cc | macOS 11 |

I binari macOS thin vengono uniti da `scripts/make_universal_macho.py`.
L'helper valida magic Mach-O, CPU type, offset, allineamento e cardinalità
degli input prima di scrivere il file fat.

## Opzioni CMake

| Opzione | Default | Scopo |
|---|---|---|
| `SYSEBA_BUILD_TESTS` | `ON` | Compila i test nativi |
| `SYSEBA_ENABLE_HARDENING` | `ON` | Hardening compilatore/linker |
| `SYSEBA_ENABLE_NATIVE_WATCHER` | `ON` | Watcher nativo dove disponibile |
| `BUILD_TESTING` | `ON` | Abilita CTest |

## Build nativa di sviluppo

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Sanitizer

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

L'amalgamation SQLite è esclusa dalla strumentazione perché applicarla a
quell'unità generata da oltre 9 MB è eccessivamente costoso. Codice SySeBa,
cJSON, CivetWeb, test e interfacce rimangono strumentati.

ThreadSanitizer dipende dal kernel e dal layout degli indirizzi virtuali.
Alcuni kernel WSL terminano TSan prima di `main` con `unexpected memory
mapping`: va registrato come limite dell'ambiente e TSan deve essere eseguito
su CI Linux nativa.

## Analisi statica

```bash
cppcheck --force --enable=warning,style,performance,portability \
  --std=c11 --inline-suppr \
  -I include -I src -I third_party/cjson \
  src include tests_c/test_native.c
shellcheck scripts/*.sh tests_c/*.sh packaging/**/*.sh
```

Le amalgamation di terze parti non vengono trattate come finding del codice
proprietario. Versioni e provenienza sono in `THIRD_PARTY_NOTICES.md`.

## Release locale completa

```bash
./scripts/build-release.sh
```

Lo script compila in una directory temporanea ext4 per non alterare owner e
mode DEB/RPM tramite filesystem montati da Windows. DEB, RPM e bundle
portabile usano il target verificato glibc 2.17. Solo gli artefatti terminati
vengono copiati in `dist/syseba-2.0.0`.

Da PowerShell:

```powershell
.\scripts\build-release.ps1
```

Variabili facoltative:

| Variabile | Significato |
|---|---|
| `SYSEBA_RELEASE_ROOT` | Radice temporanea Linux |
| `SYSEBA_DIST_DIR` | Directory artefatti finali |
| `ZIG` | Eseguibile Zig |
| `HFSPLUS_BIN` | Eseguibile `hfsplus` |
| `DMG_BIN` | Eseguibile `dmg` |
| `EMPTY_HFS_IMAGE` | Template HFS+ vuoto |

Lo script non invoca mai `git push`, upload di package o API di release.

## Workflow di pubblicazione

La pubblicazione è separata dalla compilazione:

1. tag del sorgente testato e creazione GitHub Release;
2. caricamento dei sette artefatti piattaforma/sorgente, `SHA256SUMS` e
   `release-manifest.txt`;
3. avvio manuale di `.github/workflows/publish-packages.yml`;
4. download della Release sul runner, verifica checksum e set esatto di nove
   file, pubblicazione di `ghcr.io/okno/syseba-packages`;
5. verifica pagina pubblica, tag, digest e pull anonimo.

Il workflow concede soltanto:

```yaml
permissions:
  contents: read
  packages: write
```

Usa il `GITHUB_TOKEN` della repository; non conserva un token Packages
personale. L'action checkout è fissata a uno SHA completo. L'immagine OCI usa
`scratch` e trasporta file statici sotto `/packages`.

Avvio per una release nota:

```bash
gh workflow run publish-packages.yml \
  --repo okno/SySeBa \
  --ref main \
  -f version=2.0.0
```

Il package corrente è pubblico come
`ghcr.io/okno/syseba-packages:2.0.0`, con alias `latest`.

## Note di riproducibilità

- Gli asset incorporati sono ordinati per percorso deterministico.
- Gli archivi sorgente escludono VCS, build, stato, token, lock, DB e log.
- Le slice Universal Mach-O usano allineamento fisso 16 KiB.
- I timestamp seguono ancora il tempo di build; per output byte-identico serve
  una futura politica `SOURCE_DATE_EPOCH` su CPack, NSIS, HFS e compressori.
- `SHA256SUMS` e `release-manifest.txt` identificano ogni artefatto.
- Un nuovo mirror OCI può cambiare digest se cambiano i metadata del layer,
  pur mantenendo payload identici. `SHA256SUMS` della Release resta
  l'autorità per i singoli file.
