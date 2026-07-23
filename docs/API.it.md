# API HTTP

[English](API.md) | [Indice documentazione](README.it.md)

## Trasporto e autenticazione

Endpoint predefinito del servizio: `http://SERVER:8765`.

Tutte le route `/api/*`, eccetto `GET /api/auth`, e
`GET /restore/download` richiedono:

```http
X-SySeBa-Token: token-generato-64-hex
```

Le risposte sono JSON, salvo i download. Le risposte JSON riuscite usano HTTP
200. Gli errori hanno forma:

```json
{
  "ok": false,
  "error": "invalid_request",
  "message": "Dettaglio leggibile"
}
```

I codici più comuni sono 400, 401, 403, 404, 409, 413, 500 e 503. Sono
implementati soltanto GET e POST.

## Stato autenticazione

```http
GET /api/auth
```

```json
{"required": true}
```

Non restituisce dati operativi.

## Stato

```http
GET /api/status
X-SySeBa-Token: ...
```

L'oggetto contiene versione/lingua, stato processo/watcher/Web, orari di avvio
e tempo trascorso, sincronizzazione iniziale, CPU/memoria, coda, contatori,
oggetti disco source/backup/restore, eventi recenti, configurazione attiva e
confronto tra configurazione salvata e attiva.

Le release compatibili possono aggiungere campi. I client devono ignorare i
membri sconosciuti.

## Log

```http
GET /api/logs?lines=200
```

`lines` vale 200 per default ed è limitato a 2.000.

```json
{"lines":["2026-07-23T05:10:00 ..."]}
```

## Configurazione

```http
GET /api/config
GET /api/config/state
```

Aggiornamento di alcuni o tutti i campi:

```http
POST /api/config
Content-Type: application/json
X-SySeBa-Token: ...

{
  "source": "/srv/syseba/source",
  "backup": "/srv/syseba/backup",
  "restore": "/srv/syseba/restore",
  "log_file": "/var/log/syseba/syseba.log",
  "threads": 4
}
```

Il server normalizza e valida la configurazione risultante completa, poi la
salva atomicamente. Risposta:

```json
{
  "ok": true,
  "restart_required": true,
  "config": {},
  "state": {},
  "message": "Configuration saved. Restart SySeBa to apply it."
}
```

I body JSON non possono superare 64 KiB.

## Elenco restore

```http
GET /api/restore?path=&search=&page=1&page_size=100&sort=name&direction=asc
```

`page_size` è limitato a 250. `sort` accetta `name`, `mtime` o `size`;
`direction` accetta `asc` o `desc`.

```json
{
  "path": "",
  "is_file": false,
  "items": [
    {
      "name": "report.pdf",
      "path": "documents/report.pdf",
      "is_dir": false,
      "size": 1234,
      "size_human": "1.2 KB",
      "mtime": "2026-07-23T05:00:00",
      "destination_exists": false
    }
  ],
  "search": "",
  "sort": "name",
  "direction": "asc",
  "page": 1,
  "page_size": 100,
  "pages": 1,
  "total": 1,
  "has_previous": false,
  "has_next": false
}
```

## Informazioni restore e download

```http
GET /api/restore/info?path=documents/report.pdf
GET /restore/download?path=documents/report.pdf
```

Solo i file regolari possono essere scaricati. Il download usa
`application/octet-stream`, `Cache-Control: no-store` e un nome allegato
sanitizzato.

## Mutazione restore

```http
POST /api/restore
Content-Type: application/json
X-SySeBa-Token: ...

{"path":"documents/report.pdf","strategy":"rename"}
```

`strategy` accetta `fail`, `rename` o `overwrite`.

```json
{
  "ok": true,
  "restored_to": "/srv/syseba/source/documents/report.restored-20260723-051200.pdf",
  "strategy": "rename",
  "message": "Item restored successfully."
}
```

Una destinazione esistente con `fail` restituisce HTTP 409 e
`destination_exists`.

## Riavvio servizio

```http
POST /api/service/restart
Content-Type: application/json
X-SySeBa-Token: ...

{}
```

Restituisce 503 quando piattaforma o sessione corrente non possono chiedere un
riavvio gestito. Con systemd il riavvio viene pianificato dopo l'invio della
risposta.

## Asset browser

`GET /`, `/webui.js`, `/logo` e `/favicon.ico` sono incorporati. Il server
gestisce la cache e non richiede CDN esterne.

## Esempio curl

```bash
token=$(sudo cat /etc/syseba/syseba_web.token)
curl -fsS \
  -H "X-SySeBa-Token: $token" \
  http://127.0.0.1:8765/api/status
```

Non inserire il token nella query string o nei log di shell condivisi.
