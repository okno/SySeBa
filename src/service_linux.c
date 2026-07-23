#include "syseba_internal.h"

#ifndef _WIN32
#ifndef __APPLE__

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *syseba_systemd_unit_format =
    "[Unit]\n"
    "Description=SySeBa - The Syncro Service Backup\n"
    "Wants=network-online.target\n"
    "After=network-online.target\n"
    "\n"
    "[Service]\n"
    "Type=simple\n"
    "ExecStart=\"%s\" run --silent --web --web-host 0.0.0.0 "
    "--web-port %d --config \"%s\" --web-token-file \"%s\" "
    "--db-path \"%s\" --lockfile \"%s\" --lang %s\n"
    "Restart=always\n"
    "RestartSec=5\n"
    "TimeoutStopSec=45\n"
    "User=root\n"
    "Group=root\n"
    "StandardOutput=journal\n"
    "StandardError=journal\n"
    "SyslogIdentifier=syseba\n"
    "NoNewPrivileges=true\n"
    "CapabilityBoundingSet=CAP_DAC_OVERRIDE CAP_FOWNER\n"
    "PrivateTmp=true\n"
    "PrivateDevices=true\n"
    "ProtectSystem=full\n"
    "ReadWritePaths=\"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"\n"
    "ProtectProc=invisible\n"
    "ProcSubset=pid\n"
    "ProtectKernelTunables=true\n"
    "ProtectKernelModules=true\n"
    "ProtectControlGroups=true\n"
    "ProtectKernelLogs=true\n"
    "ProtectClock=true\n"
    "ProtectHostname=true\n"
    "RestrictSUIDSGID=true\n"
    "RestrictRealtime=true\n"
    "RestrictNamespaces=true\n"
    "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6\n"
    "LockPersonality=true\n"
    "MemoryDenyWriteExecute=true\n"
    "RemoveIPC=true\n"
    "SystemCallFilter=~@mount @module @reboot @swap @raw-io @debug @cpu-emulation\n"
    "SystemCallErrorNumber=EPERM\n"
    "SystemCallArchitectures=native\n"
    "UMask=0077\n"
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";

static int syseba_systemd_escape(const char *input,
                                 char *output,
                                 size_t output_size)
{
    size_t used = 0;
    if (input == NULL || output_size == 0) {
        return -1;
    }
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0';
         cursor++) {
        bool duplicate = *cursor == '%' || *cursor == '$';
        bool slash_escape = *cursor == '\\' || *cursor == '"';
        if (*cursor < 0x20 || *cursor == 0x7f ||
            used + (duplicate || slash_escape ? 2u : 1u) >= output_size) {
            return -1;
        }
        if (duplicate) {
            output[used++] = (char)*cursor;
        } else if (slash_escape) {
            output[used++] = '\\';
        }
        output[used++] = (char)*cursor;
    }
    output[used] = '\0';
    return 0;
}

static int syseba_run_systemctl(const char *action,
                                const char *option,
                                const char *unit)
{
    pid_t child = fork();
    pid_t waited;
    int status;
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (option != NULL && unit != NULL) {
            execl("/usr/bin/systemctl",
                  "systemctl",
                  action,
                  option,
                  unit,
                  (char *)NULL);
        } else if (unit != NULL) {
            execl("/usr/bin/systemctl",
                  "systemctl",
                  action,
                  unit,
                  (char *)NULL);
        } else {
            execl("/usr/bin/systemctl",
                  "systemctl",
                  action,
                  (char *)NULL);
        }
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int syseba_write_atomic(const char *path,
                               const char *content,
                               mode_t mode,
                               char *error,
                               size_t error_size)
{
    char temporary[SYSEBA_PATH_MAX];
    FILE *file;
    if (snprintf(temporary,
                 sizeof(temporary),
                 "%s.tmp.%" PRIu64,
                 path,
                 syseba_monotonic_ns()) >= (int)sizeof(temporary)) {
        return -1;
    }
    if (syseba_open_exclusive_write(temporary,
                                    &file,
                                    (unsigned)mode) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to write %s: %s",
                       path,
                       strerror(errno));
        return -1;
    }
    {
        int write_error =
            fwrite(content, 1, strlen(content), file) != strlen(content) ||
            fflush(file) != 0 ||
            fsync(fileno(file)) != 0;
        if (fclose(file) != 0) {
            write_error = 1;
        }
        if (!write_error && chmod(temporary, mode) != 0) {
            write_error = 1;
        }
        if (!write_error &&
            syseba_atomic_replace(temporary, path) != 0) {
            write_error = 1;
        }
        if (!write_error) {
            return 0;
        }
        (void)remove(temporary);
        (void)snprintf(error,
                       error_size,
                       "Unable to write %s: %s",
                       path,
                       strerror(errno));
        return -1;
    }
}

