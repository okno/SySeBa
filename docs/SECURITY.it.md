# Modello di sicurezza

[English](SECURITY.md) | [Indice documentazione](README.it.md)

## Ambito

Questo documento copre input filesystem, configurazione/stato locale, servizio
HTTP, privilegi, codice di terze parti, archivi manutenzione e artefatti
rilasciati.

## Asset

- Disponibilità e integrità della sorgente.
- Integrità e completezza del backup corrente.
- Riservatezza e integrità del contenuto restore.
- Token Web, configurazione, database audit e log.
- Capacità di rollback esatto del software installato.

## Confini di fiducia

SySeBa considera affidabili kernel e amministratore che seleziona le radici e
installa il servizio. Nomi e contenuti nelle radici configurate non sono
fidati. I client HTTP non sono fidati prima della validazione token. Input
Git/rete non è fidato prima di build e test; gli snapshot locali usano
manifest SHA-256, non una firma esterna.

I binari sono pubblicati tramite GitHub Releases e mirror OCI GHCR pubblico.
Il mirror è un carrier di trasporto, non un runtime servizio. Il digest
identifica l'oggetto scaricato dal registry; `SHA256SUMS` verifica i singoli
payload al suo interno.

## Difese filesystem

- Normalizzazione e controllo sovrapposizione delle radici.
- Input restore relativo e contenuto sotto restore e source.
- Le letture sensibili rifiutano oggetti finali non regolari.
- POSIX usa `O_NOFOLLOW`; Windows apre e rifiuta esplicitamente reparse point.
- Temporanei destinazione creati in modo esclusivo con nomi univoci.
- Un symlink destinazione viene sostituito, mai seguito.
- Identità e metadata sorgente ricontrollati dopo la copia.
- Sostituzione finale atomica sul filesystem destinazione.
- Salvataggi config/token con temporaneo, flush e replace atomico.

La sostituzione di componenti intermedi da parte di un amministratore resta
fuori dal threat model. Le directory runtime devono appartenere a root/SYSTEM
e non essere scrivibili da utenti non fidati.

## Controllo processo

Il lock è un oggetto kernel mantenuto aperto, non una convenzione PID file. Il
riuso PID non può generare un falso owner. Il file lock persiste
intenzionalmente, evitando race su un inode ricreato.

I signal handler impostano soltanto un flag, evitando heap, mutex, logger e
operazioni Web non async-signal-safe.

## Autenticazione HTTP

- Token generato: 32 byte casuali crittografici, codificati in 64 hex.
- Token esplicito: 16-255 caratteri ASCII stampabili.
- Origine token: CLI, ambiente o file protetto.
- `--no-web-auth` rifiutato su bind non loopback.
- Confronto server-side per ogni route protetta.
- Body JSON massimo 64 KiB.
- Pagine restore massimo 250; log massimo 2.000 righe.
- Nomi download sanitizzati, octet-stream e `nosniff`.

Il server non implementa TLS. Esposizione consentita:

1. LAN/VPN fidata con firewall host; oppure
2. reverse proxy TLS autenticato su porta separata.

Non inoltrare la porta 8765 da un router Internet.

Il token è una credenziale bearer. Storage browser e log del reverse proxy non
devono esporlo. Usa `X-SySeBa-Token` e non inserirlo mai nell'URL.

## Sandboxing servizio

La unit Linux generata usa:

```text
NoNewPrivileges=true
CapabilityBoundingSet=CAP_DAC_OVERRIDE CAP_FOWNER
PrivateTmp=true
PrivateDevices=true
ProtectSystem=full
ProtectProc=invisible
ProcSubset=pid
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
ProtectKernelLogs=true
ProtectClock=true
ProtectHostname=true
RestrictSUIDSGID=true
RestrictRealtime=true
RestrictNamespaces=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
LockPersonality=true
MemoryDenyWriteExecute=true
RemoveIPC=true
SystemCallFilter=~@mount @module @reboot @swap @raw-io @debug @cpu-emulation
SystemCallErrorNumber=EPERM
SystemCallArchitectures=native
UMask=0077
```

`ReadWritePaths` viene generato da source, backup, restore e directory stato.
Source deve essere scrivibile perché il restore è una funzione esplicita.

Windows usa LocalSystem, service SID e DACL protetta sul token. macOS usa un
LaunchDaemon root. Questi privilegi sono necessari per alberi arbitrari; con
ownership omogenea si può usare un account dedicato dopo test ACL e restore.

## SQLite

Il path database deve essere regolare prima dell'apertura. Le query sono fisse
o preparate; path e messaggi sono parametri bind. Caricamento estensioni e
compatibilità stringhe con doppi apici sono disabilitati in compilazione.

La directory padre SQLite deve essere controllata dall'amministratore. Non
collocare il DB in directory world-writable.

## Manutenzione e supply chain

- I ref Git sono validati sintatticamente.
- Il candidato viene compilato fuori dall'installazione attiva.
- Test e `config-check` precedono l'arresto servizio.
- Gli snapshot avvengono solo a servizio fermo.
- Gli archivi includono ACL/xattr e owner numerici.
- Ogni snapshot contiene `SHA256SUMS` e identità.
- Il rollback verifica hash e rifiuta percorsi assoluti/parent traversal.
- Un health check fallito ripristina applicazione e unit precedenti.
- Il workflow GHCR concede solo `contents: read` e `packages: write`.
- Scarica una Release con tag, non ricompila contenuto branch non revisionato.
- Valida versione, nomi esatti, file non vuoti e tutti gli SHA-256.
- L'unica action esterna è fissata a uno SHA completo.
- Il carrier OCI usa `scratch`, senza package manager o shell.

SHA-256 rileva corruzione locale ma non autentica un amministratore malevolo.
I tag 2.0.0 sono annotati ma non firmati. Gli artefatti Windows non hanno
Authenticode e il DMG non ha Developer ID/notarizzazione. Le release future
destinate alla produzione devono aggiungere questi controlli e firme detached.

## Hardening compilatore

Le build Release abilitano warning, stack protector, `_FORTIFY_SOURCE`, RELRO,
binding immediato, NX/ASLR/high-entropy VA dove supportato e Control Flow Guard
con MSVC. `MemoryDenyWriteExecute` aggiunge W^X runtime su systemd.

## Test di sicurezza

L'integrazione automatica copre traversal, fuga restore via symlink, rifiuto
symlink token, token corto, bind remoto non autenticato, doppio lock,
sostituzione atomica e migrazione vecchio schema SQLite. ASan e UBSan coprono
il codice Linux condiviso.

## Rischi residui

- Nessun TLS o rate limiter incorporato.
- Il polling macOS può rilevare più tardi di un watcher nativo.
- Un attore privilegiato può cambiare radici o componenti intermedi.
- Mutazioni sorgente più rapide dei retry possono richiedere un evento o una
  riconciliazione successiva.
- Artefatti Windows/macOS pubblicati ma non firmati generano avvisi trust.
- SySeBa non rende i dati immutabili e non protegge dalla perdita fisica.

Segnala privatamente le vulnerabilità sospette prima di pubblicare dettagli
di exploit.
