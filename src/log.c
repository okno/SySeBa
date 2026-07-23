#include "syseba_internal.h"

#include <stdarg.h>

static char *syseba_string_duplicate(const char *value)
{
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, value, length + 1u);
    }
    return copy;
}

static void syseba_recent_add(syseba_app_t *app, const char *line)
{
    char *copy = syseba_string_duplicate(line);
    if (copy == NULL) {
        return;
    }
    syseba_mutex_lock(&app->state_mutex);
    if (app->recent_capacity == 0) {
        syseba_mutex_unlock(&app->state_mutex);
        free(copy);
        return;
    }
    free(app->recent_logs[app->recent_next]);
    app->recent_logs[app->recent_next] = copy;
    app->recent_next = (app->recent_next + 1u) % app->recent_capacity;
    if (app->recent_count < app->recent_capacity) {
        app->recent_count++;
    }
    syseba_iso_now(app->last_event_at, sizeof(app->last_event_at));
    syseba_mutex_unlock(&app->state_mutex);
}

static void *syseba_log_writer_main(void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    FILE *file = NULL;
    void *database = NULL;
    char error[512] = {0};
    bool database_error_reported = false;

    if (syseba_open_regular_append(app->config.log_file,
                                   &file,
                                   0640) != 0) {
        fprintf(stderr,
                "SySeBa: unable to open log file %s: %s\n",
                app->config.log_file,
                strerror(errno));
    }
    if (syseba_database_open_writer(app->db_path,
                                    &database,
                                    error,
                                    sizeof(error)) != 0) {
        fprintf(stderr,
                "SySeBa: SQLite logging unavailable; file logging continues: %s\n",
                error);
        database_error_reported = true;
    }

    for (;;) {
        syseba_log_record_t *record =
            (syseba_log_record_t *)syseba_queue_pop(&app->log_queue, 500);
        char line[SYSEBA_MESSAGE_MAX + 64];
        if (record == NULL) {
            if (syseba_queue_is_closed(&app->log_queue)) {
                break;
            }
            continue;
        }
        (void)snprintf(line,
                       sizeof(line),
                       "%s [%s] %s",
                       record->timestamp,
                       record->level,
                       record->message);
        if (file != NULL) {
            if (fprintf(file, "%s\n", line) < 0 || fflush(file) != 0) {
                fprintf(stderr,
                        "SySeBa: unable to write log file %s: %s\n",
                        app->config.log_file,
                        strerror(errno));
                fclose(file);
                file = NULL;
            }
        }
        if (database != NULL &&
            syseba_database_write(database, record) != 0 &&
            !database_error_reported) {
            fprintf(stderr,
                    "SySeBa: unable to write an event to SQLite; "
                    "file logging continues\n");
            database_error_reported = true;
        }
        free(record);
        syseba_queue_task_done(&app->log_queue);
    }
    if (file != NULL) {
        fclose(file);
    }
    syseba_database_close(database);
    return NULL;
}

int syseba_log_start(syseba_app_t *app)
{
    char parent[SYSEBA_PATH_MAX];
    if (syseba_parent_dir(app->config.log_file,
                          parent,
                          sizeof(parent)) != 0 ||
        syseba_mkdirs(parent, 0750) != 0) {
        return -1;
    }
    if (syseba_thread_create(&app->log_thread,
                             syseba_log_writer_main,
                             app) != 0) {
        return -1;
    }
    app->log_thread_started = true;
    return 0;
}

void syseba_log_stop(syseba_app_t *app)
{
    if (!app->log_thread_started) {
        return;
    }
    (void)syseba_queue_wait_empty(&app->log_queue, 10000);
    syseba_queue_close(&app->log_queue);
    (void)syseba_thread_join(app->log_thread);
    app->log_thread_started = false;
}

