#include "syseba_internal.h"

#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#define syseba_access _access
#define SYSEBA_ACCESS_EXISTS 0
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define syseba_access access
#define SYSEBA_ACCESS_EXISTS F_OK
#endif

static char *syseba_trim(char *value)
{
    char *end;
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return value;
}

static int syseba_config_candidate(const char *path, char *output, size_t size)
{
    if (path == NULL || *path == '\0' ||
        syseba_access(path, SYSEBA_ACCESS_EXISTS) != 0) {
        return -1;
    }
    return syseba_path_real(path, output, size);
}

int syseba_config_find(const char *requested, char *output, size_t size)
{
    const char *environment;

    if (requested != NULL && *requested != '\0') {
        return syseba_config_candidate(requested, output, size);
    }
    environment = getenv("SYSEBA_CONFIG");
    if (syseba_config_candidate(environment, output, size) == 0) {
        return 0;
    }
    if (syseba_config_candidate(SYSEBA_DEFAULT_CONFIG, output, size) == 0) {
        return 0;
    }
#ifndef _WIN32
    if (syseba_config_candidate(SYSEBA_LEGACY_CONFIG, output, size) == 0) {
        return 0;
    }
#endif
    if (syseba_config_candidate("syseba.conf", output, size) == 0) {
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static int syseba_resolve_config_path(const char *base,
                                      const char *value,
                                      char *output,
                                      size_t size)
{
    char combined[SYSEBA_PATH_MAX];
    bool absolute;
#ifdef _WIN32
    absolute = (strlen(value) >= 3 && isalpha((unsigned char)value[0]) &&
                value[1] == ':' && (value[2] == '\\' || value[2] == '/')) ||
               (value[0] == '\\' && value[1] == '\\');
#else
    absolute = value[0] == '/';
#endif
    if (absolute) {
        return syseba_path_real(value, output, size);
    }
    if (snprintf(combined,
                 sizeof(combined),
                 "%s%c%s",
                 base,
                 SYSEBA_PATH_SEP,
                 value) >= (int)sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return syseba_path_real(combined, output, size);
}

int syseba_config_load(const char *path,
                       syseba_config_t *config,
                       char *error,
                       size_t error_size)
{
    FILE *file;
    char line[SYSEBA_PATH_MAX + 128];
    char base[SYSEBA_PATH_MAX];
    char source[SYSEBA_PATH_MAX] = "/dati";
    char backup[SYSEBA_PATH_MAX] = "/backup";
    char restore[SYSEBA_PATH_MAX] = "/restore";
    char log_file[SYSEBA_PATH_MAX] = SYSEBA_DEFAULT_LOG;
    unsigned threads = 4;
    unsigned line_number = 0;
    bool settings = false;
    bool found_settings = false;

    memset(config, 0, sizeof(*config));
    if (syseba_path_real(path, config->config_path, sizeof(config->config_path)) != 0 ||
        syseba_parent_dir(config->config_path, base, sizeof(base)) != 0) {
        (void)snprintf(error, error_size, "Invalid configuration path: %s", path);
        return SYSEBA_ERR_INVALID;
    }

    if (syseba_open_regular_read(config->config_path,
                                 &file,
                                 NULL) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to open configuration %s: %s",
                       config->config_path,
                       strerror(errno));
        return SYSEBA_ERR_NOT_FOUND;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed;
        char *separator;
        char *key;
        char *value;
        line_number++;
        if (strchr(line, '\n') == NULL &&
            strlen(line) == sizeof(line) - 1u) {
            int next = fgetc(file);
            if (next != EOF) {
                fclose(file);
                (void)snprintf(error,
                               error_size,
                               "Configuration line %u is too long",
                               line_number);
                return SYSEBA_ERR_INVALID;
            }
        }
        if (line_number == 1 && (unsigned char)line[0] == 0xef &&
            (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf) {
            memmove(line, line + 3, strlen(line + 3) + 1);
        }
        trimmed = syseba_trim(line);
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }
        if (*trimmed == '[') {
            char *end = strchr(trimmed, ']');
            if (end == NULL) {
                fclose(file);
                (void)snprintf(error,
                               error_size,
                               "Malformed section at line %u",
                               line_number);
                return SYSEBA_ERR_INVALID;
            }
            *end = '\0';
#ifdef _WIN32
            settings = _stricmp(trimmed + 1, "SETTINGS") == 0;
#else
            settings = strcasecmp(trimmed + 1, "SETTINGS") == 0;
#endif
            found_settings = found_settings || settings;
            continue;
        }
        if (!settings) {
            continue;
        }
        separator = strchr(trimmed, '=');
        if (separator == NULL) {
            fclose(file);
            (void)snprintf(error,
                           error_size,
                           "Malformed setting at line %u",
                           line_number);
            return SYSEBA_ERR_INVALID;
        }
        *separator = '\0';
        key = syseba_trim(trimmed);
        value = syseba_trim(separator + 1);

#define SYSEBA_CONFIG_COPY(target)                                               \
    do {                                                                          \
        if (snprintf((target), sizeof(target), "%s", value) >= (int)sizeof(target)) { \
            fclose(file);                                                         \
            (void)snprintf(error, error_size, "Value too long at line %u", line_number); \
            return SYSEBA_ERR_INVALID;                                             \
        }                                                                         \
    } while (0)

#ifdef _WIN32
#define SYSEBA_KEY_EQUALS(name) (_stricmp(key, (name)) == 0)
#else
#define SYSEBA_KEY_EQUALS(name) (strcasecmp(key, (name)) == 0)
#endif
        if (SYSEBA_KEY_EQUALS("source")) {
            SYSEBA_CONFIG_COPY(source);
        } else if (SYSEBA_KEY_EQUALS("backup")) {
            SYSEBA_CONFIG_COPY(backup);
        } else if (SYSEBA_KEY_EQUALS("restore")) {
            SYSEBA_CONFIG_COPY(restore);
        } else if (SYSEBA_KEY_EQUALS("log")) {
            SYSEBA_CONFIG_COPY(log_file);
        } else if (SYSEBA_KEY_EQUALS("threads")) {
            char *end = NULL;
            unsigned long parsed;
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno != 0 || end == value || *syseba_trim(end) != '\0' ||
                parsed < 1 || parsed > SYSEBA_MAX_THREADS) {
                fclose(file);
                (void)snprintf(error,
                               error_size,
                               "threads must be an integer between 1 and %d",
                               SYSEBA_MAX_THREADS);
                return SYSEBA_ERR_INVALID;
            }
            threads = (unsigned)parsed;
        }
#undef SYSEBA_KEY_EQUALS
#undef SYSEBA_CONFIG_COPY
    }
    if (ferror(file)) {
        fclose(file);
        (void)snprintf(error, error_size, "Unable to read configuration");
        return SYSEBA_ERR;
    }
    fclose(file);

    if (!found_settings) {
        (void)snprintf(error, error_size, "Missing [SETTINGS] section");
        return SYSEBA_ERR_INVALID;
    }
    if (source[0] == '\0' || backup[0] == '\0' || restore[0] == '\0' ||
        log_file[0] == '\0') {
        (void)snprintf(error,
                       error_size,
                       "source, backup, restore and log cannot be empty");
        return SYSEBA_ERR_INVALID;
    }
    if (syseba_resolve_config_path(base, source, config->source, sizeof(config->source)) != 0 ||
        syseba_resolve_config_path(base, backup, config->backup, sizeof(config->backup)) != 0 ||
        syseba_resolve_config_path(base, restore, config->restore, sizeof(config->restore)) != 0 ||
        syseba_resolve_config_path(base, log_file, config->log_file, sizeof(config->log_file)) != 0) {
        (void)snprintf(error, error_size, "A configured path is invalid or too long");
        return SYSEBA_ERR_INVALID;
    }
    config->threads = threads;
    return syseba_config_validate(config, error, error_size);
}

static bool syseba_path_equal(const char *first, const char *second)
{
#ifdef _WIN32
    return _stricmp(first, second) == 0;
#else
    return strcmp(first, second) == 0;
#endif
}

static int syseba_path_resolve_links(const char *path,
                                     char *output,
                                     size_t output_size)
{
    char normalized[SYSEBA_PATH_MAX];
    char probe[SYSEBA_PATH_MAX];
    char resolved[SYSEBA_PATH_MAX];
    const char *suffix;

    if (syseba_path_real(path, normalized, sizeof(normalized)) != 0 ||
        snprintf(probe, sizeof(probe), "%s", normalized) >= (int)sizeof(probe)) {
        return -1;
    }
    while (!syseba_fs_exists(probe)) {
        char parent[SYSEBA_PATH_MAX];
        if (syseba_parent_dir(probe, parent, sizeof(parent)) != 0 ||
            syseba_path_equal(parent, probe)) {
            return -1;
        }
        (void)snprintf(probe, sizeof(probe), "%s", parent);
    }
#ifdef _WIN32
    {
        HANDLE handle = CreateFileA(probe,
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    NULL);
        DWORD count;
        const char *value;
        if (handle == INVALID_HANDLE_VALUE) {
            return -1;
        }
        count = GetFinalPathNameByHandleA(handle,
                                          resolved,
                                          (DWORD)sizeof(resolved),
                                          FILE_NAME_NORMALIZED |
                                              VOLUME_NAME_DOS);
        CloseHandle(handle);
        if (count == 0 || count >= sizeof(resolved)) {
            return -1;
        }
        value = resolved;
        if (strncmp(value, "\\\\?\\UNC\\", 8) == 0) {
            char unc[SYSEBA_PATH_MAX];
            if (snprintf(unc,
                         sizeof(unc),
                         "\\\\%s",
                         value + 8) >= (int)sizeof(unc)) {
                return -1;
            }
            (void)snprintf(resolved, sizeof(resolved), "%s", unc);
        } else if (strncmp(value, "\\\\?\\", 4) == 0) {
            memmove(resolved, resolved + 4, strlen(resolved + 4) + 1u);
        }
    }
#else
    if (realpath(probe, resolved) == NULL) {
        return -1;
    }
#endif
    suffix = normalized + strlen(probe);
    if (snprintf(output,
                 output_size,
                 "%s%s",
                 resolved,
                 suffix) >= (int)output_size) {
        return -1;
    }
    return syseba_path_real(output, output, output_size);
}

bool syseba_path_is_within(const char *base, const char *candidate)
{
    char normalized_base[SYSEBA_PATH_MAX];
    char normalized_candidate[SYSEBA_PATH_MAX];
    size_t base_length;

    if (syseba_path_resolve_links(base,
                                  normalized_base,
                                  sizeof(normalized_base)) != 0 ||
        syseba_path_resolve_links(candidate,
                                  normalized_candidate,
                                  sizeof(normalized_candidate)) != 0) {
        return false;
    }
    base_length = strlen(normalized_base);
#ifdef _WIN32
    if (_strnicmp(normalized_base, normalized_candidate, base_length) != 0) {
        return false;
    }
#else
    if (strncmp(normalized_base, normalized_candidate, base_length) != 0) {
        return false;
    }
#endif
    return (base_length > 0 &&
            normalized_base[base_length - 1] == SYSEBA_PATH_SEP) ||
           normalized_candidate[base_length] == '\0' ||
           normalized_candidate[base_length] == SYSEBA_PATH_SEP;
}

int syseba_safe_join(const char *base,
                     const char *relative,
                     char *output,
                     size_t size)
{
    char combined[SYSEBA_PATH_MAX];
    const char *cursor = relative == NULL ? "" : relative;

    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
#ifdef _WIN32
    if (strchr(cursor, ':') != NULL) {
        return SYSEBA_ERR_INVALID;
    }
#endif
    if (snprintf(combined,
                 sizeof(combined),
                 "%s%c%s",
                 base,
                 SYSEBA_PATH_SEP,
                 cursor) >= (int)sizeof(combined) ||
        syseba_path_real(combined, output, size) != 0 ||
        !syseba_path_is_within(base, output)) {
        return SYSEBA_ERR_INVALID;
    }
    return SYSEBA_OK;
}

int syseba_relative_path(const char *base,
                         const char *path,
                         char *output,
                         size_t size)
{
    char normalized_base[SYSEBA_PATH_MAX];
    char normalized_path[SYSEBA_PATH_MAX];
    const char *relative;
    size_t base_length;

    if (syseba_path_real(base, normalized_base, sizeof(normalized_base)) != 0 ||
        syseba_path_real(path, normalized_path, sizeof(normalized_path)) != 0 ||
        !syseba_path_is_within(normalized_base, normalized_path)) {
        return SYSEBA_ERR_INVALID;
    }
    base_length = strlen(normalized_base);
    relative = normalized_path + base_length;
    while (*relative == '/' || *relative == '\\') {
        relative++;
    }
    if (snprintf(output, size, "%s", relative) >= (int)size) {
        return SYSEBA_ERR_INVALID;
    }
    for (char *cursor = output; *cursor != '\0'; cursor++) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    return SYSEBA_OK;
}

int syseba_config_validate(const syseba_config_t *config,
                           char *error,
                           size_t error_size)
{
    const char *values[] = {
        config->source,
        config->backup,
        config->restore,
        config->log_file,
        config->config_path,
    };
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        if (strpbrk(values[index], "\r\n") != NULL) {
            (void)snprintf(error,
                           error_size,
                           "Configuration paths cannot contain line breaks");
            return SYSEBA_ERR_INVALID;
        }
    }
    if (!syseba_fs_is_directory(config->source)) {
        (void)snprintf(error,
                       error_size,
                       "source does not exist or is not a directory: %s",
                       config->source);
        return SYSEBA_ERR_NOT_FOUND;
    }
    if (syseba_path_equal(config->source, config->backup) ||
        syseba_path_equal(config->source, config->restore) ||
        syseba_path_equal(config->backup, config->restore)) {
        (void)snprintf(error,
                       error_size,
                       "source, backup and restore must be different directories");
        return SYSEBA_ERR_INVALID;
    }
    if (syseba_path_is_within(config->source, config->backup) ||
        syseba_path_is_within(config->backup, config->source) ||
        syseba_path_is_within(config->source, config->restore) ||
        syseba_path_is_within(config->restore, config->source) ||
        syseba_path_is_within(config->backup, config->restore) ||
        syseba_path_is_within(config->restore, config->backup)) {
        (void)snprintf(error,
                       error_size,
                       "source, backup and restore cannot contain one another");
        return SYSEBA_ERR_INVALID;
    }
    if (syseba_path_is_within(config->source, config->log_file)) {
        (void)snprintf(error, error_size, "log file cannot be inside source");
        return SYSEBA_ERR_INVALID;
    }
    if (config->threads < 1 || config->threads > SYSEBA_MAX_THREADS) {
        (void)snprintf(error,
                       error_size,
                       "threads must be between 1 and %d",
                       SYSEBA_MAX_THREADS);
        return SYSEBA_ERR_INVALID;
    }
    return SYSEBA_OK;
}

