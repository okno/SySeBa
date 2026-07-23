# SySeBa - Guida operativa completa

![Logo SySeBa](SySeBa_Logo.webp)

[README rapido in italiano](README.it.md) | [English README](README.md) | [Advanced guide in English](ReadmeAI.en.md)

Questa guida descrive installazione, configurazione, funzionamento, Web UI,
console, sicurezza, aggiornamento, rollback, log, diagnosi e ripristino di
SySeBa. I comandi assumono una macchina Linux con systemd e installazione in
`/opt/syseba`.

## 1. Che cosa fa SySeBa

SySeBa mantiene una copia continuamente aggiornata di una directory sorgente:

```text
source  --copia e aggiornamento-->  backup
source  --eliminazione----------->  backup -> restore
restore --ripristino------------->  source
```

All'avvio esegue una scansione iniziale multithread. Successivamente usa
`watchdog` e inotify per elaborare creazioni, modifiche, spostamenti ed
eliminazioni in tempo reale.

Un file eliminato dalla sorgente non viene cancellato definitivamente dal
backup: viene spostato nell'area restore. In caso di conflitto SySeBa genera un
nome univoco, così una copia precedente non viene sovrascritta per errore.

SySeBa non sostituisce un backup offline, immutabile o remoto. La sorgente, il
backup e il restore devono risiedere su storage affidabile e devono essere
inclusi nella strategia generale di disaster recovery.

## 2. Componenti e percorsi

| File o percorso | Scopo |
|---|---|
| `/opt/syseba/syseba.py` | Motore, CLI, dashboard console e server Web |
| `/opt/syseba/syseba_web.js` | Logica e traduzioni della Web UI |
| `/opt/syseba/syseba.lang` | Etichette IT/EN per console e CLI |
| `/opt/syseba/syseba.conf` | Configurazione attiva |
| `/opt/syseba/syseba_logs.db` | Audit SQLite |
| `/opt/syseba/syseba_web.token` | Token Web persistente, permessi `0600` |
| `/opt/syseba/syseba.lock` | Lock dell'istanza attiva |
| `/var/log/syseba.log` | Log applicativo predefinito |
| `/etc/systemd/system/syseba.service` | Unit systemd generata |
| `/root/syseba-backups` | Snapshot software creati dalla manutenzione |

Database, file WAL/SHM, log, lock, token e snapshot sono esclusi da Git.

## 3. Requisiti

- Linux con systemd.
- Python 3.8 o successivo.
- Moduli Python `watchdog` e `psutil`.
- Git, GNU tar, coreutils, `flock`, `journalctl` e `sha256sum` per la
  manutenzione automatica.
- Permessi di lettura sulla sorgente e di scrittura su backup, restore, log e
  directory applicativa.

Installazione di base su Debian o Ubuntu:

```bash
sudo apt update
sudo apt install -y git python3 python3-pip python3-venv
```

## 4. Nuova installazione

Clona la release:

```bash
sudo git clone https://github.com/okno/SySeBa.git /opt/syseba
sudo python3 -m pip install -r /opt/syseba/requirements.txt
sudo chmod 750 /opt/syseba/syseba.py /opt/syseba/syseba-maintenance.sh
```

Se la distribuzione impedisce l'installazione globale con `pip`, installa i
pacchetti equivalenti forniti dalla distribuzione oppure configura
esplicitamente un ambiente Python gestito. La unit predefinita usa
`/usr/bin/python3`.

Prepara le directory:

```bash
sudo mkdir -p /storage/data /storage/backup /storage/restore
sudo touch /var/log/syseba.log
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

Valida prima di avviare:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Installa e avvia il servizio:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang it
sudo systemctl start syseba.service
```

Verifica:

```bash
systemctl is-enabled syseba.service
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
```

## 5. Configurazione

La sezione obbligatoria è `[SETTINGS]`.

| Chiave | Significato | Regole |
|---|---|---|
| `source` | Albero originale monitorato | Deve esistere ed essere leggibile |
| `backup` | Copia sincronizzata corrente | Deve essere scrivibile |
| `restore` | Archivio degli elementi eliminati | Deve essere scrivibile |
| `log` | File di log testuale | La directory padre deve essere scrivibile |
| `threads` | Worker della sincronizzazione iniziale | Intero positivo |

