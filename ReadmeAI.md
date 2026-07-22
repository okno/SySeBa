# SySeBa AI Edition - Guida completa

SySeBa e un servizio Python per sincronizzare in tempo reale una directory sorgente verso una directory di backup, conservando i file cancellati in una area restore. Questa versione aggiunge una dashboard console piu leggibile, un servizio web integrato, API JSON, consultazione dell'area restore e modifica della configurazione da browser.

Questa documentazione descrive la versione corrente, mantenendo compatibili configurazione e modalita di avvio delle versioni precedenti.

## Novita principali

- Motore SySeBa rifattorizzato in classi dedicate: configurazione, lock, daemon, logging, console dashboard e web server.
- Dashboard console adattiva alla larghezza e all'altezza del terminale, con output non interattivo privo di refresh ripetuti.
- Web dashboard integrata senza Flask o altri framework web: usa solo la libreria standard Python.
- Autenticazione token per API e azioni web, con header di sicurezza HTTP.
- Endpoint API per stato completo, log, configurazione e area restore.
- Modifica della configurazione da web con salvataggio su `syseba.conf`.
- Navigazione dell'area restore con ricerca, ordinamento, paginazione e breadcrumb.
- Download dei file presenti nell'area restore.
- Ripristino guidato con rilevamento conflitti, nuovo nome timestamp oppure sovrascrittura/unione esplicita.
- Log Web filtrabili per testo e livello, refresh automatico, copia e download.
- Configurazione Web con confronto tra valori attivi e valori salvati e indicazione persistente del riavvio richiesto.
- Web UI responsive, navigabile da tastiera e dotata di ruoli e notifiche accessibili.
- Comandi CLI operativi per stato, log, validazione config e restore.
- Logging su file e SQLite con livello, operazione, sorgente, destinazione e info aggiuntive.
- Lock process piu robusto: se trova un PID vecchio non piu attivo, lo sostituisce.
- Shutdown piu pulito di observer, worker, logging thread e web server.
- Supporto `--web-only` per consultare stato/log/config/restore anche senza avviare il watcher.

## File del pacchetto

```text
syseba.py          Motore, console dashboard, web server e CLI
syseba_web.js      Logica della Web UI, servita direttamente da SySeBa
syseba.conf        Configurazione base
syseba.lang        Etichette lingua italiana/inglese
requirements.txt  Dipendenze Python
README.md         README originale repo
ReadmeAI.md       Questa guida completa
LICENSE           Licenza
SySeBa_Logo.webp  Logo usato dalla web dashboard
```

## Requisiti

- Linux per l'uso reale come daemon di backup.
- Python 3.8 o superiore.
- Permessi di lettura sulla `source`.
- Permessi di scrittura su `backup`, `restore`, log, database e lock file.
- Dipendenze:
  - `watchdog`
  - `psutil`

Installazione dipendenze:

```bash
python3 -m pip install -r requirements.txt
```

Su sistemi minimal potrebbe servire:

```bash
sudo apt update
sudo apt install python3 python3-pip python3-venv
```

## Installazione consigliata

Clona o copia il pacchetto in `/opt/syseba`:

```bash
sudo mkdir -p /opt/syseba
sudo cp -r * /opt/syseba/
cd /opt/syseba
sudo python3 -m pip install -r requirements.txt
```

Se preferisci usare virtualenv:

```bash
cd /opt/syseba
sudo python3 -m venv .venv
sudo .venv/bin/pip install -r requirements.txt
sudo .venv/bin/python syseba.py --help
```

In quel caso modifica il servizio systemd per usare `/opt/syseba/.venv/bin/python`.

## Configurazione

File standard:

```ini
#Backup config by okno
[SETTINGS]
source = /storage/4TB
backup = /storage/6TB/Backup
restore = /storage/6TB/RESTORE
log = /var/log/syseba.log
threads = 5
```

Campi:

| Campo | Descrizione |
|---|---|
| `source` | Directory da monitorare e sincronizzare |
| `backup` | Directory dove copiare/aggiornare i file |
| `restore` | Directory dove spostare le copie dei file cancellati dalla source |
| `log` | File log testuale |
| `threads` | Numero di worker che processano gli eventi filesystem |

Percorsi relativi sono risolti rispetto alla directory del file di configurazione. In produzione e consigliato usare sempre percorsi assoluti.

## Avvio console

```bash
sudo python3 /opt/syseba/syseba.py --config /opt/syseba/syseba.conf
```

La dashboard console mostra:

- stato runtime e PID;
- uptime e ora;
- uso disco di source, backup e restore;
- CPU, RAM, thread e coda eventi;
- contatori di copie, modifiche, cancellazioni, restore ed errori;
- avanzamento della sync iniziale;
- ultimi eventi log.

Per uscire in modo pulito usa `Ctrl+C`.

## Avvio silent

Utile per systemd o sessioni senza interfaccia console:

```bash
sudo python3 /opt/syseba/syseba.py --silent --config /opt/syseba/syseba.conf
```

## Comandi CLI operativi

Senza comando SySeBa continua ad avviare il watcher come nelle versioni precedenti. I comandi disponibili sono:

| Comando | Funzione |
|---|---|
| `run` | Avvia il watcher; e il comportamento predefinito |
| `status` | Mostra lock, PID, configurazione e spazio disco |
| `logs` | Legge le ultime righe del log |
| `config-check` | Valida configurazione, directory mancanti e sovrapposizioni pericolose |
| `restore-list` | Elenca, cerca, ordina e pagina l'area restore |
| `restore-copy` | Ripristina un elemento dalla CLI |
| `service-install` | Crea e abilita il servizio systemd |

Esempi:

```bash
python3 syseba.py status --config /opt/syseba/syseba.conf
python3 syseba.py logs --lines 250
python3 syseba.py config-check --json
python3 syseba.py restore-list --path Documenti --search fattura --page-size 25
python3 syseba.py restore-copy --path Documenti/file.txt --rename
python3 syseba.py restore-copy --path Documenti/file.txt --overwrite
```

`--json` produce output strutturato per `status`, `logs`, `config-check`, `restore-list` e `restore-copy`.

## Avvio con dashboard web

Avvia watcher, console disattivata e dashboard web:

```bash
sudo python3 /opt/syseba/syseba.py \
  --silent \
  --web \
  --web-host 0.0.0.0 \
  --web-port 8765 \
  --web-token-file /opt/syseba/syseba_web.token \
  --config /opt/syseba/syseba.conf
```

Apri:

```text
http://IP_DEL_SERVER:8765
```

La dashboard chiede un token. Puoi passarlo in tre modi:

```bash
# Opzione 1: variabile ambiente
export SYSEBA_WEB_TOKEN='token-lungo-casuale'

# Opzione 2: file token
sudo install -m 600 /dev/null /opt/syseba/syseba_web.token
sudo sh -c 'openssl rand -base64 32 > /opt/syseba/syseba_web.token'

# Opzione 3: argomento CLI, utile solo per test
sudo python3 /opt/syseba/syseba.py --web --web-token 'token-lungo-casuale'
```

Se non configuri un token, SySeBa ne genera uno temporaneo all'avvio e lo stampa su stdout/journal.

Per uso solo locale:

```bash
sudo python3 /opt/syseba/syseba.py --web --web-host 127.0.0.1 --web-port 8765
```

## Modalita web-only

La modalita `--web-only` non avvia il watcher e non prende il lock del daemon. Serve per consultare e modificare config, leggere log e navigare restore da web.

```bash
python3 /opt/syseba/syseba.py \
  --web-only \
  --web-host 0.0.0.0 \
  --web-port 8765 \
  --web-token-file /opt/syseba/syseba_web.token \
  --config /opt/syseba/syseba.conf
```

Nota: le modifiche salvate da web richiedono il riavvio del watcher per essere applicate al processo gia in esecuzione.

