# Documentazione tecnica SySeBa

[English](README.md) | [README del progetto](../README.it.md) |
[Manuale completo](../ReadmeAI.md)

Questo indice copre implementazione C nativa, linea Python conservata,
artefatti pubblicati, amministrazione, migrazione, API, sicurezza, test e
release engineering. Ogni documento tecnico ha una versione inglese.

## Stato corrente

| Elemento | Valore corrente |
|---|---|
| Release nativa | [SySeBa 2.0.0](https://github.com/okno/SySeBa/releases/tag/v2.0.0) |
| Release legacy | [SySeBa 1.0.0 Python](https://github.com/okno/SySeBa/releases/tag/v1.0.0-python) |
| Ramo di sviluppo | `main` |
| Ramo di manutenzione Python | `legacy/python` |
| Package OCI pubblico | [`ghcr.io/okno/syseba-packages`](https://github.com/okno/SySeBa/pkgs/container/syseba-packages) |
| Tag del package | `2.0.0`, `latest` |
| Digest del package | `sha256:823bfa56d87f2ed3deb817c4483cfe4e5951139e4820bae4a69473f0790173f8` |

GitHub Releases è il canale primario per il download diretto. GitHub Packages
è una replica OCI pubblica e statica che contiene tutti i file sotto
`/packages`; non è un'immagine del servizio SySeBa da eseguire.

## Mappa della documentazione

| Argomento | Italiano | English |
|---|---|---|
| Architettura e runtime | [Architettura](ARCHITECTURE.it.md) | [Architecture](ARCHITECTURE.md) |
| Build e release engineering | [Build](BUILD.it.md) | [Build](BUILD.md) |
| Pacchetti e installer | [Packaging](PACKAGING.it.md) | [Packaging](PACKAGING.md) |
| Migrazione e rollback | [Migrazione](MIGRATION.it.md) | [Migration](MIGRATION.md) |
| Gestione servizio e osservabilità | [Operatività](OPERATIONS.it.md) | [Operations](OPERATIONS.md) |
| API HTTP/Web | [API](API.it.md) | [API](API.md) |
| Threat model e hardening | [Sicurezza](SECURITY.it.md) | [Security](SECURITY.md) |
| Test automatici e manuali | [Test](TESTING.it.md) | [Testing](TESTING.md) |
| Note release C nativa | [2.0.0](releases/v2.0.0.it.md) | [2.0.0](releases/v2.0.0.md) |
| Note release Python legacy | [1.0.0 Python](releases/v1.0.0-python.it.md) | [1.0.0 Python](releases/v1.0.0-python.md) |
| Cronologia modifiche | [Changelog](../CHANGELOG.it.md) | [Changelog](../CHANGELOG.md) |
| Riepilogo terze parti | [Riepilogo](../THIRD_PARTY_NOTICES.it.md) | [Notice autorevoli](../THIRD_PARTY_NOTICES.md) |

## Verifica della distribuzione

Dopo il download da una Release:

```bash
sha256sum -c SHA256SUMS
```

Da GitHub Packages:

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
cd syseba-packages
sha256sum -c SHA256SUMS
```

Gli eseguibili Windows non sono attualmente firmati. Il DMG macOS non è
firmato né notarizzato ed è stato verificato strutturalmente tramite
cross-build, non eseguito su hardware Apple. Prima dell'uso produttivo consulta
[Sicurezza](SECURITY.it.md), [Test](TESTING.it.md) e
[Packaging](PACKAGING.it.md).

## Politica documentale

- I comandi si riferiscono a SySeBa 2.0.0, salvo le sezioni marcate Python
  Legacy.
- I percorsi runtime sono predefiniti; pacchetti e migrazione possono
  conservare lo stato storico sotto `/opt/syseba`.
- Gli esempi usano la porta `8765`, che rimane configurabile.
- Le API JSON possono aggiungere nuovi campi; i client devono ignorare quelli
  sconosciuti.
- I documenti italiani e inglesi devono essere aggiornati insieme.
