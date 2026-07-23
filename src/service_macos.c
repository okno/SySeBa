#include "syseba_internal.h"

#ifdef __APPLE__

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int syseba_xml_escape(const char *input,
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
        const char *replacement = NULL;
        if (*cursor < 0x20 && *cursor != '\t') {
            return -1;
        }
        switch (*cursor) {
            case '&':
                replacement = "&amp;";
                break;
            case '<':
                replacement = "&lt;";
                break;
            case '>':
                replacement = "&gt;";
                break;
            case '"':
                replacement = "&quot;";
                break;
            case '\'':
                replacement = "&apos;";
                break;
            default:
                break;
        }
        if (replacement != NULL) {
            size_t length = strlen(replacement);
            if (used + length >= output_size) {
                return -1;
            }
            memcpy(output + used, replacement, length);
            used += length;
        } else {
            if (used + 1u >= output_size) {
                return -1;
            }
            output[used++] = (char)*cursor;
        }
    }
    output[used] = '\0';
    return 0;
}

static int syseba_launchctl(const char *action,
                            const char *domain_or_target,
                            const char *path)
{
    pid_t child = fork();
    pid_t waited;
    int status;
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (path != NULL) {
            execl("/bin/launchctl",
                  "launchctl",
                  action,
                  domain_or_target,
                  path,
                  (char *)NULL);
        } else {
            execl("/bin/launchctl",
                  "launchctl",
                  action,
                  domain_or_target,
                  (char *)NULL);
        }
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? 0
               : -1;
}

static int syseba_write_plist(const char *path,
                              const char *content,
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
                                    0644) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to write LaunchDaemon plist: %s",
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
        if (!write_error && chmod(temporary, 0644) != 0) {
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
                       "Unable to write LaunchDaemon plist: %s",
                       strerror(errno));
        return -1;
    }
}

int syseba_service_install(const syseba_options_t *options,
                           char *error,
                           size_t error_size)
{
    const char *plist_path =
        "/Library/LaunchDaemons/com.okno.syseba.plist";
    syseba_config_t loaded;
    char executable[SYSEBA_PATH_MAX];
    char resolved_executable[SYSEBA_PATH_MAX];
    char escaped_executable[SYSEBA_PATH_MAX * 2];
    char escaped_config[SYSEBA_PATH_MAX * 2];
    char escaped_token[SYSEBA_PATH_MAX * 2];
    char escaped_db[SYSEBA_PATH_MAX * 2];
    char escaped_lock[SYSEBA_PATH_MAX * 2];
    char plist[SYSEBA_PATH_MAX * 12];
    uint32_t executable_size = (uint32_t)sizeof(executable);

    if (geteuid() != 0) {
        (void)snprintf(error,
                       error_size,
                       "service-install must be run as root");
        return SYSEBA_ERR_PERMISSION;
    }
    if (syseba_config_load(options->config_path,
                           &loaded,
                           error,
                           error_size) != SYSEBA_OK) {
        return SYSEBA_ERR_INVALID;
    }
    if (_NSGetExecutablePath(executable, &executable_size) != 0 ||
        realpath(executable, resolved_executable) == NULL ||
        syseba_xml_escape(resolved_executable,
                          escaped_executable,
                          sizeof(escaped_executable)) != 0 ||
        syseba_xml_escape(loaded.config_path,
                          escaped_config,
                          sizeof(escaped_config)) != 0 ||
        syseba_xml_escape(options->token_path,
                          escaped_token,
                          sizeof(escaped_token)) != 0 ||
        syseba_xml_escape(options->db_path,
                          escaped_db,
                          sizeof(escaped_db)) != 0 ||
        syseba_xml_escape(options->lock_path,
                          escaped_lock,
                          sizeof(escaped_lock)) != 0) {
        (void)snprintf(error,
                       error_size,
                       "A LaunchDaemon path is invalid or too long");
        return SYSEBA_ERR_INVALID;
    }

    if (snprintf(
            plist,
            sizeof(plist),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "<key>Label</key><string>com.okno.syseba</string>\n"
            "<key>ProgramArguments</key><array>\n"
            "<string>%s</string><string>run</string><string>--silent</string>\n"
            "<string>--web</string><string>--web-host</string>"
            "<string>0.0.0.0</string>\n"
            "<string>--web-port</string><string>%d</string>\n"
            "<string>--config</string><string>%s</string>\n"
            "<string>--web-token-file</string><string>%s</string>\n"
            "<string>--db-path</string><string>%s</string>\n"
            "<string>--lockfile</string><string>%s</string>\n"
            "<string>--lang</string><string>%s</string>\n"
            "</array>\n"
            "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>\n"
            "<key>ThrottleInterval</key><integer>5</integer>\n"
            "<key>ProcessType</key><string>Background</string>\n"
            "<key>StandardOutPath</key>"
            "<string>/usr/local/var/log/syseba/service.log</string>\n"
            "<key>StandardErrorPath</key>"
            "<string>/usr/local/var/log/syseba/service-error.log</string>\n"
            "</dict></plist>\n",
            escaped_executable,
            options->web_port,
            escaped_config,
            escaped_token,
            escaped_db,
            escaped_lock,
            options->language == SYSEBA_LANGUAGE_EN ? "en" : "it") >=
        (int)sizeof(plist)) {
        (void)snprintf(error, error_size, "LaunchDaemon plist is too large");
        return SYSEBA_ERR_INVALID;
    }
    if (syseba_mkdirs("/usr/local/var/log/syseba", 0750) != 0 ||
        syseba_write_plist(plist_path,
                           plist,
                           error,
                           error_size) != 0) {
        return SYSEBA_ERR;
    }

    (void)syseba_launchctl("bootout",
                           "system/com.okno.syseba",
                           NULL);
    if (syseba_launchctl("bootstrap", "system", plist_path) != 0 ||
        syseba_launchctl("enable",
                         "system/com.okno.syseba",
                         NULL) != 0 ||
        syseba_launchctl("kickstart",
                         "-k",
                         "system/com.okno.syseba") != 0) {
        (void)snprintf(error,
                       error_size,
                       "Plist installed, but launchctl could not start SySeBa");
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}

int syseba_service_restart_available(void)
{
    return geteuid() == 0 && access("/bin/launchctl", X_OK) == 0;
}

int syseba_service_request_restart(char *error, size_t error_size)
{
    pid_t child;
    if (!syseba_service_restart_available()) {
        (void)snprintf(error,
                       error_size,
                       "Run: %s",
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
        (void)setsid();
        syseba_sleep_ms(750);
        execl("/bin/launchctl",
              "launchctl",
              "kickstart",
              "-k",
              "system/com.okno.syseba",
              (char *)NULL);
        _exit(127);
    }
    return SYSEBA_OK;
}

const char *syseba_service_restart_command(void)
{
    return "sudo launchctl kickstart -k system/com.okno.syseba";
}

#endif
