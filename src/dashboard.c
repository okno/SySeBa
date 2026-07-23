#include "syseba_internal.h"

static const char *syseba_color(const char *name)
{
    if (strcmp(name, "green") == 0) {
        return "\033[32m";
    }
    if (strcmp(name, "yellow") == 0) {
        return "\033[33m";
    }
    if (strcmp(name, "red") == 0) {
        return "\033[31m";
    }
    if (strcmp(name, "cyan") == 0) {
        return "\033[36m";
    }
    if (strcmp(name, "bold") == 0) {
        return "\033[1m";
    }
    if (strcmp(name, "dim") == 0) {
        return "\033[2m";
    }
    return "\033[0m";
}

static void syseba_dashboard_rule(unsigned width, char character)
{
    for (unsigned index = 0; index < width; index++) {
        fputc(character, stdout);
    }
    fputc('\n', stdout);
}

static void syseba_dashboard_bar(double percent,
                                 unsigned width,
                                 char *output,
                                 size_t output_size)
{
    unsigned filled;
    if (percent < 0.0) {
        percent = 0.0;
    }
    if (percent > 100.0) {
        percent = 100.0;
    }
    if (width + 1u > output_size) {
        width = (unsigned)output_size - 1u;
    }
    filled = (unsigned)((percent / 100.0) * (double)width);
    for (unsigned index = 0; index < width; index++) {
        output[index] = index < filled ? '#' : '-';
    }
    output[width] = '\0';
}

static void syseba_dashboard_disk(const char *label,
                                  const char *path,
                                  unsigned width)
{
    syseba_disk_usage_t usage;
    char bar[64];
    unsigned bar_width = width > 96 ? 36 : (width > 72 ? 24 : 14);
    const char *tone;

    if (syseba_disk_usage(path, &usage) != 0) {
        printf(" %-9s %-*.*s  %sNOT FOUND%s\n",
               label,
               (int)(width > 31 ? width - 31 : 20),
               (int)(width > 31 ? width - 31 : 20),
               path,
               syseba_color("red"),
               syseba_color("reset"));
        return;
    }
    syseba_dashboard_bar(usage.used_percent,
                         bar_width,
                         bar,
                         sizeof(bar));
    tone = usage.used_percent >= 90.0
               ? syseba_color("red")
               : (usage.used_percent >= 75.0
                      ? syseba_color("yellow")
                      : syseba_color("green"));
    printf(" %-9s %-*.*s  %s[%s]%s %6.2f%%\n",
           label,
           (int)(width > bar_width + 25 ? width - bar_width - 25 : 18),
           (int)(width > bar_width + 25 ? width - bar_width - 25 : 18),
           path,
           tone,
           bar,
           syseba_color("reset"),
           usage.used_percent);
}

static void syseba_dashboard_uptime(syseba_app_t *app,
                                    char *output,
                                    size_t output_size)
{
    uint64_t seconds = (syseba_monotonic_ns() - app->started_ns) /
                       1000000000ULL;
    uint64_t days = seconds / 86400u;
    uint64_t hours = (seconds % 86400u) / 3600u;
    uint64_t minutes = (seconds % 3600u) / 60u;
    uint64_t remaining = seconds % 60u;
    if (days > 0) {
        (void)snprintf(output,
                       output_size,
                       "%" PRIu64 "d %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                       days,
                       hours,
                       minutes,
                       remaining);
    } else {
        (void)snprintf(output,
                       output_size,
                       "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                       hours,
                       minutes,
                       remaining);
    }
}

