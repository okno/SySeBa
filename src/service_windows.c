#include "syseba_internal.h"

#ifdef _WIN32

static SERVICE_STATUS_HANDLE syseba_service_status_handle = NULL;
static SERVICE_STATUS syseba_service_status;
static syseba_options_t syseba_service_options;
static int syseba_service_exit_code = 1;

static void syseba_windows_report_service_status(DWORD state,
                                                 DWORD win32_exit_code,
                                                 DWORD wait_hint)
{
    static DWORD checkpoint = 1;
    syseba_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    syseba_service_status.dwCurrentState = state;
    syseba_service_status.dwWin32ExitCode = win32_exit_code;
    syseba_service_status.dwServiceSpecificExitCode = 0;
    syseba_service_status.dwWaitHint = wait_hint;
    syseba_service_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
            : 0;
    syseba_service_status.dwCheckPoint =
        state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING
            ? checkpoint++
            : 0;
    if (syseba_service_status_handle != NULL) {
        (void)SetServiceStatus(syseba_service_status_handle,
                               &syseba_service_status);
    }
}

static void WINAPI syseba_windows_service_control(DWORD control)
{
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        syseba_windows_report_service_status(SERVICE_STOP_PENDING,
                                             NO_ERROR,
                                             30000);
        syseba_cli_request_stop();
    }
}

static void WINAPI syseba_windows_service_main(DWORD argc, LPSTR *argv)
{
    syseba_options_t options = syseba_service_options;
    (void)argc;
    (void)argv;
    syseba_service_status_handle =
        RegisterServiceCtrlHandlerA("SySeBa",
                                    syseba_windows_service_control);
    if (syseba_service_status_handle == NULL) {
        return;
    }
    syseba_windows_report_service_status(SERVICE_START_PENDING,
                                         NO_ERROR,
                                         30000);
    options.windows_service = false;
    options.silent = true;
    syseba_windows_report_service_status(SERVICE_RUNNING,
                                         NO_ERROR,
                                         0);
    syseba_service_exit_code = syseba_cli_execute(&options);
    syseba_windows_report_service_status(
        SERVICE_STOPPED,
        syseba_service_exit_code == 0
            ? NO_ERROR
            : ERROR_SERVICE_SPECIFIC_ERROR,
        0);
}

int syseba_windows_service_dispatch(const syseba_options_t *options,
                                    char *error,
                                    size_t error_size)
{
    SERVICE_TABLE_ENTRYA dispatch_table[] = {
        {"SySeBa", syseba_windows_service_main},
        {NULL, NULL},
    };
    syseba_service_options = *options;
    if (!StartServiceCtrlDispatcherA(dispatch_table)) {
        (void)snprintf(error,
                       error_size,
                       "StartServiceCtrlDispatcher failed (%lu)",
                       (unsigned long)GetLastError());
        fprintf(stderr, "SySeBa: %s\n", error);
        return 1;
    }
    return syseba_service_exit_code;
}

int syseba_service_install(const syseba_options_t *options,
                           char *error,
                           size_t error_size)
{
    SC_HANDLE manager = NULL;
    SC_HANDLE service = NULL;
    syseba_config_t loaded;
    char executable[SYSEBA_PATH_MAX];
    char command[SYSEBA_PATH_MAX * 4];
    DWORD length = GetModuleFileNameA(NULL,
                                      executable,
                                      (DWORD)sizeof(executable));
    if (syseba_config_load(options->config_path,
                           &loaded,
                           error,
                           error_size) != SYSEBA_OK) {
        return SYSEBA_ERR_INVALID;
    }
    if (length == 0 || length >= sizeof(executable) ||
        strpbrk(loaded.config_path, "\r\n\"") != NULL ||
        strpbrk(options->token_path, "\r\n\"") != NULL ||
        strpbrk(options->db_path, "\r\n\"") != NULL ||
        strpbrk(options->lock_path, "\r\n\"") != NULL ||
        snprintf(command,
                 sizeof(command),
                 "\"%s\" run --silent --web --web-host 0.0.0.0 "
                 "--web-port %d --config \"%s\" --web-token-file \"%s\" "
                 "--db-path \"%s\" --lockfile \"%s\" --lang %s "
                 "--windows-service",
                 executable,
                 options->web_port,
                 loaded.config_path,
                 options->token_path,
                 options->db_path,
                 options->lock_path,
                 options->language == SYSEBA_LANGUAGE_EN ? "en" : "it") >=
            (int)sizeof(command)) {
        (void)snprintf(error, error_size, "Unable to build service command");
        return SYSEBA_ERR;
    }
    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (manager == NULL) {
        (void)snprintf(error,
                       error_size,
                       "OpenSCManager failed (%lu)",
                       (unsigned long)GetLastError());
        return SYSEBA_ERR_PERMISSION;
    }
    service = CreateServiceA(manager,
                             "SySeBa",
                             "SySeBa Backup Service",
                             SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START,
                             SERVICE_ERROR_NORMAL,
                             command,
                             NULL,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
    if (service == NULL && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceA(manager,
                               "SySeBa",
                               SERVICE_CHANGE_CONFIG | SERVICE_START |
                                   SERVICE_QUERY_STATUS);
        if (service != NULL &&
            !ChangeServiceConfigA(service,
                                  SERVICE_NO_CHANGE,
                                  SERVICE_AUTO_START,
                                  SERVICE_NO_CHANGE,
                                  command,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  "SySeBa Backup Service")) {
            CloseServiceHandle(service);
            service = NULL;
        }
    }
    if (service == NULL) {
        (void)snprintf(error,
                       error_size,
                       "Unable to create/update service (%lu)",
                       (unsigned long)GetLastError());
        CloseServiceHandle(manager);
        return SYSEBA_ERR;
    }
    {
        SERVICE_FAILURE_ACTIONSA actions;
        SC_ACTION action = {SC_ACTION_RESTART, 5000};
        SERVICE_DESCRIPTIONA description = {
            "Continuous file backup, restore and Web management service"};
        SERVICE_SID_INFO sid = {SERVICE_SID_TYPE_UNRESTRICTED};
        memset(&actions, 0, sizeof(actions));
        actions.dwResetPeriod = 86400;
        actions.cActions = 1;
        actions.lpsaActions = &action;
        (void)ChangeServiceConfig2A(service,
                                    SERVICE_CONFIG_FAILURE_ACTIONS,
                                    &actions);
        (void)ChangeServiceConfig2A(service,
                                    SERVICE_CONFIG_DESCRIPTION,
                                    &description);
        (void)ChangeServiceConfig2A(service,
                                    SERVICE_CONFIG_SERVICE_SID_INFO,
                                    &sid);
    }
    if (!StartServiceA(service, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        DWORD code = GetLastError();
        (void)snprintf(error,
                       error_size,
                       "Service installed but could not be started (%lu)",
                       (unsigned long)code);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return SYSEBA_ERR;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return SYSEBA_OK;
}

int syseba_service_restart_available(void)
{
    return 0;
}

int syseba_service_request_restart(char *error, size_t error_size)
{
    (void)snprintf(error,
                   error_size,
                   "Use an elevated terminal: sc stop SySeBa && sc start SySeBa");
    return SYSEBA_ERR;
}

const char *syseba_service_restart_command(void)
{
    return "sc stop SySeBa && sc start SySeBa";
}

#endif
