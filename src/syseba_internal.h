#ifndef SYSEBA_INTERNAL_H
#define SYSEBA_INTERNAL_H

#include "syseba/syseba.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE syseba_thread_t;
typedef CRITICAL_SECTION syseba_mutex_t;
typedef CONDITION_VARIABLE syseba_cond_t;
typedef DWORD syseba_pid_t;
#define SYSEBA_PATH_SEP '\\'
#define SYSEBA_DEFAULT_CONFIG "C:\\ProgramData\\SySeBa\\syseba.conf"
#define SYSEBA_DEFAULT_LOCK "C:\\ProgramData\\SySeBa\\syseba.lock"
#define SYSEBA_DEFAULT_DB "C:\\ProgramData\\SySeBa\\syseba_logs.db"
#define SYSEBA_DEFAULT_TOKEN "C:\\ProgramData\\SySeBa\\syseba_web.token"
#define SYSEBA_DEFAULT_LOG "C:\\ProgramData\\SySeBa\\syseba.log"
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/types.h>
typedef pthread_t syseba_thread_t;
typedef pthread_mutex_t syseba_mutex_t;
typedef pthread_cond_t syseba_cond_t;
typedef pid_t syseba_pid_t;
#define SYSEBA_PATH_SEP '/'
#define SYSEBA_DEFAULT_CONFIG "/usr/local/etc/syseba/syseba.conf"
#define SYSEBA_LEGACY_CONFIG "/opt/syseba/syseba.conf"
#define SYSEBA_DEFAULT_LOCK "/usr/local/var/run/syseba/syseba.lock"
#define SYSEBA_LEGACY_LOCK "/opt/syseba/syseba.lock"
#define SYSEBA_DEFAULT_DB "/usr/local/var/lib/syseba/syseba_logs.db"
#define SYSEBA_LEGACY_DB "/opt/syseba/syseba_logs.db"
#define SYSEBA_DEFAULT_TOKEN "/usr/local/etc/syseba/syseba_web.token"
#define SYSEBA_LEGACY_TOKEN "/opt/syseba/syseba_web.token"
#define SYSEBA_DEFAULT_LOG "/usr/local/var/log/syseba/syseba.log"
#else
#include <pthread.h>
#include <sys/types.h>
typedef pthread_t syseba_thread_t;
typedef pthread_mutex_t syseba_mutex_t;
typedef pthread_cond_t syseba_cond_t;
typedef pid_t syseba_pid_t;
#define SYSEBA_PATH_SEP '/'
#define SYSEBA_DEFAULT_CONFIG "/etc/syseba/syseba.conf"
#define SYSEBA_LEGACY_CONFIG "/opt/syseba/syseba.conf"
#define SYSEBA_DEFAULT_LOCK "/run/syseba/syseba.lock"
#define SYSEBA_LEGACY_LOCK "/opt/syseba/syseba.lock"
#define SYSEBA_DEFAULT_DB "/var/lib/syseba/syseba_logs.db"
#define SYSEBA_LEGACY_DB "/opt/syseba/syseba_logs.db"
#define SYSEBA_DEFAULT_TOKEN "/etc/syseba/syseba_web.token"
#define SYSEBA_LEGACY_TOKEN "/opt/syseba/syseba_web.token"
#define SYSEBA_DEFAULT_LOG "/var/log/syseba/syseba.log"
#endif

#define SYSEBA_DEFAULT_WEB_HOST "127.0.0.1"
#define SYSEBA_SERVICE_WEB_HOST "0.0.0.0"
#define SYSEBA_DEFAULT_WEB_PORT 8765
#define SYSEBA_MAX_THREADS 64
#define SYSEBA_PATH_MAX 4096
#define SYSEBA_MESSAGE_MAX 2048
#define SYSEBA_TOKEN_MAX 256
#define SYSEBA_RECENT_LOGS 500
#define SYSEBA_MAX_JSON_BODY (64u * 1024u)
#define SYSEBA_DEFAULT_RESTORE_PAGE 100
#define SYSEBA_MAX_RESTORE_PAGE 250

