#include "syseba_internal.h"

#include "cJSON.h"

#include <signal.h>

#ifdef _WIN32
static atomic_bool syseba_stop_pending = ATOMIC_VAR_INIT(false);
#else
static volatile sig_atomic_t syseba_stop_pending = 0;
#endif

void syseba_cli_request_stop(void)
{
#ifdef _WIN32
    atomic_store_explicit(&syseba_stop_pending,
                          true,
                          memory_order_release);
#else
    syseba_stop_pending = 1;
#endif
}

bool syseba_cli_stop_pending(void)
{
#ifdef _WIN32
    return atomic_load_explicit(&syseba_stop_pending,
                                memory_order_acquire);
#else
    return syseba_stop_pending != 0;
#endif
}

static void syseba_signal_handler(int signal_number)
{
    (void)signal_number;
    syseba_cli_request_stop();
}

static bool syseba_cli_command_valid(const char *command)
{
    static const char *commands[] = {
        "run",
        "status",
        "logs",
        "config-check",
        "restore-list",
        "restore-copy",
        "restore-browser",
        "service-install",
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
        if (strcmp(command, commands[index]) == 0) {
            return true;
        }
    }
    return false;
}

static int syseba_option_copy(char *destination,
                              size_t destination_size,
                              const char *value,
                              const char *name,
                              char *error,
                              size_t error_size)
{
    if (value == NULL ||
        snprintf(destination,
                 destination_size,
                 "%s",
                 value) >= (int)destination_size) {
        (void)snprintf(error,
                       error_size,
                       "Invalid or too long value for %s",
                       name);
        return -1;
    }
    return 0;
}

