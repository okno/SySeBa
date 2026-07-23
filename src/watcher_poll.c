#include "syseba_internal.h"

static uint64_t syseba_snapshot_hash(const char *value)
{
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor != '\0') {
        hash ^= *cursor++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static syseba_snapshot_entry_t *syseba_snapshot_find(
    const syseba_snapshot_t *snapshot,
    const char *path)
{
    size_t bucket;
    syseba_snapshot_entry_t *entry;
    if (snapshot->bucket_count == 0) {
        return NULL;
    }
    bucket = (size_t)(syseba_snapshot_hash(path) % snapshot->bucket_count);
    for (entry = snapshot->buckets[bucket]; entry != NULL; entry = entry->next) {
#ifdef _WIN32
        if (_stricmp(entry->path, path) == 0) {
#else
        if (strcmp(entry->path, path) == 0) {
#endif
            return entry;
        }
    }
    return NULL;
}

static int syseba_snapshot_insert(syseba_snapshot_t *snapshot,
                                  const char *path,
                                  uint64_t size,
                                  int64_t mtime_ns,
                                  bool directory)
{
    size_t bucket = (size_t)(syseba_snapshot_hash(path) %
                             snapshot->bucket_count);
    syseba_snapshot_entry_t *entry =
        (syseba_snapshot_entry_t *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return -1;
    }
    entry->path = syseba_strdup(path);
    if (entry->path == NULL) {
        free(entry);
        return -1;
    }
    entry->size = size;
    entry->mtime_ns = mtime_ns;
    entry->directory = directory;
    entry->next = snapshot->buckets[bucket];
    snapshot->buckets[bucket] = entry;
    snapshot->size++;
    return 0;
}

static int syseba_snapshot_build_entry(const char *path,
                                       bool directory,
                                       void *opaque)
{
    syseba_snapshot_t *snapshot = (syseba_snapshot_t *)opaque;
    uint64_t size = 0;
    int64_t mtime_ns = 0;
    bool stat_directory = false;
    if (syseba_fs_stat(path, &size, &mtime_ns, &stat_directory) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    return syseba_snapshot_insert(snapshot,
                                  path,
                                  size,
                                  mtime_ns,
                                  directory);
}

int syseba_snapshot_build(const char *root, syseba_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->bucket_count = 4093;
    snapshot->buckets = (syseba_snapshot_entry_t **)calloc(
        snapshot->bucket_count,
        sizeof(*snapshot->buckets));
    if (snapshot->buckets == NULL) {
        return -1;
    }
    if (syseba_walk_files(root,
                          syseba_snapshot_build_entry,
                          snapshot) != 0) {
        syseba_snapshot_destroy(snapshot);
        return -1;
    }
    return 0;
}

void syseba_snapshot_destroy(syseba_snapshot_t *snapshot)
{
    if (snapshot->buckets != NULL) {
        for (size_t bucket = 0; bucket < snapshot->bucket_count; bucket++) {
            syseba_snapshot_entry_t *entry = snapshot->buckets[bucket];
            while (entry != NULL) {
                syseba_snapshot_entry_t *next = entry->next;
                free(entry->path);
                free(entry);
                entry = next;
            }
        }
    }
    free(snapshot->buckets);
    memset(snapshot, 0, sizeof(*snapshot));
}

void syseba_snapshot_reconcile(syseba_app_t *app,
                               syseba_snapshot_t *previous,
                               syseba_snapshot_t *current)
{
    for (size_t bucket = 0; bucket < current->bucket_count; bucket++) {
        for (syseba_snapshot_entry_t *entry = current->buckets[bucket];
             entry != NULL;
             entry = entry->next) {
            syseba_snapshot_entry_t *old =
                syseba_snapshot_find(previous, entry->path);
            if (old == NULL) {
                (void)syseba_app_enqueue(app,
                                         SYSEBA_EVENT_CREATE,
                                         entry->path,
                                         entry->directory);
            } else {
                old->seen = true;
                if (!entry->directory &&
                    (entry->size != old->size ||
                     entry->mtime_ns != old->mtime_ns)) {
                    (void)syseba_app_enqueue(app,
                                             SYSEBA_EVENT_MODIFY,
                                             entry->path,
                                             false);
                }
            }
        }
    }
    for (size_t bucket = 0; bucket < previous->bucket_count; bucket++) {
        for (syseba_snapshot_entry_t *entry = previous->buckets[bucket];
             entry != NULL;
             entry = entry->next) {
            if (!entry->seen) {
                (void)syseba_app_enqueue(app,
                                         SYSEBA_EVENT_DELETE,
                                         entry->path,
                                         entry->directory);
            }
        }
    }
}

void *syseba_watcher_poll_main(void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    syseba_snapshot_t previous = {0};
    unsigned interval_ms = 2000;
    const char *configured_interval = getenv("SYSEBA_POLL_INTERVAL_MS");

    if (configured_interval != NULL && *configured_interval != '\0') {
        unsigned long parsed = strtoul(configured_interval, NULL, 10);
        if (parsed >= 250 && parsed <= 60000) {
            interval_ms = (unsigned)parsed;
        }
    }
    if (syseba_snapshot_build(app->config.source, &previous) != 0) {
        syseba_log_emit(app,
                        "ERROR",
                        "WATCH",
                        app->config.source,
                        "",
                        strerror(errno),
                        "%s: %s",
                        syseba_tr(app->language,
                                  "Impossibile inizializzare il watcher polling",
                                  "Unable to initialize polling watcher"),
                        strerror(errno));
        return NULL;
    }
    syseba_log_emit(app,
                    "INFO",
                    "WATCH",
                    app->config.source,
                    "",
                    "poll",
                    "%s (%u ms)",
                    syseba_tr(app->language,
                              "Watcher polling attivo",
                              "Polling watcher active"),
                    interval_ms);

    while (!syseba_app_is_stopping(app)) {
        unsigned slept = 0;
        while (!syseba_app_is_stopping(app) && slept < interval_ms) {
            unsigned slice = interval_ms - slept > 200
                                 ? 200
                                 : interval_ms - slept;
            syseba_sleep_ms(slice);
            slept += slice;
        }
        if (!syseba_app_is_stopping(app)) {
            syseba_snapshot_t current = {0};
            if (syseba_snapshot_build(app->config.source, &current) == 0) {
                syseba_snapshot_reconcile(app, &previous, &current);
                syseba_snapshot_destroy(&previous);
                previous = current;
            } else {
                syseba_log_emit(app,
                                "ERROR",
                                "WATCH",
                                app->config.source,
                                "",
                                strerror(errno),
                                "%s: %s",
                                syseba_tr(app->language,
                                          "Scansione watcher fallita",
                                          "Watcher scan failed"),
                                strerror(errno));
            }
        }
    }
    syseba_snapshot_destroy(&previous);
    return NULL;
}