typedef enum {
    SYSEBA_OK = 0,
    SYSEBA_ERR = -1,
    SYSEBA_ERR_INVALID = -2,
    SYSEBA_ERR_NOT_FOUND = -3,
    SYSEBA_ERR_EXISTS = -4,
    SYSEBA_ERR_PERMISSION = -5,
    SYSEBA_ERR_BUSY = -6,
    SYSEBA_ERR_CANCELLED = -7
} syseba_result_t;

typedef enum {
    SYSEBA_EVENT_CREATE,
    SYSEBA_EVENT_MODIFY,
    SYSEBA_EVENT_DELETE
} syseba_event_kind_t;

typedef enum {
    SYSEBA_RESTORE_FAIL,
    SYSEBA_RESTORE_RENAME,
    SYSEBA_RESTORE_OVERWRITE
} syseba_restore_strategy_t;

typedef enum {
    SYSEBA_LANGUAGE_IT,
    SYSEBA_LANGUAGE_EN
} syseba_language_t;

typedef struct {
    char source[SYSEBA_PATH_MAX];
    char backup[SYSEBA_PATH_MAX];
    char restore[SYSEBA_PATH_MAX];
    char log_file[SYSEBA_PATH_MAX];
    char config_path[SYSEBA_PATH_MAX];
    unsigned threads;
} syseba_config_t;

typedef struct {
    uint64_t queued_events;
    uint64_t copied;
    uint64_t updated;
    uint64_t deleted;
    uint64_t restored;
    uint64_t skipped;
    uint64_t errors;
    uint64_t initial_total;
    uint64_t initial_done;
    uint64_t initial_copied;
    uint64_t initial_skipped;
    bool initial_running;
} syseba_stats_t;

typedef struct {
    char path[SYSEBA_PATH_MAX];
    bool exists;
    uint64_t total;
    uint64_t used;
    uint64_t free_bytes;
    double used_percent;
} syseba_disk_usage_t;

typedef struct {
    syseba_event_kind_t kind;
    char path[SYSEBA_PATH_MAX];
    bool is_directory;
    bool initial_sync;
    uint64_t observed_ns;
} syseba_event_t;

typedef struct {
    char timestamp[32];
    char level[16];
    char operation[32];
    char message[SYSEBA_MESSAGE_MAX];
    char source_path[SYSEBA_PATH_MAX];
    char target_path[SYSEBA_PATH_MAX];
    char additional_info[SYSEBA_MESSAGE_MAX];
} syseba_log_record_t;

typedef struct syseba_queue_node {
    void *data;
    struct syseba_queue_node *next;
} syseba_queue_node_t;

typedef struct {
    syseba_queue_node_t *head;
    syseba_queue_node_t *tail;
    size_t size;
    size_t unfinished;
    bool closed;
    syseba_mutex_t mutex;
    syseba_cond_t available;
    syseba_cond_t completed;
} syseba_queue_t;

typedef struct syseba_snapshot_entry {
    char *path;
    uint64_t size;
    int64_t mtime_ns;
    bool directory;
    bool seen;
    struct syseba_snapshot_entry *next;
} syseba_snapshot_entry_t;

typedef struct {
    syseba_snapshot_entry_t **buckets;
    size_t bucket_count;
    size_t size;
} syseba_snapshot_t;

struct mg_context;

