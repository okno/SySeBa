#include "syseba_internal.h"

#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define SYSEBA_STAT_STRUCT struct _stat64
#define syseba_stat_fn _stat64
#define syseba_rmdir _rmdir
#define syseba_unlink _unlink
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#define SYSEBA_STAT_STRUCT struct stat
#define syseba_stat_fn lstat
#define syseba_rmdir rmdir
#define syseba_unlink unlink
#endif

bool syseba_fs_exists(const char *path)
{
    SYSEBA_STAT_STRUCT value;
    return syseba_stat_fn(path, &value) == 0;
}

bool syseba_fs_is_directory(const char *path)
{
    SYSEBA_STAT_STRUCT value;
    if (syseba_stat_fn(path, &value) != 0) {
        return false;
    }
#ifdef _WIN32
    return (value.st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(value.st_mode);
#endif
}

bool syseba_fs_is_regular(const char *path)
{
    SYSEBA_STAT_STRUCT value;
    if (syseba_stat_fn(path, &value) != 0) {
        return false;
    }
#ifdef _WIN32
    {
        DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
               (value.st_mode & _S_IFMT) == _S_IFREG;
    }
#else
    return S_ISREG(value.st_mode);
#endif
}

int syseba_open_regular_read(const char *path, FILE **file, uint64_t *size)
{
    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }
    *file = NULL;
#ifdef _WIN32
    {
        HANDLE handle;
        BY_HANDLE_FILE_INFORMATION information = {0};
        int descriptor;
        handle = CreateFileA(path,
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            errno = error == ERROR_FILE_NOT_FOUND ||
                            error == ERROR_PATH_NOT_FOUND
                        ? ENOENT
                        : EACCES;
            return -1;
        }
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            CloseHandle(handle);
            errno = EINVAL;
            return -1;
        }
        descriptor = _open_osfhandle((intptr_t)handle,
                                     _O_RDONLY | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(handle);
            return -1;
        }
        *file = _fdopen(descriptor, "rb");
        if (*file == NULL) {
            _close(descriptor);
            return -1;
        }
        if (size != NULL) {
            *size = ((uint64_t)information.nFileSizeHigh << 32u) |
                    information.nFileSizeLow;
        }
    }
#else
    {
        struct stat information;
        int descriptor = open(path,
                              O_RDONLY
#ifdef O_CLOEXEC
                                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                                  | O_NOFOLLOW
#endif
        );
        if (descriptor < 0) {
            return -1;
        }
        if (fstat(descriptor, &information) != 0 ||
            !S_ISREG(information.st_mode)) {
            close(descriptor);
            errno = EINVAL;
            return -1;
        }
        *file = fdopen(descriptor, "rb");
        if (*file == NULL) {
            close(descriptor);
            return -1;
        }
        if (size != NULL) {
            *size = (uint64_t)information.st_size;
        }
    }
#endif
    return 0;
}

int syseba_open_regular_append(const char *path,
                               FILE **file,
                               unsigned mode)
{
    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }
    *file = NULL;
#ifdef _WIN32
    {
        HANDLE handle;
        BY_HANDLE_FILE_INFORMATION information = {0};
        int descriptor;
        (void)mode;
        handle = CreateFileA(path,
                             FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             NULL,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            errno = EACCES;
            return -1;
        }
        descriptor = _open_osfhandle((intptr_t)handle,
                                     _O_WRONLY | _O_APPEND | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(handle);
            return -1;
        }
        *file = _fdopen(descriptor, "ab");
        if (*file == NULL) {
            _close(descriptor);
            return -1;
        }
    }
#else
    {
        struct stat information;
        int descriptor = open(path,
                              O_WRONLY | O_APPEND | O_CREAT
#ifdef O_CLOEXEC
                                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                                  | O_NOFOLLOW
#endif
                              ,
                              (mode_t)mode);
        if (descriptor < 0) {
            return -1;
        }
        if (fstat(descriptor, &information) != 0 ||
            !S_ISREG(information.st_mode)) {
            close(descriptor);
            errno = EINVAL;
            return -1;
        }
        *file = fdopen(descriptor, "ab");
        if (*file == NULL) {
            close(descriptor);
            return -1;
        }
    }
#endif
    return 0;
}

int syseba_open_exclusive_write(const char *path,
                                FILE **file,
                                unsigned mode)
{
    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }
    *file = NULL;
#ifdef _WIN32
    {
        HANDLE handle;
        int descriptor;
        (void)mode;
        handle = CreateFileA(path,
                             GENERIC_WRITE,
                             0,
                             NULL,
                             CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL |
                                 FILE_FLAG_WRITE_THROUGH,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            errno = GetLastError() == ERROR_FILE_EXISTS ||
                            GetLastError() == ERROR_ALREADY_EXISTS
                        ? EEXIST
                        : EACCES;
            return -1;
        }
        descriptor = _open_osfhandle((intptr_t)handle,
                                     _O_WRONLY | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(handle);
            return -1;
        }
        *file = _fdopen(descriptor, "wb");
        if (*file == NULL) {
            _close(descriptor);
            return -1;
        }
    }