## Servizio systemd

Creazione automatica:

```bash
sudo python3 /opt/syseba/syseba.py --create-daemon --web --web-host 0.0.0.0 --web-port 8765 --web-token-file /opt/syseba/syseba_web.token
sudo systemctl start syseba
sudo systemctl status syseba
```

Comandi utili:

```bash
sudo systemctl restart syseba
sudo systemctl stop syseba
sudo journalctl -u syseba -f
```

Il servizio generato usa:

```text
/usr/bin/python3 /opt/syseba/syseba.py --silent --web --web-host 0.0.0.0 --web-port 8765 --web-token-file /opt/syseba/syseba_web.token
```

## Dashboard web

La web dashboard contiene quattro aree:

| Area | Funzione |
|---|---|
| Stato | Stato processo, aggiornamento dati, percorsi, dischi, risorse, contatori e stato esplicito della sync iniziale |
| Log | Filtri per livello/testo, numero righe, auto-refresh, copia e download |
| Configurazione | Modifica dei valori e confronto tra configurazione attiva e salvata |
| Restore | Breadcrumb, ricerca, ordinamento, paginazione, download e ripristino guidato |

Gli stati della sincronizzazione iniziale distinguono `pending`, `running`, `completed`, `completed_with_errors`, `skipped`, `stopped`, `failed` e `not_available`. In modalita Web-only o con sync saltata non viene mostrato un falso avanzamento al 100%.

Quando la configurazione salvata differisce da quella usata dal watcher, la dashboard mostra un avviso persistente. Se SySeBa e avviato dal proprio servizio systemd, puo richiedere il riavvio dalla Web UI; altrimenti visualizza il comando manuale.

## API disponibili

Le API rispondono in JSON.

### Stato completo

```http
GET /api/status
```

Contiene:

- stato running/web-only;
- PID;
- uptime;
- configurazione attiva;
- uso disco source/backup/restore;
- CPU/RAM/thread/coda;
- contatori operazioni;
- sync iniziale;
- ultimi log recenti;
- stato lock file.

### Log

```http
GET /api/logs?lines=200
```

Restituisce le ultime righe del log testuale.

### Configurazione

```http
GET /api/config
```

Restituisce la configurazione letta dal file.

```http
GET /api/config/state
```

Restituisce separatamente `active`, `saved`, i campi presenti in `changes` e `restart_required`.

```http
POST /api/config
Content-Type: application/json

{
  "source": "/storage/4TB",
  "backup": "/storage/6TB/Backup",
  "restore": "/storage/6TB/RESTORE",
  "log_file": "/var/log/syseba.log",
  "threads": 5
}
```

Salva `syseba.conf`. Se il daemon sta gia girando, riavvia SySeBa per applicare i nuovi percorsi al watcher.

### Area restore

```http
GET /api/restore?path=&search=&page=1&page_size=100&sort=name&direction=asc
```

Lista directory e file nell'area restore. `page_size` e limitato a 250; `sort` accetta `name`, `mtime` o `size`.

```http
GET /api/restore/info?path=cartella/file.txt
```

Restituisce metadati, destinazione prevista e `destination_exists`, usato dal flusso di conferma.

```http
GET /restore/download?path=cartella/file.txt
```

Scarica un file dalla restore area.

```http
POST /api/restore
Content-Type: application/json

{
  "path": "cartella/file.txt",
  "strategy": "rename"
}
```

Ripristina il file o la directory indicata dalla restore area verso la source. Le strategie sono:

| Strategia | Comportamento |
|---|---|
| `fail` | Non modifica una destinazione esistente; e il default |
| `rename` | Crea un nuovo nome con suffisso `.restored-DATA-ORA` |
| `overwrite` | Sovrascrive il file o unisce una directory dello stesso tipo |

Per compatibilita e ancora accettato `"overwrite": true`.

```http
POST /api/service/restart
```

