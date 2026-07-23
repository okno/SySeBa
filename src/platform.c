#include "syseba_internal.h"

#include <ctype.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <direct.h>
#include <io.h>
#include <psapi.h>
#include <sddl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

const char *syseba_version(void)
{
    return SYSEBA_VERSION;
}

int syseba_mutex_init(syseba_mutex_t *mutex)
{
#ifdef _WIN32
    InitializeCriticalSection(mutex);
    return 0;
#else
    return pthread_mutex_init(mutex, NULL);
#endif
}

void syseba_mutex_destroy(syseba_mutex_t *mutex)
{
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    (void)pthread_mutex_destroy(mutex);
#endif
}

void syseba_mutex_lock(syseba_mutex_t *mutex)
{
#ifdef _WIN32
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

void syseba_mutex_unlock(syseba_mutex_t *mutex)
{
#ifdef _WIN32
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

int syseba_cond_init(syseba_cond_t *cond)
{
#ifdef _WIN32
    InitializeConditionVariable(cond);
    return 0;
#else
    return pthread_cond_init(cond, NULL);
#endif
}

void syseba_cond_destroy(syseba_cond_t *cond)
{
#ifndef _WIN32
    (void)pthread_cond_destroy(cond);
#else
    (void)cond;
#endif
}

void syseba_cond_signal(syseba_cond_t *cond)
{
#ifdef _WIN32
    WakeConditionVariable(cond);
#else
    (void)pthread_cond_signal(cond);
#endif
}

void syseba_cond_broadcast(syseba_cond_t *cond)
{
#ifdef _WIN32
    WakeAllConditionVariable(cond);
#else
    (void)pthread_cond_broadcast(cond);
#endif
}

int syseba_cond_wait(syseba_cond_t *cond, syseba_mutex_t *mutex)
{
#ifdef _WIN32
    return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
#else
    return pthread_cond_wait(cond, mutex);
#endif
}

int syseba_cond_timedwait(syseba_cond_t *cond, syseba_mutex_t *mutex, unsigned timeout_ms)
{
#ifdef _WIN32
    if (SleepConditionVariableCS(cond, mutex, (DWORD)timeout_ms)) {
        return 0;
    }
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
#else
    struct timespec deadline;
    int result;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return -1;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    result = pthread_cond_timedwait(cond, mutex, &deadline);
    return result == 0 ? 0 : (result == ETIMEDOUT ? 1 : -1);
#endif
}

#ifdef _WIN32
typedef struct {
    syseba_thread_fn fn;
    void *arg;
} syseba_thread_start_t;

static DWORD WINAPI syseba_thread_trampoline(LPVOID opaque)
{
    syseba_thread_start_t *start = (syseba_thread_start_t *)opaque;
    syseba_thread_fn fn = start->fn;
    void *arg = start->arg;
    free(start);
    (void)fn(arg);
    return 0;
}
#endif

int syseba_thread_create(syseba_thread_t *thread, syseba_thread_fn fn, void *arg)
{
#ifdef _WIN32
    syseba_thread_start_t *start = (syseba_thread_start_t *)malloc(sizeof(*start));
    if (start == NULL) {
        return -1;
    }
    start->fn = fn;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, syseba_thread_trampoline, start, 0, NULL);
    if (*thread == NULL) {
        free(start);
        return -1;
    }
    return 0;
#else
    return pthread_create(thread, NULL, fn, arg);
#endif
}

int syseba_thread_join(syseba_thread_t thread)
{
#ifdef _WIN32
    DWORD result = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0 ? 0 : -1;
#else
    return pthread_join(thread, NULL);
#endif
}

void syseba_sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec requested;
    requested.tv_sec = (time_t)(milliseconds / 1000u);
    requested.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
    }
#endif
}

uint64_t syseba_monotonic_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart <= 0 || counter.QuadPart < 0) {
        return 0;
    }
    return ((uint64_t)counter.QuadPart * 1000000000ULL) /
           (uint64_t)frequency.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