I percorsi possono essere assoluti o relativi alla directory del file di
configurazione. Non devono sovrapporsi: per esempio il backup non deve stare
dentro la sorgente e il restore non deve stare dentro il backup.

Una quantità elevata di thread non rende automaticamente più veloce il
processo. Per dischi meccanici sono normalmente adatti `2-5` worker; storage
SSD o pool veloci possono beneficiare di un valore maggiore. Verifica sempre
latenza, I/O e carico reale.

Controllo leggibile:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Controllo JSON per automazioni:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --json
```

Le modifiche salvate dalla Web UI vengono validate prima della scrittura. Se
cambiano percorsi o thread, la pagina mostra `Riavvio necessario`: il watcher
continua con la configurazione attiva finché il servizio non viene riavviato.

## 6. Servizio e avvio automatico Web

`service-install` genera e abilita sempre una unit che avvia insieme watcher e
Web UI:

```text
/usr/bin/python3 /opt/syseba/syseba.py
  --silent
  --web
  --web-host 0.0.0.0
  --web-port 8765
  --lang it
  --config /opt/syseba/syseba.conf
  --web-token-file /opt/syseba/syseba_web.token
```

La riga reale è una singola direttiva `ExecStart`. L'ascolto su `0.0.0.0`
rende la pagina disponibile sulle interfacce LAN del server. Il servizio viene
abilitato per `multi-user.target` e riparte automaticamente dopo errori o
riavvii della macchina.

Per rigenerare una vecchia unit che contiene soltanto `--silent`:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang it
sudo systemctl restart syseba.service
```

In alternativa, `syseba-maintenance.sh quick-update` migra e verifica la unit
anche quando il commit installato è già quello più recente.

Porta o indirizzo personalizzati:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --web-host 192.168.1.10 \
  --web-port 9876 \
  --lang it
sudo systemctl restart syseba.service
```

## 7. Accesso alla Web UI

Apri dal browser:

```text
http://IP_DEL_SERVER:8765
```

Mostra il token:

```bash
sudo cat /opt/syseba/syseba_web.token
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
```

Il token viene generato una sola volta con entropia crittografica, scritto in
modo atomico, protetto con permessi `0600` e riutilizzato ai riavvii. SySeBa
rifiuta un token file che sia un link simbolico o non sia un file regolare.

Il browser conserva il token in `sessionStorage`: resta nella sessione della
scheda e può essere eliminato con `Dimentica token`.

La Web UI offre:

- stato di servizio, PID, uptime, sincronizzazione iniziale e lock;
- utilizzo dischi, CPU, RAM, thread, coda e contatori della sessione;
- log filtrabili per testo e livello;
- confronto fra configurazione attiva e salvata;
- modifica validata della configurazione e richiesta di riavvio;
- navigazione restore con breadcrumb, ricerca, ordinamento e paginazione;
- informazioni, download e ripristino di file o directory;
- gestione dei conflitti tramite rinomina sicura oppure sovrascrittura
  esplicita.

Il server HTTP integrato non offre TLS. Limita la porta alla LAN necessaria:

```bash
sudo ufw allow from 192.168.1.0/24 to any port 8765 proto tcp
```

Non pubblicare direttamente `8765` su Internet. Per accesso remoto usa una VPN
oppure un reverse proxy HTTPS con autenticazione aggiuntiva.

## 8. Avvio manuale e modalità operative

Watcher con dashboard console:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Watcher e Web UI manuale, limitata per impostazione predefinita a localhost:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Web UI senza watcher:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web-only \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Modalità servizio senza dashboard:

```bash
sudo python3 /opt/syseba/syseba.py \
  --silent \
  --web \
  --config /opt/syseba/syseba.conf \
  --lang it