static const char *syseba_option_value(int argc,
                                       char **argv,
                                       int *index,
                                       const char *argument,
                                       char *error,
                                       size_t error_size)
{
    const char *equals = strchr(argument, '=');
    if (equals != NULL) {
        return equals + 1;
    }
    if (*index + 1 >= argc) {
        (void)snprintf(error,
                       error_size,
                       "Missing value for %s",
                       argument);
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static int syseba_parse_unsigned(const char *value,
                                 unsigned minimum,
                                 unsigned maximum,
                                 unsigned *output)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *output = (unsigned)parsed;
    return 0;
}

void syseba_options_defaults(syseba_options_t *options)
{
    memset(options, 0, sizeof(*options));
    options->command = "run";
    options->language = SYSEBA_LANGUAGE_IT;
    options->web_port = SYSEBA_DEFAULT_WEB_PORT;
    options->console_refresh = 3.0;
    options->lines = 100;
    options->page = 1;
    options->page_size = SYSEBA_DEFAULT_RESTORE_PAGE;
    (void)snprintf(options->lock_path,
                   sizeof(options->lock_path),
                   "%s",
                   SYSEBA_DEFAULT_LOCK);
    (void)snprintf(options->db_path,
                   sizeof(options->db_path),
                   "%s",
                   SYSEBA_DEFAULT_DB);
    (void)snprintf(options->token_path,
                   sizeof(options->token_path),
                   "%s",
                   SYSEBA_DEFAULT_TOKEN);
    (void)snprintf(options->web_host,
                   sizeof(options->web_host),
                   "%s",
                   SYSEBA_DEFAULT_WEB_HOST);
    (void)snprintf(options->sort, sizeof(options->sort), "name");
    (void)snprintf(options->direction, sizeof(options->direction), "asc");
}

void syseba_cli_usage(FILE *stream, syseba_language_t language)
{
    bool italian = language == SYSEBA_LANGUAGE_IT;
    fprintf(
        stream,
        "SySeBa %s - The Syncro Service Backup\n\n"
        "%s:\n"
        "  syseba [command] [options]\n\n"
        "%s:\n"
        "  run               %s\n"
        "  status            %s\n"
        "  logs              %s\n"
        "  config-check      %s\n"
        "  restore-list      %s\n"
        "  restore-copy      %s\n"
        "  restore-browser   %s\n"
        "  service-install   %s\n\n"
        "%s:\n"
        "  --config PATH             %s\n"
        "  --lang it|en              %s\n"
        "  --silent                  %s\n"
        "  --web                     %s\n"
        "  --web-only                %s\n"
        "  --web-host ADDRESS        %s\n"
        "  --web-port PORT           %s\n"
        "  --web-token TOKEN         %s\n"
        "  --web-token-file PATH     %s\n"
        "  --no-web-auth             %s\n"
        "  --no-initial-sync         %s\n"
        "  --lockfile PATH           %s\n"
        "  --db-path PATH            %s\n"
        "  --console-refresh SEC     %s\n"
        "  --lines N                 %s\n"
        "  --path RELATIVE           %s\n"
        "  --search TEXT             %s\n"
        "  --page N --page-size N    %s\n"
        "  --sort name|mtime|size    %s\n"
        "  --direction asc|desc      %s\n"
        "  --overwrite | --rename    %s\n"
        "  --json                    %s\n"
        "  --version                 %s\n"
        "  --help                    %s\n",
        SYSEBA_VERSION,
        italian ? "Uso" : "Usage",
        italian ? "Comandi" : "Commands",
        italian ? "avvia watcher, sincronizzazione e dashboard"
                : "start watcher, synchronization, and dashboard",
        italian ? "mostra stato e utilizzo dischi"
                : "show status and disk usage",
        italian ? "mostra la coda finale del log"
                : "show the log tail",
        italian ? "valida la configurazione"
                : "validate configuration",
        italian ? "elenca l'area restore"
                : "list the restore area",
        italian ? "ripristina l'elemento indicato da --path"
                : "restore the item selected by --path",
        italian ? "navigazione e scelta restore interattive"
                : "interactive restore navigation and selection",
        italian ? "installa e abilita il servizio di sistema"
                : "install and enable the system service",
        italian ? "Opzioni" : "Options",
        italian ? "file di configurazione alternativo"
                : "alternate configuration file",
        italian ? "lingua CLI, console e Web UI"
                : "CLI, console, and Web UI language",
        italian ? "nessuna dashboard console"
                : "disable the console dashboard",
        italian ? "avvia la Web UI insieme al watcher"
                : "start the Web UI with the watcher",
        italian ? "avvia solo la Web UI"
                : "start only the Web UI",
        italian ? "indirizzo di ascolto Web"
                : "Web listen address",
        italian ? "porta Web (default 8765)"
                : "Web port (default 8765)",
        italian ? "token Web esplicito"
                : "explicit Web token",
        italian ? "file persistente del token Web"
                : "persistent Web token file",
        italian ? "disabilita autenticazione (non sicuro)"
                : "disable authentication (unsafe)",
        italian ? "salta la sincronizzazione iniziale"
                : "skip initial synchronization",
        italian ? "file lock del processo"
                : "process lock file",
        italian ? "database SQLite"
                : "SQLite database",
        italian ? "refresh dashboard"
                : "dashboard refresh",
        italian ? "righe di log (massimo 2000)"
                : "log lines (maximum 2000)",
        italian ? "percorso relativo nel restore"
                : "relative path in restore",
        italian ? "filtro nome"
                : "name filter",
        italian ? "paginazione restore"
                : "restore pagination",
        italian ? "campo di ordinamento"
                : "sort field",
        italian ? "direzione"
                : "direction",
        italian ? "strategia in caso di conflitto"
                : "conflict strategy",
        italian ? "output JSON"
                : "JSON output",
        italian ? "mostra versione"
                : "show version",
        italian ? "mostra questo aiuto"
                : "show this help");
}

int syseba_cli_parse(int argc,
                     char **argv,
                     syseba_options_t *options,
                     char *error,
                     size_t error_size)
{
    bool command_set = false;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        const char *value;

        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            syseba_cli_usage(stdout, options->language);
            return 1;
        }
        if (strcmp(argument, "--version") == 0 ||
            strcmp(argument, "-V") == 0) {
            printf("SySeBa %s\n", SYSEBA_VERSION);
            return 1;
        }
        if (argument[0] != '-') {
            if (command_set || !syseba_cli_command_valid(argument)) {
                (void)snprintf(error,
                               error_size,
                               "Unknown command: %s",
                               argument);
                return -1;
            }
            options->command = argument;
            command_set = true;
            continue;
        }
        if (strcmp(argument, "--silent") == 0) {
            options->silent = true;
        } else if (strcmp(argument, "--web") == 0) {
            options->web = true;
        } else if (strcmp(argument, "--web-only") == 0) {
            options->web_only = true;
        } else if (strcmp(argument, "--no-web-auth") == 0) {
            options->no_web_auth = true;
        } else if (strcmp(argument, "--no-initial-sync") == 0) {
            options->no_initial_sync = true;
        } else if (strcmp(argument, "--json") == 0) {
            options->output_json = true;
        } else if (strcmp(argument, "--overwrite") == 0) {
            options->overwrite = true;
        } else if (strcmp(argument, "--rename") == 0) {
            options->rename = true;
        } else if (strcmp(argument, "--create-daemon") == 0) {
            options->create_daemon = true;
            options->command = "service-install";
            command_set = true;
#ifdef _WIN32
        } else if (strcmp(argument, "--windows-service") == 0) {
            options->windows_service = true;
#endif
        } else if (strncmp(argument, "--config", 8) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->config_path,
                                   sizeof(options->config_path),
                                   value,
                                   "--config",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--lang", 6) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                (strcmp(value, "it") != 0 && strcmp(value, "en") != 0)) {
                (void)snprintf(error,
                               error_size,
                               "--lang must be it or en");
                return -1;
            }
            options->language = strcmp(value, "en") == 0
                                    ? SYSEBA_LANGUAGE_EN
                                    : SYSEBA_LANGUAGE_IT;
        } else if (strncmp(argument, "--web-host", 10) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->web_host,
                                   sizeof(options->web_host),
                                   value,
                                   "--web-host",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--web-port", 10) == 0) {
            unsigned port;
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                syseba_parse_unsigned(value, 1, 65535, &port) != 0) {
                (void)snprintf(error,
                               error_size,
                               "--web-port must be between 1 and 65535");
                return -1;
            }
            options->web_port = (int)port;
        } else if (strncmp(argument, "--web-token-file", 16) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->token_path,
                                   sizeof(options->token_path),
                                   value,
                                   "--web-token-file",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--web-token", 11) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->explicit_token,
                                   sizeof(options->explicit_token),
                                   value,
                                   "--web-token",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--lockfile", 10) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->lock_path,
                                   sizeof(options->lock_path),
                                   value,
                                   "--lockfile",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--db-path", 9) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->db_path,
                                   sizeof(options->db_path),
                                   value,
                                   "--db-path",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--console-refresh", 17) == 0) {
            char *end = NULL;
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            options->console_refresh = value == NULL ? 0.0 : strtod(value, &end);
            if (value == NULL || end == value || *end != '\0' ||
                options->console_refresh <= 0.0 ||
                options->console_refresh > 3600.0) {
                (void)snprintf(error,
                               error_size,
                               "Invalid --console-refresh");
                return -1;
            }
        } else if (strncmp(argument, "--lines", 7) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                syseba_parse_unsigned(value,
                                      1,
                                      2000,
                                      &options->lines) != 0) {
                (void)snprintf(error, error_size, "Invalid --lines");
                return -1;
            }
        } else if (strncmp(argument, "--path", 6) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->path,
                                   sizeof(options->path),
                                   value,
                                   "--path",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--search", 8) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (syseba_option_copy(options->search,
                                   sizeof(options->search),
                                   value,
                                   "--search",
                                   error,
                                   error_size) != 0) {
                return -1;
            }
        } else if (strncmp(argument, "--page-size", 11) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                syseba_parse_unsigned(value,
                                      1,
                                      SYSEBA_MAX_RESTORE_PAGE,
                                      &options->page_size) != 0) {
                (void)snprintf(error, error_size, "Invalid --page-size");
                return -1;
            }
        } else if (strncmp(argument, "--page", 6) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                syseba_parse_unsigned(value,
                                      1,
                                      UINT32_MAX,
                                      &options->page) != 0) {
                (void)snprintf(error, error_size, "Invalid --page");
                return -1;
            }
        } else if (strncmp(argument, "--sort", 6) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                (strcmp(value, "name") != 0 &&
                 strcmp(value, "mtime") != 0 &&
                 strcmp(value, "size") != 0)) {
                (void)snprintf(error, error_size, "Invalid --sort");
                return -1;
            }
            (void)snprintf(options->sort, sizeof(options->sort), "%s", value);
        } else if (strncmp(argument, "--direction", 11) == 0) {
            value = syseba_option_value(argc,
                                        argv,
                                        &index,
                                        argument,
                                        error,
                                        error_size);
            if (value == NULL ||
                (strcmp(value, "asc") != 0 &&
                 strcmp(value, "desc") != 0)) {
                (void)snprintf(error, error_size, "Invalid --direction");
                return -1;
            }
            (void)snprintf(options->direction,
                           sizeof(options->direction),
                           "%s",
                           value);
        } else {
            (void)snprintf(error,
                           error_size,
                           "Unknown option: %s",
                           argument);
            return -1;
        }
    }
    if (options->overwrite && options->rename) {
        (void)snprintf(error,
                       error_size,
                       "--overwrite and --rename are mutually exclusive");
        return -1;
    }
    return 0;
}

