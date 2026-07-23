#include "syseba_internal.h"

#ifdef __linux__

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

typedef struct {
    int descriptor;
    char path[SYSEBA_PATH_MAX];
} syseba_linux_watch_t;

typedef struct {
    int inotify_fd;
    syseba_linux_watch_t *watches;
    size_t count;
    size_t capacity;
} syseba_linux_watcher_t;

static syseba_linux_watch_t *syseba_linux_watch_find(
    syseba_linux_watcher_t *watcher,
    int descriptor)
{
    for (size_t index = 0; index < watcher->count; index++) {
        if (watcher->watches[index].descriptor == descriptor) {
            return &watcher->watches[index];
        }
    }
    return NULL;
}

static int syseba_linux_watch_add(syseba_linux_watcher_t *watcher,
                                  const char *path)
{
    uint32_t mask = IN_CREATE | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE |
                    IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                    IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW;
    int descriptor = inotify_add_watch(watcher->inotify_fd, path, mask);
    if (descriptor < 0) {
        return -1;
    }
    for (size_t index = 0; index < watcher->count; index++) {
        if (watcher->watches[index].descriptor == descriptor) {
            (void)snprintf(watcher->watches[index].path,
                           sizeof(watcher->watches[index].path),
                           "%s",
                           path);
            return 0;
        }
    }
    if (watcher->count == watcher->capacity) {
        size_t capacity = watcher->capacity == 0 ? 64 : watcher->capacity * 2;
        syseba_linux_watch_t *items = (syseba_linux_watch_t *)realloc(
            watcher->watches,
            capacity * sizeof(*items));
        if (items == NULL) {
            (void)inotify_rm_watch(watcher->inotify_fd, descriptor);
            return -1;
        }
        watcher->watches = items;
        watcher->capacity = capacity;
    }
    watcher->watches[watcher->count].descriptor = descriptor;
    (void)snprintf(watcher->watches[watcher->count].path,
                   sizeof(watcher->watches[watcher->count].path),
                   "%s",
                   path);
    watcher->count++;
    return 0;
}

static void syseba_linux_watch_remove(syseba_linux_watcher_t *watcher,
                                      int descriptor)
{
    for (size_t index = 0; index < watcher->count; index++) {
        if (watcher->watches[index].descriptor == descriptor) {
            watcher->watches[index] = watcher->watches[watcher->count - 1u];
            watcher->count--;
            return;
        }
    }
}

static int syseba_linux_add_directory(const char *path,
                                      bool directory,
                                      void *opaque)
{
    if (!directory) {
        return 0;
    }
    return syseba_linux_watch_add((syseba_linux_watcher_t *)opaque, path);
}

static int syseba_linux_watch_initialize(syseba_linux_watcher_t *watcher,
                                         const char *root)
{
    memset(watcher, 0, sizeof(*watcher));
    watcher->inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watcher->inotify_fd < 0 ||
        syseba_linux_watch_add(watcher, root) != 0 ||
        syseba_walk_files(root, syseba_linux_add_directory, watcher) != 0) {
        if (watcher->inotify_fd >= 0) {
            close(watcher->inotify_fd);
        }
        free(watcher->watches);
        memset(watcher, 0, sizeof(*watcher));
        watcher->inotify_fd = -1;
        return -1;
    }
    return 0;
}

static void syseba_linux_process_event(syseba_app_t *app,
                                       syseba_linux_watcher_t *watcher,
                                       const struct inotify_event *event)
{
    syseba_linux_watch_t *watch =
        syseba_linux_watch_find(watcher, event->wd);
    char path[SYSEBA_PATH_MAX];
    bool directory = (event->mask & IN_ISDIR) != 0;

    if ((event->mask & IN_Q_OVERFLOW) != 0) {
        syseba_log_emit(app,
                        "WARNING",
                        "WATCH",
                        app->config.source,
                        "",
                        "IN_Q_OVERFLOW",
                        "%s",
                        syseba_tr(app->language,
                                  "Coda inotify satura; avvio riconciliazione",
                                  "inotify queue overflow; reconciling"));
        (void)syseba_app_initial_sync(app);
        return;
    }
    if (watch == NULL) {
        return;
    }
    if (event->len > 0 && event->name[0] != '\0') {
        if (snprintf(path,
                     sizeof(path),
                     "%s/%s",
                     watch->path,
                     event->name) >= (int)sizeof(path)) {
            return;
        }
    } else {
        (void)snprintf(path, sizeof(path), "%s", watch->path);
    }
    if ((event->mask & IN_IGNORED) != 0) {
        syseba_linux_watch_remove(watcher, event->wd);
        return;
    }
    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        syseba_linux_watch_remove(watcher, event->wd);
        return;
    }
    if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
        if (directory) {
            (void)syseba_linux_watch_add(watcher, path);
        }
        (void)syseba_app_enqueue(app,
                                 SYSEBA_EVENT_CREATE,
                                 path,
                                 directory);
    }
    if ((event->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0 && !directory) {
        (void)syseba_app_enqueue(app,
                                 SYSEBA_EVENT_MODIFY,
                                 path,
                                 false);
    }
    if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
        (void)syseba_app_enqueue(app,
                                 SYSEBA_EVENT_DELETE,
                                 path,
                                 directory);
    }
}

void *syseba_watcher_native_main(void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    syseba_linux_watcher_t watcher;
    unsigned char buffer[256 * 1024]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    if (syseba_linux_watch_initialize(&watcher, app->config.source) != 0) {
        syseba_log_emit(app,
                        "WARNING",
                        "WATCH",
                        app->config.source,
                        "",
                        strerror(errno),
                        "%s; %s",
                        syseba_tr(app->language,
                                  "inotify non disponibile",
                                  "inotify unavailable"),
                        syseba_tr(app->language,
                                  "uso il polling",
                                  "using polling"));
        return syseba_watcher_poll_main(app);
    }
    syseba_log_emit(app,
                    "INFO",
                    "WATCH",
                    app->config.source,
                    "",
                    "inotify",
                    "%s (%zu %s)",
                    syseba_tr(app->language,
                              "Watcher inotify attivo",
                              "inotify watcher active"),
                    watcher.count,
                    syseba_tr(app->language, "directory", "directories"));
    while (!syseba_app_is_stopping(app)) {
        struct pollfd poll_fd = {watcher.inotify_fd, POLLIN, 0};
        int poll_result = poll(&poll_fd, 1, 500);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0) {
            break;
        }
        if (poll_result == 0 || (poll_fd.revents & POLLIN) == 0) {
            continue;
        }
        for (;;) {
            ssize_t count = read(watcher.inotify_fd, buffer, sizeof(buffer));
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                break;
            }
            if (count <= 0) {
                break;
            }
            for (char *cursor = (char *)buffer;
                 cursor < (char *)buffer + count;) {
                const struct inotify_event *event =
                    (const struct inotify_event *)cursor;
                syseba_linux_process_event(app, &watcher, event);
                cursor += sizeof(*event) + event->len;
            }
        }
    }
    close(watcher.inotify_fd);
    free(watcher.watches);
    return NULL;
}

#endif