#else
    {
        int descriptor = open(path,
                              O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                                  | O_NOFOLLOW
#endif
                              ,
                              (mode_t)mode);
        if (descriptor < 0) {
            return -1;
        }
        *file = fdopen(descriptor, "wb");
        if (*file == NULL) {
            close(descriptor);
            (void)unlink(path);
            return -1;
        }
    }
#endif
    return 0;
}

int syseba_fs_stat(const char *path,
                   uint64_t *size,
                   int64_t *mtime_ns,
                   bool *directory)
{
    SYSEBA_STAT_STRUCT value;
    if (syseba_stat_fn(path, &value) != 0) {
        return -1;
    }
#ifdef _WIN32
    {
        DWORD attributes = GetFileAttributesA(path);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            errno = ELOOP;
            return -1;
        }
        *directory = (value.st_mode & _S_IFMT) == _S_IFDIR;
        *mtime_ns = (int64_t)value.st_mtime * 1000000000LL;
    }
#else
    if (S_ISLNK(value.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    *directory = S_ISDIR(value.st_mode);
#if defined(__APPLE__)
    *mtime_ns = (int64_t)value.st_mtimespec.tv_sec * 1000000000LL +
                (int64_t)value.st_mtimespec.tv_nsec;
#else
    *mtime_ns = (int64_t)value.st_mtim.tv_sec * 1000000000LL +
                (int64_t)value.st_mtim.tv_nsec;
#endif
#endif
    *size = *directory ? 0 : (uint64_t)value.st_size;
    return 0;
}

static int syseba_prepare_destination(const char *destination)
{
    char parent[SYSEBA_PATH_MAX];
    if (syseba_parent_dir(destination, parent, sizeof(parent)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return syseba_mkdirs(parent, 0750);
}

#ifndef _WIN32
static int syseba_copy_file_once_posix(const char *source, const char *destination)
{
    int input = -1;
    int output = -1;
    struct stat source_stat;
    struct stat final_stat;
    unsigned char buffer[1024 * 1024];
    uint64_t copied = 0;
    int result = -1;

    input = open(source,
                 O_RDONLY
#ifdef O_CLOEXEC
                     | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                     | O_NOFOLLOW
#endif
    );
    if (input < 0 || fstat(input, &source_stat) != 0 ||
        !S_ISREG(source_stat.st_mode)) {
        goto cleanup;
    }
    if (syseba_prepare_destination(destination) != 0) {
        goto cleanup;
    }
    output = open(destination,
                  O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                      | O_NOFOLLOW
#endif
                  ,
                  source_stat.st_mode & 0777);
    if (output < 0) {
        goto cleanup;
    }
    for (;;) {
        ssize_t read_count = read(input, buffer, sizeof(buffer));
        size_t written = 0;
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0) {
            goto cleanup;
        }
        if (read_count == 0) {
            break;
        }
        while (written < (size_t)read_count) {
            ssize_t write_count = write(output,
                                        buffer + written,
                                        (size_t)read_count - written);
            if (write_count < 0 && errno == EINTR) {
                continue;
            }
            if (write_count <= 0) {
                goto cleanup;
            }
            written += (size_t)write_count;
            copied += (uint64_t)write_count;
        }
    }
    if (fstat(input, &final_stat) != 0 ||
        source_stat.st_dev != final_stat.st_dev ||
        source_stat.st_ino != final_stat.st_ino ||
        source_stat.st_size != final_stat.st_size ||
        copied != (uint64_t)source_stat.st_size) {
        errno = EAGAIN;
        goto cleanup;
    }
#if defined(__APPLE__)
    if (source_stat.st_mtimespec.tv_sec != final_stat.st_mtimespec.tv_sec ||
        source_stat.st_mtimespec.tv_nsec != final_stat.st_mtimespec.tv_nsec) {
        errno = EAGAIN;
        goto cleanup;
    }
    {
        struct timeval times[2];
        times[0].tv_sec = source_stat.st_atimespec.tv_sec;
        times[0].tv_usec = (suseconds_t)(source_stat.st_atimespec.tv_nsec / 1000);
        times[1].tv_sec = source_stat.st_mtimespec.tv_sec;
        times[1].tv_usec = (suseconds_t)(source_stat.st_mtimespec.tv_nsec / 1000);
        (void)futimes(output, times);
    }
#else
    if (source_stat.st_mtim.tv_sec != final_stat.st_mtim.tv_sec ||
        source_stat.st_mtim.tv_nsec != final_stat.st_mtim.tv_nsec) {
        errno = EAGAIN;
        goto cleanup;
    }
    {
        struct timespec times[2] = {source_stat.st_atim, source_stat.st_mtim};
        (void)futimens(output, times);
    }
#endif
    if (fchmod(output, source_stat.st_mode & 0777) != 0 || fsync(output) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (output >= 0) {
        close(output);
    }
    if (input >= 0) {
        close(input);
    }
    if (result != 0) {
        int saved = errno;
        (void)unlink(destination);
        errno = saved;
    }
    return result;
}
#else
static void syseba_windows_copy_error(DWORD error)
{
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            errno = EACCES;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            errno = EEXIST;
            break;
        default:
            errno = EIO;
            break;
    }
}

static int syseba_copy_file_once_windows(const char *source,
                                         const char *destination)
{
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION initial = {0};
    BY_HANDLE_FILE_INFORMATION final = {0};
    unsigned char buffer[1024 * 1024];
    uint64_t copied = 0;
    int result = -1;

    input = CreateFileA(source,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
                        NULL);
    if (input == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(input, &initial) ||
        (initial.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        syseba_windows_copy_error(GetLastError());
        goto cleanup;
    }
    output = CreateFileA(destination,
                         GENERIC_WRITE,
                         0,
                         NULL,
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         NULL);
    if (output == INVALID_HANDLE_VALUE) {
        syseba_windows_copy_error(GetLastError());
        goto cleanup;
    }
    for (;;) {
        DWORD read_count = 0;
        DWORD written = 0;
        if (!ReadFile(input, buffer, (DWORD)sizeof(buffer), &read_count, NULL)) {
            syseba_windows_copy_error(GetLastError());
            goto cleanup;
        }
        if (read_count == 0) {
            break;
        }
        while (written < read_count) {
            DWORD chunk = 0;
            if (!WriteFile(output,
                           buffer + written,
                           read_count - written,
                           &chunk,
                           NULL) ||
                chunk == 0) {
                syseba_windows_copy_error(GetLastError());
                goto cleanup;
            }
            written += chunk;
            copied += (uint64_t)chunk;
        }
    }
    if (!GetFileInformationByHandle(input, &final) ||
        initial.dwVolumeSerialNumber != final.dwVolumeSerialNumber ||
        initial.nFileIndexHigh != final.nFileIndexHigh ||
        initial.nFileIndexLow != final.nFileIndexLow ||
        initial.nFileSizeHigh != final.nFileSizeHigh ||
        initial.nFileSizeLow != final.nFileSizeLow ||
        initial.ftLastWriteTime.dwHighDateTime !=
            final.ftLastWriteTime.dwHighDateTime ||
        initial.ftLastWriteTime.dwLowDateTime !=
            final.ftLastWriteTime.dwLowDateTime ||
        copied != (((uint64_t)initial.nFileSizeHigh << 32u) |
                   initial.nFileSizeLow)) {
        errno = EAGAIN;
        goto cleanup;
    }
    if (!SetFileTime(output,
                     &initial.ftCreationTime,
                     &initial.ftLastAccessTime,
                     &initial.ftLastWriteTime) ||
        !FlushFileBuffers(output)) {
        syseba_windows_copy_error(GetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (output != INVALID_HANDLE_VALUE) {
        CloseHandle(output);
    }
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (result != 0) {
        int saved = errno;
        (void)DeleteFileA(destination);
        errno = saved;
    }
    return result;
}
#endif

#ifndef _WIN32
static int syseba_sync_parent_directory(const char *path)
{
    char parent[SYSEBA_PATH_MAX];
    int descriptor;
    int result;
    if (syseba_parent_dir(path, parent, sizeof(parent)) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(parent,
                      O_RDONLY
#ifdef O_CLOEXEC
                          | O_CLOEXEC
#endif
#ifdef O_DIRECTORY
                          | O_DIRECTORY
#endif
    );
    if (descriptor < 0) {
        return -1;
    }
    result = fsync(descriptor);
    if (result != 0 && (errno == EINVAL || errno == EROFS)) {
        result = 0;
    }
    close(descriptor);
    return result;
}
#endif

int syseba_atomic_replace(const char *temporary, const char *destination)
{
#ifdef _WIN32
    if (!MoveFileExA(temporary,
                     destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errno = EIO;
        return -1;
    }
    return 0;
#else
    if (rename(temporary, destination) != 0) {
        return -1;
    }
    return syseba_sync_parent_directory(destination);
#endif
}

int syseba_copy_file(const char *source,
                     const char *destination,
                     unsigned attempts)
{
    char temporary[SYSEBA_PATH_MAX];
    if (syseba_prepare_destination(destination) != 0) {
        return -1;
    }
    if (attempts == 0) {
        attempts = 1;
    }
    for (unsigned attempt = 0; attempt < attempts; attempt++) {
        int result;
        if (snprintf(temporary,
                     sizeof(temporary),
                     "%s.syseba-tmp.%" PRIu64 ".%" PRIu64 ".%u",
                     destination,
                     (uint64_t)syseba_process_id(),
                     syseba_monotonic_ns(),
                     attempt) >= (int)sizeof(temporary)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!syseba_fs_is_regular(source)) {
            errno = ENOENT;
            return -1;
        }
#ifdef _WIN32
        result = syseba_copy_file_once_windows(source, temporary);
        if (result == 0) {
            result = syseba_atomic_replace(temporary, destination);
        }
#else
        result = syseba_copy_file_once_posix(source, temporary);
        if (result == 0) {
            result = syseba_atomic_replace(temporary, destination);
        }
#endif
        if (result == 0) {
            return 0;
        }
        {
            int saved = errno;
            (void)syseba_unlink(temporary);
            errno = saved;
        }
        if (attempt + 1u < attempts) {
            syseba_sleep_ms(250u * (attempt + 1u));
        }
    }
    return -1;
}

typedef struct {
    int (*callback)(const char *, bool, void *);
    void *context;
} syseba_walk_context_t;

static int syseba_walk_internal(const char *root, syseba_walk_context_t *walk)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE handle;
    char pattern[SYSEBA_PATH_MAX];

    if (snprintf(pattern,
                 sizeof(pattern),
                 "%s\\*",
                 root) >= (int)sizeof(pattern)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        char path[SYSEBA_PATH_MAX];
        bool directory;
        if (strcmp(data.cFileName, ".") == 0 ||
            strcmp(data.cFileName, "..") == 0 ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        if (snprintf(path,
                     sizeof(path),
                     "%s\\%s",
                     root,
                     data.cFileName) >= (int)sizeof(path)) {
            FindClose(handle);
            errno = ENAMETOOLONG;
            return -1;
        }
        directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (walk->callback(path, directory, walk->context) != 0 ||
            (directory && syseba_walk_internal(path, walk) != 0)) {
            FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    return 0;
#else
    DIR *directory_stream = opendir(root);
    struct dirent *entry;
    if (directory_stream == NULL) {
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory_stream)) != NULL) {
        char path[SYSEBA_PATH_MAX];
        struct stat value;
        bool directory;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(path,
                     sizeof(path),
                     "%s/%s",
                     root,
                     entry->d_name) >= (int)sizeof(path)) {
            closedir(directory_stream);
            errno = ENAMETOOLONG;
            return -1;
        }
        if (lstat(path, &value) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            closedir(directory_stream);
            return -1;
        }
        if (S_ISLNK(value.st_mode) ||
            (!S_ISDIR(value.st_mode) && !S_ISREG(value.st_mode))) {
            continue;
        }
        directory = S_ISDIR(value.st_mode);
        if (walk->callback(path, directory, walk->context) != 0 ||
            (directory && syseba_walk_internal(path, walk) != 0)) {
            closedir(directory_stream);
            return -1;
        }
    }
    {
        int saved = errno;
        closedir(directory_stream);
        if (saved != 0) {
            errno = saved;
            return -1;
        }
    }
    return 0;
#endif
}

int syseba_walk_files(const char *root,
                      int (*callback)(const char *, bool, void *),
                      void *context)
{
    syseba_walk_context_t walk = {callback, context};
    if (!syseba_fs_is_directory(root)) {
        errno = ENOTDIR;
        return -1;
    }
    return syseba_walk_internal(root, &walk);
}

typedef struct {
    const char *source;
    const char *destination;
    bool overwrite;
} syseba_copy_tree_context_t;

static int syseba_copy_tree_entry(const char *path,
                                  bool directory,
                                  void *opaque)
{
    syseba_copy_tree_context_t *context =
        (syseba_copy_tree_context_t *)opaque;
    char relative[SYSEBA_PATH_MAX];
    char target[SYSEBA_PATH_MAX];

    if (syseba_relative_path(context->source,
                             path,
                             relative,
                             sizeof(relative)) != SYSEBA_OK ||
        syseba_safe_join(context->destination,
                         relative,
                         target,
                         sizeof(target)) != SYSEBA_OK) {
        errno = EINVAL;
        return -1;
    }
    if (directory) {
        return syseba_mkdirs(target, 0750);
    }
    if (!context->overwrite && syseba_fs_exists(target)) {
        errno = EEXIST;
        return -1;
    }
    return syseba_copy_file(path, target, 4);
}

int syseba_copy_tree(const char *source,
                     const char *destination,
                     bool overwrite)
{
    syseba_copy_tree_context_t context = {source, destination, overwrite};
    if (!syseba_fs_is_directory(source)) {
        errno = ENOTDIR;
        return -1;
    }
    if (syseba_fs_exists(destination) && !syseba_fs_is_directory(destination)) {
        errno = EEXIST;
        return -1;
    }
    if (syseba_mkdirs(destination, 0750) != 0) {
        return -1;
    }
    return syseba_walk_files(source, syseba_copy_tree_entry, &context);
}

typedef struct {
    char **paths;
    bool *directories;
    size_t count;
    size_t capacity;
} syseba_remove_context_t;

static int syseba_remove_collect(const char *path,
                                 bool directory,
                                 void *opaque)
{
    syseba_remove_context_t *context = (syseba_remove_context_t *)opaque;
    if (context->count == context->capacity) {
        size_t capacity = context->capacity == 0 ? 64 : context->capacity * 2;
        char **paths = (char **)realloc(context->paths,
                                       capacity * sizeof(*paths));
        bool *directories;
        if (paths == NULL) {
            return -1;
        }
        context->paths = paths;
        directories = (bool *)realloc(context->directories,
                                      capacity * sizeof(*directories));
        if (directories == NULL) {
            return -1;
        }
        context->directories = directories;
        context->capacity = capacity;
    }
    context->paths[context->count] = syseba_strdup(path);
    if (context->paths[context->count] == NULL) {
        return -1;
    }
    context->directories[context->count] = directory;
    context->count++;
    return 0;
}

int syseba_remove_path(const char *path)
{
    if (!syseba_fs_exists(path)) {
        return 0;
    }
    if (!syseba_fs_is_directory(path)) {
        return syseba_unlink(path);
    }
    {
        syseba_remove_context_t context = {0};
        int result = syseba_walk_files(path, syseba_remove_collect, &context);
        if (result == 0) {
            for (size_t index = context.count; index > 0; index--) {
                size_t item = index - 1;
                result = context.directories[item]
                             ? syseba_rmdir(context.paths[item])
                             : syseba_unlink(context.paths[item]);
                if (result != 0) {
                    break;
                }
            }
        }
        if (result == 0) {
            result = syseba_rmdir(path);
        }
        for (size_t index = 0; index < context.count; index++) {
            free(context.paths[index]);
        }
        free(context.paths);
        free(context.directories);
        return result;
    }
}

int syseba_move_path(const char *source, const char *destination)
{
    if (syseba_prepare_destination(destination) != 0) {
        return -1;
    }
    if (rename(source, destination) == 0) {
        return 0;
    }
#ifdef _WIN32
    if (GetLastError() != ERROR_NOT_SAME_DEVICE && errno != EXDEV) {
        return -1;
    }
#else
    if (errno != EXDEV) {
        return -1;
    }
#endif
    if (syseba_fs_is_directory(source)) {
        if (syseba_copy_tree(source, destination, true) != 0) {
            return -1;
        }
    } else if (syseba_copy_file(source, destination, 4) != 0) {
        return -1;
    }
    return syseba_remove_path(source);
}

static void syseba_compact_stamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm local = {0};
#ifdef _WIN32
    (void)localtime_s(&local, &now);
#else
    (void)localtime_r(&now, &local);
#endif
    (void)strftime(buffer, size, "%Y%m%d-%H%M%S", &local);
}

int syseba_unique_restore_path(const char *path, char *output, size_t size)
{
    char stamp[32];
    if (!syseba_fs_exists(path)) {
        return snprintf(output, size, "%s", path) < (int)size ? 0 : -1;
    }
    syseba_compact_stamp(stamp, sizeof(stamp));
    for (unsigned counter = 0; counter < 100000; counter++) {
        int length = counter == 0
                         ? snprintf(output, size, "%s.%s", path, stamp)
                         : snprintf(output,
                                    size,
                                    "%s.%s.%u",
                                    path,
                                    stamp,
                                    counter);
        if (length < 0 || length >= (int)size) {
            return -1;
        }
        if (!syseba_fs_exists(output)) {
            return 0;
        }
    }
    errno = EEXIST;
    return -1;
}

int syseba_unique_restored_path(const char *path, char *output, size_t size)
{
    const char *slash = strrchr(path, '/');
    const char *name;
    const char *dot;
    char parent[SYSEBA_PATH_MAX];
    char stem[SYSEBA_PATH_MAX];
    char extension[SYSEBA_PATH_MAX];
    char stamp[32];

#ifdef _WIN32
    {
        const char *backslash = strrchr(path, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) {
            slash = backslash;
        }
    }
#endif
    if (!syseba_fs_exists(path)) {
        return snprintf(output, size, "%s", path) < (int)size ? 0 : -1;
    }
    name = slash == NULL ? path : slash + 1;
    dot = strrchr(name, '.');
    if (dot == name) {
        dot = NULL;
    }
    if (syseba_parent_dir(path, parent, sizeof(parent)) != 0) {
        return -1;
    }
    {
        size_t stem_length = dot == NULL ? strlen(name) : (size_t)(dot - name);
        if (stem_length + 1 > sizeof(stem)) {
            return -1;
        }
        memcpy(stem, name, stem_length);
        stem[stem_length] = '\0';
        if (snprintf(extension,
                     sizeof(extension),
                     "%s",
                     dot == NULL ? "" : dot) >= (int)sizeof(extension)) {
            return -1;
        }
    }
    syseba_compact_stamp(stamp, sizeof(stamp));
    for (unsigned counter = 0; counter < 100000; counter++) {
        int length;
        if (counter == 0) {
            length = snprintf(output,
                              size,
                              "%s%c%s.restored-%s%s",
                              parent,
                              SYSEBA_PATH_SEP,
                              stem,
                              stamp,
                              extension);
        } else {
            length = snprintf(output,
                              size,
                              "%s%c%s.restored-%s-%u%s",
                              parent,
                              SYSEBA_PATH_SEP,
                              stem,
                              stamp,
                              counter,
                              extension);
        }
        if (length < 0 || length >= (int)size) {
            return -1;
        }
        if (!syseba_fs_exists(output)) {
            return 0;
        }
    }
    errno = EEXIST;
    return -1;
}

static bool syseba_contains_case_insensitive(const char *value,
                                             const char *search)
{
    size_t search_length = strlen(search);
    if (search_length == 0) {
        return true;
    }
    for (; *value != '\0'; value++) {
        size_t index = 0;
        while (index < search_length &&
               value[index] != '\0' &&
               tolower((unsigned char)value[index]) ==
                   tolower((unsigned char)search[index])) {
            index++;
        }
        if (index == search_length) {
            return true;
        }
    }
    return false;
}

static int syseba_restore_item_compare_common(const syseba_restore_item_t *first,
                                              const syseba_restore_item_t *second)
{
    if (first->is_directory != second->is_directory) {
        return first->is_directory ? -1 : 1;
    }
    return 0;
}

static int syseba_restore_compare_name_asc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    if (common != 0) {
        return common;
    }
#ifdef _WIN32
    return _stricmp(first->name, second->name);
#else
    return strcasecmp(first->name, second->name);
#endif
}

static int syseba_restore_compare_name_desc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    return common != 0 ? common : -syseba_restore_compare_name_asc(left, right);
}

static int syseba_restore_compare_mtime_asc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    if (common != 0) {
        return common;
    }
    return strcmp(first->mtime, second->mtime);
}

static int syseba_restore_compare_mtime_desc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    return common != 0 ? common : -strcmp(first->mtime, second->mtime);
}

static int syseba_restore_compare_size_asc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    if (common != 0) {
        return common;
    }
    if (first->size == second->size) {
        return syseba_restore_compare_name_asc(left, right);
    }
    return first->size < second->size ? -1 : 1;
}

