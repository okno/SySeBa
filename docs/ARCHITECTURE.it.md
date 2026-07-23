# Architettura

[English](ARCHITECTURE.md) | [Indice documentazione](README.it.md)

## Topologia runtime

SySeBa è un singolo processo C11. Non esegue fork di worker e non invoca
comandi shell durante le normali operazioni di backup.

```text
main/CLI
   |
   +-- stato applicazione + flag atomico di arresto
   +-- scanner iniziale
   +-- watcher filesystem
   +-- coda eventi ---> N worker filesystem
   +-- coda log ------> un writer testo/SQLite
   +-- pool thread CivetWeb
   +-- renderer dashboard (solo foreground interattivo)
```

`syseba_app_t` possiede configurazione, code, handle dei worker, stato del
watcher, contesto del writer SQLite, eventi recenti, contatori, tempi e
contesto del server Web. L'inizializzazione è a fasi; ogni allocazione riuscita
ha un percorso di cleanup anche in caso di errore parziale.

## Modello di threading

Le code eventi e log sono liste concatenate protette da mutex e condition
variable. Ogni inserimento incrementa `unfinished`; ogni consumer chiama
`task_done`. La sincronizzazione iniziale può quindi attendere il completamento
di tutto il lavoro senza interrogare ripetutamente la lunghezza della coda.

I principali domini di sincronizzazione sono:

| Dominio | Protezione |
|---|---|
| Richiesta di arresto | `atomic_bool` C11 |
| Nodi e contatori delle code | Mutex e due condition variable |
| Metriche, eventi recenti e configurazione | Mutex dello stato applicazione |
| Ordine eventi testo/SQLite | Thread writer dedicato |
| Istanza singola | `flock` POSIX, sharing mode Windows |

I signal handler non acquisiscono lock, non allocano, non scrivono log e non
accedono allo stato applicativo. Impostano un flag `sig_atomic_t`/atomico; il
loop foreground esegue poi la normale transizione di arresto.

## Sequenza di avvio

1. Analisi CLI e risoluzione dei percorsi predefiniti o legacy.
2. Caricamento e normalizzazione della configurazione INI.
3. Validazione di separazione delle radici e opzioni runtime.
4. Acquisizione del lock di processo.
5. Creazione delle directory padre per backup, restore, log, DB, token e lock.
6. Migrazione/apertura SQLite e avvio del writer log.
7. Avvio dei worker filesystem.
8. Scansione iniziale e attesa del lavoro accodato.
9. Avvio watcher nativo, salvo modalità solo Web.
10. Avvio del server Web quando richiesto.
11. Ingresso nel loop servizio silenzioso o nella dashboard interattiva.

La modalità `--web-only` inizializza stato condiviso e logging, ma omette
deliberatamente watcher e sincronizzazione iniziale.

## Elaborazione eventi

I watcher traducono gli eventi del sistema operativo nel modello interno
create, modify, delete, move e rescan. I worker calcolano i percorsi relativi
alla sorgente; i percorsi assoluti ricevuti dal watcher non vengono mai usati
direttamente come destinazione backup o restore.

La perdita di eventi o l'overflow del watcher causa una riconciliazione, senza
presumere che lo stream sia completo. Il backend polling confronta snapshot
ricorsivi ed è anche il backend macOS di questa release.

## Protocollo di commit dei file

Il protocollo di copia evita destinazioni parziali:

1. interrogazione `stat`/handle della sorgente;
2. creazione esclusiva di un temporaneo univoco accanto alla destinazione;
3. rifiuto di symlink/reparse point finali;
4. copia streaming con buffer limitati;
5. nuovo controllo di identità, dimensione e timestamp sorgente;
6. flush dei dati;
7. rename/sostituzione atomica della destinazione;
8. flush della directory padre su POSIX.

Una sorgente modificata durante la copia causa un retry. La visibilità atomica
vale sullo stesso filesystem; un rename fra volumi non può essere reso
atomico. Il temporaneo viene quindi sempre creato nella directory di
destinazione.

## Modello eliminazione e restore

L'eliminazione dalla sorgente sposta il percorso di backup corrente sotto
`restore`, conservando il percorso relativo. I conflitti ricevono timestamp
locale e contatore monotono.

Il restore usa due join contenuti e indipendenti:

```text
radice restore + input relativo -> sorgente restore
radice source  + input relativo -> destinazione ripristinata
```

Controlli canonici di contenimento e dei componenti rifiutano traversal e link
in fuga. L'oggetto finale viene aperto con helper no-follow o reparse-safe.

## Logging

I producer allocano un evento strutturato e lo inseriscono nella coda log. Un
solo writer aggiunge il record testuale ed esegue un insert SQLite preparato.
In questo modo non esistono interleaving di scrittura né condivisione della
connessione database tra worker.

SQLite viene inizializzato prima del writer. Le modifiche di schema sono
eseguite in una transazione immediata; WAL viene abilitato dopo l'apertura.

## Livello Web

CivetWeb, cJSON, SQLite, HTML, JavaScript e logo sono compilati
nell'eseguibile. `cmake/EmbedAssets.cmake` genera array di byte: il servizio
installato non dipende dalla working directory o da file statici separati.

L'handler Web autentica prima del dispatch delle route dati. I body sono
limitati a 64 KiB. Le richieste mutative usano le stesse funzioni di
configurazione e restore della CLI; le regole di business non sono duplicate
nel JavaScript.

## Confini di piattaforma

| Funzione | Linux | Windows | macOS |
|---|---|---|---|
| Watcher | inotify | ReadDirectoryChangesW | polling |
| Thread | pthread | Win32 threads | pthread |
| Lock | flock | CreateFile sharing | flock |
| Servizio | systemd | SCM | launchd |
| RNG crittografico | getrandom/sorgente OS | BCryptGenRandom | sorgente OS |
| Sostituzione atomica | rename | MoveFileEx | rename |

I sorgenti specifici di piattaforma implementano solo questi confini.
Applicazione, code, copia, database, CLI, dashboard e API Web restano condivisi.