typedef struct syseba_app {
    syseba_config_t config;
    syseba_language_t language;
    char lock_path[SYSEBA_PATH_MAX];
    char db_path[SYSEBA_PATH_MAX];
    char web_host[256];
    int web_port;
    char web_token[SYSEBA_TOKEN_MAX];
    char web_token_source[64];
    bool web_auth_enabled;
    bool web_enabled;
    bool web_only;
    bool no_initial_sync;
    bool silent;
    bool running;
    bool restart_required;
    atomic_bool stop_requested;
    uint64_t started_ns;
    char started_at[32];
    char last_event_at[32];
    char initial_state[32];
    char initial_started_at[32];
    char initial_completed_at[32];
    char initial_error[SYSEBA_MESSAGE_MAX];
    syseba_stats_t stats;
    syseba_mutex_t state_mutex;
    syseba_queue_t event_queue;
    syseba_queue_t log_queue;
    syseba_thread_t *workers;
    size_t worker_count;
    syseba_thread_t log_thread;
    syseba_thread_t initial_thread;
    syseba_thread_t watcher_thread;
    bool log_thread_started;
    bool initial_thread_started;
    bool watcher_thread_started;
    FILE *lock_file;
    struct mg_context *web_context;
    char **recent_logs;
    size_t recent_capacity;
    size_t recent_count;
    size_t recent_next;
    void *watcher_state;
} syseba_app_t;

static inline bool syseba_app_is_stopping(const syseba_app_t *app)
{
    return atomic_load_explicit(&app->stop_requested, memory_order_acquire);
}

static inline void syseba_app_set_stopping(syseba_app_t *app)
{
    atomic_store_explicit(&app->stop_requested, true, memory_order_release);
}

typedef struct {
    const char *command;
    char config_path[SYSEBA_PATH_MAX];
    char lock_path[SYSEBA_PATH_MAX];
    char db_path[SYSEBA_PATH_MAX];
    char token_path[SYSEBA_PATH_MAX];
    char web_host[256];
    int web_port;
    syseba_language_t language;
    bool silent;
    bool web;
    bool web_only;
    bool no_web_auth;
    bool no_initial_sync;
    bool output_json;
    bool overwrite;
    bool rename;
    bool create_daemon;
    bool windows_service;
    double console_refresh;
    unsigned lines;
    char path[SYSEBA_PATH_MAX];
    char search[512];
    unsigned page;
    unsigned page_size;
    char sort[16];
    char direction[8];
    char explicit_token[SYSEBA_TOKEN_MAX];
} syseba_options_t;

typedef struct {
    char name[SYSEBA_PATH_MAX];
    char path[SYSEBA_PATH_MAX];
    char mtime[32];
    uint64_t size;
    bool is_directory;
    bool destination_exists;
} syseba_restore_item_t;

typedef struct {
    syseba_restore_item_t *items;
    size_t count;
    size_t total;
    unsigned page;
    unsigned pages;
    unsigned page_size;
    bool has_previous;
    bool has_next;
    bool is_file;
    char path[SYSEBA_PATH_MAX];
} syseba_restore_listing_t;

typedef void *(*syseba_thread_fn)(void *);

/* platform.c */
int syseba_mutex_init(syseba_mutex_t *mutex);
void syseba_mutex_destroy(syseba_mutex_t *mutex);
void syseba_mutex_lock(syseba_mutex_t *mutex);
void syseba_mutex_unlock(syseba_mutex_t *mutex);
int syseba_cond_init(syseba_cond_t *cond);
void syseba_cond_destroy(syseba_cond_t *cond);
void syseba_cond_signal(syseba_cond_t *cond);
void syseba_cond_broadcast(syseba_cond_t *cond);
int syseba_cond_wait(syseba_cond_t *cond, syseba_mutex_t *mutex);
int syseba_cond_timedwait(syseba_cond_t *cond, syseba_mutex_t *mutex, unsigned timeout_ms);
int syseba_thread_create(syseba_thread_t *thread, syseba_thread_fn fn, void *arg);
int syseba_thread_join(syseba_thread_t thread);
void syseba_sleep_ms(unsigned milliseconds);
uint64_t syseba_monotonic_ns(void);
void syseba_timestamp_now(char *buffer, size_t size);
void syseba_iso_now(char *buffer, size_t size);
void syseba_iso_from_time(time_t value, char *buffer, size_t size);
syseba_pid_t syseba_process_id(void);
bool syseba_process_exists(syseba_pid_t pid);
bool syseba_lock_is_held(const char *path);
int syseba_disk_usage(const char *path, syseba_disk_usage_t *usage);
int syseba_process_metrics(double *cpu_percent, double *memory_mb, unsigned *thread_count);
int syseba_random_bytes(unsigned char *buffer, size_t size);
int syseba_set_private_permissions(const char *path);
int syseba_path_real(const char *path, char *output, size_t size);
int syseba_mkdirs(const char *path, unsigned mode);
int syseba_parent_dir(const char *path, char *output, size_t size);
const char *syseba_last_error_string(char *buffer, size_t size);
char *syseba_strdup(const char *value);
bool syseba_is_terminal(FILE *stream);
unsigned syseba_terminal_width(void);
unsigned syseba_terminal_height(void);
void syseba_console_clear(void);