static int syseba_restore_compare_size_desc(const void *left, const void *right)
{
    const syseba_restore_item_t *first = left;
    const syseba_restore_item_t *second = right;
    int common = syseba_restore_item_compare_common(first, second);
    if (common != 0) {
        return common;
    }
    if (first->size == second->size) {
        return syseba_restore_compare_name_asc(left, right);
    }
    return first->size > second->size ? -1 : 1;
}

static int syseba_restore_listing_append(syseba_restore_listing_t *listing,
                                         const syseba_restore_item_t *item)
{
    syseba_restore_item_t *items =
        (syseba_restore_item_t *)realloc(listing->items,
                                        (listing->count + 1u) *
                                            sizeof(*listing->items));
    if (items == NULL) {
        return -1;
    }
    listing->items = items;
    listing->items[listing->count++] = *item;
    return 0;
}

static int syseba_restore_read_directory(syseba_app_t *app,
                                         const char *target,
                                         const char *search,
                                         syseba_restore_listing_t *listing)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE handle;
    char pattern[SYSEBA_PATH_MAX];
    if (snprintf(pattern,
                 sizeof(pattern),
                 "%s\\*",
                 target) >= (int)sizeof(pattern)) {
        return -1;
    }
    handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        char path[SYSEBA_PATH_MAX];
        syseba_restore_item_t item = {0};
        uint64_t size;
        int64_t mtime_ns;
        bool directory;
        if (strcmp(data.cFileName, ".") == 0 ||
            strcmp(data.cFileName, "..") == 0 ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !syseba_contains_case_insensitive(data.cFileName, search)) {
            continue;
        }
        if (snprintf(path,
                     sizeof(path),
                     "%s\\%s",
                     target,
                     data.cFileName) >= (int)sizeof(path) ||
            syseba_fs_stat(path, &size, &mtime_ns, &directory) != 0) {
            continue;
        }
        (void)snprintf(item.name, sizeof(item.name), "%s", data.cFileName);
        (void)syseba_relative_path(app->config.restore,
                                   path,
                                   item.path,
                                   sizeof(item.path));
        item.size = size;
        item.is_directory = directory;
        syseba_iso_from_time((time_t)(mtime_ns / 1000000000LL),
                             item.mtime,
                             sizeof(item.mtime));
        {
            char destination[SYSEBA_PATH_MAX];
            item.destination_exists =
                syseba_safe_join(app->config.source,
                                 item.path,
                                 destination,
                                 sizeof(destination)) == SYSEBA_OK &&
                syseba_fs_exists(destination);
        }
        if (syseba_restore_listing_append(listing, &item) != 0) {
            FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    DIR *stream = opendir(target);
    struct dirent *entry;
    if (stream == NULL) {
        return -1;
    }
    errno = 0;
    while ((entry = readdir(stream)) != NULL) {
        char path[SYSEBA_PATH_MAX];
        syseba_restore_item_t item = {0};
        uint64_t size;
        int64_t mtime_ns;
        bool directory;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            !syseba_contains_case_insensitive(entry->d_name, search)) {
            continue;
        }
        if (snprintf(path,
                     sizeof(path),
                     "%s/%s",
                     target,
                     entry->d_name) >= (int)sizeof(path) ||
            syseba_fs_stat(path, &size, &mtime_ns, &directory) != 0) {
            continue;
        }
        (void)snprintf(item.name, sizeof(item.name), "%s", entry->d_name);
        (void)syseba_relative_path(app->config.restore,
                                   path,
                                   item.path,
                                   sizeof(item.path));
        item.size = size;
        item.is_directory = directory;
        syseba_iso_from_time((time_t)(mtime_ns / 1000000000LL),
                             item.mtime,
                             sizeof(item.mtime));
        {
            char destination[SYSEBA_PATH_MAX];
            item.destination_exists =
                syseba_safe_join(app->config.source,
                                 item.path,
                                 destination,
                                 sizeof(destination)) == SYSEBA_OK &&
                syseba_fs_exists(destination);
        }
        if (syseba_restore_listing_append(listing, &item) != 0) {
            closedir(stream);
            return -1;
        }
    }
    {
        int saved = errno;
        closedir(stream);
        if (saved != 0) {
            errno = saved;
            return -1;
        }
    }