```

Usa `--no-initial-sync` solo dopo aver valutato il rischio di lasciare il
backup non allineato. Usa `--no-web-auth` esclusivamente in test isolati.

Il lock `/opt/syseba/syseba.lock` impedisce di avviare due watcher sulla stessa
installazione. `--web-only` non acquisisce il lock del watcher.

## 9. Dashboard console

La dashboard si adatta a larghezza e altezza del terminale:

- vista completa per terminali ampi;
- vista compatta quando lo spazio verticale è ridotto;
- barre stabili per source, backup, restore, CPU e sincronizzazione iniziale;
- stato, uptime, coda, worker, contatori ed eventi recenti;
- output senza sequenze colore quando lo standard output non è una TTY o è
  impostata la variabile `NO_COLOR`.

Non eseguire la dashboard manuale mentre `syseba.service` è attivo. Per
consultare lo stato senza un secondo watcher usa:

```bash
sudo python3 /opt/syseba/syseba.py status \
  --config /opt/syseba/syseba.conf \
  --lang it
```

## 10. Lingue

Lingue supportate:

```bash
--lang it
--lang en
```

La scelta controlla dashboard, comandi CLI, messaggi operativi e Web UI. Il
servizio conserva la lingua nella propria `ExecStart`.

`syseba.lang` usa il formato:

```text
CHIAVE;Testo italiano;English text
```

Le traduzioni Web sono definite in `syseba_web.js`. Dopo una modifica verifica
che ogni chiave esista in entrambe le lingue ed esegui i test.

## 11. Riferimento CLI

| Comando | Funzione |
|---|---|
| `run` | Avvia il watcher; è il comando predefinito |
| `status` | Mostra stato, lock, PID, percorsi e dischi |
| `logs --lines N` | Legge le ultime righe del log applicativo |
| `config-check` | Valida configurazione e percorsi |
| `restore-list` | Elenca e cerca nell'area restore |
| `restore-copy --path PATH` | Ripristina un elemento nella sorgente |
| `service-install` | Genera e abilita la unit Web-enabled |

Opzioni comuni:

| Opzione | Funzione |
|---|---|
| `--config PATH` | Configurazione alternativa |
| `--lang it\|en` | Lingua |
| `--json` | Risposta JSON |
| `--silent` | Nessuna dashboard interattiva |
| `--web` | Web UI insieme al watcher |
| `--web-only` | Solo amministrazione Web |
| `--web-host HOST` | Indirizzo di ascolto |
| `--web-port PORT` | Porta da `1` a `65535` |
| `--web-token TOKEN` | Token passato direttamente; evitare nella shell |
| `--web-token-file PATH` | File token persistente |
| `--no-web-auth` | Disabilita l'autenticazione Web |
| `--no-initial-sync` | Salta la scansione iniziale |

Esempi:

```bash
sudo python3 /opt/syseba/syseba.py status --json
sudo python3 /opt/syseba/syseba.py logs --lines 200
sudo python3 /opt/syseba/syseba.py restore-list --search report --page-size 100
sudo python3 /opt/syseba/syseba.py restore-copy \
  --path documenti/report.pdf \
  --rename
```

Senza `--rename` o `--overwrite`, un conflitto nella sorgente interrompe il
restore. `--rename` sceglie una destinazione libera; `--overwrite` sostituisce
il file o unisce la directory in modo esplicito.

## 12. API HTTP

La pagina `/`, le risorse statiche e `/api/auth` sono pubblici per consentire
il form di accesso senza generare tentativi falliti ripetuti. `/api/auth`
comunica soltanto se il token è richiesto. Tutte le API operative e i download
richiedono:

```http
X-SySeBa-Token: TOKEN
```

È accettato anche `Authorization: Bearer TOKEN`.

| Metodo | Endpoint | Funzione |
|---|---|---|
| `GET` | `/api/auth` | Indica se l'autenticazione è richiesta |
| `GET` | `/api/status` | Stato completo |
| `GET` | `/api/logs?lines=200` | Ultime righe del log |
| `GET` | `/api/config` | Configurazione salvata |
| `GET` | `/api/config/state` | Confronto attiva/salvata |
| `POST` | `/api/config` | Valida e salva configurazione |
| `GET` | `/api/restore` | Elenco restore paginato |
| `GET` | `/api/restore/info?path=...` | Dettagli e conflitti |
| `POST` | `/api/restore` | Ripristino con strategia |
| `GET` | `/restore/download?path=...` | Download di un file |
| `POST` | `/api/service/restart` | Richiesta di riavvio systemd |

Esempio:

```bash
TOKEN="$(sudo cat /opt/syseba/syseba_web.token)"
curl -sS \
  -H "X-SySeBa-Token: ${TOKEN}" \
  http://127.0.0.1:8765/api/status