#endif
}

static void syseba_localtime(time_t value, struct tm *result)
{
    memset(result, 0, sizeof(*result));
#ifdef _WIN32
    (void)localtime_s(result, &value);
#else
    (void)localtime_r(&value, result);
#endif
}

void syseba_timestamp_now(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm local = {0};
    syseba_localtime(now, &local);
    (void)strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local);
}

void syseba_iso_now(char *buffer, size_t size)
{
    time_t now = time(NULL);
    syseba_iso_from_time(now, buffer, size);
}

void syseba_iso_from_time(time_t value, char *buffer, size_t size)
{
    struct tm local = {0};
    syseba_localtime(value, &local);
    (void)strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &local);
}

syseba_pid_t syseba_process_id(void)
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

bool syseba_process_exists(syseba_pid_t pid)
{
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        DWORD code = 0;
        bool exists = false;
        if (process != NULL) {
            exists = GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
            CloseHandle(process);
        }
        return exists;
    }
#else
    return kill(pid, 0) == 0 || errno == EPERM;
#endif
}

bool syseba_lock_is_held(const char *path)
{
#ifdef _WIN32
    HANDLE handle = CreateFileA(path,
                                GENERIC_READ,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL |
                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_SHARING_VIOLATION ||
               error == ERROR_LOCK_VIOLATION;
    }
    CloseHandle(handle);
    return false;
#else
    int descriptor = open(path,
                          O_RDWR
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                              | O_NOFOLLOW
#endif
    );
    bool held = false;
    if (descriptor < 0) {
        return false;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        held = errno == EWOULDBLOCK || errno == EAGAIN;
    } else {
        (void)flock(descriptor, LOCK_UN);
    }
    close(descriptor);
    return held;
#endif
}

int syseba_disk_usage(const char *path, syseba_disk_usage_t *usage)
{
    memset(usage, 0, sizeof(*usage));
    (void)snprintf(usage->path, sizeof(usage->path), "%s", path);
#ifdef _WIN32
    {
        ULARGE_INTEGER available;
        ULARGE_INTEGER total;
        ULARGE_INTEGER free_bytes;
        if (!GetDiskFreeSpaceExA(path, &available, &total, &free_bytes)) {
            return -1;
        }
        usage->exists = true;
        usage->total = (uint64_t)total.QuadPart;
        usage->free_bytes = (uint64_t)free_bytes.QuadPart;
    }
#else
    {
        struct statvfs value;
        if (statvfs(path, &value) != 0) {
            return -1;
        }
        usage->exists = true;
        usage->total = (uint64_t)value.f_blocks * (uint64_t)value.f_frsize;
        usage->free_bytes = (uint64_t)value.f_bavail * (uint64_t)value.f_frsize;
    }
#endif
    usage->used = usage->total >= usage->free_bytes
                      ? usage->total - usage->free_bytes
                      : 0;
    usage->used_percent = usage->total == 0
                              ? 0.0
                              : ((double)usage->used * 100.0) / (double)usage->total;
    return 0;
}

int syseba_process_metrics(double *cpu_percent, double *memory_mb, unsigned *thread_count)
{
    if (cpu_percent != NULL) {
        *cpu_percent = -1.0;
    }
    if (memory_mb != NULL) {
        *memory_mb = -1.0;
    }
    if (thread_count != NULL) {
        *thread_count = 0;
    }
#ifdef _WIN32
    {
        PROCESS_MEMORY_COUNTERS_EX memory;
        if (memory_mb != NULL &&
            GetProcessMemoryInfo(GetCurrentProcess(),
                                 (PROCESS_MEMORY_COUNTERS *)&memory,
                                 sizeof(memory))) {
            *memory_mb = (double)memory.WorkingSetSize / (1024.0 * 1024.0);
        }
    }
    return 0;
#elif defined(__linux__)
    {
        FILE *status = fopen("/proc/self/status", "r");
        char line[256];
        if (status == NULL) {
            return -1;
        }
        while (fgets(line, sizeof(line), status) != NULL) {
            unsigned long value = 0;
            if (memory_mb != NULL && sscanf(line, "VmRSS: %lu kB", &value) == 1) {
                *memory_mb = (double)value / 1024.0;
            }
            if (thread_count != NULL && sscanf(line, "Threads: %lu", &value) == 1) {
                *thread_count = (unsigned)value;
            }
        }
        fclose(status);
    }
    return 0;
#else
    return 0;
#endif
}