static void syseba_cli_app_destroy(syseba_app_t *app)
{
    for (size_t index = 0; index < app->recent_capacity; index++) {
        free(app->recent_logs[index]);
    }
    free(app->recent_logs);
    app->recent_logs = NULL;
    syseba_queue_destroy(&app->log_queue, free);
    syseba_queue_destroy(&app->event_queue, free);
    syseba_mutex_destroy(&app->state_mutex);
}

static int syseba_cli_load(const syseba_options_t *input,
                           syseba_options_t *options,
                           syseba_config_t *config,
                           char *error,
                           size_t error_size)
{
    *options = *input;
    if (syseba_config_find(options->config_path,
                           options->config_path,
                           sizeof(options->config_path)) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Configuration not found; use --config PATH");
        return SYSEBA_ERR_NOT_FOUND;
    }
    return syseba_config_load(options->config_path,
                              config,
                              error,
                              error_size);
}

static void syseba_cli_print_disk(const char *label, const char *path)
{
    syseba_disk_usage_t usage;
    if (syseba_disk_usage(path, &usage) == 0) {
        printf("%-8s %s [%.2f%%]\n", label, path, usage.used_percent);
    } else {
        printf("%-8s %s [not found]\n", label, path);
    }
}

static int syseba_cli_status(const syseba_options_t *options,
                             const syseba_config_t *config)
{
    FILE *lock = NULL;
    uint64_t pid = 0;
    bool running = syseba_lock_is_held(options->lock_path);
    if (syseba_open_regular_read(options->lock_path,
                                 &lock,
                                 NULL) == 0) {
        if (fscanf(lock, "%" SCNu64, &pid) != 1) {
            pid = 0;
        }
        fclose(lock);
    }
    if (options->output_json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *json_config = cJSON_CreateObject();
        cJSON *disk = cJSON_CreateObject();
        char *text;
        cJSON_AddBoolToObject(root, "running", running);
        if (pid > 0) {
            cJSON_AddNumberToObject(root, "pid", (double)pid);
        } else {
            cJSON_AddNullToObject(root, "pid");
        }
        cJSON_AddStringToObject(json_config, "source", config->source);
        cJSON_AddStringToObject(json_config, "backup", config->backup);
        cJSON_AddStringToObject(json_config, "restore", config->restore);
        cJSON_AddStringToObject(json_config, "log_file", config->log_file);
        cJSON_AddNumberToObject(json_config, "threads", config->threads);
        cJSON_AddStringToObject(json_config,
                               "config_path",
                               config->config_path);
        cJSON_AddItemToObject(root, "config", json_config);
        {
            const char *names[] = {"source", "backup", "restore"};
            const char *paths[] = {
                config->source,
                config->backup,
                config->restore,
            };
            for (size_t index = 0; index < 3; index++) {
                syseba_disk_usage_t usage;
                cJSON *item = cJSON_CreateObject();
                int result = syseba_disk_usage(paths[index], &usage);
                cJSON_AddStringToObject(item, "path", paths[index]);
                cJSON_AddBoolToObject(item,
                                      "exists",
                                      result == 0 && usage.exists);
                cJSON_AddNumberToObject(item,
                                        "used_percent",
                                        result == 0
                                            ? usage.used_percent
                                            : 0.0);
                cJSON_AddItemToObject(disk, names[index], item);
            }
        }
        cJSON_AddItemToObject(root, "disk", disk);
        text = cJSON_Print(root);
        puts(text);
        cJSON_free(text);
        cJSON_Delete(root);
    } else {
        printf("SySeBa: %s%s",
               running
                   ? syseba_tr(options->language, "ATTIVO", "RUNNING")
                   : syseba_tr(options->language, "FERMO", "STOPPED"),
               pid > 0 ? " (PID " : "");
        if (pid > 0) {
            printf("%" PRIu64 ")", pid);
        }
        fputc('\n', stdout);
        syseba_cli_print_disk(syseba_tr(options->language,
                                        "Sorgente",
                                        "Source"),
                              config->source);
        syseba_cli_print_disk("Backup", config->backup);
        syseba_cli_print_disk("Restore", config->restore);
    }
    return running ? 0 : 3;
}