int syseba_config_normalize(syseba_config_t *config,
                            char *error,
                            size_t error_size)
{
    char config_path[SYSEBA_PATH_MAX];
    char base[SYSEBA_PATH_MAX];
    char source[SYSEBA_PATH_MAX];
    char backup[SYSEBA_PATH_MAX];
    char restore[SYSEBA_PATH_MAX];
    char log_file[SYSEBA_PATH_MAX];

    if (config == NULL || config->config_path[0] == '\0' ||
        strpbrk(config->config_path, "\r\n") != NULL ||
        strpbrk(config->source, "\r\n") != NULL ||
        strpbrk(config->backup, "\r\n") != NULL ||
        strpbrk(config->restore, "\r\n") != NULL ||
        strpbrk(config->log_file, "\r\n") != NULL ||
        syseba_path_real(config->config_path,
                         config_path,
                         sizeof(config_path)) != 0 ||
        syseba_parent_dir(config_path, base, sizeof(base)) != 0 ||
        syseba_resolve_config_path(base,
                                   config->source,
                                   source,
                                   sizeof(source)) != 0 ||
        syseba_resolve_config_path(base,
                                   config->backup,
                                   backup,
                                   sizeof(backup)) != 0 ||
        syseba_resolve_config_path(base,
                                   config->restore,
                                   restore,
                                   sizeof(restore)) != 0 ||
        syseba_resolve_config_path(base,
                                   config->log_file,
                                   log_file,
                                   sizeof(log_file)) != 0) {
        (void)snprintf(error,
                       error_size,
                       "A configuration path is invalid or too long");
        return SYSEBA_ERR_INVALID;
    }

    (void)snprintf(config->config_path,
                   sizeof(config->config_path),
                   "%s",
                   config_path);
    (void)snprintf(config->source, sizeof(config->source), "%s", source);
    (void)snprintf(config->backup, sizeof(config->backup), "%s", backup);
    (void)snprintf(config->restore, sizeof(config->restore), "%s", restore);
    (void)snprintf(config->log_file,
                   sizeof(config->log_file),
                   "%s",
                   log_file);
    return syseba_config_validate(config, error, error_size);
}