static void syseba_dashboard_render(syseba_app_t *app)
{
    unsigned width = syseba_terminal_width();
    unsigned height = syseba_terminal_height();
    syseba_stats_t stats;
    double cpu = -1.0;
    double memory = -1.0;
    unsigned threads = 0;
    char uptime[64];
    char now[32];
    char cpu_text[32];
    char memory_text[32];
    double initial_percent = -1.0;
    bool compact;

    if (width < 64) {
        width = 64;
    }
    if (width > 140) {
        width = 140;
    }
    compact = height < 27 || width < 80;
    syseba_mutex_lock(&app->state_mutex);
    stats = app->stats;
    syseba_mutex_unlock(&app->state_mutex);
    (void)syseba_process_metrics(&cpu, &memory, &threads);
    if (cpu < 0.0) {
        (void)snprintf(cpu_text, sizeof(cpu_text), "n/a");
    } else {
        (void)snprintf(cpu_text, sizeof(cpu_text), "%.2f%%", cpu);
    }
    if (memory < 0.0) {
        (void)snprintf(memory_text, sizeof(memory_text), "n/a");
    } else {
        (void)snprintf(memory_text, sizeof(memory_text), "%.2f MB", memory);
    }
    syseba_dashboard_uptime(app, uptime, sizeof(uptime));
    syseba_timestamp_now(now, sizeof(now));
    if (stats.initial_total > 0) {
        initial_percent = (double)stats.initial_done * 100.0 /
                          (double)stats.initial_total;
    } else if (strncmp(app->initial_state, "completed", 9) == 0) {
        initial_percent = 100.0;
    }

    syseba_console_clear();
    printf("%s", syseba_color("bold"));
    syseba_dashboard_rule(width, '=');
    printf(" SySeBa %-*s %sRUNNING%s  PID %" PRIu64 "\n",
           (int)(width > 43 ? width - 43 : 1),
           "The Syncro Service Backup",
           syseba_color("green"),
           syseba_color("reset"),
           (uint64_t)syseba_process_id());
    syseba_dashboard_rule(width, '=');
    printf("%s", syseba_color("reset"));

    printf(" %-19s %-18s  %-8s %s\n",
           now,
           uptime,
           "Version",
           SYSEBA_VERSION);
    if (!compact) {
        printf(" %-9s %-*.*s\n",
               syseba_tr(app->language, "Sorgente", "Source"),
               (int)(width - 12),
               (int)(width - 12),
               app->config.source);
    }
    fputc('\n', stdout);

    syseba_dashboard_disk(syseba_tr(app->language, "Sorgente", "Source"),
                          app->config.source,
                          width);
    syseba_dashboard_disk("Backup",
                          app->config.backup,
                          width);
    syseba_dashboard_disk("Restore",
                          app->config.restore,
                          width);
    fputc('\n', stdout);

    printf(" %s%-18s%s %-22s",
           syseba_color("cyan"),
           syseba_tr(app->language,
                     "Sincronizzazione",
                     "Synchronization"),
           syseba_color("reset"),
           app->initial_state);
    if (initial_percent >= 0.0) {
        printf(" %6.2f%%  %" PRIu64 "/%" PRIu64,
               initial_percent,
               stats.initial_done,
               stats.initial_total);
    }
    fputc('\n', stdout);

    syseba_dashboard_rule(width, '-');
    printf(" %-13s %8" PRIu64 "   %-13s %8" PRIu64
           "   %-13s %8" PRIu64 "\n",
           syseba_tr(app->language, "Copiati", "Copied"),
           stats.copied,
           syseba_tr(app->language, "Aggiornati", "Updated"),
           stats.updated,
           syseba_tr(app->language, "Eliminati", "Deleted"),
           stats.deleted);
    printf(" %-13s %8" PRIu64 "   %-13s %8" PRIu64
           "   %-13s %8" PRIu64 "\n",
           syseba_tr(app->language, "Ripristinati", "Restored"),
           stats.restored,
           syseba_tr(app->language, "Saltati", "Skipped"),
           stats.skipped,
           syseba_tr(app->language, "Errori", "Errors"),
           stats.errors);
    printf(" CPU %-9s   RAM %-12s   Threads %-4u   Queue %-6zu\n",
           cpu_text,
           memory_text,
           threads,
           syseba_queue_size(&app->event_queue));

    if (!compact) {
        size_t available = height > 25 ? height - 25 : 2;
        syseba_dashboard_rule(width, '-');
        printf(" %s%s%s\n",
               syseba_color("bold"),
               syseba_tr(app->language,
                         "Attivita recente",
                         "Recent activity"),
               syseba_color("reset"));
        syseba_mutex_lock(&app->state_mutex);
        {
            size_t shown = app->recent_count < available
                               ? app->recent_count
                               : available;
            size_t first = (app->recent_next + app->recent_capacity -
                            app->recent_count) %
                           app->recent_capacity;
            size_t start = app->recent_count - shown;
            for (size_t index = start; index < app->recent_count; index++) {
                size_t slot = (first + index) % app->recent_capacity;
                if (app->recent_logs[slot] != NULL) {
                    printf(" %-*.*s\n",
                           (int)(width - 2),
                           (int)(width - 2),
                           app->recent_logs[slot]);
                }
            }
            if (shown == 0) {
                printf(" %s\n",
                       syseba_tr(app->language,
                                 "In attesa di eventi...",
                                 "Waiting for events..."));
            }
        }
        syseba_mutex_unlock(&app->state_mutex);
    }
    syseba_dashboard_rule(width, '-');
    printf(" %s: %s",
           syseba_tr(app->language, "Log", "Log"),
           app->config.log_file);
    if (app->web_enabled) {
        printf("   Web: http://%s:%d", app->web_host, app->web_port);
    }
    printf("\n %s\n",
           syseba_tr(app->language,
                     "Ctrl+C per arrestare in modo ordinato",
                     "Ctrl+C for a graceful shutdown"));
    fflush(stdout);
}

int syseba_dashboard_run(syseba_app_t *app, double refresh_seconds)
{
    unsigned refresh_ms;
    if (refresh_seconds < 0.2) {
        refresh_seconds = 0.2;
    }
    refresh_ms = (unsigned)(refresh_seconds * 1000.0);
    if (syseba_is_terminal(stdout)) {
        fputs("\033[?25l", stdout);
    }
    while (!syseba_app_is_stopping(app)) {
        unsigned elapsed = 0;
        if (syseba_cli_stop_pending()) {
            syseba_app_set_stopping(app);
            break;
        }
        syseba_dashboard_render(app);
        while (!syseba_app_is_stopping(app) && elapsed < refresh_ms) {
            unsigned slice = refresh_ms - elapsed > 100
                                 ? 100
                                 : refresh_ms - elapsed;
            if (syseba_cli_stop_pending()) {
                syseba_app_set_stopping(app);
                break;
            }
            syseba_sleep_ms(slice);
            elapsed += slice;
        }
    }
    if (syseba_is_terminal(stdout)) {
        fputs("\033[?25h\033[0m\n", stdout);
        fflush(stdout);
    }
    return 0;
}