void syseba_log_emit(syseba_app_t *app,
                     const char *level,
                     const char *operation,
                     const char *source,
                     const char *target,
                     const char *additional,
                     const char *format,
                     ...)
{
    syseba_log_record_t *record =
        (syseba_log_record_t *)calloc(1, sizeof(*record));
    va_list arguments;
    char line[SYSEBA_MESSAGE_MAX + 64];

    if (record == NULL) {
        return;
    }
    syseba_timestamp_now(record->timestamp, sizeof(record->timestamp));
    (void)snprintf(record->level,
                   sizeof(record->level),
                   "%s",
                   level == NULL ? "INFO" : level);
    (void)snprintf(record->operation,
                   sizeof(record->operation),
                   "%s",
                   operation == NULL ? "INFO" : operation);
    (void)snprintf(record->source_path,
                   sizeof(record->source_path),
                   "%s",
                   source == NULL ? "" : source);
    (void)snprintf(record->target_path,
                   sizeof(record->target_path),
                   "%s",
                   target == NULL ? "" : target);
    (void)snprintf(record->additional_info,
                   sizeof(record->additional_info),
                   "%s",
                   additional == NULL ? "" : additional);
    va_start(arguments, format);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    (void)vsnprintf(record->message,
                    sizeof(record->message),
                    format,
                    arguments);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(arguments);

    (void)snprintf(line,
                   sizeof(line),
                   "%s [%s] %s",
                   record->timestamp,
                   record->level,
                   record->message);
    syseba_recent_add(app, line);
    if (strcmp(record->level, "ERROR") == 0) {
        syseba_mutex_lock(&app->state_mutex);
        app->stats.errors++;
        syseba_mutex_unlock(&app->state_mutex);
    }
    if (syseba_queue_push(&app->log_queue, record) != 0) {
        fprintf(stderr, "%s\n", line);
        free(record);
    }
}

static int syseba_line_array_push(char ***lines,
                                  size_t *count,
                                  size_t *capacity,
                                  const char *start,
                                  size_t length)
{
    char *line;
    if (*count == *capacity) {
        size_t next_capacity = *capacity == 0 ? 64 : *capacity * 2;
        char **next = (char **)realloc(*lines,
                                      next_capacity * sizeof(**lines));
        if (next == NULL) {
            return -1;
        }
        *lines = next;
        *capacity = next_capacity;
    }
    while (length > 0 && (start[length - 1] == '\r' || start[length - 1] == '\n')) {
        length--;
    }
    line = (char *)malloc(length + 1u);
    if (line == NULL) {
        return -1;
    }
    memcpy(line, start, length);
    line[length] = '\0';
    (*lines)[(*count)++] = line;
    return 0;
}

char **syseba_log_tail(const char *path, unsigned lines, size_t *count)
{
    FILE *file;
    long end;
    long position;
    size_t capacity = 0;
    char **result = NULL;
    char *data = NULL;
    size_t data_size;
    unsigned newline_count = 0;

    *count = 0;
    if (syseba_open_regular_read(path, &file, NULL) != 0 ||
        fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    end = ftell(file);
    if (end < 0) {
        fclose(file);
        return NULL;
    }
    position = end;
    while (position > 0 && newline_count <= lines) {
        long chunk = position > 8192 ? 8192 : position;
        char buffer[8192];
        position -= chunk;
        if (fseek(file, position, SEEK_SET) != 0 ||
            fread(buffer, 1, (size_t)chunk, file) != (size_t)chunk) {
            fclose(file);
            return NULL;
        }
        for (long index = chunk - 1; index >= 0; index--) {
            if (buffer[index] == '\n') {
                newline_count++;
                if (newline_count > lines) {
                    position += index + 1;
                    goto found_start;
                }
            }
        }
    }

found_start:
    data_size = (size_t)(end - position);
    data = (char *)malloc(data_size + 1u);
    if (data == NULL ||
        fseek(file, position, SEEK_SET) != 0 ||
        fread(data, 1, data_size, file) != data_size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    data[data_size] = '\0';

    {
        char *cursor = data;
        char *limit = data + data_size;
        while (cursor < limit) {
            char *newline = memchr(cursor, '\n', (size_t)(limit - cursor));
            char *line_end = newline == NULL ? limit : newline;
            if (line_end > cursor &&
                syseba_line_array_push(&result,
                                       count,
                                       &capacity,
                                       cursor,
                                       (size_t)(line_end - cursor)) != 0) {
                syseba_log_tail_free(result, *count);
                free(data);
                *count = 0;
                return NULL;
            }
            cursor = newline == NULL ? limit : newline + 1;
        }
    }
    free(data);
    return result;
}

void syseba_log_tail_free(char **lines, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        free(lines[index]);
    }
    free(lines);
}