Richiede il riavvio solo quando il processo rileva di essere eseguito dal servizio systemd. Negli altri casi l'API restituisce il comando manuale da usare.

## Log e database

SySeBa scrive:

- log testuale nel percorso `log` configurato;
- database SQLite in `/opt/syseba/syseba_logs.db` di default.

Il database contiene la tabella `logs`:

```sql
id INTEGER PRIMARY KEY AUTOINCREMENT
timestamp TEXT
level TEXT
operation TEXT
source_path TEXT
target_path TEXT
additional_info TEXT
```

Il path SQLite puo essere cambiato:

```bash
python3 syseba.py --db-path /opt/syseba/syseba_logs.db
```

## Lock file

Default:

```text
/opt/syseba/syseba.lock
```

Il lock evita due istanze del watcher in parallelo. Se il PID nel lock non esiste piu, SySeBa considera il lock vecchio e lo sostituisce.

Per usare un percorso diverso:

```bash
python3 syseba.py --lockfile /run/syseba.lock
```

## Sync iniziale

All'avvio SySeBa scansiona la source e copia nel backup solo i file mancanti o piu vecchi. Questo riduce lavoro inutile rispetto a una copia sempre forzata.

Per saltare la sync iniziale:

```bash
python3 syseba.py --no-initial-sync
```

Usalo solo se sei sicuro che il backup sia gia allineato.

## Restore: comportamento

Quando un file viene cancellato dalla source:

1. SySeBa calcola il percorso relativo.
2. Cerca la copia nel backup.
3. Sposta quella copia nella restore area.
4. Mantiene la struttura directory.
5. Se il file esiste gia in restore, aggiunge un suffisso timestamp per non sovrascrivere.

Esempio:

```text
source:  /storage/4TB/docs/a.txt
backup:  /storage/6TB/Backup/docs/a.txt
restore: /storage/6TB/RESTORE/docs/a.txt
```

## Sicurezza web

La dashboard web richiede un token per tutte le API e per le azioni amministrative. Per usarla in sicurezza:

- esponila solo su LAN fidata;
- preferisci `--web-host 127.0.0.1` se la usi via SSH tunnel;
- se la pubblichi in rete, mettila dietro reverse proxy con HTTPS e autenticazione;
- limita firewall e IP autorizzati;
- ricorda che da web si puo modificare configurazione e ripristinare file.
- usa token lunghi e casuali, leggibili solo dall'utente che esegue SySeBa.

Puoi disattivare l'autenticazione solo in laboratorio:

```bash
python3 syseba.py --web --no-web-auth
```

Non usare `--no-web-auth` su una rete condivisa.

Esempio tunnel SSH:

```bash
ssh -L 8765:127.0.0.1:8765 utente@server
```

Poi apri sul PC:

```text
http://127.0.0.1:8765
```

## Prestazioni

Suggerimenti:

- `threads = 4` o `5` va bene per molti dischi meccanici/NAS.
- Aumenta i thread solo se backup e storage reggono piu I/O parallelo.
- Evita di mettere source, backup e restore nello stesso filesystem se vuoi resilienza migliore.
- La sync iniziale puo generare picchi I/O: eseguila in una finestra tranquilla.
- Il watcher usa eventi filesystem, quindi dopo la sync iniziale il carico resta basso.

## Troubleshooting

### `Missing dependencies`

Errore:

```text
Missing dependencies: psutil, watchdog
```

Soluzione:

```bash
cd /opt/syseba
sudo python3 -m pip install -r requirements.txt
```

### `Source directory does not exist`

La directory `source` configurata non esiste o non e montata.

Verifica:

```bash
ls -la /storage/4TB
mount | grep storage
```

### Dashboard web non raggiungibile

Controlla host e porta:

```bash
ss -ltnp | grep 8765
sudo journalctl -u syseba -f
```

Se usi `127.0.0.1`, la dashboard e raggiungibile solo dalla macchina locale. Usa `0.0.0.0` per ascoltare su tutte le interfacce.

