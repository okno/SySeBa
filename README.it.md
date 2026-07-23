# SySeBa - Syncro Service Backup

![Logo SySeBa](SySeBa_Logo.webp)

[English](README.md) | [Italiano](README.it.md) | [Guida operativa avanzata](ReadmeAI.md)

SySeBa è un demone Linux per la protezione continua dei file. Esegue una
sincronizzazione iniziale multithread, replica in tempo reale le modifiche del
filesystem e conserva in un'area restore separata le copie eliminate dal
backup, invece di cancellarle definitivamente.

Il sistema comprende dashboard console adattiva, Web UI protetta da token, log
testuale e SQLite, procedure sicure di restore e uno strumento automatico per
aggiornamento e rollback.

## Funzionalità principali

- Copia iniziale multithread dalla sorgente al backup.
- Monitoraggio in tempo reale tramite `watchdog` e inotify Linux.
- Soft delete: gli elementi eliminati dalla sorgente vengono spostati dal
  backup al restore.
- Tentativi automatici di copia per gestire scritture e rinomine transitorie.
- Log testuale e audit SQLite con migrazione automatica dello schema.
- Dashboard console adattiva per terminali stretti o con poche righe.
- CLI, console, Web UI, messaggi operativi e documentazione in italiano e
  inglese.
- Stato Web, metriche processo, dischi, log ricercabili, modifica
  configurazione, riavvio e navigazione restore.
- Ricerca, ordinamento, paginazione, download e restore con rinomina sicura o
  sovrascrittura esplicita.
- Lock di processo contro istanze duplicate.
- Servizio systemd rinforzato con Web UI avviata automaticamente al boot.
- Token Web persistente creato con permessi `0600`.
- Aggiornamenti in staging, snapshot a servizio fermo, health check e rollback
  automatico.
- Selettore testuale interattivo per tornare a uno snapshot software preciso.

## Architettura

| Percorso | Funzione |
|---|---|
| `/opt/syseba` | Applicazione, configurazione, database, token e lock |
| `/var/log/syseba.log` | Log testuale predefinito |
| `source` | Dati originali monitorati |
| `backup` | Copia sincronizzata corrente |
| `restore` | Area versionata per le eliminazioni |
| `/etc/systemd/system/syseba.service` | Unit generata |
| `/root/syseba-backups` | Snapshot di manutenzione consigliati |

SySeBa non considera il restore come spazio temporaneo. Quando un elemento
viene eliminato dalla sorgente, la sua copia nel backup viene spostata nel
restore con un nome univoco quando necessario.

## Requisiti

- Linux con systemd.
- Python 3.8 o successivo.
- `watchdog`.
- `psutil`.
- Git, GNU tar, coreutils e `flock` per lo strumento di manutenzione.

Su Debian o Ubuntu:

```bash
sudo apt update
sudo apt install -y git python3 python3-pip python3-venv
```

## Nuova installazione

```bash
sudo git clone https://github.com/okno/SySeBa.git /opt/syseba
sudo python3 -m pip install -r /opt/syseba/requirements.txt
sudo chmod 750 /opt/syseba/syseba.py /opt/syseba/syseba-maintenance.sh
```

Modifica `/opt/syseba/syseba.conf`:

```ini
[SETTINGS]
source = /storage/data
backup = /storage/backup
restore = /storage/restore
log = /var/log/syseba.log
threads = 5
```

Valida la configurazione:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Installa il servizio:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang it
sudo systemctl start syseba.service
```

`service-install` abilita sempre la Web UI nella unit systemd:

```text
--silent
--web
--web-host 0.0.0.0
--web-port 8765
--web-token-file /opt/syseba/syseba_web.token
```

Il token viene generato una sola volta, salvato con permessi `0600` e
riutilizzato a ogni riavvio.

## Web UI

Apri:

```text
http://IP_DEL_SERVER:8765
```

Leggi il token sul server:

```bash
sudo cat /opt/syseba/syseba_web.token
```

Controlla che il servizio sia in ascolto:

```bash
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
```

Il servizio systemd ascolta su tutte le interfacce perché è pensato per una
rete locale fidata. Limita la porta `8765` alla sottorete LAN necessaria
tramite il firewall. Non esporre direttamente su Internet il server HTTP
integrato: usa una VPN oppure un reverse proxy TLS autenticato.

Gli avvii Web manuali restano limitati a localhost:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web \
  --config /opt/syseba/syseba.conf
```

Il comando usa `127.0.0.1`. Specifica `--web-host 0.0.0.0` soltanto quando
l'accesso dalla LAN è intenzionale.

## Console e lingue

