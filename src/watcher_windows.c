#include "syseba_internal.h"

#ifdef _WIN32

static int syseba_utf8_to_wide(const char *input,
                               wchar_t *output,
                               size_t output_count)
{
    int count = MultiByteToWideChar(CP_UTF8,
                                    MB_ERR_INVALID_CHARS,
                                    input,
                                    -1,
                                    output,
                                    (int)output_count);
    return count > 0 ? 0 : -1;
}

static int syseba_wide_slice_to_utf8(const wchar_t *input,
                                     size_t input_count,
                                     char *output,
                                     size_t output_size)
{
    int count = WideCharToMultiByte(CP_UTF8,
                                    WC_ERR_INVALID_CHARS,
                                    input,
                                    (int)input_count,
                                    output,
                                    (int)output_size - 1,
                                    NULL,
                                    NULL);
    if (count <= 0 || (size_t)count >= output_size) {
        return -1;
    }
    output[count] = '\0';
    return 0;
}

static void syseba_windows_dispatch(syseba_app_t *app,
                                    const FILE_NOTIFY_INFORMATION *event)
{
    char relative[SYSEBA_PATH_MAX];
    char path[SYSEBA_PATH_MAX];
    DWORD attributes;
    bool directory;
    syseba_event_kind_t kind;

    if (syseba_wide_slice_to_utf8(event->FileName,
                                  event->FileNameLength / sizeof(wchar_t),
                                  relative,
                                  sizeof(relative)) != 0 ||
        snprintf(path,
                 sizeof(path),
                 "%s\\%s",
                 app->config.source,
                 relative) >= (int)sizeof(path)) {
        return;
    }
    attributes = GetFileAttributesA(path);
    directory = attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    switch (event->Action) {
    case FILE_ACTION_ADDED:
    case FILE_ACTION_RENAMED_NEW_NAME:
        kind = SYSEBA_EVENT_CREATE;
        break;
    case FILE_ACTION_REMOVED:
    case FILE_ACTION_RENAMED_OLD_NAME:
        kind = SYSEBA_EVENT_DELETE;
        break;
    case FILE_ACTION_MODIFIED:
        kind = SYSEBA_EVENT_MODIFY;
        break;
    default:
        return;
    }
    (void)syseba_app_enqueue(app, kind, path, directory);
}

void *syseba_watcher_native_main(void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    wchar_t source[SYSEBA_PATH_MAX];
    HANDLE directory;
    HANDLE completed;
    OVERLAPPED overlapped;
    unsigned char buffer[256 * 1024];
    bool pending = false;

    if (syseba_utf8_to_wide(app->config.source,
                            source,
                            sizeof(source) / sizeof(source[0])) != 0) {
        return syseba_watcher_poll_main(app);
    }
    directory = CreateFileW(source,
                            FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS |
                                FILE_FLAG_OVERLAPPED,
                            NULL);
    if (directory == INVALID_HANDLE_VALUE) {
        syseba_log_emit(app,
                        "WARNING",
                        "WATCH",
                        app->config.source,
                        "",
                        "ReadDirectoryChangesW",
                        "%s",
                        syseba_tr(app->language,
                                  "Watcher Windows non disponibile; uso il polling",
                                  "Windows watcher unavailable; using polling"));
        return syseba_watcher_poll_main(app);
    }
    completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (completed == NULL) {
        CloseHandle(directory);
        return syseba_watcher_poll_main(app);
    }
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = completed;
    syseba_log_emit(app,
                    "INFO",
                    "WATCH",
                    app->config.source,
                    "",
                    "ReadDirectoryChangesW",
                    "%s",
                    syseba_tr(app->language,
                              "Watcher Windows nativo attivo",
                              "Native Windows watcher active"));

    while (!syseba_app_is_stopping(app)) {
        if (!pending) {
            ResetEvent(completed);
            memset(&overlapped, 0, sizeof(overlapped));
            overlapped.hEvent = completed;
            if (!ReadDirectoryChangesW(
                    directory,
                    buffer,
                    (DWORD)sizeof(buffer),
                    TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME |
                        FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_SIZE |
                        FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_CREATION |
                        FILE_NOTIFY_CHANGE_ATTRIBUTES,
                    NULL,
                    &overlapped,
                    NULL)) {
                break;
            }
            pending = true;
        }
        {
            DWORD wait_result = WaitForSingleObject(completed, 500);
            if (wait_result == WAIT_TIMEOUT) {
                continue;
            }
            if (wait_result != WAIT_OBJECT_0) {
                break;
            }
        }
        {
            DWORD bytes = 0;
            if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
                if (GetLastError() == ERROR_OPERATION_ABORTED) {
                    break;
                }
                pending = false;
                continue;
            }
            pending = false;
            if (bytes == 0) {
                syseba_log_emit(app,
                                "WARNING",
                                "WATCH",
                                app->config.source,
                                "",
                                "buffer-overflow",
                                "%s",
                                syseba_tr(app->language,
                                          "Overflow del watcher Windows",
                                          "Windows watcher overflow"));
                (void)syseba_app_initial_sync(app);
                continue;
            }
            for (DWORD offset = 0; offset < bytes;) {
                const FILE_NOTIFY_INFORMATION *event =
                    (const FILE_NOTIFY_INFORMATION *)(buffer + offset);
                syseba_windows_dispatch(app, event);
                if (event->NextEntryOffset == 0) {
                    break;
                }
                offset += event->NextEntryOffset;
            }
        }
    }
    if (pending) {
        (void)CancelIoEx(directory, &overlapped);
        (void)WaitForSingleObject(completed, 1000);
    }
    CloseHandle(completed);
    CloseHandle(directory);
    return NULL;
}

#endif