#endif
    return 0;
}

int syseba_restore_list(syseba_app_t *app,
                        const char *relative,
                        const char *search,
                        unsigned page,
                        unsigned page_size,
                        const char *sort,
                        const char *direction,
                        syseba_restore_listing_t *listing,
                        char *error,
                        size_t error_size)
{
    char target[SYSEBA_PATH_MAX];
    int (*comparator)(const void *, const void *) = syseba_restore_compare_name_asc;

    memset(listing, 0, sizeof(*listing));
    if (page == 0) {
        page = 1;
    }
    if (page_size == 0) {
        page_size = SYSEBA_DEFAULT_RESTORE_PAGE;
    }
    if (page_size > SYSEBA_MAX_RESTORE_PAGE) {
        page_size = SYSEBA_MAX_RESTORE_PAGE;
    }
    if (syseba_safe_join(app->config.restore,
                         relative,
                         target,
                         sizeof(target)) != SYSEBA_OK) {
        (void)snprintf(error, error_size, "Invalid restore path");
        return SYSEBA_ERR_INVALID;
    }
    if (!syseba_fs_exists(target)) {
        (void)snprintf(error, error_size, "Restore path not found");
        return SYSEBA_ERR_NOT_FOUND;
    }
    (void)syseba_relative_path(app->config.restore,
                               target,
                               listing->path,
                               sizeof(listing->path));
    if (!syseba_fs_is_directory(target)) {
        syseba_restore_item_t item = {0};
        uint64_t size;
        int64_t mtime_ns;
        bool directory;
        const char *name = strrchr(target, SYSEBA_PATH_SEP);
        if (syseba_fs_stat(target, &size, &mtime_ns, &directory) != 0) {
            return SYSEBA_ERR;
        }
        listing->is_file = true;
        listing->page = 1;
        listing->pages = 1;
        listing->page_size = 1;
        listing->total = 1;
        (void)snprintf(item.name,
                       sizeof(item.name),
                       "%s",
                       name == NULL ? target : name + 1);
        (void)snprintf(item.path, sizeof(item.path), "%s", listing->path);
        item.size = size;
        item.is_directory = false;
        syseba_iso_from_time((time_t)(mtime_ns / 1000000000LL),
                             item.mtime,
                             sizeof(item.mtime));
        if (syseba_restore_listing_append(listing, &item) != 0) {
            return SYSEBA_ERR;
        }
        return SYSEBA_OK;
    }
    if (syseba_restore_read_directory(app,
                                      target,
                                      search == NULL ? "" : search,
                                      listing) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to read restore directory: %s",
                       strerror(errno));
        syseba_restore_listing_free(listing);
        return SYSEBA_ERR;
    }

    if (strcmp(sort, "mtime") == 0) {
        comparator = strcmp(direction, "desc") == 0
                         ? syseba_restore_compare_mtime_desc
                         : syseba_restore_compare_mtime_asc;
    } else if (strcmp(sort, "size") == 0) {
        comparator = strcmp(direction, "desc") == 0
                         ? syseba_restore_compare_size_desc
                         : syseba_restore_compare_size_asc;
    } else if (strcmp(direction, "desc") == 0) {
        comparator = syseba_restore_compare_name_desc;
    }
    qsort(listing->items, listing->count, sizeof(*listing->items), comparator);

    listing->total = listing->count;
    listing->pages = (unsigned)((listing->total + page_size - 1u) / page_size);
    if (listing->pages == 0) {
        listing->pages = 1;
    }
    if (page > listing->pages) {
        page = listing->pages;
    }
    listing->page = page;
    listing->page_size = page_size;
    listing->has_previous = page > 1;
    listing->has_next = page < listing->pages;

    {
        size_t offset = ((size_t)page - 1u) * page_size;
        size_t available = listing->count > offset ? listing->count - offset : 0;
        size_t selected = available > page_size ? page_size : available;
        if (selected > 0 && offset > 0) {
            memmove(listing->items,
                    listing->items + offset,
                    selected * sizeof(*listing->items));
        }
        listing->count = selected;
    }
    return SYSEBA_OK;
}