int syseba_random_bytes(unsigned char *buffer, size_t size)
{
#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(NULL,
                                      buffer,
                                      (ULONG)size,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0 ? 0 : -1;
#else
    int fd;
    size_t offset = 0;
    fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
    );
    if (fd < 0) {
        return -1;
    }
    while (offset < size) {
        ssize_t count = read(fd, buffer + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)count;
    }
    close(fd);
    return 0;
#endif
}

int syseba_set_private_permissions(const char *path)
{
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = NULL;
    BOOL converted =
        ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)",
            SDDL_REVISION_1,
            &descriptor,
            NULL);
    BOOL applied = converted &&
                   SetFileSecurityA(
                       path,
                       DACL_SECURITY_INFORMATION |
                           PROTECTED_DACL_SECURITY_INFORMATION,
                       descriptor);
    if (descriptor != NULL) {
        LocalFree(descriptor);
    }
    if (!applied) {
        errno = EACCES;
        return -1;
    }
    return 0;
#else
    return chmod(path, S_IRUSR | S_IWUSR);
#endif
}

static void syseba_normalize_separators(char *path)
{
    char *cursor;
    for (cursor = path; *cursor != '\0'; cursor++) {
#ifdef _WIN32
        if (*cursor == '/') {
            *cursor = '\\';
        }
#else
        if (*cursor == '\\') {
            *cursor = '/';
        }
#endif
    }
}

int syseba_path_real(const char *path, char *output, size_t size)
{
    char absolute[SYSEBA_PATH_MAX];
    char normalized[SYSEBA_PATH_MAX];
    char *parts[SYSEBA_PATH_MAX / 2];
    size_t part_count = 0;
    char *context = NULL;
    char *token;
    char *cursor;
    size_t used = 0;
    bool rooted;
#ifdef _WIN32
    bool unc;
    if (_fullpath(absolute, path, sizeof(absolute)) == NULL) {
        return -1;
    }
    unc = absolute[0] == '\\' && absolute[1] == '\\';
    rooted = (strlen(absolute) >= 3 && absolute[1] == ':') || unc;
#else
    if (path[0] == '/') {
        if (snprintf(absolute, sizeof(absolute), "%s", path) >= (int)sizeof(absolute)) {
            return -1;
        }
    } else {
        if (getcwd(absolute, sizeof(absolute)) == NULL) {
            return -1;
        }
        used = strlen(absolute);
        if (snprintf(absolute + used,
                     sizeof(absolute) - used,
                     "/%s",
                     path) >= (int)(sizeof(absolute) - used)) {
            return -1;
        }
    }
    rooted = true;
#endif
    syseba_normalize_separators(absolute);
    (void)snprintf(normalized, sizeof(normalized), "%s", absolute);

#ifdef _WIN32
    cursor = normalized + (unc ? 2 : (rooted ? 3 : 0));
    token = strtok_s(cursor, "\\", &context);
#else
    cursor = normalized + (rooted ? 1 : 0);
    token = strtok_r(cursor, "/", &context);
#endif
    while (token != NULL) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            /* Skip. */
        } else if (strcmp(token, "..") == 0) {
            if (part_count > 0) {
                part_count--;
            }
        } else {
            parts[part_count++] = token;
        }
#ifdef _WIN32
        token = strtok_s(NULL, "\\", &context);
#else
        token = strtok_r(NULL, "/", &context);
#endif
    }

