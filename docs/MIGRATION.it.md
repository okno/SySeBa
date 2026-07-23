# Migrazione e rollback

[English](MIGRATION.md) | [Indice documentazione](README.it.md)

## Obiettivo

Migrare un'installazione attiva dell'epoca Python in `/opt/syseba` al runtime C
senza perdere lo stato esatto precedente all'aggiornamento e senza modificare
gli alberi configurati source, backup o restore.

## Prerequisiti

- Host Linux con systemd.
- Servizio esistente `syseba.service`.
- Privilegi root.
- Checkout separato, normalmente `/root/SySeBa-release`.
- Compilatore C, CMake, Git, tar, strumenti SHA-256 e spazio sufficiente.
- Configurazione esistente compatibile con le nuove regole di separazione.

Non clonare in `/opt/syseba`: quella directory è l'oggetto del rollback.

Usa `main` per la linea C nativa. La precedente implementazione Python rimane
al tag `v1.0.0-python` e nel ramo `legacy/python`; non spostare su quel ramo il
checkout usato per la migrazione C.

## Migrazione automatizzata

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

`migrate-from-python.sh` delega a:

```bash
sudo ./scripts/syseba-maintenance.sh install-local
```

## Confine transazionale

Operazioni prima dell'arresto:

1. validazione percorsi e acquisizione lock di manutenzione;
2. configurazione/compilazione candidato in directory temporanea adiacente;
3. test nativi e integrazioni disponibili;
4. `config-check` del candidato sulla configurazione corrente.

Operazioni a servizio fermo:

1. registrazione dello stato active/enabled precedente;
2. arresto e attesa inattività systemd;
3. archivio installazione e archivio stato esterno;
4. hash di archivi e manifest;
5. copia dello stato nell'installazione C preparata;
6. rename dell'installazione attiva in una directory previous adiacente;
7. rename dello stage sul percorso installazione attivo;
8. generazione unit systemd irrobustita e avvio;
9. verifica servizio active e risposta locale `/api/auth`.

Un errore dopo il punto 6 attiva il rollback automatico. Il candidato fallito
resta in quarantena adiacente per la diagnosi.

## Contenuto dello snapshot

Incluso:

- intera directory installazione, esclusi i lock;
- configurazione, DB, WAL, SHM, token Web e log applicativo esterni;
- unit systemd attiva;
- ACL, attributi estesi e owner numerici;
- motivo, stato servizio, percorsi, identità software e checksum.

Escluso:

- albero source configurato;
- albero backup configurato;
- albero restore configurato.

Il rollback software resta così limitato e non può copiare o sostituire i
volumi dati dell'utente.

## Selezione rollback

```bash
sudo ./scripts/syseba-maintenance.sh list
sudo ./scripts/syseba-maintenance.sh rollback
```

Selettori diretti:

```bash
sudo ./scripts/syseba-maintenance.sh rollback latest
sudo ./scripts/syseba-maintenance.sh rollback pre-update
sudo ./scripts/syseba-maintenance.sh rollback YYYYMMDD-HHMMSS
```

Prima dell'estrazione vengono validati manifest SHA-256, identità della
directory installazione e ogni membro degli archivi. Elementi assoluti o con
parent traversal vengono rifiutati.

## Garanzia di rollback

`pre-update` ripristina applicazione fermata, configurazione, file SQLite,
token, log testuale, owner/ACL e unit systemd acquisiti immediatamente prima
dello switch. Non riporta indietro nel tempo i volumi dati. Se la nuova
versione ha elaborato eventi source, tali effetti richiedono snapshot storage
o le normali operazioni di restore.

## Aggiornamenti Git successivi

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/syseba-maintenance.sh quick-update main
```

I primi tre comandi aggiornano solo il checkout di manutenzione.
`quick-update` confronta il commit remoto con `BUILD-INFO` installato e, se
necessario, esegue la stessa transazione.

## Recupero se systemd non funziona

Non improvvisare l'estrazione di archivi sopra un demone attivo. Controlla:

```bash
sudo ./scripts/syseba-maintenance.sh list
sudo sha256sum -c /root/syseba-backups/ID/SHA256SUMS
sudo systemctl stop syseba.service
```

Preferisci il rollback guidato: conserva l'installazione fallita e annulla
anche un tentativo di rollback non riuscito. L'estrazione manuale è una
procedura amministrativa di ultima istanza.
