# Changelog

[English](CHANGELOG.md) | [Indice documentazione](docs/README.it.md)

## Non rilasciato

### Documentazione e distribuzione

- Aggiunta documentazione tecnica completa italiano/inglese con navigazione.
- Pubblicati tutti gli artefatti verificati 2.0.0 nel package OCI pubblico e
  collegato alla repository `ghcr.io/okno/syseba-packages`.
- Aggiunto workflow GitHub Actions manuale con privilegi minimi che verifica
  nuovamente una Release prima di pubblicare i tag package versione e
  `latest`.
- Documentati digest corrente, verifica estrazione anonima e stato non firmato
  degli artefatti Windows/macOS.

## 2.0.0

### Aggiunto

- Runtime C11 completo per Linux, Windows e macOS.
- Watcher nativi Linux/Windows con polling di ripiego.
- Sincronizzazione iniziale parallela e code worker limitate.
- Dashboard console e Web UI bilingui.
- API JSON, browser restore e gestione servizio nativa.
- Pacchetti DEB, RPM, Linux portabile, Windows ZIP/NSIS, DMG Universal 2 e
  archivio sorgente.
- Migrazione, snapshot, health check e rollback esatto automatizzati.

### Modificato

- Python, watchdog, psutil e pip non sono più dipendenze runtime.
- Copia con temporanei esclusivi, verifica sorgente, flush e replace atomico.
- Web UI del servizio avviata automaticamente sulla porta 8765.
- Unit systemd con hardening e percorsi scrivibili espliciti.

### Corretto

- Migrazione automatica dei vecchi database SQLite privi della colonna
  `level`.
- Lock kernel resistente a file obsoleti e riuso PID.
- Difese contro traversal restore, fuga symlink/reparse point, attacchi al
  token, copie parziali e race di configurazione.
- Correzione della durata della strategia JSON nell'endpoint restore POST.

### Compatibilità

- Formato INI `[SETTINGS]` conservato.
- Configurazione, DB, token, log e stato `/opt/syseba` migrabili e
  ripristinabili tramite `scripts/syseba-maintenance.sh`.
