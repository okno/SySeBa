#include "syseba_internal.h"

int syseba_watcher_start(syseba_app_t *app)
{
    const char *mode = getenv("SYSEBA_WATCHER");
    syseba_thread_fn watcher = syseba_watcher_poll_main;

#if defined(SYSEBA_ENABLE_NATIVE_WATCHER) && (defined(__linux__) || defined(_WIN32))
    if (mode == NULL || strcmp(mode, "poll") != 0) {
        watcher = syseba_watcher_native_main;
    }
#else
    (void)mode;
#endif
    if (syseba_thread_create(&app->watcher_thread, watcher, app) != 0) {
        return -1;
    }
    app->watcher_thread_started = true;
    return 0;
}

void syseba_watcher_stop(syseba_app_t *app)
{
    if (!app->watcher_thread_started) {
        return;
    }
    syseba_app_set_stopping(app);
    (void)syseba_thread_join(app->watcher_thread);
    app->watcher_thread_started = false;
}

#if !defined(__linux__) && !defined(_WIN32)
void *syseba_watcher_native_main(void *context)
{
    return syseba_watcher_poll_main(context);
}
#endif
