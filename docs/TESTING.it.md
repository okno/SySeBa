# Strategia di test

[English](TESTING.md) | [Indice documentazione](README.it.md)

## Livelli di test

### Unit test nativi

`tests_c/test_native.c` verifica normalizzazione/rifiuto configurazione,
operazioni sui percorsi, code, copia atomica, symlink di destinazione e helper
condivisi di piattaforma.

```bash
ctest --test-dir build -R native-unit --output-on-failure
```

### Integrazione Linux

`tests_c/integration_linux.sh` avvia un vero processo temporaneo e verifica:

- sincronizzazione iniziale;
- propagazione inotify ed eliminazione verso restore;
- file con stessa dimensione e timestamp retrodatato;
- migrazione schema SQLite legacy;
- comportamento HTTP protetto e non protetto;
- persistenza token e rifiuto symlink;
- rifiuto bind remoto senza autenticazione;
- API elenco restore/configurazione;
- traversal e fuga tramite symlink;
- rifiuto seconda istanza e status basato su lock kernel;
- arresto pulito e rimozione temporanei.

Se il processo termina inaspettatamente, lo script stampa lo stderr acquisito.

### Integrazione Windows

`tests_c/integration_windows.ps1` avvia su Windows l'eseguibile cross-compilato
e valida sincronizzazione iniziale, autenticazione/stato Web, propagazione
eventi, log testuale e terminazione pulita. Il log viene aperto con sharing per
riflettere l'accesso concorrente del servizio Windows.

### Integrazione manutenzione

`tests_c/maintenance_integration.sh` verifica snapshot a servizio fermo,
manifest/checksum, selettori esatti, rifiuto di membri archivio malevoli e
rollback in una fixture isolata.

### Integrazione package pubblicato

Il workflow GitHub Actions manuale scarica la Release selezionata e fallisce
se i nove asset attesi non sono presenti, non vuoti e coperti da un manifest
checksum valido. Dopo la pubblicazione l'accettazione richiede anche:

- visibilità pubblica;
- collegamento package a `okno/SySeBa`;
- tag versione e `latest` sul digest previsto;
- pull anonimo riuscito;
- nove file estratti da `/packages` con SHA-256 uguali alla Release.

## Sanitizer

ASan e UBSan eseguono tutti i target CTest Linux. Un heap use-after-free nella
durata della strategia POST restore è stato corretto convertendo il testo
analizzato in nomi enum statici prima di eliminare l'albero cJSON.

TSan deve essere eseguito su CI Linux nativa. Alcuni layout WSL fanno
terminare il runtime prima del codice applicativo; ciò non costituisce un test
race superato.

## Analisi statica e script

- Warning GCC: `-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion`.
- `cppcheck --force` sul C proprietario.
- ShellCheck su script Bash e package.
- Parsing PowerShell e integrazione Windows.
- `systemd-analyze verify` sulla unit pacchettizzata.

## Matrice di accettazione release

| Controllo | Linux | Windows | macOS |
|---|---|---|---|
| Compilazione Release | Obbligatoria | Obbligatoria | Entrambi i target thin |
| Unit test nativo | Eseguito | Eseguito | Solo cross-build |
| Integrazione | Eseguita | Eseguita su host Windows | Test manuale target |
| Formato binario | ELF | PE32+ | Universal Mach-O |
| Ispezione package | tar/DEB/RPM | ZIP/NSIS | Estrazione DMG/HFS |
| Manifest hash | Obbligatorio | Obbligatorio | Obbligatorio |
| Estrazione mirror OCI | Obbligatoria per ogni artefatto | Obbligatoria | Obbligatoria |

Il cross-builder Linux non può attestare l'esecuzione funzionale macOS. DMG e
slice Mach-O sono verificati strutturalmente; launchd, watcher, Gatekeeper e
restore devono essere provati su Intel e Apple Silicon prima di considerare
l'artefatto macOS validato per produzione.

## Checklist regressione manuale

1. Avvia con una copia di un vecchio database SQLite.
2. Verifica una copia iniziale e una modifica live.
3. Elimina dalla sorgente e trova l'elemento nel restore.
4. Ripristina con strategie fail, rename e overwrite.
5. Salva la configurazione e verifica lo stato restart-required.
6. Riavvia e controlla che la nuova configurazione diventi attiva.
7. Apri la CLI con terminale stretto e largo.
8. Apri la Web UI a larghezze desktop e mobile.
9. Arresta durante una coda attiva e verifica la terminazione ordinata.
10. Esegui snapshot manutenzione, aggiornamento e rollback.
11. Scarica il package OCI pubblico, estrai `/packages` e verifica SHA-256.