#ifdef _WIN32
    if (unc) {
        used = (size_t)snprintf(output, size, "\\\\");
    } else {
        used = rooted ? (size_t)snprintf(output, size, "%c:\\", absolute[0]) : 0;
    }
#else
    used = rooted ? (size_t)snprintf(output, size, "/") : 0;
#endif
    if (used >= size) {
        return -1;
    }
    for (size_t index = 0; index < part_count; index++) {
        size_t length = strlen(parts[index]);
        bool needs_separator = used > 0 && output[used - 1] != SYSEBA_PATH_SEP;
        if (used + (needs_separator ? 1u : 0u) + length + 1u > size) {
            return -1;
        }
        if (needs_separator) {
            output[used++] = SYSEBA_PATH_SEP;
        }
        memcpy(output + used, parts[index], length);
        used += length;
        output[used] = '\0';
    }
    return 0;
}

int syseba_mkdirs(const char *path, unsigned mode)
{
    char current[SYSEBA_PATH_MAX];
    size_t length;

    if (path == NULL || *path == '\0') {
        return 0;
    }
#ifdef _WIN32
    (void)mode;
#endif
    if (snprintf(current, sizeof(current), "%s", path) >= (int)sizeof(current)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    syseba_normalize_separators(current);
    length = strlen(current);
    while (length > 1 && current[length - 1] == SYSEBA_PATH_SEP) {
        current[--length] = '\0';
    }

    for (char *cursor = current + 1; *cursor != '\0'; cursor++) {
        if (*cursor != SYSEBA_PATH_SEP) {
            continue;
        }
#ifdef _WIN32
        if (cursor == current + 2 && current[1] == ':') {
            continue;
        }
#endif
        *cursor = '\0';
#ifdef _WIN32
        if (_mkdir(current) != 0 && errno != EEXIST) {
#else
        if (mkdir(current, (mode_t)mode) != 0 && errno != EEXIST) {
#endif
            *cursor = SYSEBA_PATH_SEP;
            return -1;
        }
        *cursor = SYSEBA_PATH_SEP;
    }
#ifdef _WIN32
    if (_mkdir(current) != 0 && errno != EEXIST) {
#else
    if (mkdir(current, (mode_t)mode) != 0 && errno != EEXIST) {
#endif
        return -1;
    }
    return 0;
}

int syseba_parent_dir(const char *path, char *output, size_t size)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }
#endif
    if (slash == NULL) {
        return snprintf(output, size, ".") < (int)size ? 0 : -1;
    }
    if (slash == path) {
        return snprintf(output, size, "%c", SYSEBA_PATH_SEP) < (int)size ? 0 : -1;
    }
    if ((size_t)(slash - path) + 1u > size) {
        return -1;
    }
    memcpy(output, path, (size_t)(slash - path));
    output[slash - path] = '\0';
    return 0;
}

const char *syseba_last_error_string(char *buffer, size_t size)
{
#ifdef _WIN32
    DWORD code = GetLastError();
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (FormatMessageA(flags, NULL, code, 0, buffer, (DWORD)size, NULL) == 0) {
        (void)snprintf(buffer, size, "Windows error %lu", (unsigned long)code);
    }
    return buffer;
#else
    (void)snprintf(buffer, size, "%s", strerror(errno));
    return buffer;
#endif
}

char *syseba_strdup(const char *value)
{
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, value, length + 1u);
    }
    return copy;
}

bool syseba_is_terminal(FILE *stream)
{
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

unsigned syseba_terminal_width(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return (unsigned)(info.srWindow.Right - info.srWindow.Left + 1);
    }
#else
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
#endif
    return 100;
}

unsigned syseba_terminal_height(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return (unsigned)(info.srWindow.Bottom - info.srWindow.Top + 1);
    }
#else
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) {
        return size.ws_row;
    }
#endif
    return 30;
}

void syseba_console_clear(void)
{
    fputs("\033[2J\033[H", stdout);
}
