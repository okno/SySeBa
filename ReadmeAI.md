# SySeBa 2 - Manuale completo

![Logo SySeBa](SySeBa_Logo.webp)

[README italiano](README.it.md) | [English README](README.md) |
[Complete English guide](ReadmeAI.en.md)

Questo manuale descrive la versione nativa C11 di SySeBa: architettura,
installazione, configurazione, CLI, dashboard, Web UI, servizi, logging,
restore, sicurezza, prestazioni, aggiornamento, rollback e diagnosi.

## 1. Scopo e limiti

SySeBa mantiene tre alberi:

```text
source  --sincronizzazione iniziale/eventi--> backup
source  --eliminazione---------------------> backup -> restore
restore --ripristino controllato-----------> source
```

- `source` è l'origine autorevole.
- `backup` è la copia corrente.
- `restore` conserva gli elementi rimossi dalla sorgente.

All'avvio, salvo `--no-initial-sync`, SySeBa attraversa ricorsivamente
`source`, crea le directory mancanti e accoda i file diversi o assenti. Dopo
la scansione resta in ascolto degli eventi del filesystem.

SySeBa non è un sostituto di snapshot storage immutabili, backup offline,
versioning remoto o disaster recovery. Un errore hardware, un account
amministrativo compromesso o la perdita simultanea dei volumi può coinvolgere
tutti e tre gli alberi.

## 2. Novità della versione 2

- Runtime interamente in C11; Python, `watchdog`, `psutil` e `pip` non sono
  richiesti.
- Un singolo eseguibile contiene motore, CLI, dashboard, Web UI e gestione
  servizio.
- Watcher inotify e `ReadDirectoryChangesW`, con polling portabile di
  ripiego.
- Copie su file temporaneo esclusivo, verifica che la sorgente non cambi
  durante la lettura, flush e sostituzione atomica della destinazione.
- Lock di kernel verificabile; il file `.lock` può rimanere sul disco senza
  indicare che il processo sia attivo.
- Token Web casuale da 256 bit, file protetto e rifiuto di link simbolici.
- Migrazione SQLite transazionale, compresa l'aggiunta automatica di `level`.
- Console e Web UI ridisegnate e responsive.
- Pacchetti per Linux, Windows e macOS, più archivio sorgente.

## 3. Percorsi predefiniti

### Linux

| Elemento | Percorso |
|---|---|
| Eseguibile | `/usr/bin/syseba` oppure `/opt/syseba/syseba` dopo migrazione |
| Configurazione | `/etc/syseba/syseba.conf` |
| Database | `/var/lib/syseba/syseba_logs.db` |
| Token Web | `/etc/syseba/syseba_web.token` |
| Lock | `/run/syseba/syseba.lock` |
| Log testuale | `/var/log/syseba/syseba.log` |
| Unit | `/etc/systemd/system/syseba.service` o unit del pacchetto |

Il resolver accetta anche i vecchi percorsi sotto `/opt/syseba`, permettendo
una migrazione senza cambiare subito la configurazione.

### Windows

Lo stato risiede in `C:\ProgramData\SySeBa`: configurazione, database, token,
lock e log. Il servizio viene registrato come `SySeBa`.

### macOS

| Elemento | Percorso |
|---|---|
| Eseguibile | `/usr/local/bin/syseba` |
| Configurazione/token | `/usr/local/etc/syseba` |
| Database | `/usr/local/var/lib/syseba` |
| Lock | `/usr/local/var/run/syseba` |
| Log | `/usr/local/var/log/syseba` |
| LaunchDaemon | `/Library/LaunchDaemons/com.okno.syseba.plist` |

## 4. Configurazione

Formato INI compatibile con la versione storica:

```ini
[SETTINGS]
source = /storage/4TB
backup = /storage/6TB/Backup
restore = /storage/6TB/RESTORE
log = /var/log/syseba/syseba.log
threads = 5
```

| Chiave | Significato | Vincoli |
|---|---|---|
| `source` | Albero autorevole | Deve esistere ed essere leggibile |
| `backup` | Copia corrente | Scrivibile, distinta dagli altri alberi |
| `restore` | Elementi rimossi | Scrivibile, distinta dagli altri alberi |
| `log` | Log testuale | Directory padre creabile/scrivibile |
| `threads` | Worker di copia | Intero da `1` a `64` |