```

Le richieste JSON hanno dimensione limitata. I percorsi restore vengono
normalizzati e verificati per impedire traversal e fuga tramite link simbolici.

## 13. Log e audit

Vista combinata consigliata:

```bash
cd /root
/root/SySeBa-release/syseba-maintenance.sh logs 200
```

Segui systemd e log applicativo insieme:

```bash
/root/SySeBa-release/syseba-maintenance.sh follow
```

Comandi diretti:

```bash
journalctl -fu syseba.service -o short-iso-precise
tail -n 200 -F /var/log/syseba.log
```

Il journal è la fonte principale per avvio, arresto, eccezioni Python e
politiche di riavvio. Il file configurato contiene attività applicativa e
operazioni sui file. L'audit SQLite conserva eventi strutturati con data,
livello, azione, percorso, dettagli ed esito.

All'avvio `initialize_database()` controlla lo schema e aggiunge le colonne
mancanti delle versioni precedenti. Questo evita il ciclo
`table logs has no column named level` senza cancellare la cronologia.

## 14. Aggiornamento automatico sicuro

Mantieni una copia separata dello strumento di manutenzione:

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root
sudo /root/SySeBa-release/syseba-maintenance.sh quick-update
```

Non eseguire `git pull` direttamente dentro `/opt/syseba` durante il servizio.

`quick-update`:

1. confronta identità installata e commit remoto;
2. scarica in staging e controlla sintassi e dipendenze;
3. verifica lo spazio disponibile;
4. ferma il servizio e controlla processi non gestiti;
5. crea uno snapshot consistente con SHA-256;
6. preserva configurazione, DB, WAL/SHM, log e token;
7. valida la configurazione nella release candidata;
8. sostituisce la directory applicativa;
9. rigenera e valida la unit con Web UI automatica;
10. avvia il servizio e verifica che resti attivo;
11. mostra log recenti e percorso di rollback.

Se il commit è già aggiornato, non crea uno snapshot ridondante: verifica
l'installazione, migra comunque la vecchia unit e riavvia il servizio.

Comandi disponibili:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh quick-update
sudo /root/SySeBa-release/syseba-maintenance.sh backup
sudo /root/SySeBa-release/syseba-maintenance.sh update main
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo /root/SySeBa-release/syseba-maintenance.sh list
sudo /root/SySeBa-release/syseba-maintenance.sh logs 200
sudo /root/SySeBa-release/syseba-maintenance.sh follow
```

Lo snapshot predefinito viene creato nella directory corrente:

```text
./syseba-backups/YYYYMMDD-HHMMSS/
```

Avviando lo script da `/root`, il percorso diventa
`/root/syseba-backups/...`.

Variabili principali:

| Variabile | Predefinito |
|---|---|
| `SYSEBA_INSTALL_DIR` | `/opt/syseba` |
| `SYSEBA_BACKUP_ROOT` | `./syseba-backups` |
| `SYSEBA_REPO_URL` | Repository ufficiale |
| `SYSEBA_REF` | `main` |
| `SYSEBA_CONFIG_PATH` | `/opt/syseba/syseba.conf` |
| `SYSEBA_DB_PATH` | `/opt/syseba/syseba_logs.db` |
| `SYSEBA_TOKEN_PATH` | `/opt/syseba/syseba_web.token` |
| `SYSEBA_WEB_HOST` | `0.0.0.0` |
| `SYSEBA_WEB_PORT` | `8765` |
| `SYSEBA_LANG` | `it` |
| `SYSEBA_HEALTH_WAIT` | `3` secondi |

Esempio con porta diversa:

```bash
sudo env SYSEBA_WEB_PORT=9876 SYSEBA_LANG=it \
  /root/SySeBa-release/syseba-maintenance.sh quick-update