static int syseba_cli_logs(const syseba_options_t *options,
                           const syseba_config_t *config)
{
    size_t count = 0;
    char **lines = syseba_log_tail(config->log_file,
                                   options->lines,
                                   &count);
    if (options->output_json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *array = cJSON_AddArrayToObject(root, "lines");
        char *text;
        for (size_t index = 0; index < count; index++) {
            cJSON_AddItemToArray(array, cJSON_CreateString(lines[index]));
        }
        text = cJSON_Print(root);
        puts(text);
        cJSON_free(text);
        cJSON_Delete(root);
    } else {
        for (size_t index = 0; index < count; index++) {
            puts(lines[index]);
        }
    }
    syseba_log_tail_free(lines, count);
    return 0;
}

static int syseba_cli_restore_list(const syseba_options_t *options,
                                   const syseba_config_t *config)
{
    syseba_app_t app;
    syseba_restore_listing_t listing;
    char error[1024] = {0};
    int result;
    if (syseba_app_init(&app,
                        config,
                        options,
                        error,
                        sizeof(error)) != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
    }
    result = syseba_restore_list(&app,
                                 options->path,
                                 options->search,
                                 options->page,
                                 options->page_size,
                                 options->sort,
                                 options->direction,
                                 &listing,
                                 error,
                                 sizeof(error));
    if (result != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        syseba_cli_app_destroy(&app);
        return 1;
    }
    if (options->output_json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *items = cJSON_AddArrayToObject(root, "items");
        char *text;
        cJSON_AddStringToObject(root, "path", listing.path);
        cJSON_AddBoolToObject(root, "is_file", listing.is_file);
        cJSON_AddNumberToObject(root, "page", listing.page);
        cJSON_AddNumberToObject(root, "pages", listing.pages);
        cJSON_AddNumberToObject(root, "page_size", listing.page_size);
        cJSON_AddNumberToObject(root, "total", (double)listing.total);
        cJSON_AddBoolToObject(root,
                              "has_previous",
                              listing.has_previous);
        cJSON_AddBoolToObject(root, "has_next", listing.has_next);
        for (size_t index = 0; index < listing.count; index++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item,
                                    "name",
                                    listing.items[index].name);
            cJSON_AddStringToObject(item,
                                    "path",
                                    listing.items[index].path);
            cJSON_AddBoolToObject(item,
                                  "is_dir",
                                  listing.items[index].is_directory);
            if (listing.items[index].is_directory) {
                cJSON_AddNullToObject(item, "size");
            } else {
                cJSON_AddNumberToObject(
                    item,
                    "size",
                    (double)listing.items[index].size);
            }
            cJSON_AddStringToObject(item,
                                    "mtime",
                                    listing.items[index].mtime);
            cJSON_AddBoolToObject(
                item,
                "destination_exists",
                listing.items[index].destination_exists);
            cJSON_AddItemToArray(items, item);
        }
        text = cJSON_Print(root);
        puts(text);
        cJSON_free(text);
        cJSON_Delete(root);
    } else {
        printf("%s - %zu %s - %s %u/%u\n",
               listing.path[0] == '\0' ? "/" : listing.path,
               listing.total,
               syseba_tr(options->language, "elementi", "items"),
               syseba_tr(options->language, "pagina", "page"),
               listing.page,
               listing.pages);
        for (size_t index = 0; index < listing.count; index++) {
            printf("%-4s %12" PRIu64 "  %s  %s%s\n",
                   listing.items[index].is_directory ? "DIR" : "FILE",
                   listing.items[index].size,
                   listing.items[index].mtime,
                   listing.items[index].path,
                   listing.items[index].destination_exists
                       ? syseba_tr(options->language,
                                   " [destinazione esistente]",
                                   " [destination exists]")
                       : "");
        }
    }
    syseba_restore_listing_free(&listing);
    syseba_cli_app_destroy(&app);
    return 0;
}