Console italiana:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Console inglese:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang en
```

`syseba.lang` contiene tutte le etichette console e CLI IT/EN modificabili:

```text
CHIAVE;Testo italiano;English text
```

Le traduzioni Web sono in `syseba_web.js`. L'opzione `--lang` viene mantenuta
anche nella unit systemd e controlla console, CLI, Web UI e messaggi operativi
nel log.

## Riferimento CLI

| Comando | Descrizione |
|---|---|
| `run` | Avvia watcher e console; comando predefinito |
| `status` | Mostra lock, PID, percorsi e utilizzo dischi |
| `logs --lines N` | Stampa le ultime righe del log applicativo |
| `config-check` | Valida percorsi, sovrapposizioni, thread e log |
| `restore-list` | Cerca e consulta l'area restore |
| `restore-copy --path PATH` | Ripristina un file o una directory |
| `service-install` | Genera e abilita la unit systemd con Web UI |

Opzioni principali:

| Opzione | Descrizione |
|---|---|
| `--config PATH` | File di configurazione alternativo |
| `--lang it\|en` | Lingua di interfaccia e messaggi |
| `--silent` | Disabilita la dashboard console interattiva |
| `--web` | Avvia la Web UI insieme al watcher |
| `--web-only` | Avvia la Web UI senza watcher |
| `--web-host HOST` | Indirizzo di ascolto |
| `--web-port PORT` | Porta, predefinita `8765` |
| `--web-token-file PATH` | Token di autenticazione persistente |
| `--no-web-auth` | Disabilita autenticazione; non sicuro fuori dai test |
| `--no-initial-sync` | Salta la scansione completa iniziale |
| `--json` | Output leggibile da altri programmi |

Esempi restore:

```bash
sudo python3 /opt/syseba/syseba.py restore-list \
  --config /opt/syseba/syseba.conf \
  --search report \
  --page 1 \
  --page-size 100

sudo python3 /opt/syseba/syseba.py restore-copy \
  --config /opt/syseba/syseba.conf \
  --path documenti/report.pdf \
  --rename
```

Usa `--overwrite` solo quando desideri sostituire o unire la destinazione.

## Aggiornamento sicuro

Clona una copia separata per la manutenzione. Non clonare sopra
un'installazione `/opt/syseba` funzionante.

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root
/root/SySeBa-release/syseba-maintenance.sh quick-update
```

L'updater:

1. Risolve e confronta revisione installata e remota.
2. Scarica e controlla la release mentre SySeBa è ancora attivo.
3. Verifica lo spazio disponibile.
4. Ferma il servizio.
5. Crea uno snapshot con checksum a servizio fermo.
6. Preserva configurazione, database, WAL/SHM, log e token Web.
7. Valida la configurazione nello staging.
8. Sostituisce atomicamente le directory applicative.
9. Rigenera e valida l'avvio automatico Web nella unit systemd.
10. Avvia il servizio e verifica che rimanga attivo.
11. Ripristina automaticamente la versione precedente in caso di errore.

Se viene avviato da `/root`, gli snapshot si trovano in:

```text
/root/syseba-backups/YYYYMMDD-HHMMSS/
```

L'updater non copia e non modifica le directory dati `source`, `backup` e
`restore` definite nella configurazione.

## Rollback

Selettore testuale interattivo:

```bash
cd /root
/root/SySeBa-release/syseba-maintenance.sh rollback
```

Il menu mostra ID, data, motivo, commit Git, dimensione e SHA-256 prima della
conferma.

Rollback diretto:

```bash
/root/SySeBa-release/syseba-maintenance.sh rollback pre-update
/root/SySeBa-release/syseba-maintenance.sh rollback latest
/root/SySeBa-release/syseba-maintenance.sh rollback 20260723-023230
```

Prima dell'estrazione vengono verificati i checksum. L'installazione sostituita
resta in quarantena. Se il servizio ripristinato non parte, SySeBa rimette al
suo posto l'installazione precedente al rollback.

## Log

Cronologia applicativa e systemd:

```bash
/root/SySeBa-release/syseba-maintenance.sh logs 200
```

Segui entrambe le sorgenti:

```bash
/root/SySeBa-release/syseba-maintenance.sh follow
```

Comandi diretti:

```bash
journalctl -fu syseba.service -o short-iso-precise
tail -n 200 -F /var/log/syseba.log
```

Il journal contiene ciclo di vita del servizio ed errori Python. Le operazioni
sui file vengono scritte principalmente nel log configurato e nel database
SQLite.

## Modello di sicurezza

- Le modifiche tramite API Web richiedono `X-SySeBa-Token`.
- Il token Web non è tracciato da Git e ha permessi `0600`.
- I link simbolici sul file token vengono rifiutati.
- La dimensione delle richieste JSON è limitata.
- I percorsi restore sono verificati contro traversal e fuga tramite symlink.
- Il lock impedisce watcher duplicati.
- La unit usa `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`, umask
  restrittiva e percorsi scrivibili espliciti.
- Database, WAL, log, lock, token e snapshot non entrano in Git.
- Ogni snapshot di manutenzione possiede un manifest SHA-256.

## Risoluzione problemi

### Web UI non raggiungibile

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 100 --no-pager
```

La unit deve contenere `--web --web-host 0.0.0.0 --web-port 8765`.

### Token Web non valido

```bash
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
sudo cat /opt/syseba/syseba_web.token
```

I permessi attesi sono `600`. Elimina il token salvato nella sessione del
browser e inserisci nuovamente il valore corrente.

### SQLite segnala la colonna `level` mancante

Riavvia la release corrente. `initialize_database()` migra gli schemi
precedenti prima dell'avvio del log writer.

### Configurazione modificata ma percorsi non aggiornati

Il salvataggio non cambia silenziosamente i percorsi del watcher già attivo:

```bash
sudo systemctl restart syseba.service
```

### SySeBa risulta già avviato

Non eseguire un secondo watcher manuale mentre il servizio è attivo:

```bash
systemctl status syseba.service
cat /opt/syseba/syseba.lock
```

## Test

```bash
python3 -m unittest discover -s tests -v
```

La suite copre migrazione SQLite, lock, traversal e symlink, API Web protette,
token persistenti, unit systemd, restore, layout console, localizzazione e CLI
JSON.

## Licenza

MIT. Consulta [LICENSE](LICENSE).

Progetto mantenuto da [okno](https://github.com/okno).