La validazione rifiuta:

- percorsi vuoti;
- radice filesystem come albero dati;
- sovrapposizione o annidamento fra source, backup e restore;
- log collocato dentro uno degli alberi monitorati;
- link o percorsi che risolvono fuori dalla radice attesa nelle operazioni di
  restore;
- numero di thread fuori intervallo.

Verifica prima dell'avvio:

```bash
syseba config-check --config /etc/syseba/syseba.conf --lang it
syseba config-check --config /etc/syseba/syseba.conf --json
```

Una modifica salvata dalla Web UI non cambia i path del processo già attivo.
La schermata mostra configurazione attiva e salvata; applicare con il pulsante
di riavvio o con il gestore servizi.

## 5. Installazione Linux

### Pacchetto Debian

```bash
sudo apt install ./syseba_2.0.0_amd64.deb
sudoedit /etc/syseba/syseba.conf
sudo syseba config-check --lang it
sudo systemctl enable --now syseba.service
```

### Pacchetto RPM

```bash
sudo rpm -Uvh syseba-2.0.0-1.x86_64.rpm
sudoedit /etc/syseba/syseba.conf
sudo syseba config-check --lang it
sudo systemctl enable --now syseba.service
```

### Build dal sorgente

Dipendenze Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build
```

Compilazione e test:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSYSEBA_BUILD_TESTS=ON \
  -DSYSEBA_ENABLE_HARDENING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo syseba service-install --config /etc/syseba/syseba.conf --lang it
```

Le dipendenze runtime SQLite, cJSON e CivetWeb sono vendorizzate e compilate
nel binario; non occorre installare le relative librerie di sviluppo.

## 6. Installazione Windows 11/Server

1. Estrarre il bundle ZIP o eseguire il setup NSIS.
2. Aprire PowerShell come amministratore.
3. Eseguire `.\install-service.ps1`.
4. Modificare `C:\ProgramData\SySeBa\syseba.conf`.
5. Riavviare con `Restart-Service SySeBa`.

Comandi utili:

```powershell
Get-Service SySeBa
Get-Content C:\ProgramData\SySeBa\syseba.log -Wait -Tail 100
Get-Content C:\ProgramData\SySeBa\syseba_web.token
Get-NetTCPConnection -LocalPort 8765 -State Listen
```

Il servizio usa l'account `LocalSystem`, avvio automatico e un SID di servizio.
Il token riceve una DACL protetta per SYSTEM, Administrators e proprietario.

## 7. Installazione macOS

Il DMG contiene un eseguibile Universal 2 per Intel x86_64 e Apple Silicon
arm64:

```bash
cd /Volumes/SySeBa
sudo ./install.sh
```

La release locale non è firmata né notarizzata. Prima di una distribuzione
pubblica occorrono identità Developer ID, `codesign`, notarizzazione Apple e
stapling. Per un test manuale controllato può essere necessario autorizzare il
file dalle impostazioni Privacy e sicurezza.

Log:

```bash
sudo launchctl print system/com.okno.syseba
log stream --predicate 'process == "syseba"' --style compact
tail -F /usr/local/var/log/syseba/syseba.log
```

## 8. CLI completa

```text
syseba [command] [options]

run               watcher, sincronizzazione e dashboard
status            stato, lock e dischi
logs              coda del log testuale
config-check      validazione configurazione
restore-list      elenco restore
restore-copy      ripristino non interattivo
restore-browser   navigazione testuale interattiva
service-install   installazione/abilitazione servizio
```

Opzioni globali:

| Opzione | Uso |
|---|---|
| `--config PATH` | Configurazione alternativa |
| `--lang it\|en` | Lingua CLI, dashboard e Web |
| `--silent` | Disabilita la dashboard ANSI |
| `--web` | Avvia Web UI insieme al watcher |
| `--web-only` | Solo Web UI, senza watcher |
| `--web-host ADDRESS` | Bind manuale, default `127.0.0.1` |
| `--web-port PORT` | Porta, default `8765` |
| `--web-token TOKEN` | Token esplicito, sconsigliato nella command line |
| `--web-token-file PATH` | Token persistente |
| `--no-web-auth` | Consentito solo su indirizzo loopback |
| `--no-initial-sync` | Salta la scansione iniziale |
| `--lockfile PATH` | Lock alternativo |
| `--db-path PATH` | Database alternativo |
| `--console-refresh SEC` | Frequenza dashboard |
| `--json` | Output strutturato |

Opzioni restore:

```text
--path RELATIVO
--search TESTO
--page N
--page-size N
--sort name|mtime|size
--direction asc|desc
--rename
--overwrite
```

Esempi:

```bash
syseba status --json
syseba logs --lines 500
syseba restore-list --search fattura --sort mtime --direction desc
syseba restore-copy --path clienti/fattura.pdf --rename
syseba restore-browser
```

## 9. Dashboard console

Senza `--silent`, `run` usa una vista adattiva:

- intestazione compatta;
- stato iniziale, watcher e Web;
- barre stabili per uso source, backup, restore, CPU e memoria;
- contatori di eventi, copie, cancellazioni, restore, errori e coda;
- percorsi e ultime attività;
- layout ridotto per terminali stretti o bassi.

Il rendering usa ANSI solo su terminali interattivi. In modalità servizio si
usa `--silent`; l'output operativo resta nel journal e nel log applicativo.

## 10. Web UI

Il servizio abilita automaticamente:

```text
http://IP_DEL_SERVER:8765
```

Recupero token Linux:

```bash
sudo cat /etc/syseba/syseba_web.token
```

La pagina iniziale e `/api/auth` sono pubblici per mostrare il form di accesso.
Tutte le API dati richiedono l'header:

```http
X-SySeBa-Token: <token>
```

Funzioni:

- riepilogo salute, uptime, watcher, sincronizzazione e code;
- CPU, memoria e spazio dei tre alberi;
- ultimi eventi e log filtrabili;
- visualizzazione e modifica validata della configurazione;
- indicazione di configurazione attiva/salvata;
- ricerca, ordinamento e paginazione restore;
- dettagli e download di un elemento;
- restore con strategia `fail`, `rename` o `overwrite`;
- riavvio controllato del servizio quando il gestore OS lo consente;
- cambio lingua italiano/inglese.

Il server integrato è HTTP, non HTTPS. Usarlo su LAN/VPN fidata, limitare la
porta col firewall e non pubblicarlo direttamente su Internet. Per accesso
remoto usare un reverse proxy TLS con ulteriore autenticazione.

## 11. Semantica di copia e restore

### Copia

Per ogni file:

1. vengono acquisiti metadati della sorgente;
2. viene creato un file temporaneo esclusivo accanto alla destinazione;
3. i byte sono copiati senza seguire link finali;
4. la sorgente è ricontrollata per dimensione, timestamp e identità;
5. il contenuto è sincronizzato su disco;
6. il temporaneo sostituisce atomicamente la destinazione;
7. la directory padre è sincronizzata dove supportato.

Se la sorgente cambia durante la copia, l'operazione viene ritentata. Un file
con stessa dimensione ma timestamp diverso viene comunque considerato.

### Eliminazione

Quando un elemento sparisce da `source`, la copia corrispondente viene spostata
da `backup` a `restore`. Un conflitto produce:

```text
nome.ext.YYYYMMDD-HHMMSS
nome.ext.YYYYMMDD-HHMMSS.1
```

### Ripristino

- `fail`: rifiuta se la destinazione esiste.
- `rename`: crea `nome.restored-YYYYMMDD-HHMMSS.ext`.
- `overwrite`: sostituisce il file o unisce una directory compatibile.

Tipi incompatibili fra sorgente restore e destinazione richiedono `rename`.
Traversal `..`, percorsi assoluti e attraversamento di link fuori radice sono
rifiutati.

## 12. Logging e database

SySeBa scrive:

1. log testuale leggibile;
2. tabella SQLite `logs`;
3. journal/Event Log/Unified Log per il ciclo di vita del servizio.

Schema logico:

```sql
CREATE TABLE logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  timestamp TEXT,
  level TEXT DEFAULT 'INFO',
  operation TEXT,
  source_path TEXT,
  target_path TEXT,
  additional_info TEXT
);
CREATE INDEX idx_logs_timestamp ON logs(timestamp);
```

Il database usa `WAL`, `synchronous=NORMAL`, busy timeout di 5 secondi e un
writer dedicato. All'apertura, una transazione `BEGIN IMMEDIATE` crea la tabella
o aggiunge singolarmente le colonne mancanti. Questo elimina in modo
permanente l'errore:

```text
sqlite3.OperationalError: table logs has no column named level
```

Non cancellare `-wal` e `-shm` mentre SySeBa è attivo.

Monitoraggio Linux:

```bash
sudo journalctl -fu syseba.service -o short-iso-precise
sudo tail -n 200 -F /var/log/syseba/syseba.log
sudo ./scripts/syseba-maintenance.sh follow
```

## 13. Migrazione dalla vecchia installazione

Usare un checkout separato dall'installazione attiva:

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

Lo script:

1. convalida percorsi e privilegi;
2. compila la sorgente C in una directory temporanea;
3. esegue unit test e integrazione disponibili;
4. valida la configurazione esistente;
5. arresta il servizio;
6. archivia installazione e stato esterno con ACL/xattr;
7. genera `SHA256SUMS` e manifest di identità;
8. conserva config, DB/WAL/SHM, log e token;
9. effettua uno switch atomico di directory;
10. scrive una unit systemd irrobustita e abilita Web autostart;
11. avvia e controlla servizio più `/api/auth`;
12. esegue rollback automatico se un passaggio post-switch fallisce.

Gli alberi `source`, `backup` e `restore` non vengono archiviati, spostati,
cancellati o sostituiti.

## 14. Backup software e rollback

Snapshot manuale:

```bash
cd /root
sudo /root/SySeBa-release/scripts/syseba-maintenance.sh backup
```

Elenco e verifica:

```bash
sudo ./scripts/syseba-maintenance.sh list
sudo ./scripts/syseba-maintenance.sh verify
```

Selettore testuale:

```bash
sudo ./scripts/syseba-maintenance.sh rollback
```

Scelte dirette:

```bash
sudo ./scripts/syseba-maintenance.sh rollback latest
sudo ./scripts/syseba-maintenance.sh rollback pre-update
sudo ./scripts/syseba-maintenance.sh rollback 20260723-023230
```

Il rollback verifica checksum e membri tar, mette l'installazione sostituita in
quarantena, ripristina unit e stato esterno, quindi controlla l'avvio. Se la
versione scelta non parte, rimette a posto l'installazione pre-rollback.

Quindi sì: lo snapshot `pre-update` riporta esattamente applicazione,
configurazione, database, token, log e unit allo stato fermo precedente. Non
può annullare modifiche avvenute nei volumi dati dopo l'aggiornamento, perché
quei volumi sono deliberatamente esclusi.

## 15. Aggiornamenti futuri

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/syseba-maintenance.sh quick-update main
```

`quick-update` confronta il commit remoto con `BUILD-INFO`, evita lavoro se
coincidono e usa lo stesso flusso test/snapshot/health-check/rollback.

## 16. Sicurezza

- Nessun interprete o modulo Python nel runtime.
- Dipendenze vendorizzate, versionate e riportate nelle note di terze parti.
- Token minimo 16 caratteri; quello generato contiene 256 bit casuali.
- Token espliciti con caratteri di controllo o troppo lunghi sono rifiutati.
- `--no-web-auth` è accettato soltanto su loopback.
- Body JSON massimo 64 KiB; pagina restore massima 250 elementi.
- Aperture sensibili rifiutano symlink/reparse point finali.
- Configurazione e token sono salvati con sostituzione atomica.
- Lock tramite `flock`/sharing Windows, non mediante sola presenza del file.
- Unit systemd con `NoNewPrivileges`, filesystem protetto, device privati,
  famiglie socket limitate, W^X e percorsi scrivibili espliciti.
- Restore contenuto sotto radici canoniche.

Il processo esegue come root/SYSTEM perché deve poter leggere e ripristinare
alberi arbitrari. Ridurre i permessi è possibile se tutti i volumi e i file di
stato appartengono a un account dedicato; testare ACL, mount e restore prima
di modificare la unit.

Dettagli e threat model: [docs/SECURITY.md](docs/SECURITY.md).

## 17. Prestazioni

`threads` controlla solo i worker di operazioni file. Il writer log e il
watcher hanno thread separati.

Indicazioni:

| Carico | Valore iniziale |
|---|---|
| HDD singolo | `2-4` |
| RAID/NAS con latenza | `4-8` |
| SSD/NVMe | `4-16` |
| File molto grandi | evitare concorrenza eccessiva |
| Milioni di file piccoli | aumentare gradualmente e osservare la coda |

Più thread non garantiscono più throughput: possono aumentare seek, cache
pressure, traffico di rete e contesa metadata. Usare Web UI/`status --json`
per confrontare coda, errori, CPU e memoria.

## 18. FAQ e troubleshooting

### La Web UI non risponde

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 150 --no-pager
```