int syseba_config_save(const syseba_config_t *config,
                       char *error,
                       size_t error_size)
{
    syseba_config_t normalized = *config;
    char temporary[SYSEBA_PATH_MAX];
    char parent[SYSEBA_PATH_MAX];
    FILE *file;
    int result;

    result = syseba_config_normalize(&normalized, error, error_size);
    if (result != SYSEBA_OK) {
        return result;
    }
    if (syseba_parent_dir(normalized.config_path,
                          parent,
                          sizeof(parent)) != 0 ||
        syseba_mkdirs(parent, 0750) != 0 ||
        snprintf(temporary,
                 sizeof(temporary),
                 "%s.tmp.%" PRIu64,
                 normalized.config_path,
                 syseba_monotonic_ns()) >= (int)sizeof(temporary)) {
        (void)snprintf(error, error_size, "Unable to prepare configuration file");
        return SYSEBA_ERR;
    }
    if (syseba_open_exclusive_write(temporary,
                                    &file,
                                    0640) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to write configuration: %s",
                       strerror(errno));
        return SYSEBA_ERR_PERMISSION;
    }
    {
        int write_error =
            fprintf(file,
                    "# SySeBa configuration\n"
                    "[SETTINGS]\n"
                    "source = %s\n"
                    "backup = %s\n"
                    "restore = %s\n"
                    "log = %s\n"
                    "threads = %u\n",
                    normalized.source,
                    normalized.backup,
                    normalized.restore,
                    normalized.log_file,
                    normalized.threads) < 0 ||
            fflush(file) != 0
#ifdef _WIN32
            || _commit(_fileno(file)) != 0
#else
            || fsync(fileno(file)) != 0
#endif
            ;
        if (fclose(file) != 0) {
            write_error = 1;
        }
        if (write_error) {
            (void)remove(temporary);
            (void)snprintf(error, error_size, "Unable to flush configuration");
            return SYSEBA_ERR;
        }
    }
#ifndef _WIN32
    (void)chmod(temporary, 0640);
#endif
    if (syseba_atomic_replace(temporary,
                              normalized.config_path) != 0) {
        (void)remove(temporary);
        (void)snprintf(error,
                       error_size,
                       "Unable to replace configuration: %s",
                       strerror(errno));
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}
