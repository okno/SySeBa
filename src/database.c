#include "syseba_internal.h"

#include "sqlite3.h"

typedef struct {
    sqlite3 *connection;
    sqlite3_stmt *insert;
} syseba_database_writer_t;

static int syseba_database_exec(sqlite3 *database,
                                const char *sql,
                                char *error,
                                size_t error_size)
{
    char *sqlite_error = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &sqlite_error);
    if (result != SQLITE_OK) {
        if (error != NULL && error_size > 0) {
            (void)snprintf(error,
                           error_size,
                           "%s",
                           sqlite_error == NULL
                               ? sqlite3_errmsg(database)
                               : sqlite_error);
        }
        sqlite3_free(sqlite_error);
        return -1;
    }
    return 0;
}

static bool syseba_database_has_column(sqlite3 *database, const char *name)
{
    sqlite3_stmt *statement = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(database,
                           "PRAGMA table_info(logs)",
                           -1,
                           &statement,
                           NULL) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *column = sqlite3_column_text(statement, 1);
        if (column != NULL && strcmp((const char *)column, name) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found;
}

static int syseba_database_migrate(sqlite3 *database,
                                   char *error,
                                   size_t error_size)
{
    static const struct {
        const char *name;
        const char *sql;
    } columns[] = {
        {"timestamp", "ALTER TABLE logs ADD COLUMN timestamp TEXT"},
        {"level", "ALTER TABLE logs ADD COLUMN level TEXT DEFAULT 'INFO'"},
        {"operation", "ALTER TABLE logs ADD COLUMN operation TEXT"},
        {"source_path", "ALTER TABLE logs ADD COLUMN source_path TEXT"},
        {"target_path", "ALTER TABLE logs ADD COLUMN target_path TEXT"},
        {"additional_info", "ALTER TABLE logs ADD COLUMN additional_info TEXT"},
    };

    if (syseba_database_exec(database, "BEGIN IMMEDIATE", error, error_size) != 0) {
        return -1;
    }
    if (syseba_database_exec(
            database,
            "CREATE TABLE IF NOT EXISTS logs ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "timestamp TEXT,"
            "level TEXT DEFAULT 'INFO',"
            "operation TEXT,"
            "source_path TEXT,"
            "target_path TEXT,"
            "additional_info TEXT"
            ")",
            error,
            error_size) != 0) {
        (void)sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    for (size_t index = 0; index < sizeof(columns) / sizeof(columns[0]); index++) {
        if (!syseba_database_has_column(database, columns[index].name) &&
            syseba_database_exec(database,
                                 columns[index].sql,
                                 error,
                                 error_size) != 0) {
            (void)sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
    }
    if (syseba_database_exec(
            database,
            "CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON logs(timestamp)",
            error,
            error_size) != 0 ||
        syseba_database_exec(database, "COMMIT", error, error_size) != 0) {
        (void)sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int syseba_database_open(const char *path,
                                sqlite3 **database,
                                char *error,
                                size_t error_size)
{
    char parent[SYSEBA_PATH_MAX];
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                SQLITE_OPEN_FULLMUTEX;
    int result;

    if (syseba_parent_dir(path, parent, sizeof(parent)) != 0 ||
        syseba_mkdirs(parent, 0750) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to create database directory: %s",
                       strerror(errno));
        return -1;
    }
    if (syseba_fs_exists(path) && !syseba_fs_is_regular(path)) {
        (void)snprintf(error,
                       error_size,
                       "Database path is not a regular file: %s",
                       path);
        errno = EINVAL;
        return -1;
    }
    result = sqlite3_open_v2(path, database, flags, NULL);
    if (result != SQLITE_OK) {
        (void)snprintf(error,
                       error_size,
                       "%s",
                       *database == NULL ? "Unable to open SQLite database"
                                         : sqlite3_errmsg(*database));
        if (*database != NULL) {
            sqlite3_close(*database);
            *database = NULL;
        }
        return -1;
    }
    sqlite3_extended_result_codes(*database, 1);
    sqlite3_busy_timeout(*database, 5000);
    if (syseba_database_exec(*database,
                             "PRAGMA journal_mode=WAL",
                             error,
                             error_size) != 0 ||
        syseba_database_exec(*database,
                             "PRAGMA synchronous=NORMAL",
                             error,
                             error_size) != 0 ||
        syseba_database_exec(*database,
                             "PRAGMA foreign_keys=ON",
                             error,
                             error_size) != 0 ||
        syseba_database_migrate(*database, error, error_size) != 0) {
        sqlite3_close(*database);
        *database = NULL;
        return -1;
    }
    return 0;
}

int syseba_database_initialize(const char *path,
                               char *error,
                               size_t error_size)
{
    sqlite3 *database = NULL;
    int result = syseba_database_open(path, &database, error, error_size);
    if (database != NULL) {
        sqlite3_close(database);
    }
    return result;
}

int syseba_database_open_writer(const char *path,
                                void **database,
                                char *error,
                                size_t error_size)
{
    static const char *insert_sql =
        "INSERT INTO logs "
        "(timestamp, level, operation, source_path, target_path, additional_info) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    syseba_database_writer_t *writer =
        (syseba_database_writer_t *)calloc(1, sizeof(*writer));
    if (writer == NULL) {
        (void)snprintf(error, error_size, "Out of memory");
        return -1;
    }
    if (syseba_database_open(path,
                             &writer->connection,
                             error,
                             error_size) != 0 ||
        sqlite3_prepare_v2(writer->connection,
                           insert_sql,
                           -1,
                           &writer->insert,
                           NULL) != SQLITE_OK) {
        if (writer->connection != NULL && writer->insert == NULL) {
            (void)snprintf(error,
                           error_size,
                           "%s",
                           sqlite3_errmsg(writer->connection));
        }
        syseba_database_close(writer);
        return -1;
    }
    *database = writer;
    return 0;
}

int syseba_database_write(void *database,
                          const syseba_log_record_t *record)
{
    syseba_database_writer_t *writer =
        (syseba_database_writer_t *)database;
    sqlite3_stmt *statement = writer->insert;
    int result;

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (sqlite3_bind_text(statement, 1, record->timestamp, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, record->level, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 3, record->operation, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 4, record->source_path, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 5, record->target_path, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 6, record->additional_info, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        return -1;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_SCHEMA) {
        char error[256];
        sqlite3_finalize(writer->insert);
        writer->insert = NULL;
        if (syseba_database_migrate(writer->connection,
                                    error,
                                    sizeof(error)) != 0 ||
            sqlite3_prepare_v2(
                writer->connection,
                "INSERT INTO logs "
                "(timestamp, level, operation, source_path, target_path, additional_info) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                -1,
                &writer->insert,
                NULL) != SQLITE_OK) {
            return -1;
        }
        return syseba_database_write(database, record);
    }
    return result == SQLITE_DONE ? 0 : -1;
}

void syseba_database_close(void *database)
{
    syseba_database_writer_t *writer =
        (syseba_database_writer_t *)database;
    if (writer == NULL) {
        return;
    }
    if (writer->insert != NULL) {
        sqlite3_finalize(writer->insert);
    }
    if (writer->connection != NULL) {
        sqlite3_close(writer->connection);
    }
    free(writer);
}
