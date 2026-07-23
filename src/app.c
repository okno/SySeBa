#include "syseba_internal.h"

#include "cJSON.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    syseba_app_t *app;
    uint64_t files;
    bool enqueue;
    int errors;
} syseba_sync_context_t;

static bool syseba_files_need_copy(const char *source, const char *destination)
{
    uint64_t source_size = 0;
    uint64_t destination_size = 0;
    int64_t source_mtime = 0;
    int64_t destination_mtime = 0;
    bool source_directory = false;
    bool destination_directory = false;

    if (syseba_fs_stat(source,
                       &source_size,
                       &source_mtime,
                       &source_directory) != 0) {
        return false;
    }
    if (syseba_fs_stat(destination,
                       &destination_size,
                       &destination_mtime,
                       &destination_directory) != 0) {
        return true;
    }
    return source_directory != destination_directory ||
           source_size != destination_size ||
           source_mtime != destination_mtime;
}

static void syseba_stats_increment(syseba_app_t *app, uint64_t *value)
{
    syseba_mutex_lock(&app->state_mutex);
    (*value)++;
    syseba_mutex_unlock(&app->state_mutex);
}

static int syseba_app_enqueue_internal(syseba_app_t *app,
                                       syseba_event_kind_t kind,
                                       const char *path,
                                       bool is_directory,
                                       bool initial_sync)
{
    syseba_event_t *event;
    if (syseba_app_is_stopping(app) ||
        !syseba_path_is_within(app->config.source, path)) {
        return SYSEBA_ERR_INVALID;
    }
    event = (syseba_event_t *)calloc(1, sizeof(*event));
    if (event == NULL) {
        return SYSEBA_ERR;
    }
    event->kind = kind;
    event->is_directory = is_directory;
    event->initial_sync = initial_sync;
    event->observed_ns = syseba_monotonic_ns();
    if (snprintf(event->path,
                 sizeof(event->path),
                 "%s",
                 path) >= (int)sizeof(event->path)) {
        free(event);
        return SYSEBA_ERR_INVALID;
    }
    syseba_mutex_lock(&app->state_mutex);
    app->stats.queued_events++;
    syseba_mutex_unlock(&app->state_mutex);
    if (syseba_queue_push(&app->event_queue, event) != 0) {
        free(event);
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}

int syseba_app_enqueue(syseba_app_t *app,
                       syseba_event_kind_t kind,
                       const char *path,
                       bool is_directory)
{
    return syseba_app_enqueue_internal(app,
                                       kind,
                                       path,
                                       is_directory,
                                       false);
}

int syseba_app_enqueue_initial(syseba_app_t *app,
                               syseba_event_kind_t kind,
                               const char *path,
                               bool is_directory)
{
    return syseba_app_enqueue_internal(app,
                                       kind,
                                       path,
                                       is_directory,
                                       true);
}

static int syseba_prepare_runtime_paths(syseba_app_t *app,
                                        char *error,
                                        size_t error_size)
{
    char parent[SYSEBA_PATH_MAX];
    const char *files[] = {
        app->config.log_file,
        app->db_path,
        app->lock_path,
    };

    if (syseba_mkdirs(app->config.backup, 0750) != 0 ||
        syseba_mkdirs(app->config.restore, 0750) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to create backup/restore directory: %s",
                       strerror(errno));
        return -1;
    }
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++) {
        if (syseba_parent_dir(files[index], parent, sizeof(parent)) != 0 ||
            syseba_mkdirs(parent, 0750) != 0) {
            (void)snprintf(error,
                           error_size,
                           "Unable to create runtime directory for %s: %s",
                           files[index],
                           strerror(errno));
            return -1;
        }
    }
    return 0;
}