void syseba_restore_listing_free(syseba_restore_listing_t *listing)
{
    free(listing->items);
    memset(listing, 0, sizeof(*listing));
}

int syseba_restore_item(syseba_app_t *app,
                        const char *relative,
                        syseba_restore_strategy_t strategy,
                        char *destination,
                        size_t destination_size,
                        char *error,
                        size_t error_size)
{
    char source[SYSEBA_PATH_MAX];
    char target[SYSEBA_PATH_MAX];
    bool source_directory;
    bool destination_exists;

    if (relative == NULL || *relative == '\0' ||
        syseba_safe_join(app->config.restore,
                         relative,
                         source,
                         sizeof(source)) != SYSEBA_OK ||
        syseba_safe_join(app->config.source,
                         relative,
                         target,
                         sizeof(target)) != SYSEBA_OK) {
        (void)snprintf(error, error_size, "Invalid restore item path");
        return SYSEBA_ERR_INVALID;
    }
    if (!syseba_fs_exists(source)) {
        (void)snprintf(error, error_size, "Restore item not found");
        return SYSEBA_ERR_NOT_FOUND;
    }
    source_directory = syseba_fs_is_directory(source);
    destination_exists = syseba_fs_exists(target);

    if (destination_exists && strategy == SYSEBA_RESTORE_FAIL) {
        (void)snprintf(error,
                       error_size,
                       "Destination already exists; choose rename or overwrite");
        return SYSEBA_ERR_EXISTS;
    }
    if (destination_exists && strategy == SYSEBA_RESTORE_RENAME) {
        if (syseba_unique_restored_path(target,
                                         target,
                                         sizeof(target)) != 0) {
            (void)snprintf(error, error_size, "Unable to create a unique name");
            return SYSEBA_ERR;
        }
        destination_exists = false;
    }
    if (destination_exists &&
        source_directory != syseba_fs_is_directory(target)) {
        (void)snprintf(error,
                       error_size,
                       "Destination type differs from restore item; choose rename");
        return SYSEBA_ERR_EXISTS;
    }

    if (source_directory) {
        if (syseba_copy_tree(source,
                             target,
                             strategy == SYSEBA_RESTORE_OVERWRITE) != 0) {
            (void)snprintf(error,
                           error_size,
                           "Unable to restore directory: %s",
                           strerror(errno));
            return SYSEBA_ERR;
        }
    } else if (syseba_copy_file(source, target, 4) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to restore file: %s",
                       strerror(errno));
        return SYSEBA_ERR;
    }

    syseba_mutex_lock(&app->state_mutex);
    app->stats.restored++;
    syseba_mutex_unlock(&app->state_mutex);
    syseba_log_emit(app,
                    "INFO",
                    "RESTORE",
                    source,
                    target,
                    "",
                    "%s: %s -> %s",
                    syseba_tr(app->language,
                              "Ripristinato dall'area restore",
                              "Restored from restore area"),
                    source,
                    target);
    if (snprintf(destination, destination_size, "%s", target) >=
        (int)destination_size) {
        return SYSEBA_ERR;
    }
    return SYSEBA_OK;
}