/* queue.c */
int syseba_queue_init(syseba_queue_t *queue);
void syseba_queue_close(syseba_queue_t *queue);
void syseba_queue_destroy(syseba_queue_t *queue, void (*free_fn)(void *));
int syseba_queue_push(syseba_queue_t *queue, void *data);
void *syseba_queue_pop(syseba_queue_t *queue, unsigned timeout_ms);
void syseba_queue_task_done(syseba_queue_t *queue);
bool syseba_queue_wait_empty(syseba_queue_t *queue, unsigned timeout_ms);
size_t syseba_queue_size(syseba_queue_t *queue);
bool syseba_queue_is_closed(syseba_queue_t *queue);

/* config.c */
int syseba_config_find(const char *requested, char *output, size_t size);
int syseba_config_load(const char *path, syseba_config_t *config, char *error, size_t error_size);
int syseba_config_normalize(syseba_config_t *config, char *error, size_t error_size);
int syseba_config_save(const syseba_config_t *config, char *error, size_t error_size);
int syseba_config_validate(const syseba_config_t *config, char *error, size_t error_size);
bool syseba_path_is_within(const char *base, const char *candidate);
int syseba_safe_join(const char *base, const char *relative, char *output, size_t size);
int syseba_relative_path(const char *base, const char *path, char *output, size_t size);

/* fs.c */
bool syseba_fs_exists(const char *path);
bool syseba_fs_is_directory(const char *path);
bool syseba_fs_is_regular(const char *path);
int syseba_open_regular_read(const char *path, FILE **file, uint64_t *size);
int syseba_open_regular_append(const char *path, FILE **file, unsigned mode);
int syseba_open_exclusive_write(const char *path, FILE **file, unsigned mode);
int syseba_atomic_replace(const char *temporary, const char *destination);
int syseba_fs_stat(const char *path, uint64_t *size, int64_t *mtime_ns, bool *directory);
int syseba_copy_file(const char *source, const char *destination, unsigned attempts);
int syseba_copy_tree(const char *source, const char *destination, bool overwrite);
int syseba_move_path(const char *source, const char *destination);
int syseba_remove_path(const char *path);
int syseba_unique_restore_path(const char *path, char *output, size_t size);
int syseba_unique_restored_path(const char *path, char *output, size_t size);
int syseba_walk_files(const char *root,
                      int (*callback)(const char *, bool, void *),
                      void *context);
int syseba_restore_list(syseba_app_t *app,
                        const char *relative,
                        const char *search,
                        unsigned page,
                        unsigned page_size,
                        const char *sort,
                        const char *direction,
                        syseba_restore_listing_t *listing,
                        char *error,
                        size_t error_size);
void syseba_restore_listing_free(syseba_restore_listing_t *listing);
int syseba_restore_item(syseba_app_t *app,
                        const char *relative,
                        syseba_restore_strategy_t strategy,
                        char *destination,
                        size_t destination_size,
                        char *error,
                        size_t error_size);