int syseba_app_acquire_lock(syseba_app_t *app,
                            char *error,
                            size_t error_size)
{
#ifdef _WIN32
    int descriptor;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION information = {0};
    FILE *file;
    handle = CreateFileA(app->lock_path,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ,
                         NULL,
                         OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        (void)snprintf(error,
                       error_size,
                       "SySeBa is already running or lock is inaccessible: %s",
                       app->lock_path);
        return SYSEBA_ERR_BUSY;
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_TEXT);
    if (descriptor < 0) {
        CloseHandle(handle);
        return SYSEBA_ERR;
    }
    file = _fdopen(descriptor, "w+");
    if (file == NULL) {
        _close(descriptor);
        return SYSEBA_ERR;
    }
    if (_chsize_s(descriptor, 0) != 0) {
        fclose(file);
        return SYSEBA_ERR;
    }
    rewind(file);
#else
    int descriptor = open(app->lock_path,
                          O_RDWR | O_CREAT | O_CLOEXEC
#ifdef O_NOFOLLOW
                              | O_NOFOLLOW
#endif
                          ,
                          0640);
    struct stat lock_stat;
    FILE *file;
    if (descriptor < 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to open lock %s: %s",
                       app->lock_path,
                       strerror(errno));
        return SYSEBA_ERR_PERMISSION;
    }
    if (fstat(descriptor, &lock_stat) != 0 ||
        !S_ISREG(lock_stat.st_mode)) {
        close(descriptor);
        (void)snprintf(error,
                       error_size,
                       "Lock path is not a regular file: %s",
                       app->lock_path);
        return SYSEBA_ERR_PERMISSION;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        char pid_buffer[64] = {0};
        ssize_t count = read(descriptor,
                             pid_buffer,
                             sizeof(pid_buffer) - 1u);
        close(descriptor);
        (void)snprintf(error,
                       error_size,
                       "SySeBa is already running%s%s",
                       count > 0 ? " with PID " : "",
                       count > 0 ? pid_buffer : "");
        return SYSEBA_ERR_BUSY;
    }
    if (ftruncate(descriptor, 0) != 0 || lseek(descriptor, 0, SEEK_SET) < 0) {
        close(descriptor);
        return SYSEBA_ERR;
    }
    file = fdopen(descriptor, "w+");
    if (file == NULL) {
        close(descriptor);
        return SYSEBA_ERR;
    }
#endif
    if (fprintf(file, "%" PRIu64 "\n", (uint64_t)syseba_process_id()) < 0 ||
        fflush(file) != 0
#ifdef _WIN32
        || _commit(_fileno(file)) != 0
#else
        || fsync(fileno(file)) != 0
#endif
    ) {
        fclose(file);
        return SYSEBA_ERR;
    }
    app->lock_file = file;
    return SYSEBA_OK;
}

void syseba_app_release_lock(syseba_app_t *app)
{
    if (app->lock_file != NULL) {
        fclose(app->lock_file);
        app->lock_file = NULL;
    }
}