static int syseba_cli_restore_copy(const syseba_options_t *options,
                                   const syseba_config_t *config)
{
    syseba_app_t app;
    char error[1024] = {0};
    char destination[SYSEBA_PATH_MAX];
    syseba_restore_strategy_t strategy =
        options->rename
            ? SYSEBA_RESTORE_RENAME
            : (options->overwrite
                   ? SYSEBA_RESTORE_OVERWRITE
                   : SYSEBA_RESTORE_FAIL);
    int result;

    if (options->path[0] == '\0') {
        fprintf(stderr,
                "%s\n",
                syseba_tr(options->language,
                          "restore-copy richiede --path",
                          "restore-copy requires --path"));
        return 2;
    }
    if (syseba_app_init(&app,
                        config,
                        options,
                        error,
                        sizeof(error)) != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
    }
    {
        char parent[SYSEBA_PATH_MAX];
        (void)syseba_parent_dir(config->log_file,
                                parent,
                                sizeof(parent));
        (void)syseba_mkdirs(parent, 0750);
        (void)syseba_database_initialize(options->db_path,
                                         error,
                                         sizeof(error));
    }
    if (syseba_log_start(&app) != 0) {
        fprintf(stderr, "Error: unable to start logging\n");
        syseba_cli_app_destroy(&app);
        return 1;
    }
    result = syseba_restore_item(&app,
                                 options->path,
                                 strategy,
                                 destination,
                                 sizeof(destination),
                                 error,
                                 sizeof(error));
    syseba_log_stop(&app);
    if (result != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        syseba_cli_app_destroy(&app);
        return 1;
    }
    if (options->output_json) {
        cJSON *root = cJSON_CreateObject();
        char *text;
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "restored_to", destination);
        cJSON_AddStringToObject(
            root,
            "strategy",
            strategy == SYSEBA_RESTORE_RENAME
                ? "rename"
                : (strategy == SYSEBA_RESTORE_OVERWRITE
                       ? "overwrite"
                       : "fail"));
        text = cJSON_Print(root);
        puts(text);
        cJSON_free(text);
        cJSON_Delete(root);
    } else {
        puts(destination);
    }
    syseba_cli_app_destroy(&app);
    return 0;
}

