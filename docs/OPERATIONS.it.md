# Operatività e osservabilità

[English](OPERATIONS.md) | [Indice documentazione](README.it.md)

## Controllo del servizio

Linux:

```bash
systemctl status syseba.service --no-pager -l
systemctl restart syseba.service
systemctl stop syseba.service
systemctl cat syseba.service
```

Windows:

```powershell
Get-Service SySeBa
Restart-Service SySeBa
sc.exe qc SySeBa
```

macOS:

```bash
sudo launchctl print system/com.okno.syseba
sudo launchctl kickstart -k system/com.okno.syseba
```

Registra sempre la versione in esecuzione prima della manutenzione:

```bash
syseba --version
syseba status --json
```

## Log console in modalità servizio

La dashboard interattiva è disabilitata intenzionalmente quando SySeBa viene
eseguito come servizio. Per avvio, errori, signal e binding server su Linux:

```bash
sudo journalctl -fu syseba.service -o short-iso-precise
```

Per seguire le operazioni sui file:

```bash
sudo tail -n 200 -F /var/log/syseba/syseba.log
```

Entrambi i flussi:

```bash
sudo ./scripts/syseba-maintenance.sh follow
```

Boot corrente in forma storica:

```bash
sudo journalctl -u syseba.service -b -n 300 --no-pager
```

## Controlli di salute

```bash
syseba status
syseba status --json
curl -fsS http://127.0.0.1:8765/api/auth
ss -lntp | grep ':8765'
```

`/api/auth` dimostra che il listener HTTP risponde senza mostrare dati di stato
e senza richiedere il bearer token.

## Metriche importanti

- `initial_sync`: stato e avanzamento riconciliazione iniziale.
- `watcher`: salute backend eventi.
- `queue_size`: operazioni filesystem in attesa.
- `queued_events`, `copied`, `deleted`, `restored`, `errors`.
- Tempo trascorso, CPU processo e memoria residente.
- Spazio usato/libero per source, backup e restore.
- Differenza tra configurazione salvata e attiva.

Una coda in crescita continua indica throughput storage insufficiente,
modifiche source senza limite, errori di permesso con retry o worker
insufficienti. CPU alta e throughput basso possono indicare milioni di file
piccoli o troppi worker.

## Runbook: backup fermo

1. Esegui `syseba config-check --json`.
2. Controlla servizio e presenza di una seconda istanza.
3. Verifica spazio libero e stato read/write dei mount.
4. Cerca il primo errore nel log, non soltanto le ripetizioni successive.
5. Controlla watcher e profondità coda.
6. Verifica che i file source restino stabili abbastanza a lungo.
7. Riavvia solo dopo aver raccolto journal e coda del log.

## Runbook: Web UI non raggiungibile

1. Verifica che `ExecStart` contenga `--web`.
2. Verifica il listener sulla porta 8765.
3. Prova `/api/auth` dall'host.
4. Prova l'IP server dalla LAN.
5. Controlla firewall host e ACL di rete.
6. Cerca conflitti di porta ed errori bind nel journal.

## Runbook: errore SQLite

Arresta eventuali vecchi processi Python e verifica che systemd avvii
l'eseguibile C:

```bash
systemctl cat syseba.service | grep ExecStart
ps aux | grep '[s]yseba'
syseba --version
```

Riavvia una volta. La migrazione viene eseguita prima del writer log. In una
copia manuale a servizio fermo conserva insieme DB, `-wal` e `-shm`.

## Pianificazione capacità

Il restore può crescere senza limite perché il contenuto eliminato dal backup
viene conservato. Monitoralo separatamente dal backup. Definisci una retention
esterna solo dopo aver chiarito requisiti aziendali e di recupero; questa
release non elimina automaticamente la storia restore.

## Modifiche di configurazione

Usa la Web UI o modifica l'INI, quindi:

```bash
syseba config-check
sudo systemctl restart syseba.service
```

Crea uno snapshot software prima di cambiare le radici storage. Il cambio
radice avvia una nuova riconciliazione e non migra il contenuto precedente di
backup o restore.
