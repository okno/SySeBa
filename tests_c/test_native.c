#include "syseba_internal.h"

#include <stdarg.h>

#ifdef _WIN32
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

static int failures = 0;

#define TEST_ASSERT(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s\n",                                           \
                    __FILE__,                                                     \
                    __LINE__,                                                     \
                    #condition);                                                  \
            failures++;                                                           \
        }                                                                         \
    } while (0)

void syseba_log_emit(syseba_app_t *app,
                     const char *level,
                     const char *operation,
                     const char *source,
                     const char *target,
                     const char *additional,
                     const char *format,
                     ...)
{
    (void)app;
    (void)level;
    (void)operation;
    (void)source;
    (void)target;
    (void)additional;
    (void)format;
}

static void test_paths(void)
{
    char root[SYSEBA_PATH_MAX];
    char child[SYSEBA_PATH_MAX];
    char escaped[SYSEBA_PATH_MAX];
    char temporary[SYSEBA_PATH_MAX];
    (void)snprintf(temporary,
                   sizeof(temporary),
#ifdef _WIN32
                   "%s\\syseba-native-test-%" PRIu64,
                   getenv("TEMP") == NULL ? "." : getenv("TEMP"),
#else
                   "/tmp/syseba-native-test-%" PRIu64,
#endif
                   syseba_monotonic_ns());
    TEST_ASSERT(syseba_mkdirs(temporary, 0700) == 0);
    TEST_ASSERT(syseba_path_real(temporary, root, sizeof(root)) == 0);
    TEST_ASSERT(syseba_safe_join(root,
                                 "folder/file.txt",
                                 child,
                                 sizeof(child)) == SYSEBA_OK);
    TEST_ASSERT(syseba_path_is_within(root, child));
    TEST_ASSERT(syseba_safe_join(root,
                                 "../../outside",
                                 escaped,
                                 sizeof(escaped)) == SYSEBA_ERR_INVALID);
    TEST_ASSERT(test_rmdir(root) == 0);
}

static void test_queue(void)
{
    syseba_queue_t queue;
    int *value = (int *)malloc(sizeof(*value));
    TEST_ASSERT(value != NULL);
    if (value == NULL) {
        return;
    }
    *value = 42;
    TEST_ASSERT(syseba_queue_init(&queue) == 0);
    TEST_ASSERT(syseba_queue_push(&queue, value) == 0);
    TEST_ASSERT(syseba_queue_size(&queue) == 1);
    {
        int *received = (int *)syseba_queue_pop(&queue, 10);
        TEST_ASSERT(received != NULL);
        if (received != NULL) {
            TEST_ASSERT(*received == 42);
            free(received);
        }
    }
    syseba_queue_task_done(&queue);
    TEST_ASSERT(syseba_queue_wait_empty(&queue, 10));
    TEST_ASSERT(!syseba_queue_is_closed(&queue));
    syseba_queue_close(&queue);
    TEST_ASSERT(syseba_queue_is_closed(&queue));
    syseba_queue_destroy(&queue, free);
}

static void test_config_normalization(void)
{
    syseba_config_t config;
    syseba_config_t loaded;
    char root[SYSEBA_PATH_MAX];
    char source[SYSEBA_PATH_MAX];
    char backup[SYSEBA_PATH_MAX];
    char restore[SYSEBA_PATH_MAX];
    char error[512] = {0};

    (void)snprintf(root,
                   sizeof(root),
#ifdef _WIN32
                   "%s\\syseba-config-test-%" PRIu64,
                   getenv("TEMP") == NULL ? "." : getenv("TEMP"),
#else
                   "/tmp/syseba-config-test-%" PRIu64,
#endif
                   syseba_monotonic_ns());
    (void)snprintf(source,
                   sizeof(source),
                   "%s%csource",
                   root,
                   SYSEBA_PATH_SEP);
    (void)snprintf(backup,
                   sizeof(backup),
                   "%s%cbackup",
                   root,
                   SYSEBA_PATH_SEP);
    (void)snprintf(restore,
                   sizeof(restore),
                   "%s%crestore",
                   root,
                   SYSEBA_PATH_SEP);
    TEST_ASSERT(syseba_mkdirs(source, 0700) == 0);
    TEST_ASSERT(syseba_mkdirs(backup, 0700) == 0);
    TEST_ASSERT(syseba_mkdirs(restore, 0700) == 0);

    memset(&config, 0, sizeof(config));
    (void)snprintf(config.config_path,
                   sizeof(config.config_path),
                   "%s%csyseba.conf",
                   root,
                   SYSEBA_PATH_SEP);
    (void)snprintf(config.source, sizeof(config.source), "source");
    (void)snprintf(config.backup, sizeof(config.backup), "backup");
    (void)snprintf(config.restore, sizeof(config.restore), "restore");
    (void)snprintf(config.log_file, sizeof(config.log_file), "syseba.log");
    config.threads = 3;
    TEST_ASSERT(syseba_config_normalize(&config,
                                        error,
                                        sizeof(error)) == SYSEBA_OK);
    TEST_ASSERT(strcmp(config.source, source) == 0);
    TEST_ASSERT(syseba_config_save(&config,
                                   error,
                                   sizeof(error)) == SYSEBA_OK);
    TEST_ASSERT(syseba_config_load(config.config_path,
                                   &loaded,
                                   error,
                                   sizeof(error)) == SYSEBA_OK);
    TEST_ASSERT(strcmp(loaded.backup, backup) == 0);
    TEST_ASSERT(loaded.threads == 3);

    (void)snprintf(config.source,
                   sizeof(config.source),
                   "source\nthreads = 64");
    TEST_ASSERT(syseba_config_normalize(&config,
                                        error,
                                        sizeof(error)) ==
                SYSEBA_ERR_INVALID);

    (void)remove(config.config_path);
    TEST_ASSERT(test_rmdir(source) == 0);
    TEST_ASSERT(test_rmdir(backup) == 0);
    TEST_ASSERT(test_rmdir(restore) == 0);
    TEST_ASSERT(test_rmdir(root) == 0);
}