static int syseba_cli_restore_browser(const syseba_options_t *options,
                                      const syseba_config_t *config)
{
    syseba_app_t app;
    char error[1024] = {0};
    char current[SYSEBA_PATH_MAX] = {0};
    char input[128];
    unsigned page = 1;
    unsigned page_size = options->page_size > 40 ? 40 : options->page_size;
    bool done = false;

    if (!syseba_is_terminal(stdin) || !syseba_is_terminal(stdout)) {
        fprintf(stderr,
                "%s\n",
                syseba_tr(options->language,
                          "restore-browser richiede un terminale interattivo",
                          "restore-browser requires an interactive terminal"));
        return 2;
    }
    if (syseba_app_init(&app,
                        config,
                        options,
                        error,
                        sizeof(error)) != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
    }
    {
        char parent[SYSEBA_PATH_MAX];
        (void)syseba_parent_dir(config->log_file,
                                parent,
                                sizeof(parent));
        (void)syseba_mkdirs(parent, 0750);
        (void)syseba_database_initialize(options->db_path,
                                         error,
                                         sizeof(error));
    }
    if (syseba_log_start(&app) != 0) {
        fprintf(stderr, "Error: unable to start logging\n");
        syseba_cli_app_destroy(&app);
        return 1;
    }

    while (!done) {
        syseba_restore_listing_t listing;
        int list_result = syseba_restore_list(
            &app,
            current,
            "",
            page,
            page_size,
            "name",
            "asc",
            &listing,
            error,
            sizeof(error));
        if (list_result != SYSEBA_OK) {
            fprintf(stderr, "Error: %s\n", error);
            break;
        }
        syseba_console_clear();
        printf("SySeBa Restore Browser\n");
        printf("======================\n");
        printf("%s: /%s\n",
               syseba_tr(options->language, "Percorso", "Path"),
               listing.path);
        printf("%s %u/%u - %zu %s\n\n",
               syseba_tr(options->language, "Pagina", "Page"),
               listing.page,
               listing.pages,
               listing.total,
               syseba_tr(options->language, "elementi", "items"));
        for (size_t index = 0; index < listing.count; index++) {
            printf("%3zu. %-4s %-12" PRIu64 " %s%s\n",
                   index + 1u,
                   listing.items[index].is_directory ? "DIR" : "FILE",
                   listing.items[index].size,
                   listing.items[index].name,
                   listing.items[index].destination_exists
                       ? syseba_tr(options->language,
                                   "  [conflitto]",
                                   "  [conflict]")
                       : "");
        }
        printf("\n%s\n",
               syseba_tr(
                   options->language,
                   "Numero=apri/ripristina, u=su, n=pagina successiva, "
                   "p=precedente, q=esci",
                   "Number=open/restore, u=up, n=next page, "
                   "p=previous, q=quit"));
        printf("> ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL) {
            done = true;
            syseba_restore_listing_free(&listing);
            continue;
        }
        input[strcspn(input, "\r\n")] = '\0';
        if (strcmp(input, "q") == 0) {
            done = true;
        } else if (strcmp(input, "u") == 0) {
            char *slash = strrchr(current, '/');
            if (slash == NULL) {
                current[0] = '\0';
            } else {
                *slash = '\0';
            }
            page = 1;
        } else if (strcmp(input, "n") == 0 && listing.has_next) {
            page++;
        } else if (strcmp(input, "p") == 0 && listing.has_previous) {
            page--;
        } else {
            char *end = NULL;
            unsigned long selection = strtoul(input, &end, 10);
            if (end != input && *end == '\0' &&
                selection >= 1 &&
                selection <= listing.count) {
                syseba_restore_item_t selected =
                    listing.items[selection - 1u];
                if (selected.is_directory) {
                    (void)snprintf(current,
                                   sizeof(current),
                                   "%s",
                                   selected.path);
                    page = 1;
                } else {
                    syseba_restore_strategy_t strategy =
                        SYSEBA_RESTORE_FAIL;
                    char destination[SYSEBA_PATH_MAX];
                    char choice[32];
                    if (selected.destination_exists) {
                        printf(
                            "%s [r=%s, o=%s, c=%s]: ",
                            syseba_tr(options->language,
                                      "La destinazione esiste",
                                      "Destination exists"),
                            syseba_tr(options->language,
                                      "rinomina",
                                      "rename"),
                            syseba_tr(options->language,
                                      "sovrascrivi",
                                      "overwrite"),
                            syseba_tr(options->language,
                                      "annulla",
                                      "cancel"));
                        fflush(stdout);
                        if (fgets(choice, sizeof(choice), stdin) == NULL) {
                            done = true;
                        } else if (choice[0] == 'r' ||
                                   choice[0] == 'R') {
                            strategy = SYSEBA_RESTORE_RENAME;
                        } else if (choice[0] == 'o' ||
                                   choice[0] == 'O') {
                            strategy = SYSEBA_RESTORE_OVERWRITE;
                        } else {
                            syseba_restore_listing_free(&listing);
                            continue;
                        }
                    }
                    if (!done &&
                        syseba_restore_item(&app,
                                            selected.path,
                                            strategy,
                                            destination,
                                            sizeof(destination),
                                            error,
                                            sizeof(error)) == SYSEBA_OK) {
                        printf("%s: %s\n",
                               syseba_tr(options->language,
                                         "Ripristinato in",
                                         "Restored to"),
                               destination);
                    } else if (!done) {
                        printf("Error: %s\n", error);
                    }
                    if (!done) {
                        printf("%s",
                               syseba_tr(options->language,
                                         "Invio per continuare...",
                                         "Press Enter to continue..."));
                        fflush(stdout);
                        if (fgets(choice, sizeof(choice), stdin) == NULL) {
                            done = true;
                        }
                    }
                }
            }
        }
        syseba_restore_listing_free(&listing);
    }
    syseba_log_stop(&app);
    syseba_cli_app_destroy(&app);
    return 0;
}