int syseba_app_init(syseba_app_t *app,
                    const syseba_config_t *config,
                    const syseba_options_t *options,
                    char *error,
                    size_t error_size)
{
    memset(app, 0, sizeof(*app));
    atomic_init(&app->stop_requested, false);
    app->config = *config;
    app->language = options->language;
    app->web_port = options->web_port;
    app->web_enabled = options->web || options->web_only;
    app->web_only = options->web_only;
    app->no_initial_sync = options->no_initial_sync;
    app->silent = options->silent;
    app->recent_capacity = SYSEBA_RECENT_LOGS;
    (void)snprintf(app->lock_path,
                   sizeof(app->lock_path),
                   "%s",
                   options->lock_path);
    (void)snprintf(app->db_path,
                   sizeof(app->db_path),
                   "%s",
                   options->db_path);
    (void)snprintf(app->web_host,
                   sizeof(app->web_host),
                   "%s",
                   options->web_host);
    (void)snprintf(app->initial_state,
                   sizeof(app->initial_state),
                   "%s",
                   app->web_only ? "not_available"
                                 : (app->no_initial_sync ? "skipped" : "pending"));

    if (syseba_mutex_init(&app->state_mutex) != 0) {
        (void)snprintf(error, error_size, "Unable to initialize synchronization");
        return SYSEBA_ERR;
    }
    if (syseba_queue_init(&app->event_queue) != 0) {
        (void)snprintf(error, error_size, "Unable to initialize synchronization");
        syseba_mutex_destroy(&app->state_mutex);
        return SYSEBA_ERR;
    }
    if (syseba_queue_init(&app->log_queue) != 0) {
        (void)snprintf(error, error_size, "Unable to initialize synchronization");
        syseba_queue_destroy(&app->event_queue, free);
        syseba_mutex_destroy(&app->state_mutex);
        return SYSEBA_ERR;
    }
    app->recent_logs = (char **)calloc(app->recent_capacity,
                                       sizeof(*app->recent_logs));
    if (app->recent_logs == NULL) {
        (void)snprintf(error, error_size, "Out of memory");
        syseba_queue_destroy(&app->log_queue, free);
        syseba_queue_destroy(&app->event_queue, free);
        syseba_mutex_destroy(&app->state_mutex);
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}

static void syseba_initial_progress(syseba_app_t *app,
                                    bool copied,
                                    bool skipped)
{
    syseba_mutex_lock(&app->state_mutex);
    app->stats.initial_done++;
    if (copied) {
        app->stats.initial_copied++;
    }
    if (skipped) {
        app->stats.initial_skipped++;
    }
    syseba_mutex_unlock(&app->state_mutex);
}

int syseba_app_process_event(syseba_app_t *app,
                             const syseba_event_t *event)
{
    char relative[SYSEBA_PATH_MAX];
    char backup[SYSEBA_PATH_MAX];
    char restore[SYSEBA_PATH_MAX];
    bool copied = false;
    bool skipped = false;
    int result = SYSEBA_OK;

    if (syseba_relative_path(app->config.source,
                             event->path,
                             relative,
                             sizeof(relative)) != SYSEBA_OK ||
        syseba_safe_join(app->config.backup,
                         relative,
                         backup,
                         sizeof(backup)) != SYSEBA_OK ||
        syseba_safe_join(app->config.restore,
                         relative,
                         restore,
                         sizeof(restore)) != SYSEBA_OK) {
        return SYSEBA_ERR_INVALID;
    }

    if (event->kind == SYSEBA_EVENT_CREATE ||
        event->kind == SYSEBA_EVENT_MODIFY) {
        if (event->is_directory) {
            if (syseba_mkdirs(backup, 0750) != 0) {
                result = SYSEBA_ERR;
            } else if (!event->initial_sync) {
                syseba_log_emit(app,
                                "INFO",
                                "MKDIR",
                                event->path,
                                backup,
                                "",
                                "%s: %s",
                                syseba_tr(app->language,
                                          "Directory creata",
                                          "Directory created"),
                                backup);
            }
            goto completed;
        }
        if (!syseba_fs_is_regular(event->path)) {
            skipped = true;
            syseba_stats_increment(app, &app->stats.skipped);
            goto completed;
        }
        if (event->kind == SYSEBA_EVENT_MODIFY) {
            syseba_sleep_ms(150);
        }
        if (!syseba_files_need_copy(event->path, backup)) {
            skipped = true;
            syseba_stats_increment(app, &app->stats.skipped);
            goto completed;
        }
        if (syseba_copy_file(event->path, backup, 4) != 0) {
            result = errno == ENOENT ? SYSEBA_ERR_NOT_FOUND : SYSEBA_ERR;
            goto completed;
        }
        copied = true;
        if (event->kind == SYSEBA_EVENT_CREATE) {
            syseba_stats_increment(app, &app->stats.copied);
        } else {
            syseba_stats_increment(app, &app->stats.updated);
        }
        syseba_log_emit(
            app,
            "INFO",
            event->kind == SYSEBA_EVENT_CREATE ? "CREATE" : "MODIFY",
            event->path,
            backup,
            "",
            "%s: %s -> %s",
            event->kind == SYSEBA_EVENT_CREATE
                ? syseba_tr(app->language, "Creato", "Created")
                : syseba_tr(app->language, "Modificato", "Modified"),
            event->path,
            backup);
        goto completed;
    }

    if (event->kind == SYSEBA_EVENT_DELETE && syseba_fs_exists(backup)) {
        char destination[SYSEBA_PATH_MAX];
        if (syseba_unique_restore_path(restore,
                                       destination,
                                       sizeof(destination)) != 0 ||
            syseba_move_path(backup, destination) != 0) {
            result = SYSEBA_ERR;
            goto completed;
        }
        syseba_stats_increment(app, &app->stats.deleted);
        syseba_log_emit(app,
                        "INFO",
                        "DELETE",
                        backup,
                        destination,
                        "",
                        "%s: %s",
                        syseba_tr(
                            app->language,
                            "Eliminato dalla sorgente; backup spostato nel restore",
                            "Deleted from source; backup moved to restore"),
                        destination);
    }

completed:
    if (event->initial_sync && !event->is_directory) {
        syseba_initial_progress(app, copied, skipped);
    }
    return result;
}

static void *syseba_worker_main(void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    for (;;) {
        syseba_event_t *event =
            (syseba_event_t *)syseba_queue_pop(&app->event_queue, 500);
        if (event == NULL) {
            if (syseba_queue_is_closed(&app->event_queue)) {
                break;
            }
            continue;
        }
        {
            int result = syseba_app_process_event(app, event);
            if (result != SYSEBA_OK &&
                !(result == SYSEBA_ERR_NOT_FOUND &&
                  event->kind != SYSEBA_EVENT_DELETE)) {
                syseba_log_emit(app,
                                "ERROR",
                                "ERROR",
                                event->path,
                                "",
                                strerror(errno),
                                "%s %s: %s",
                                syseba_tr(app->language,
                                          "Errore durante l'operazione su",
                                          "Error processing"),
                                event->path,
                                strerror(errno));
            }
        }
        free(event);
        syseba_queue_task_done(&app->event_queue);
    }
    return NULL;
}

static int syseba_sync_walk(const char *path,
                            bool directory,
                            void *opaque)
{
    syseba_sync_context_t *context = (syseba_sync_context_t *)opaque;
    if (syseba_app_is_stopping(context->app)) {
        return -1;
    }
    if (!directory) {
        context->files++;
    }
    if (context->enqueue &&
        syseba_app_enqueue_initial(context->app,
                                   SYSEBA_EVENT_CREATE,
                                   path,
                                   directory) != SYSEBA_OK) {
        context->errors++;
    }
    return 0;
}

int syseba_app_initial_sync(syseba_app_t *app)
{
    syseba_sync_context_t context = {app, 0, false, 0};

    syseba_mutex_lock(&app->state_mutex);
    if (app->stats.initial_running) {
        syseba_mutex_unlock(&app->state_mutex);
        return SYSEBA_ERR_BUSY;
    }
    app->stats.initial_running = true;
    app->stats.initial_total = 0;
    app->stats.initial_done = 0;
    app->stats.initial_copied = 0;
    app->stats.initial_skipped = 0;
    (void)snprintf(app->initial_state,
                   sizeof(app->initial_state),
                   "running");
    syseba_iso_now(app->initial_started_at,
                   sizeof(app->initial_started_at));
    app->initial_completed_at[0] = '\0';
    app->initial_error[0] = '\0';
    syseba_mutex_unlock(&app->state_mutex);

    if (syseba_walk_files(app->config.source,
                          syseba_sync_walk,
                          &context) != 0 &&
        !syseba_app_is_stopping(app)) {
        context.errors++;
    }
    syseba_mutex_lock(&app->state_mutex);
    app->stats.initial_total = context.files;
    syseba_mutex_unlock(&app->state_mutex);

    syseba_log_emit(app,
                    "INFO",
                    "SYNC",
                    app->config.source,
                    app->config.backup,
                    "",
                    "%s: %" PRIu64,
                    syseba_tr(app->language,
                              "Sincronizzazione iniziale avviata; file trovati",
                              "Initial synchronization started; files found"),
                    context.files);

    context.enqueue = true;
    context.files = 0;
    if (!syseba_app_is_stopping(app) &&
        syseba_walk_files(app->config.source,
                          syseba_sync_walk,
                          &context) != 0) {
        context.errors++;
    }
    (void)syseba_queue_wait_empty(&app->event_queue, 24u * 60u * 60u * 1000u);

    syseba_mutex_lock(&app->state_mutex);
    app->stats.initial_running = false;
    syseba_iso_now(app->initial_completed_at,
                   sizeof(app->initial_completed_at));
    if (syseba_app_is_stopping(app)) {
        (void)snprintf(app->initial_state,
                       sizeof(app->initial_state),
                       "stopped");
    } else if (context.errors > 0) {
        (void)snprintf(app->initial_state,
                       sizeof(app->initial_state),
                       "completed_with_errors");
        (void)snprintf(app->initial_error,
                       sizeof(app->initial_error),
                       "%d scan/enqueue errors",
                       context.errors);
    } else {
        (void)snprintf(app->initial_state,
                       sizeof(app->initial_state),
                       "completed");
    }
    syseba_mutex_unlock(&app->state_mutex);
    syseba_log_emit(app,
                    context.errors > 0 ? "WARNING" : "INFO",
                    "SYNC",
                    app->config.source,
                    app->config.backup,
                    context.errors > 0 ? app->initial_error : "",
                    "%s",
                    syseba_tr(app->language,
                              "Sincronizzazione iniziale completata",
                              "Initial synchronization completed"));
    return context.errors == 0 ? SYSEBA_OK : SYSEBA_ERR;
}

static void *syseba_initial_sync_main(void *opaque)
{
    (void)syseba_app_initial_sync((syseba_app_t *)opaque);
    return NULL;
}

int syseba_app_start(syseba_app_t *app,
                     char *error,
                     size_t error_size)
{
    if (syseba_prepare_runtime_paths(app, error, error_size) != 0) {
        return SYSEBA_ERR;
    }
    if (!app->web_only &&
        syseba_app_acquire_lock(app, error, error_size) != SYSEBA_OK) {
        return SYSEBA_ERR_BUSY;
    }
    if (syseba_database_initialize(app->db_path,
                                   error,
                                   error_size) != 0 ||
        syseba_log_start(app) != 0) {
        syseba_app_release_lock(app);
        if (error[0] == '\0') {
            (void)snprintf(error,
                           error_size,
                           "Unable to initialize logging");
        }
        return SYSEBA_ERR;
    }
    app->started_ns = syseba_monotonic_ns();
    syseba_iso_now(app->started_at, sizeof(app->started_at));
    app->running = !app->web_only;

    if (!app->web_only) {
        app->worker_count = app->config.threads;
        app->workers = (syseba_thread_t *)calloc(app->worker_count,
                                                 sizeof(*app->workers));
        if (app->workers == NULL) {
            (void)snprintf(error, error_size, "Out of memory");
            syseba_app_stop(app);
            return SYSEBA_ERR;
        }
        for (size_t index = 0; index < app->worker_count; index++) {
            if (syseba_thread_create(&app->workers[index],
                                     syseba_worker_main,
                                     app) != 0) {
                app->worker_count = index;
                (void)snprintf(error,
                               error_size,
                               "Unable to start worker %zu",
                               index + 1u);
                syseba_app_stop(app);
                return SYSEBA_ERR;
            }
        }
        if (syseba_watcher_start(app) != 0) {
            (void)snprintf(error, error_size, "Unable to start watcher");
            syseba_app_stop(app);
            return SYSEBA_ERR;
        }
        if (!app->no_initial_sync &&
            syseba_thread_create(&app->initial_thread,
                                 syseba_initial_sync_main,
                                 app) == 0) {
            app->initial_thread_started = true;
        } else if (!app->no_initial_sync) {
            (void)snprintf(error,
                           error_size,
                           "Unable to start initial synchronization");
            syseba_app_stop(app);
            return SYSEBA_ERR;
        }
    }

    if (app->web_enabled &&
        syseba_web_start(app, error, error_size) != SYSEBA_OK) {
        syseba_app_stop(app);
        return SYSEBA_ERR;
    }
    syseba_log_emit(app,
                    "INFO",
                    "START",
                    app->config.source,
                    app->config.backup,
                    "",
                    "%s %s (PID %" PRIu64 ")",
                    SYSEBA_APP_NAME,
                    syseba_tr(app->language, "avviato", "started"),
                    (uint64_t)syseba_process_id());
    return SYSEBA_OK;
}

void syseba_app_request_stop(syseba_app_t *app)
{
    syseba_app_set_stopping(app);
}

void syseba_app_stop(syseba_app_t *app)
{
    syseba_app_set_stopping(app);
    syseba_web_stop(app);
    syseba_watcher_stop(app);
    if (app->initial_thread_started) {
        (void)syseba_thread_join(app->initial_thread);
        app->initial_thread_started = false;
    }
    if (app->workers != NULL) {
        (void)syseba_queue_wait_empty(&app->event_queue, 30000);
        syseba_queue_close(&app->event_queue);
        for (size_t index = 0; index < app->worker_count; index++) {
            (void)syseba_thread_join(app->workers[index]);
        }
        free(app->workers);
        app->workers = NULL;
        app->worker_count = 0;
    }
    if (app->log_thread_started) {
        syseba_log_emit(app,
                        "INFO",
                        "STOP",
                        "",
                        "",
                        "",
                        "%s",
                        syseba_tr(app->language,
                                  "Arresto di SySeBa",
                                  "SySeBa stopping"));
        syseba_log_stop(app);
    }
    app->running = false;
    syseba_app_release_lock(app);
}

static void syseba_json_add_config(cJSON *parent,
                                   const char *name,
                                   const syseba_config_t *config)
{
    cJSON *object = cJSON_AddObjectToObject(parent, name);
    cJSON_AddStringToObject(object, "source", config->source);
    cJSON_AddStringToObject(object, "backup", config->backup);
    cJSON_AddStringToObject(object, "restore", config->restore);
    cJSON_AddStringToObject(object, "log_file", config->log_file);
    cJSON_AddNumberToObject(object, "threads", config->threads);
    cJSON_AddStringToObject(object, "config_path", config->config_path);
}

static void syseba_json_add_disk(cJSON *parent,
                                 const char *name,
                                 const char *path)
{
    syseba_disk_usage_t usage;
    cJSON *object = cJSON_AddObjectToObject(parent, name);
    int result = syseba_disk_usage(path, &usage);
    cJSON_AddStringToObject(object, "path", path);
    cJSON_AddBoolToObject(object, "exists", result == 0 && usage.exists);
    cJSON_AddNumberToObject(object,
                            "used_percent",
                            result == 0 ? usage.used_percent : 0.0);
    cJSON_AddNumberToObject(object,
                            "total",
                            result == 0 ? (double)usage.total : 0.0);
    cJSON_AddNumberToObject(object,
                            "used",
                            result == 0 ? (double)usage.used : 0.0);
    cJSON_AddNumberToObject(object,
                            "free",
                            result == 0 ? (double)usage.free_bytes : 0.0);
}

static void syseba_format_uptime(uint64_t seconds,
                                 char *output,
                                 size_t size)
{
    uint64_t days = seconds / 86400u;
    uint64_t hours = (seconds % 86400u) / 3600u;
    uint64_t minutes = (seconds % 3600u) / 60u;
    uint64_t remaining = seconds % 60u;
    if (days > 0) {
        (void)snprintf(output,
                       size,
                       "%" PRIu64 "d %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                       days,
                       hours,
                       minutes,
                       remaining);
    } else {
        (void)snprintf(output,
                       size,
                       "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                       hours,
                       minutes,
                       remaining);
    }
}

static bool syseba_json_add_config_state(syseba_app_t *app, cJSON *root)
{
    syseba_config_t saved;
    char error[512] = {0};
    cJSON *state = cJSON_AddObjectToObject(root, "config_state");
    cJSON *changes;
    bool restart_required = false;
    bool previous_restart_required;

    syseba_mutex_lock(&app->state_mutex);
    previous_restart_required = app->restart_required;
    syseba_mutex_unlock(&app->state_mutex);

    syseba_json_add_config(state, "active", &app->config);
    if (syseba_config_load(app->config.config_path,
                           &saved,
                           error,
                           sizeof(error)) != SYSEBA_OK) {
        cJSON_AddNullToObject(state, "saved");
        cJSON_AddObjectToObject(state, "changes");
        cJSON_AddBoolToObject(state,
                              "restart_required",
                              previous_restart_required);
        return previous_restart_required;
    }
    syseba_json_add_config(state, "saved", &saved);
    changes = cJSON_AddObjectToObject(state, "changes");
#define SYSEBA_CONFIG_CHANGE_STRING(key, field)                                  \
    do {                                                                          \
        if (strcmp(app->config.field, saved.field) != 0) {                        \
            cJSON *change = cJSON_AddObjectToObject(changes, (key));              \
            cJSON_AddStringToObject(change, "active", app->config.field);         \
            cJSON_AddStringToObject(change, "saved", saved.field);                \
            restart_required = true;                                               \
        }                                                                         \
    } while (0)
    SYSEBA_CONFIG_CHANGE_STRING("source", source);
    SYSEBA_CONFIG_CHANGE_STRING("backup", backup);
    SYSEBA_CONFIG_CHANGE_STRING("restore", restore);
    SYSEBA_CONFIG_CHANGE_STRING("log_file", log_file);
#undef SYSEBA_CONFIG_CHANGE_STRING
    if (app->config.threads != saved.threads) {
        cJSON *change = cJSON_AddObjectToObject(changes, "threads");
        cJSON_AddNumberToObject(change, "active", app->config.threads);
        cJSON_AddNumberToObject(change, "saved", saved.threads);
        restart_required = true;
    }
    syseba_mutex_lock(&app->state_mutex);
    app->restart_required = restart_required;
    syseba_mutex_unlock(&app->state_mutex);
    cJSON_AddBoolToObject(state, "restart_required", restart_required);
    return restart_required;
}

int syseba_app_status_json(syseba_app_t *app, char **json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *disk;
    cJSON *process;
    cJSON *stats;
    cJSON *initial;
    cJSON *logs;
    cJSON *service;
    syseba_stats_t stats_copy;
    double cpu = -1.0;
    double memory = -1.0;
    unsigned process_threads = 0;
    char uptime[64];
    char now[32];
    char started_at[32];
    char last_event_at[32];
    char initial_state[32];
    char initial_started_at[32];
    char initial_completed_at[32];
    char initial_error[SYSEBA_MESSAGE_MAX];
    bool running;
    bool web_only;
    bool restart_required;
    double initial_percent = -1.0;

    if (root == NULL) {
        return SYSEBA_ERR;
    }
    syseba_mutex_lock(&app->state_mutex);
    stats_copy = app->stats;
    running = app->running;
    web_only = app->web_only;
    (void)snprintf(started_at, sizeof(started_at), "%s", app->started_at);
    (void)snprintf(last_event_at,
                   sizeof(last_event_at),
                   "%s",
                   app->last_event_at);
    (void)snprintf(initial_state,
                   sizeof(initial_state),
                   "%s",
                   app->initial_state);
    (void)snprintf(initial_started_at,
                   sizeof(initial_started_at),
                   "%s",
                   app->initial_started_at);
    (void)snprintf(initial_completed_at,
                   sizeof(initial_completed_at),
                   "%s",
                   app->initial_completed_at);
    (void)snprintf(initial_error,
                   sizeof(initial_error),
                   "%s",
                   app->initial_error);
    syseba_mutex_unlock(&app->state_mutex);
    if (stats_copy.initial_total > 0) {
        initial_percent = ((double)stats_copy.initial_done * 100.0) /
                          (double)stats_copy.initial_total;
    } else if (strncmp(initial_state, "completed", 9) == 0) {
        initial_percent = 100.0;
    }
    syseba_format_uptime((syseba_monotonic_ns() - app->started_ns) /
                             1000000000ULL,
                         uptime,
                         sizeof(uptime));
    syseba_iso_now(now, sizeof(now));
    (void)syseba_process_metrics(&cpu, &memory, &process_threads);

    cJSON_AddStringToObject(root, "app", SYSEBA_APP_NAME);
    cJSON_AddStringToObject(root, "version", SYSEBA_VERSION);
    cJSON_AddBoolToObject(root, "running", running);
    cJSON_AddBoolToObject(root, "web_only", web_only);
    cJSON_AddNumberToObject(root, "pid", (double)syseba_process_id());
    cJSON_AddStringToObject(root, "lockfile", app->lock_path);
    cJSON_AddStringToObject(root, "db_path", app->db_path);
    cJSON_AddStringToObject(root, "uptime", uptime);
    cJSON_AddStringToObject(root, "started_at", started_at);
    cJSON_AddStringToObject(root, "now", now);
    if (last_event_at[0] == '\0') {
        cJSON_AddNullToObject(root, "last_event_at");
    } else {
        cJSON_AddStringToObject(root, "last_event_at", last_event_at);
    }
    syseba_json_add_config(root, "config", &app->config);
    restart_required = syseba_json_add_config_state(app, root);
    cJSON_AddBoolToObject(root, "restart_required", restart_required);

    disk = cJSON_AddObjectToObject(root, "disk");
    syseba_json_add_disk(disk, "source", app->config.source);
    syseba_json_add_disk(disk, "backup", app->config.backup);
    syseba_json_add_disk(disk, "restore", app->config.restore);

    process = cJSON_AddObjectToObject(root, "process");
    if (cpu < 0) {
        cJSON_AddNullToObject(process, "cpu_percent");
    } else {
        cJSON_AddNumberToObject(process, "cpu_percent", cpu);
    }
    if (memory < 0) {
        cJSON_AddNullToObject(process, "memory_mb");
    } else {
        cJSON_AddNumberToObject(process, "memory_mb", memory);
    }
    cJSON_AddNumberToObject(process,
                            "threads",
                            process_threads == 0
                                ? (double)(app->worker_count + 3u)
                                : process_threads);
    cJSON_AddNumberToObject(process,
                            "queue_size",
                            (double)syseba_queue_size(&app->event_queue));

    stats = cJSON_AddObjectToObject(root, "stats");
#define SYSEBA_ADD_STAT(field) \
    cJSON_AddNumberToObject(stats, #field, (double)stats_copy.field)
    SYSEBA_ADD_STAT(queued_events);
    SYSEBA_ADD_STAT(copied);
    SYSEBA_ADD_STAT(updated);
    SYSEBA_ADD_STAT(deleted);
    SYSEBA_ADD_STAT(restored);
    SYSEBA_ADD_STAT(skipped);
    SYSEBA_ADD_STAT(errors);
    SYSEBA_ADD_STAT(initial_total);
    SYSEBA_ADD_STAT(initial_done);
    SYSEBA_ADD_STAT(initial_copied);
    SYSEBA_ADD_STAT(initial_skipped);
    cJSON_AddBoolToObject(stats, "initial_running", stats_copy.initial_running);
#undef SYSEBA_ADD_STAT

    if (initial_percent < 0) {
        cJSON_AddNullToObject(root, "initial_sync_percent");
    } else {
        cJSON_AddNumberToObject(root,
                                "initial_sync_percent",
                                initial_percent);
    }
    initial = cJSON_AddObjectToObject(root, "initial_sync");
    cJSON_AddStringToObject(initial, "state", initial_state);
    if (initial_percent < 0) {
        cJSON_AddNullToObject(initial, "percent");
    } else {
        cJSON_AddNumberToObject(initial, "percent", initial_percent);
    }
    cJSON_AddNumberToObject(initial, "total", (double)stats_copy.initial_total);
    cJSON_AddNumberToObject(initial, "done", (double)stats_copy.initial_done);
    if (initial_started_at[0] == '\0') {
        cJSON_AddNullToObject(initial, "started_at");
    } else {
        cJSON_AddStringToObject(initial,
                               "started_at",
                               initial_started_at);
    }
    if (initial_completed_at[0] == '\0') {
        cJSON_AddNullToObject(initial, "completed_at");
    } else {
        cJSON_AddStringToObject(initial,
                               "completed_at",
                               initial_completed_at);
    }
    if (initial_error[0] == '\0') {
        cJSON_AddNullToObject(initial, "error");
    } else {
        cJSON_AddStringToObject(initial, "error", initial_error);
    }

    logs = cJSON_AddArrayToObject(root, "recent_logs");
    syseba_mutex_lock(&app->state_mutex);
    {
        size_t first = (app->recent_next + app->recent_capacity -
                        app->recent_count) %
                       app->recent_capacity;
        size_t start = app->recent_count > 20 ? app->recent_count - 20 : 0;
        for (size_t index = start; index < app->recent_count; index++) {
            size_t slot = (first + index) % app->recent_capacity;
            if (app->recent_logs[slot] != NULL) {
                cJSON_AddItemToArray(logs,
                                     cJSON_CreateString(app->recent_logs[slot]));
            }
        }
    }
    syseba_mutex_unlock(&app->state_mutex);

    {
        cJSON *lock = cJSON_AddObjectToObject(root, "external_lock");
        cJSON_AddBoolToObject(lock,
                              "exists",
                              syseba_fs_exists(app->lock_path));
        cJSON_AddNumberToObject(lock, "pid", (double)syseba_process_id());
        cJSON_AddBoolToObject(lock, "running", running);
    }
    service = cJSON_AddObjectToObject(root, "service");
    cJSON_AddBoolToObject(service,
                          "managed",
                          syseba_service_restart_available() != 0);
    cJSON_AddBoolToObject(service,
                          "restart_available",
                          syseba_service_restart_available() != 0);
    cJSON_AddStringToObject(service, "name", "syseba.service");
    cJSON_AddStringToObject(service,
                            "restart_command",
                            syseba_service_restart_command());

    *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *json == NULL ? SYSEBA_ERR : SYSEBA_OK;
}