La unit deve contenere `--web --web-host 0.0.0.0 --web-port 8765`.
Controllare firewall e indirizzo server.

### Il token non funziona

```bash
sudo stat /etc/syseba/syseba_web.token
sudo cat /etc/syseba/syseba_web.token
```

Rimuovere dalla sessione browser il vecchio token e inserire quello corrente.
Non sostituire il file con un link simbolico.

### `table logs has no column named level`

Verificare di eseguire il binario 2.x:

```bash
syseba --version
systemctl cat syseba.service | grep ExecStart
```

Arrestare eventuale vecchio processo Python e riavviare il servizio. Il binario
C migra il database prima di avviare il writer.

### Risulta già in esecuzione

```bash
syseba status
systemctl status syseba.service
```

Non basarsi sul fatto che `.lock` esista: il file persiste intenzionalmente.
`status` tenta il lock del kernel e riporta la reale proprietà.

### I file non vengono copiati

1. eseguire `config-check`;
2. controllare permessi e spazio;
3. leggere log applicativo e journal;
4. verificare che source, backup e restore non siano annidati;
5. verificare watcher e dimensione coda nella Web UI.

### Configurazione salvata ma non applicata

```bash
sudo systemctl restart syseba.service
```

La separazione è intenzionale per non cambiare alberi attivi a metà sessione.

### Il servizio termina subito

```bash
journalctl -u syseba.service -b -n 200 --no-pager
/usr/bin/syseba config-check --config /etc/syseba/syseba.conf
```

Cause tipiche: source assente, permessi, path sovrapposti, porta occupata,
token non regolare o seconda istanza.

### Come raccolgo un report diagnostico

```bash
syseba --version
syseba status --json
syseba config-check --json
systemctl status syseba.service --no-pager -l
journalctl -u syseba.service -b -n 300 --no-pager
```

Oscurare token e percorsi sensibili prima di condividere l'output.

## 19. Build, pacchetti e test

Build di tutti gli artefatti da Linux/WSL:

```bash
./scripts/build-release.sh
```

Da PowerShell:

```powershell
.\scripts\build-release.ps1
```

Verifica rapida:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
shellcheck scripts/*.sh tests_c/*.sh packaging/**/*.sh
```

La pipeline locale verifica test nativi, integrazione Linux, manutenzione,
eseguibile Windows, build macOS per entrambe le architetture, contenuto dei
pacchetti, compatibilità glibc del bundle Linux e checksum SHA-256.

I dettagli sono in:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/BUILD.md](docs/BUILD.md)
- [docs/PACKAGING.md](docs/PACKAGING.md)
- [docs/TESTING.md](docs/TESTING.md)
- [docs/API.md](docs/API.md)
- [docs/OPERATIONS.md](docs/OPERATIONS.md)
- [docs/MIGRATION.md](docs/MIGRATION.md)
- [docs/SECURITY.md](docs/SECURITY.md)

## 20. Licenze

SySeBa: MIT. SQLite è di pubblico dominio; cJSON è MIT; CivetWeb è MIT.
Consultare [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) per versioni,
provenienza e testi applicabili.