static int syseba_cli_run(const syseba_options_t *options,
                          const syseba_config_t *config)
{
    syseba_app_t app;
    char error[1024] = {0};
    int result;

    if (syseba_app_init(&app,
                        config,
                        options,
                        error,
                        sizeof(error)) != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
    }
    if (app.web_enabled &&
        syseba_web_load_token(&app,
                              options,
                              error,
                              sizeof(error)) != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        syseba_cli_app_destroy(&app);
        return 1;
    }
    result = syseba_app_start(&app, error, sizeof(error));
    if (result != SYSEBA_OK) {
        fprintf(stderr, "Error: %s\n", error);
        syseba_cli_app_destroy(&app);
        return 1;
    }

    if (syseba_cli_stop_pending()) {
        syseba_app_set_stopping(&app);
    }
    (void)signal(SIGINT, syseba_signal_handler);
    (void)signal(SIGTERM, syseba_signal_handler);
    if (app.web_enabled && !options->silent) {
        printf("%s: http://%s:%d\n",
               syseba_tr(options->language,
                         "Interfaccia Web",
                         "Web interface"),
               app.web_host,
               app.web_port);
        if (app.web_auth_enabled &&
            (strcmp(app.web_token_source, "file") == 0 ||
             strcmp(app.web_token_source, "generated-file") == 0)) {
            printf("%s: %s\n",
                   syseba_tr(options->language,
                             "File token Web",
                             "Web token file"),
                   options->token_path);
        }
    }
    if (!options->silent && !options->web_only &&
        syseba_is_terminal(stdout)) {
        (void)syseba_dashboard_run(&app, options->console_refresh);
    } else {
        while (!syseba_app_is_stopping(&app)) {
            if (syseba_cli_stop_pending()) {
                syseba_app_set_stopping(&app);
                break;
            }
            syseba_sleep_ms(250);
        }
    }
    syseba_app_stop(&app);
    syseba_cli_app_destroy(&app);
    return 0;
}

