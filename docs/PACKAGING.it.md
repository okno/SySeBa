# Packaging

[English](PACKAGING.md) | [Indice documentazione](README.it.md)

## Set di artefatti

La directory release contiene:

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

CPack può produrre differenze nei suffissi di piattaforma; il manifest
registra nomi finali, dimensioni, hash, compilatori e stato test.

## Bundle OCI GitHub Packages

GitHub Packages non offre un registro binario generico. L'intera directory di
release viene quindi replicata come immagine OCI `scratch` in
`ghcr.io/okno/syseba-packages`. Non contiene sistema operativo né processo
runtime; tutti gli asset sono sotto `/packages`.

Build da una release già verificata:

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

La label `org.opencontainers.image.source` associa il package alla repository.
Pubblicare soltanto dopo il successo di `sha256sum -c SHA256SUMS`.

Il workflow manuale `.github/workflows/publish-packages.yml` usa il
`GITHUB_TOKEN` limitato alla repository. Scarica la Release selezionata,
valida versione, checksum e nomi attesi e pubblica tag versione più `latest`.

Package corrente:

```text
pagina: https://github.com/okno/SySeBa/pkgs/container/syseba-packages
image:  ghcr.io/okno/syseba-packages:2.0.0
alias:  ghcr.io/okno/syseba-packages:latest
digest: sha256:823bfa56d87f2ed3deb817c4483cfe4e5951139e4820bae4a69473f0790173f8
```

Il package è pubblico, collegato a `okno/SySeBa` e scaricabile senza login. La
piattaforma OCI mostrata dagli strumenti descrive il carrier statico, non i
sistemi operativi degli artefatti al suo interno.

Estrazione senza esecuzione:

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
cd syseba-packages
sha256sum -c SHA256SUMS
```

PowerShell:

```powershell
$container = docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true
docker cp "${container}:/packages" .\syseba-packages
docker rm $container
Push-Location .\syseba-packages
Get-Content .\SHA256SUMS | ForEach-Object {
    $expected, $file = $_ -split '\s+', 2
    $actual = (Get-FileHash -LiteralPath $file.Trim() -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "Checksum non valido: $file" }
}
Pop-Location
```

## Bundle Linux portabile

L'eseguibile è cross-compilato per ABI glibc 2.17. Il bundle include template
config, unit servizio, strumento manutenzione, licenza, notice e
documentazione. Non è un'installazione gestita dal package manager:
l'amministratore deve posizionare i file ed eseguire `service-install`.

## DEB

Il pacchetto Debian usa lo stesso eseguibile glibc 2.17 e dichiara
`libc6 (>= 2.17)`. Installa:

- `/usr/bin/syseba`;
- `/usr/sbin/syseba-maintenance`;
- `/etc/syseba/syseba.conf`;
- `/usr/lib/systemd/system/syseba.service`;
- documentazione sotto `/usr/share/doc/syseba`.

Gli script maintainer creano le directory stato, ricaricano systemd,
conservano la configurazione e non cancellano dati utente durante uninstall.

Ispezione:

```bash
dpkg-deb --info package.deb
dpkg-deb --contents package.deb
```

## RPM

L'RPM ha layout e script lifecycle equivalenti. I requisiti simboli generati
non superano `GLIBC_2.17`.

```bash
rpm -qip package.rpm
rpm -qlp package.rpm
rpm -qp --scripts package.rpm
```

La rimozione non deve cancellare source, backup, restore, config, DB, token o
log.

## Windows

Lo ZIP è portabile e include script PowerShell di installazione/rimozione
servizio. Il setup NSIS installa lo stesso layout. La registrazione servizio è
esplicita per consentire la revisione della configurazione prima che
LocalSystem inizi a osservare i dati.

I file Windows 2.0.0 pubblicati non sono attualmente firmati. Prima di
considerarli affidabili per produzione:

1. firma `SySeBa.exe` e setup con Authenticode;
2. applica timestamp alle firme;
3. analizza gli hash esatti con i controlli malware aziendali;
4. prova upgrade/uninstall su Windows 11 e Server supportati.

## DMG macOS

Il builder cross-compila slice x86_64 e arm64, crea un Mach-O Universal 2,
prepara config/plist/script, scrive un'immagine HFS+ e la comprime in DMG.

La verifica strutturale estrae il DMG e conferma il binario universale. Il DMG
2.0.0 pubblicato non è firmato né notarizzato. Prima dell'uso produttivo
occorrono firma, notarizzazione, stapling e test su host Intel e Apple Silicon.

Lo strumento open source `libdmg-hfsplus` è esterno al runtime distribuito.
Consulta la sua licenza GPLv3 per riprodurre il build DMG.

## Archivio sorgente

Il tar sorgente include CMake, C proprietario, test, script, asset Web,
metadata packaging, documentazione e dipendenze vendorizzate con licenze.
Esclude metadata VCS e tutto lo stato runtime.

## Integrità e autenticità

Verifica:

```bash
sha256sum -c SHA256SUMS
```

SHA-256 dimostra l'integrità del trasferimento rispetto al manifest. Il digest
OCI identifica l'oggetto registry, mentre `SHA256SUMS` identifica i singoli
payload. I tag Git correnti sono annotati ma non firmati
crittograficamente; non sono ancora presenti Authenticode, Developer ID o
firma detached della release.
