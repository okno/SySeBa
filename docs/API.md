# HTTP API

[Italiano](API.it.md) | [Documentation index](README.md)

## Transport and Authentication

Default service endpoint: `http://SERVER:8765`.

All `/api/*` routes except `GET /api/auth`, plus
`GET /restore/download`, require:

```http
X-SySeBa-Token: 64-hex-generated-token
```

Responses are JSON except downloads. Successful JSON responses use HTTP 200.
Errors have:

```json
{
  "ok": false,
  "error": "invalid_request",
  "message": "Human-readable detail"
}
```

Common status codes are 400, 401, 403, 404, 409, 413, 500, and 503.
Only GET and POST are implemented.

## Authentication State

```http
GET /api/auth
```

```json
{"required": true}
```

No operational data is returned.

## Status

```http
GET /api/status
X-SySeBa-Token: ...
```

The object contains version/language, running/watcher/Web state, start and
elapsed times, initial-sync state, process CPU/memory, queue and operation
counters, disk objects for source/backup/restore, recent events, active
configuration, and saved/active configuration comparison.

Fields may be added in compatible releases. Clients should ignore unknown
members.

## Logs

```http
GET /api/logs?lines=200
```

`lines` defaults to 200 and is capped at 2,000.

```json
{"lines":["2026-07-23T05:10:00 ..."]}
```

## Configuration

```http
GET /api/config
GET /api/config/state
```

Update any or all fields:

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

The server normalizes and validates the complete resulting configuration,
then atomically saves it. Response:

```json
{
  "ok": true,
  "restart_required": true,
  "config": {},
  "state": {},
  "message": "Configuration saved. Restart SySeBa to apply it."
}
```

JSON request bodies cannot exceed 64 KiB.

## Restore Listing

```http
GET /api/restore?path=&search=&page=1&page_size=100&sort=name&direction=asc
```

`page_size` is capped at 250. `sort` is `name`, `mtime`, or `size`;
`direction` is `asc` or `desc`.

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

## Restore Information and Download

```http
GET /api/restore/info?path=documents/report.pdf
GET /restore/download?path=documents/report.pdf
```

Only regular files can be downloaded. The download response is
`application/octet-stream`, `Cache-Control: no-store`, and a sanitized
attachment filename.

## Restore Mutation

```http
POST /api/restore
Content-Type: application/json
X-SySeBa-Token: ...

{"path":"documents/report.pdf","strategy":"rename"}
```

`strategy` is `fail`, `rename`, or `overwrite`.

```json
{
  "ok": true,
  "restored_to": "/srv/syseba/source/documents/report.restored-20260723-051200.pdf",
  "strategy": "rename",
  "message": "Item restored successfully."
}
```

An existing destination with `fail` returns HTTP 409 and
`destination_exists`.

## Service Restart

```http
POST /api/service/restart
Content-Type: application/json
X-SySeBa-Token: ...

{}
```

Returns 503 when the current platform/session cannot request a managed
restart. On systemd, restart is scheduled after the response can be sent.

## Browser Assets

`GET /`, `/webui.js`, `/logo`, and `/favicon.ico` are embedded assets. They
are cache-controlled by the server and require no external CDN.

## curl Example

```bash
token=$(sudo cat /etc/syseba/syseba_web.token)
curl -fsS \
  -H "X-SySeBa-Token: $token" \
  http://127.0.0.1:8765/api/status
```

Do not place the token in a query string or shared shell logs.