int syseba_cli_execute(const syseba_options_t *input)
{
    syseba_options_t options;
    syseba_config_t config;
    char error[1024] = {0};
    int result;

#ifdef _WIN32
    if (input->windows_service) {
        return syseba_windows_service_dispatch(input,
                                               error,
                                               sizeof(error));
    }
#endif
    if (strcmp(input->command, "service-install") == 0) {
        options = *input;
        if (options.config_path[0] == '\0' &&
            syseba_config_find(NULL,
                               options.config_path,
                               sizeof(options.config_path)) != 0) {
            (void)snprintf(options.config_path,
                           sizeof(options.config_path),
                           "%s",
                           SYSEBA_DEFAULT_CONFIG);
        }
        result = syseba_service_install(&options,
                                        error,
                                        sizeof(error));
        if (result != SYSEBA_OK) {
            fprintf(stderr, "Error: %s\n", error);
            return 1;
        }
        printf("%s\n",
               syseba_tr(options.language,
                         "Servizio SySeBa installato e abilitato.",
                         "SySeBa service installed and enabled."));
        return 0;
    }

    result = syseba_cli_load(input,
                             &options,
                             &config,
                             error,
                             sizeof(error));
    if (result != SYSEBA_OK) {
        fprintf(stderr,
                "%s: %s\n",
                syseba_tr(input->language, "Errore", "Error"),
                error);
        return 2;
    }
    if (strcmp(options.command, "status") == 0) {
        return syseba_cli_status(&options, &config);
    }
    if (strcmp(options.command, "logs") == 0) {
        return syseba_cli_logs(&options, &config);
    }
    if (strcmp(options.command, "config-check") == 0) {
        if (options.output_json) {
            cJSON *root = cJSON_CreateObject();
            char *text;
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddArrayToObject(root, "errors");
            cJSON_AddArrayToObject(root, "warnings");
            text = cJSON_Print(root);
            puts(text);
            cJSON_free(text);
            cJSON_Delete(root);
        } else {
            puts("OK");
            if (!syseba_fs_exists(config.backup)) {
                printf("%s: %s\n",
                       syseba_tr(options.language,
                                 "Verrà creata la directory",
                                 "Directory will be created"),
                       config.backup);
            }
            if (!syseba_fs_exists(config.restore)) {
                printf("%s: %s\n",
                       syseba_tr(options.language,
                                 "Verrà creata la directory",
                                 "Directory will be created"),
                       config.restore);
            }
        }
        return 0;
    }
    if (strcmp(options.command, "restore-list") == 0) {
        return syseba_cli_restore_list(&options, &config);
    }
    if (strcmp(options.command, "restore-browser") == 0) {
        return syseba_cli_restore_browser(&options, &config);
    }
    if (strcmp(options.command, "restore-copy") == 0) {
        return syseba_cli_restore_copy(&options, &config);
    }
    return syseba_cli_run(&options, &config);
}