### Config modificata ma watcher usa ancora i vecchi path

E normale. Le modifiche salvate da web aggiornano il file, ma il watcher gia avviato deve essere riavviato:

```bash
sudo systemctl restart syseba
```

### `SySeBa is already running`

Esiste un lock con PID ancora attivo.

Verifica:

```bash
cat /opt/syseba/syseba.lock
ps -fp $(cat /opt/syseba/syseba.lock)
```

Se il processo e davvero fermo, puoi rimuovere il lock:

```bash
sudo rm /opt/syseba/syseba.lock
```

### Permission denied su log o database

Controlla permessi:

```bash
sudo mkdir -p /var/log
sudo touch /var/log/syseba.log
sudo chown root:root /var/log/syseba.log
```

Se esegui SySeBa con utente non root, assegna log, backup, restore e database a quell'utente.

### `table logs has no column named level`

Questo errore puo comparire aggiornando da una versione vecchia che aveva gia creato `/opt/syseba/syseba_logs.db` con lo schema precedente.

La versione aggiornata migra automaticamente la tabella `logs` aggiungendo la colonna `level`. Se l'errore continua dopo l'aggiornamento, riavvia il servizio:

```bash
sudo systemctl restart syseba
```

In caso estremo puoi fare backup e rimuovere il database SQLite: SySeBa lo ricrea all'avvio.

```bash
sudo systemctl stop syseba
sudo cp /opt/syseba/syseba_logs.db /opt/syseba/syseba_logs.db.bak
sudo rm /opt/syseba/syseba_logs.db
sudo systemctl start syseba
```

### Restore non ripristina perche il file esiste gia

Per sicurezza SySeBa non sovrascrive implicitamente. La Web UI apre un dialogo e permette di:

- ripristinare con un nuovo nome timestamp;
- sovrascrivere il file;
- unire una directory dello stesso tipo;
- annullare.

Dalla CLI:

```bash
python3 syseba.py restore-copy --path cartella/file.txt --rename
python3 syseba.py restore-copy --path cartella/file.txt --overwrite
```

## FAQ

### SySeBa cancella davvero i file?

No. Quando un file sparisce dalla source, la copia nel backup viene spostata nella restore area.

### Posso usare solo la dashboard web?

Si, con `--web-only`. In quel caso non parte il watcher e non viene fatta sincronizzazione.

### Posso cambiare configurazione da web?

Si. La configurazione viene salvata su `syseba.conf`. Riavvia SySeBa per applicarla al watcher.

### Posso vedere i log dal browser?

Si. La sezione Log legge il file configurato, filtra testo e livelli, aggiorna automaticamente e permette copia o download delle righe visualizzate.

### Serve un database esterno?

No. SySeBa usa SQLite locale.

### Posso esporre la dashboard su Internet?

Non direttamente. Anche con token integrato, e meglio usare VPN, SSH tunnel o reverse proxy con HTTPS e autenticazione aggiuntiva.

### Cosa succede se il backup contiene gia il file?

Durante la sync iniziale SySeBa copia solo se il file manca o se la source risulta piu recente/differente per dimensione.

### Cosa succede se un file e modificato mentre viene copiato?

La copia viene ritentata alcune volte. Se fallisce, SySeBa registra un errore nel log e nel database.

### Posso cambiare lingua console?

Si:

```bash
python3 syseba.py --lang en
python3 syseba.py --lang it
```

## Checklist installazione o aggiornamento

1. Copia tutti i file, incluso `syseba_web.js`, in `/opt/syseba`.
2. Installa o aggiorna `requirements.txt`.
3. Esegui `python3 syseba.py config-check --config /opt/syseba/syseba.conf`.
4. Riavvia il servizio e controlla `systemctl status syseba`.
5. Verifica la dashboard console o Web.
6. Crea, modifica e cancella un file di prova nella source.
7. Verifica copia in backup e spostamento in restore.
8. Prova download, rinomina e restore da Web UI o CLI.