static void test_atomic_copy(void)
{
    char root[SYSEBA_PATH_MAX];
    char source[SYSEBA_PATH_MAX];
    char destination[SYSEBA_PATH_MAX];
    char outside[SYSEBA_PATH_MAX];
    FILE *file = NULL;
    char content[64] = {0};
    uint64_t size = 0;

    (void)snprintf(root,
                   sizeof(root),
#ifdef _WIN32
                   "%s\\syseba-copy-test-%" PRIu64,
                   getenv("TEMP") == NULL ? "." : getenv("TEMP"),
#else
                   "/tmp/syseba-copy-test-%" PRIu64,
#endif
                   syseba_monotonic_ns());
    (void)snprintf(source,
                   sizeof(source),
                   "%s%csource.txt",
                   root,
                   SYSEBA_PATH_SEP);
    (void)snprintf(destination,
                   sizeof(destination),
                   "%s%cdestination.txt",
                   root,
                   SYSEBA_PATH_SEP);
    (void)snprintf(outside,
                   sizeof(outside),
                   "%s%coutside.txt",
                   root,
                   SYSEBA_PATH_SEP);
    TEST_ASSERT(syseba_mkdirs(root, 0700) == 0);

    file = fopen(source, "wb");
    TEST_ASSERT(file != NULL);
    if (file != NULL) {
        TEST_ASSERT(fputs("atomic-content\n", file) >= 0);
        TEST_ASSERT(fclose(file) == 0);
    }
    TEST_ASSERT(syseba_copy_file(source, destination, 2) == 0);
    TEST_ASSERT(syseba_open_regular_read(destination, &file, &size) == 0);
    TEST_ASSERT(size == strlen("atomic-content\n"));
    if (file != NULL) {
        TEST_ASSERT(fread(content, 1, sizeof(content) - 1u, file) ==
                    strlen("atomic-content\n"));
        TEST_ASSERT(fclose(file) == 0);
        file = NULL;
    }
    TEST_ASSERT(strcmp(content, "atomic-content\n") == 0);

#ifndef _WIN32
    file = fopen(outside, "wb");
    TEST_ASSERT(file != NULL);
    if (file != NULL) {
        TEST_ASSERT(fputs("sentinel\n", file) >= 0);
        TEST_ASSERT(fclose(file) == 0);
    }
    TEST_ASSERT(remove(destination) == 0);
    TEST_ASSERT(symlink(outside, destination) == 0);
    TEST_ASSERT(syseba_copy_file(source, destination, 2) == 0);
    TEST_ASSERT(syseba_fs_is_regular(destination));
    memset(content, 0, sizeof(content));
    TEST_ASSERT(syseba_open_regular_read(outside, &file, &size) == 0);
    if (file != NULL) {
        TEST_ASSERT(fread(content, 1, sizeof(content) - 1u, file) ==
                    strlen("sentinel\n"));
        TEST_ASSERT(fclose(file) == 0);
        file = NULL;
    }
    TEST_ASSERT(strcmp(content, "sentinel\n") == 0);
#endif

    (void)remove(destination);
    (void)remove(outside);
    (void)remove(source);
    TEST_ASSERT(test_rmdir(root) == 0);
}

static void test_version(void)
{
    TEST_ASSERT(strcmp(syseba_version(), SYSEBA_VERSION) == 0);
}

int main(void)
{
    test_version();
    test_paths();
    test_queue();
    test_config_normalization();
    test_atomic_copy();
    if (failures != 0) {
        fprintf(stderr, "%d native test(s) failed\n", failures);
        return 1;
    }
    puts("native unit tests: OK");
    return 0;
}
