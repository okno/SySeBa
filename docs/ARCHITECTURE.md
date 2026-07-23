# Architecture

[Italiano](ARCHITECTURE.it.md) | [Documentation index](README.md)

## Runtime Topology

SySeBa is one C11 process. The process does not fork workers and does not
invoke shell commands during normal backup operations.

```text
main/CLI
   |
   +-- application state + atomic stop flag
   +-- initial scanner
   +-- filesystem watcher
   +-- event queue ---> N filesystem workers
   +-- log queue ----> one text/SQLite writer
   +-- CivetWeb thread pool
   +-- dashboard renderer (interactive foreground only)
```

`syseba_app_t` owns configuration, queues, worker handles, watcher state,
SQLite writer context, recent events, counters, timing data, and the Web
server context. Initialization is staged; every successful allocation has a
matching partial-failure cleanup path.

## Threading Model

The event and log queues are mutex/condition-variable linked queues. Every
push increments `unfinished`; every consumer calls `task_done`; initial sync
can therefore wait for all queued work without polling queue length.

The principal synchronization domains are:

| Domain | Protection |
|---|---|
| Stop request | C11 `atomic_bool` |
| Queue nodes/counts | Queue mutex and two condition variables |
| Metrics/recent events/config state | Application state mutex |
| Text and SQLite event order | Dedicated log-writer thread |
| Single process | `flock` on POSIX, sharing mode on Windows |

Signal handlers do not lock, allocate, log, or dereference application state.
They set a `sig_atomic_t`/atomic pending flag; the foreground loop performs the
normal shutdown transition.

## Startup Sequence

1. Parse CLI and resolve default/legacy paths.
2. Load and normalize INI configuration.
3. Validate root separation and runtime options.
4. Acquire the process lock.
5. Create backup, restore, log, DB, token, and lock parents.
6. Migrate/open SQLite and start the log writer.
7. Start filesystem workers.
8. Perform initial scan and wait for queued work.
9. Start native watcher unless Web-only.
10. Start Web server when requested.
11. Enter silent service loop or interactive dashboard.

Web-only mode initializes shared state and logging but deliberately omits the
watcher and initial synchronization.

## Event Processing

Watcher implementations translate OS events into the internal create, modify,
delete, move, and rescan model. Workers derive paths relative to the source;
absolute watcher paths are never used directly as backup or restore targets.

Native watcher loss or overflow triggers reconciliation rather than assuming
the event stream is complete. The polling backend compares recursive snapshots
and is also the macOS backend in this release.

## File Commit Protocol

The copy protocol is designed to prevent partial destination files:

1. `stat`/handle query source.
2. Create a unique sibling temporary file with exclusive creation.
3. Reject source/destination final symlinks or reparse points.
4. Stream with bounded buffers.
5. Re-query source identity, size, and modification time.
6. Flush file data.
7. Atomically rename/replace destination.
8. Flush the parent directory on POSIX.

Changing sources cause a retry. This protocol gives atomic visibility on one
filesystem; it cannot make a cross-volume rename atomic. Temporary files are
always created in the destination directory to preserve the same-filesystem
property.

## Delete and Restore Model

A source delete moves the corresponding current backup path under restore.
The relative path is retained. Existing restore names receive a local-time
timestamp and monotonic collision counter.

Restore uses two independently contained joins:

```text
restore root + relative input -> restore source
source root  + relative input -> restored destination
```

Canonical containment and component checks reject traversal and escaping
links. The final input is opened with no-follow/reparse-safe helpers.

## Logging

Producers allocate a structured event and push it to the log queue. The
single writer appends one text record and executes one prepared SQLite insert.
This avoids write interleaving and database connection sharing across worker
threads.

SQLite initialization occurs before the writer starts. Schema changes are
performed in one immediate transaction; WAL is enabled after opening.

## Web Layer

CivetWeb, cJSON, SQLite, HTML, JavaScript, and the logo are compiled into the
executable. `cmake/EmbedAssets.cmake` emits byte arrays, so installed services
do not depend on a working directory or loose static files.

The Web handler authenticates before dispatching data routes. Request bodies
are bounded at 64 KiB. Mutating requests call the same config and restore
functions used by the CLI; business rules are not duplicated in JavaScript.

## Platform Boundaries

| Facility | Linux | Windows | macOS |
|---|---|---|---|
| Watcher | inotify | ReadDirectoryChangesW | polling |
| Threads | pthread | Win32 threads | pthread |
| Lock | flock | CreateFile sharing | flock |
| Service | systemd | SCM | launchd |
| Crypto RNG | getrandom/OS source | BCryptGenRandom | OS source |
| Atomic replace | rename | MoveFileEx | rename |

Platform-specific sources implement only these boundaries; the application,
queues, copy logic, database, CLI, dashboard, and Web API remain shared.