int syseba_service_install(const syseba_options_t *options,
                           char *error,
                           size_t error_size)
{
    const char *unit_path = "/etc/systemd/system/syseba.service";
    syseba_config_t loaded;
    char executable[SYSEBA_PATH_MAX];
    char escaped_executable[SYSEBA_PATH_MAX * 2];
    char escaped_config[SYSEBA_PATH_MAX * 2];
    char escaped_token[SYSEBA_PATH_MAX * 2];
    char escaped_db[SYSEBA_PATH_MAX * 2];
    char escaped_lock[SYSEBA_PATH_MAX * 2];
    char writable_source[SYSEBA_PATH_MAX * 2];
    char writable_backup[SYSEBA_PATH_MAX * 2];
    char writable_restore[SYSEBA_PATH_MAX * 2];
    char writable_config[SYSEBA_PATH_MAX * 2];
    char writable_token[SYSEBA_PATH_MAX * 2];
    char writable_db[SYSEBA_PATH_MAX * 2];
    char writable_lock[SYSEBA_PATH_MAX * 2];
    char writable_log[SYSEBA_PATH_MAX * 2];
    char config_parent[SYSEBA_PATH_MAX];
    char token_parent[SYSEBA_PATH_MAX];
    char db_parent[SYSEBA_PATH_MAX];
    char lock_parent[SYSEBA_PATH_MAX];
    char log_parent[SYSEBA_PATH_MAX];
    char unit[SYSEBA_PATH_MAX * 20];
    ssize_t executable_length;
    if (geteuid() != 0) {
        (void)snprintf(error,
                       error_size,
                       "service-install must be run as root");
        return SYSEBA_ERR_PERMISSION;
    }
    if (syseba_config_load(options->config_path,
                           &loaded,
                           error,
                           error_size) != SYSEBA_OK ||
        syseba_parent_dir(loaded.config_path,
                          config_parent,
                          sizeof(config_parent)) != 0 ||
        syseba_parent_dir(options->token_path,
                          token_parent,
                          sizeof(token_parent)) != 0 ||
        syseba_parent_dir(options->db_path,
                          db_parent,
                          sizeof(db_parent)) != 0 ||
        syseba_parent_dir(options->lock_path,
                          lock_parent,
                          sizeof(lock_parent)) != 0 ||
        syseba_parent_dir(loaded.log_file,
                          log_parent,
                          sizeof(log_parent)) != 0) {
        if (error[0] == '\0') {
            (void)snprintf(error,
                           error_size,
                           "Unable to resolve service paths");
        }
        return SYSEBA_ERR_INVALID;
    }
    if (syseba_mkdirs(loaded.backup, 0750) != 0 ||
        syseba_mkdirs(loaded.restore, 0750) != 0 ||
        syseba_mkdirs(token_parent, 0750) != 0 ||
        syseba_mkdirs(db_parent, 0750) != 0 ||
        syseba_mkdirs(lock_parent, 0750) != 0 ||
        syseba_mkdirs(log_parent, 0750) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to create service runtime directories: %s",
                       strerror(errno));
        return SYSEBA_ERR_PERMISSION;
    }
    executable_length = readlink("/proc/self/exe",
                                 executable,
                                 sizeof(executable) - 1u);
    if (executable_length <= 0 ||
        (size_t)executable_length >= sizeof(executable)) {
        (void)snprintf(error,
                       error_size,
                       "Unable to resolve the current executable");
        return SYSEBA_ERR;
    }
    executable[executable_length] = '\0';
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    if (syseba_systemd_escape(executable,
                              escaped_executable,
                              sizeof(escaped_executable)) != 0 ||
        syseba_systemd_escape(loaded.config_path,
                              escaped_config,
                              sizeof(escaped_config)) != 0 ||
        syseba_systemd_escape(options->token_path,
                              escaped_token,
                              sizeof(escaped_token)) != 0 ||
        syseba_systemd_escape(options->db_path,
                              escaped_db,
                              sizeof(escaped_db)) != 0 ||
        syseba_systemd_escape(options->lock_path,
                              escaped_lock,
                              sizeof(escaped_lock)) != 0 ||
        syseba_systemd_escape(loaded.source,
                              writable_source,
                              sizeof(writable_source)) != 0 ||
        syseba_systemd_escape(loaded.backup,
                              writable_backup,
                              sizeof(writable_backup)) != 0 ||
        syseba_systemd_escape(loaded.restore,
                              writable_restore,
                              sizeof(writable_restore)) != 0 ||
        syseba_systemd_escape(config_parent,
                              writable_config,
                              sizeof(writable_config)) != 0 ||
        syseba_systemd_escape(token_parent,
                              writable_token,
                              sizeof(writable_token)) != 0 ||
        syseba_systemd_escape(db_parent,
                              writable_db,
                              sizeof(writable_db)) != 0 ||
        syseba_systemd_escape(lock_parent,
                              writable_lock,
                              sizeof(writable_lock)) != 0 ||
        syseba_systemd_escape(log_parent,
                              writable_log,
                              sizeof(writable_log)) != 0 ||
        snprintf(unit,
                 sizeof(unit),
                 syseba_systemd_unit_format,
                 escaped_executable,
                 options->web_port,
                 escaped_config,
                 escaped_token,
                 escaped_db,
                 escaped_lock,
                 options->language == SYSEBA_LANGUAGE_EN ? "en" : "it",
                 writable_source,
                 writable_backup,
                 writable_restore,
                 writable_config,
                 writable_token,
                 writable_db,
                 writable_lock,
                 writable_log) >= (int)sizeof(unit)) {
        (void)snprintf(error,
                       error_size,
                       "A service path is invalid or too long");
        return SYSEBA_ERR_INVALID;
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (syseba_write_atomic(unit_path,
                            unit,
                            0644,
                            error,
                            error_size) != 0) {
        return SYSEBA_ERR;
    }
    if (syseba_run_systemctl("daemon-reload", NULL, NULL) != 0 ||
        syseba_run_systemctl("enable",
                             "--now",
                             "syseba.service") != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unit written, but systemctl daemon-reload/enable --now failed");
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}

int syseba_service_restart_available(void)
{
    const char *invocation = getenv("INVOCATION_ID");
    return invocation != NULL && *invocation != '\0' &&
           access("/usr/bin/systemctl", X_OK) == 0;
}

int syseba_service_request_restart(char *error, size_t error_size)
{
    pid_t child;
    if (!syseba_service_restart_available()) {
        (void)snprintf(error,
                       error_size,
                       "Automatic restart unavailable; run: %s",
                       syseba_service_restart_command());
        return SYSEBA_ERR;
    }
    child = fork();
    if (child < 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to schedule restart: %s",
                       strerror(errno));
        return SYSEBA_ERR;
    }
    if (child == 0) {
        int null_fd;
        (void)setsid();
        syseba_sleep_ms(750);
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
        }
        execl("/usr/bin/systemctl",
              "systemctl",
              "restart",
              "syseba.service",
              (char *)NULL);
        _exit(127);
    }
    return SYSEBA_OK;
}

const char *syseba_service_restart_command(void)
{
    return "sudo systemctl restart syseba.service";
}

#endif
#endif