/* database.c */
int syseba_database_initialize(const char *path, char *error, size_t error_size);
int syseba_database_open_writer(const char *path, void **database, char *error, size_t error_size);
int syseba_database_write(void *database, const syseba_log_record_t *record);
void syseba_database_close(void *database);

/* log.c */
int syseba_log_start(syseba_app_t *app);
void syseba_log_stop(syseba_app_t *app);
void syseba_log_emit(syseba_app_t *app,
                     const char *level,
                     const char *operation,
                     const char *source,
                     const char *target,
                     const char *additional,
                     const char *format,
                     ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 7, 8)))
#endif
    ;
char **syseba_log_tail(const char *path, unsigned lines, size_t *count);
void syseba_log_tail_free(char **lines, size_t count);

/* watcher.c */
int syseba_watcher_start(syseba_app_t *app);
void syseba_watcher_stop(syseba_app_t *app);
void *syseba_watcher_native_main(void *context);
void *syseba_watcher_poll_main(void *context);
int syseba_snapshot_build(const char *root, syseba_snapshot_t *snapshot);
void syseba_snapshot_destroy(syseba_snapshot_t *snapshot);
void syseba_snapshot_reconcile(syseba_app_t *app,
                               syseba_snapshot_t *previous,
                               syseba_snapshot_t *current);

/* app.c */
int syseba_app_init(syseba_app_t *app,
                    const syseba_config_t *config,
                    const syseba_options_t *options,
                    char *error,
                    size_t error_size);
int syseba_app_start(syseba_app_t *app, char *error, size_t error_size);
void syseba_app_request_stop(syseba_app_t *app);
void syseba_app_stop(syseba_app_t *app);
int syseba_app_enqueue(syseba_app_t *app,
                       syseba_event_kind_t kind,
                       const char *path,
                       bool is_directory);
int syseba_app_enqueue_initial(syseba_app_t *app,
                               syseba_event_kind_t kind,
                               const char *path,
                               bool is_directory);
int syseba_app_process_event(syseba_app_t *app, const syseba_event_t *event);
int syseba_app_initial_sync(syseba_app_t *app);
int syseba_app_acquire_lock(syseba_app_t *app, char *error, size_t error_size);
void syseba_app_release_lock(syseba_app_t *app);
int syseba_app_status_json(syseba_app_t *app, char **json);

/* web.c */
int syseba_web_load_token(syseba_app_t *app,
                          const syseba_options_t *options,
                          char *error,
                          size_t error_size);
int syseba_web_start(syseba_app_t *app, char *error, size_t error_size);
void syseba_web_stop(syseba_app_t *app);

/* dashboard.c */
int syseba_dashboard_run(syseba_app_t *app, double refresh_seconds);

/* cli.c */
void syseba_options_defaults(syseba_options_t *options);
int syseba_cli_parse(int argc,
                     char **argv,
                     syseba_options_t *options,
                     char *error,
                     size_t error_size);
void syseba_cli_usage(FILE *stream, syseba_language_t language);
int syseba_cli_execute(const syseba_options_t *options);
void syseba_cli_request_stop(void);
bool syseba_cli_stop_pending(void);

/* service_<platform>.c */
int syseba_service_install(const syseba_options_t *options, char *error, size_t error_size);
int syseba_service_restart_available(void);
int syseba_service_request_restart(char *error, size_t error_size);
const char *syseba_service_restart_command(void);
#ifdef _WIN32
int syseba_windows_service_dispatch(const syseba_options_t *options,
                                    char *error,
                                    size_t error_size);
#endif

static inline const char *syseba_tr(syseba_language_t language,
                                    const char *italian,
                                    const char *english)
{
    return language == SYSEBA_LANGUAGE_IT ? italian : english;
}

#endif