```

## 15. Contenuto degli snapshot

Ogni snapshot include:

```text
manifest.txt
SHA256SUMS
syseba-app.tar.gz
syseba.service
external-state.tar.gz       se necessario
external-state.paths        se necessario
```

`syseba-app.tar.gz` contiene l'intera installazione `/opt/syseba`, esclusi
lock e cache Python. Se configurazione, token, DB o log sono esterni
all'installazione, vengono archiviati in `external-state.tar.gz`.

Le directory dati configurate come `source`, `backup` e `restore` non vengono
mai copiate, cancellate o sostituite dallo strumento di manutenzione.

## 16. Rollback

Menu testuale:

```bash
cd /root
sudo /root/SySeBa-release/syseba-maintenance.sh rollback
```

Il selettore mostra ID, data, motivo, commit, dimensione, stato originario del
servizio e hash dell'archivio. Nessun file viene modificato prima della
conferma.

Selezione diretta:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh rollback pre-update
sudo /root/SySeBa-release/syseba-maintenance.sh rollback latest
sudo /root/SySeBa-release/syseba-maintenance.sh rollback 20260723-023230
```

Prima del ripristino vengono controllati tutti gli SHA-256. L'installazione
corrente viene spostata in quarantena, la unit originale viene ripristinata e
il servizio viene sottoposto a health check.

Se lo snapshot non parte, lo script rimette automaticamente l'installazione
che era attiva prima del tentativo. La release fallita resta disponibile per
diagnosi.

Il rollback ripristina esattamente applicazione e stato contenuti in
`/opt/syseba` al momento dello snapshot, compresi configurazione, database e
token quando erano in quella directory. Lo stato esterno viene archiviato ma
non sovrascritto automaticamente, per evitare di sostituire file esterni
modificati nel frattempo.

## 17. Sicurezza

- Usa il token Web; non passarlo come argomento della shell in produzione.
- Mantieni `/opt/syseba/syseba_web.token` a `0600`.
- Limita `8765/tcp` alla LAN o alla VPN necessaria.
- Non esporre il server HTTP integrato direttamente su Internet.
- Non usare `--no-web-auth` su reti condivise.
- Proteggi `/root/syseba-backups`: contiene configurazione e stato operativo.
- Conserva una copia offline o remota degli snapshot importanti.
- Verifica periodicamente restore e rollback, non soltanto la creazione.
- Mantieni Python e dipendenze aggiornati con il gestore del sistema.

La unit generata applica `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`,
`ProtectHome`, umask `0077` e una lista esplicita di percorsi scrivibili. Se
usi destinazioni non standard e systemd nega l'accesso, consulta il journal e
adatta con cautela `ReadWritePaths`.

## 18. Troubleshooting

### La Web UI non risponde

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 100 --no-pager
```

`ExecStart` deve contenere `--web`, host, porta e token file. Se contiene solo
`--silent`, esegui `quick-update` oppure rigenera la unit con
`service-install`, quindi riavvia.

### La porta è in ascolto solo su localhost

Controlla che la unit contenga:

```text
--web-host 0.0.0.0
```

Poi:

```bash
sudo systemctl daemon-reload
sudo systemctl restart syseba.service
```

### Il token non viene accettato

```bash
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
sudo cat /opt/syseba/syseba_web.token
```

I permessi attesi sono `600`. Premi `Dimentica token` nel browser e inserisci
il valore corrente. Controlla di non avere spazi o righe aggiuntive.

### Il servizio entra in un ciclo di riavvio

```bash
journalctl -u syseba.service -b -n 200 --no-pager
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf
```

Verifica dipendenze, permessi, spazio, mount disponibili e sovrapposizione dei
percorsi.

### SQLite segnala `no column named level`

La release corrente migra lo schema prima di avviare il writer:

```bash
sudo systemctl restart syseba.service
journalctl -u syseba.service -n 100 --no-pager
```

Non cancellare il DB come prima azione. Se l'errore persiste, salva DB, WAL e
SHM, ferma il servizio e verifica che stia realmente eseguendo il file
`/opt/syseba/syseba.py` aggiornato.

### Configurazione salvata ma non attiva

```bash
sudo systemctl restart syseba.service
```

La distinzione è intenzionale: evita che il watcher cambi radice durante
l'elaborazione degli eventi.

### Il lock segnala un'altra istanza

```bash
systemctl status syseba.service
cat /opt/syseba/syseba.lock
ps -fp "$(cat /opt/syseba/syseba.lock)"
```

Non eliminare il lock se il PID è attivo. Se non esiste alcun processo SySeBa,
il lock obsoleto può essere rimosso a servizio fermo.

### Il restore viene rifiutato

Controlla che il percorso richiesto sia relativo all'area restore, che non
attraversi link simbolici esterni e che la sorgente sia scrivibile. In caso di
destinazione esistente scegli esplicitamente rinomina o sovrascrittura.

### L'updater si interrompe

Leggi il messaggio finale e:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh list
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo /root/SySeBa-release/syseba-maintenance.sh logs 200
```

Se l'errore avviene dopo lo scambio delle directory, lo script tenta il
rollback automatico. Non rimuovere le directory `.syseba-*` finché la diagnosi
non è conclusa.

## 19. FAQ

### Su quale porta funziona la Web UI?

La porta predefinita è `8765`. Con il servizio standard l'indirizzo è
`http://IP_DEL_SERVER:8765`.

### La Web UI parte automaticamente al boot?

Sì. La unit generata da `service-install` contiene `--web`, è abilitata per
`multi-user.target` e usa `Restart=always`.

### Posso usare SySeBa soltanto da console?

Sì. Un avvio manuale senza `--web` mostra la dashboard. Il servizio standard
include comunque la Web UI per l'amministrazione remota locale.

### Posso consultare la Web UI senza avviare il watcher?

Sì, con `--web-only`. Questa modalità è utile per consultazione e gestione,
ma non sincronizza i file.

### Un aggiornamento modifica i miei dati?

Lo strumento di manutenzione non copia né sostituisce `source`, `backup` e
`restore`. Durante l'arresto necessario all'aggiornamento, gli eventi verranno
recuperati dalla sincronizzazione iniziale al riavvio.

### Il rollback torna davvero alla versione precedente?

Sì, se lo snapshot e i checksum sono validi. Ripristina l'intera installazione
archiviata e la sua unit systemd. La versione sostituita resta in quarantena e,
se il servizio ripristinato non parte, viene rimessa automaticamente.

### Posso cambiare il token?

Sì. Ferma il servizio, conserva una copia del token corrente, sostituisci il
file con un valore lungo e casuale, applica `chmod 600` e riavvia. Non usare
link simbolici.

### Perché il token non è nel repository?

È una credenziale locale. Pubblicarlo in Git permetterebbe a chiunque lo
conosca di leggere log, modificare la configurazione e richiedere restore.

### I log SQLite crescono nel tempo?

Sì. Pianifica monitoraggio e retention coerenti con lo spazio disponibile e
con i requisiti di audit. Qualunque manutenzione SQLite deve essere eseguita a
servizio fermo e dopo uno snapshot.

## 20. Verifica prima della produzione

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo systemctl restart syseba.service
sudo systemctl is-active syseba.service
ss -lntp | grep ':8765'
sudo /root/SySeBa-release/syseba-maintenance.sh logs 100
```

Controlla inoltre:

- accesso Web con token da un host LAN autorizzato;
- creazione e modifica di un file di prova;
- eliminazione verso restore;
- restore con destinazione libera e in conflitto;
- snapshot manuale e selezione rollback;
- regole firewall;
- spazio libero su source, backup, restore e snapshot.

## 21. Test per sviluppatori

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile syseba.py
bash -n syseba-maintenance.sh
```

La suite verifica migrazione SQLite, lock, copie e restore, sicurezza dei
percorsi, API protette, token persistente, unit systemd, layout console,
localizzazione e output CLI.

## 22. Licenza e progetto

SySeBa è distribuito con licenza MIT. Consulta [LICENSE](LICENSE).

Repository: [okno/SySeBa](https://github.com/okno/SySeBa)
