#include "syseba_internal.h"

#include "cJSON.h"
#include "civetweb.h"
#include "syseba_assets.h"

#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define SYSEBA_WEB_TOKEN_MIN 16u

static const char *syseba_http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

static int syseba_http_send(struct mg_connection *connection,
                            int status,
                            const char *content_type,
                            const void *body,
                            size_t body_size,
                            const char *extra_headers)
{
    int result = mg_printf(
        connection,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %" PRIu64 "\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; "
        "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
        "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; "
        "form-action 'self'\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        status,
        syseba_http_reason(status),
        content_type,
        (uint64_t)body_size,
        extra_headers == NULL ? "" : extra_headers);
    if (result < 0) {
        return -1;
    }
    if (body_size > 0 &&
        mg_write(connection, body, body_size) != (int)body_size) {
        return -1;
    }
    return status;
}

static int syseba_http_send_json_text(struct mg_connection *connection,
                                      int status,
                                      const char *json,
                                      const char *extra_headers)
{
    return syseba_http_send(connection,
                            status,
                            "application/json; charset=utf-8",
                            json,
                            strlen(json),
                            extra_headers);
}

static int syseba_http_send_json(struct mg_connection *connection,
                                 int status,
                                 cJSON *json,
                                 const char *extra_headers)
{
    char *body = cJSON_PrintUnformatted(json);
    int result;
    if (body == NULL) {
        return syseba_http_send_json_text(
            connection,
            500,
            "{\"ok\":false,\"code\":\"internal_error\","
            "\"error\":\"Unable to serialize response\"}",
            NULL);
    }
    result = syseba_http_send_json_text(connection,
                                        status,
                                        body,
                                        extra_headers);
    cJSON_free(body);
    return result;
}

static int syseba_http_error(struct mg_connection *connection,
                             int status,
                             const char *code,
                             const char *message)
{
    cJSON *root = cJSON_CreateObject();
    int result;
    if (root == NULL) {
        return syseba_http_send_json_text(connection,
                                          500,
                                          "{\"ok\":false}",
                                          NULL);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "error", message);
    result = syseba_http_send_json(connection, status, root, NULL);
    cJSON_Delete(root);
    return result;
}

static bool syseba_constant_time_equal(const char *first,
                                       const char *second)
{
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    size_t length = first_length > second_length
                        ? first_length
                        : second_length;
    unsigned difference = (unsigned)(first_length ^ second_length);
    for (size_t index = 0; index < length; index++) {
        unsigned char left =
            index < first_length ? (unsigned char)first[index] : 0;
        unsigned char right =
            index < second_length ? (unsigned char)second[index] : 0;
        difference |= (unsigned)(left ^ right);
    }
    return difference == 0;
}

static bool syseba_http_authorized(struct mg_connection *connection,
                                   const syseba_app_t *app)
{
    const char *supplied;
    const char *authorization;
    if (!app->web_auth_enabled) {
        return true;
    }
    supplied = mg_get_header(connection, "X-SySeBa-Token");
    if (supplied != NULL &&
        syseba_constant_time_equal(supplied, app->web_token)) {
        return true;
    }
    authorization = mg_get_header(connection, "Authorization");
    if (authorization != NULL &&
        strlen(authorization) > 7 &&
#ifdef _WIN32
        _strnicmp(authorization, "Bearer ", 7) == 0 &&
#else
        strncasecmp(authorization, "Bearer ", 7) == 0 &&
#endif
        syseba_constant_time_equal(authorization + 7, app->web_token)) {
        return true;
    }
    return false;
}

static int syseba_http_require_auth(struct mg_connection *connection,
                                    const syseba_app_t *app)
{
    if (syseba_http_authorized(connection, app)) {
        return 0;
    }
    {
        cJSON *root = cJSON_CreateObject();
        int result;
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(
            root,
            "error",
            syseba_tr(app->language,
                      "Autenticazione richiesta.",
                      "Authentication required."));
        result = syseba_http_send_json(
            connection,
            401,
            root,
            "WWW-Authenticate: Bearer realm=\"SySeBa\"\r\n");
        cJSON_Delete(root);
        return result;
    }
}

static int syseba_query_value(const struct mg_request_info *request,
                              const char *name,
                              const char *fallback,
                              char *output,
                              size_t output_size)
{
    int result;
    if (request->query_string == NULL) {
        return snprintf(output, output_size, "%s", fallback) < (int)output_size
                   ? 0
                   : -1;
    }
    result = mg_get_var(request->query_string,
                        strlen(request->query_string),
                        name,
                        output,
                        output_size);
    if (result < 0) {
        return snprintf(output, output_size, "%s", fallback) < (int)output_size
                   ? 0
                   : -1;
    }
    return 0;
}

static unsigned syseba_query_unsigned(const struct mg_request_info *request,
                                      const char *name,
                                      unsigned fallback,
                                      unsigned maximum)
{
    char value[32];
    char *end = NULL;
    unsigned long parsed;
    if (syseba_query_value(request,
                           name,
                           "",
                           value,
                           sizeof(value)) != 0 ||
        value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return fallback;
    }
    if (parsed > maximum) {
        parsed = maximum;
    }
    return (unsigned)parsed;
}

static cJSON *syseba_config_json(const syseba_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "source", config->source);
    cJSON_AddStringToObject(root, "backup", config->backup);
    cJSON_AddStringToObject(root, "restore", config->restore);
    cJSON_AddStringToObject(root, "log_file", config->log_file);
    cJSON_AddNumberToObject(root, "threads", config->threads);
    cJSON_AddStringToObject(root, "config_path", config->config_path);
    return root;
}

static cJSON *syseba_config_state_json(syseba_app_t *app)
{
    char *status_text = NULL;
    cJSON *status;
    cJSON *state;
    if (syseba_app_status_json(app, &status_text) != SYSEBA_OK) {
        return NULL;
    }
    status = cJSON_Parse(status_text);
    cJSON_free(status_text);
    if (status == NULL) {
        return NULL;
    }
    state = cJSON_DetachItemFromObject(status, "config_state");
    cJSON_Delete(status);
    return state;
}

static int syseba_http_status(struct mg_connection *connection,
                              syseba_app_t *app)
{
    char *json = NULL;
    int result;
    if (syseba_app_status_json(app, &json) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 500,
                                 "internal_error",
                                 "Unable to build status");
    }
    result = syseba_http_send_json_text(connection, 200, json, NULL);
    cJSON_free(json);
    return result;
}

static int syseba_http_logs(struct mg_connection *connection,
                            const struct mg_request_info *request,
                            syseba_app_t *app)
{
    unsigned requested = syseba_query_unsigned(request,
                                               "lines",
                                               200,
                                               2000);
    size_t count = 0;
    char **lines = syseba_log_tail(app->config.log_file,
                                   requested,
                                   &count);
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "lines");
    int result;
    for (size_t index = 0; index < count; index++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(lines[index]));
    }
    syseba_log_tail_free(lines, count);
    result = syseba_http_send_json(connection, 200, root, NULL);
    cJSON_Delete(root);
    return result;
}

static int syseba_http_config_get(struct mg_connection *connection,
                                  syseba_app_t *app)
{
    syseba_config_t config;
    char error[512] = {0};
    cJSON *json;
    int result;
    if (syseba_config_load(app->config.config_path,
                           &config,
                           error,
                           sizeof(error)) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 500,
                                 "config_error",
                                 error);
    }
    json = syseba_config_json(&config);
    result = syseba_http_send_json(connection, 200, json, NULL);
    cJSON_Delete(json);
    return result;
}

static int syseba_http_read_json(struct mg_connection *connection,
                                 const struct mg_request_info *request,
                                 cJSON **json,
                                 char *error,
                                 size_t error_size)
{
    char *body;
    size_t offset = 0;
    if (request->content_length < 0 ||
        request->content_length > (long long)SYSEBA_MAX_JSON_BODY) {
        (void)snprintf(error, error_size, "Invalid request body size");
        return request->content_length > (long long)SYSEBA_MAX_JSON_BODY
                   ? 413
                   : 400;
    }
    body = (char *)malloc((size_t)request->content_length + 1u);
    if (body == NULL) {
        (void)snprintf(error, error_size, "Out of memory");
        return 500;
    }
    while (offset < (size_t)request->content_length) {
        int count = mg_read(connection,
                            body + offset,
                            (size_t)request->content_length - offset);
        if (count <= 0) {
            free(body);
            (void)snprintf(error, error_size, "Incomplete request body");
            return 400;
        }
        offset += (size_t)count;
    }
    body[offset] = '\0';
    *json = cJSON_ParseWithLength(body, offset);
    free(body);
    if (*json == NULL || !cJSON_IsObject(*json)) {
        cJSON_Delete(*json);
        *json = NULL;
        (void)snprintf(error, error_size, "Invalid JSON object");
        return 400;
    }
    return 0;
}

static int syseba_json_copy_path(cJSON *root,
                                 const char *name,
                                 char *output,
                                 size_t output_size,
                                 char *error,
                                 size_t error_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (value == NULL) {
        return 0;
    }
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
        value->valuestring[0] == '\0' ||
        snprintf(output,
                 output_size,
                 "%s",
                 value->valuestring) >= (int)output_size) {
        (void)snprintf(error, error_size, "Invalid %s", name);
        return -1;
    }
    return 0;
}

static int syseba_http_config_post(struct mg_connection *connection,
                                   const struct mg_request_info *request,
                                   syseba_app_t *app)
{
    cJSON *input = NULL;
    cJSON *response = NULL;
    cJSON *response_config = NULL;
    cJSON *response_state = NULL;
    cJSON *threads;
    syseba_config_t candidate = app->config;
    char error[1024] = {0};
    int http_status = syseba_http_read_json(connection,
                                            request,
                                            &input,
                                             error,
                                             sizeof(error));
    int result;
    bool restart_required;
    if (http_status != 0) {
        return syseba_http_error(connection,
                                 http_status,
                                 "invalid_request",
                                 error);
    }
    if (syseba_json_copy_path(input,
                              "source",
                              candidate.source,
                              sizeof(candidate.source),
                              error,
                              sizeof(error)) != 0 ||
        syseba_json_copy_path(input,
                              "backup",
                              candidate.backup,
                              sizeof(candidate.backup),
                              error,
                              sizeof(error)) != 0 ||
        syseba_json_copy_path(input,
                              "restore",
                              candidate.restore,
                              sizeof(candidate.restore),
                              error,
                              sizeof(error)) != 0 ||
        syseba_json_copy_path(input,
                              "log_file",
                              candidate.log_file,
                              sizeof(candidate.log_file),
                              error,
                              sizeof(error)) != 0) {
        cJSON_Delete(input);
        return syseba_http_error(connection,
                                 400,
                                 "invalid_request",
                                 error);
    }
    threads = cJSON_GetObjectItemCaseSensitive(input, "threads");
    if (threads != NULL) {
        long parsed = 0;
        if (cJSON_IsNumber(threads)) {
            parsed = (long)threads->valuedouble;
            if ((double)parsed != threads->valuedouble) {
                parsed = 0;
            }
        } else if (cJSON_IsString(threads) && threads->valuestring != NULL) {
            char *end = NULL;
            parsed = strtol(threads->valuestring, &end, 10);
            if (end == threads->valuestring || *end != '\0') {
                parsed = 0;
            }
        }
        if (parsed < 1 || parsed > SYSEBA_MAX_THREADS) {
            cJSON_Delete(input);
            return syseba_http_error(
                connection,
                400,
                "invalid_request",
                "threads must be between 1 and 64");
        }
        candidate.threads = (unsigned)parsed;
    }
    cJSON_Delete(input);

    if (syseba_config_normalize(&candidate,
                                error,
                                sizeof(error)) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 400,
                                 "config_error",
                                 error);
    }
    if (syseba_config_save(&candidate,
                           error,
                           sizeof(error)) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 errno == EACCES ? 403 : 400,
                                 "config_error",
                                 error);
    }
    restart_required =
        strcmp(candidate.source, app->config.source) != 0 ||
        strcmp(candidate.backup, app->config.backup) != 0 ||
        strcmp(candidate.restore, app->config.restore) != 0 ||
        strcmp(candidate.log_file, app->config.log_file) != 0 ||
        candidate.threads != app->config.threads;
    syseba_mutex_lock(&app->state_mutex);
    app->restart_required = restart_required;
    syseba_mutex_unlock(&app->state_mutex);
    syseba_log_emit(app,
                    "INFO",
                    "CONFIG",
                    app->config.config_path,
                    "",
                    "",
                    "%s",
                    syseba_tr(app->language,
                              "Configurazione aggiornata dalla Web UI",
                              "Configuration updated from Web UI"));

    response = cJSON_CreateObject();
    response_config = syseba_config_json(&candidate);
    response_state = syseba_config_state_json(app);
    if (response == NULL || response_config == NULL || response_state == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(response_config);
        cJSON_Delete(response_state);
        return syseba_http_error(connection,
                                 500,
                                 "internal_error",
                                 "Unable to build configuration response");
    }
    syseba_mutex_lock(&app->state_mutex);
    restart_required = app->restart_required;
    syseba_mutex_unlock(&app->state_mutex);
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddBoolToObject(response,
                          "restart_required",
                          restart_required);
    cJSON_AddItemToObject(response,
                          "config",
                          response_config);
    cJSON_AddItemToObject(response,
                          "state",
                          response_state);
    cJSON_AddStringToObject(
        response,
        "message",
        restart_required
            ? syseba_tr(app->language,
                        "Configurazione salvata. Riavvia SySeBa per applicarla.",
                        "Configuration saved. Restart SySeBa to apply it.")
            : syseba_tr(app->language,
                        "Configurazione salvata e già attiva.",
                        "Configuration saved and already active."));
    result = syseba_http_send_json(connection, 200, response, NULL);
    cJSON_Delete(response);
    return result;
}

static void syseba_human_size(uint64_t size, char *output, size_t output_size)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)size;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1u < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    (void)snprintf(output, output_size, "%.1f %s", value, units[unit]);
}

static cJSON *syseba_restore_item_json(const syseba_restore_item_t *item)
{
    cJSON *json = cJSON_CreateObject();
    char human[64];
    cJSON_AddStringToObject(json, "name", item->name);
    cJSON_AddStringToObject(json, "path", item->path);
    cJSON_AddBoolToObject(json, "is_dir", item->is_directory);
    if (item->is_directory) {
        cJSON_AddNullToObject(json, "size");
        cJSON_AddStringToObject(json, "size_human", "-");
    } else {
        cJSON_AddNumberToObject(json, "size", (double)item->size);
        syseba_human_size(item->size, human, sizeof(human));
        cJSON_AddStringToObject(json, "size_human", human);
    }
    cJSON_AddStringToObject(json, "mtime", item->mtime);
    cJSON_AddBoolToObject(json,
                          "destination_exists",
                          item->destination_exists);
    return json;
}

static int syseba_http_restore_list(struct mg_connection *connection,
                                    const struct mg_request_info *request,
                                    syseba_app_t *app)
{
    char path[SYSEBA_PATH_MAX];
    char search[512];
    char sort[16];
    char direction[8];
    char error[1024] = {0};
    unsigned page = syseba_query_unsigned(request, "page", 1, UINT32_MAX);
    unsigned page_size = syseba_query_unsigned(
        request,
        "page_size",
        SYSEBA_DEFAULT_RESTORE_PAGE,
        SYSEBA_MAX_RESTORE_PAGE);
    syseba_restore_listing_t listing;
    cJSON *root;
    cJSON *items;
    int result;

    (void)syseba_query_value(request, "path", "", path, sizeof(path));
    (void)syseba_query_value(request,
                             "search",
                             "",
                             search,
                             sizeof(search));
    (void)syseba_query_value(request,
                             "sort",
                             "name",
                             sort,
                             sizeof(sort));
    (void)syseba_query_value(request,
                             "direction",
                             "asc",
                             direction,
                             sizeof(direction));
    if (strcmp(sort, "name") != 0 &&
        strcmp(sort, "mtime") != 0 &&
        strcmp(sort, "size") != 0) {
        (void)snprintf(sort, sizeof(sort), "name");
    }
    if (strcmp(direction, "desc") != 0) {
        (void)snprintf(direction, sizeof(direction), "asc");
    }
    result = syseba_restore_list(app,
                                 path,
                                 search,
                                 page,
                                 page_size,
                                 sort,
                                 direction,
                                 &listing,
                                 error,
                                 sizeof(error));
    if (result != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 result == SYSEBA_ERR_NOT_FOUND ? 404 : 400,
                                 result == SYSEBA_ERR_NOT_FOUND
                                     ? "not_found"
                                     : "invalid_request",
                                 error);
    }
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", listing.path);
    cJSON_AddBoolToObject(root, "is_file", listing.is_file);
    items = cJSON_AddArrayToObject(root, "items");
    for (size_t index = 0; index < listing.count; index++) {
        cJSON_AddItemToArray(items,
                             syseba_restore_item_json(&listing.items[index]));
    }
    cJSON_AddStringToObject(root, "search", search);
    cJSON_AddStringToObject(root, "sort", sort);
    cJSON_AddStringToObject(root, "direction", direction);
    cJSON_AddNumberToObject(root, "page", listing.page);
    cJSON_AddNumberToObject(root, "page_size", listing.page_size);
    cJSON_AddNumberToObject(root, "pages", listing.pages);
    cJSON_AddNumberToObject(root, "total", (double)listing.total);
    cJSON_AddBoolToObject(root, "has_previous", listing.has_previous);
    cJSON_AddBoolToObject(root, "has_next", listing.has_next);
    syseba_restore_listing_free(&listing);
    result = syseba_http_send_json(connection, 200, root, NULL);
    cJSON_Delete(root);
    return result;
}

static int syseba_http_restore_info(struct mg_connection *connection,
                                    const struct mg_request_info *request,
                                    syseba_app_t *app)
{
    char relative[SYSEBA_PATH_MAX];
    char source[SYSEBA_PATH_MAX];
    char destination[SYSEBA_PATH_MAX];
    uint64_t size;
    int64_t mtime_ns;
    bool directory;
    cJSON *root;
    char human[64];
    char mtime[32];
    const char *name;
    int result;

    (void)syseba_query_value(request,
                             "path",
                             "",
                             relative,
                             sizeof(relative));
    if (relative[0] == '\0' ||
        syseba_safe_join(app->config.restore,
                         relative,
                         source,
                         sizeof(source)) != SYSEBA_OK ||
        syseba_safe_join(app->config.source,
                         relative,
                         destination,
                         sizeof(destination)) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 400,
                                 "invalid_request",
                                 "Invalid restore item path");
    }
    if (syseba_fs_stat(source,
                       &size,
                       &mtime_ns,
                       &directory) != 0) {
        return syseba_http_error(connection,
                                 404,
                                 "not_found",
                                 "Restore item not found");
    }
    name = strrchr(source, SYSEBA_PATH_SEP);
    syseba_iso_from_time((time_t)(mtime_ns / 1000000000LL),
                         mtime,
                         sizeof(mtime));
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root,
                            "name",
                            name == NULL ? source : name + 1);
    cJSON_AddStringToObject(root, "path", relative);
    cJSON_AddBoolToObject(root, "is_dir", directory);
    if (directory) {
        cJSON_AddNullToObject(root, "size");
        cJSON_AddStringToObject(root, "size_human", "-");
    } else {
        syseba_human_size(size, human, sizeof(human));
        cJSON_AddNumberToObject(root, "size", (double)size);
        cJSON_AddStringToObject(root, "size_human", human);
    }
    cJSON_AddStringToObject(root, "mtime", mtime);
    cJSON_AddStringToObject(root, "destination", destination);
    cJSON_AddBoolToObject(root,
                          "destination_exists",
                          syseba_fs_exists(destination));
    result = syseba_http_send_json(connection, 200, root, NULL);
    cJSON_Delete(root);
    return result;
}

static int syseba_http_restore_post(struct mg_connection *connection,
                                    const struct mg_request_info *request,
                                    syseba_app_t *app)
{
    cJSON *input = NULL;
    cJSON *path;
    cJSON *strategy_json;
    cJSON *response;
    char destination[SYSEBA_PATH_MAX];
    char error[1024] = {0};
    syseba_restore_strategy_t strategy = SYSEBA_RESTORE_FAIL;
    const char *strategy_name = "fail";
    int http_status = syseba_http_read_json(connection,
                                            request,
                                            &input,
                                            error,
                                            sizeof(error));
    int result;

    if (http_status != 0) {
        return syseba_http_error(connection,
                                 http_status,
                                 "invalid_request",
                                 error);
    }
    path = cJSON_GetObjectItemCaseSensitive(input, "path");
    strategy_json = cJSON_GetObjectItemCaseSensitive(input, "strategy");
    if (!cJSON_IsString(path) || path->valuestring == NULL ||
        path->valuestring[0] == '\0') {
        cJSON_Delete(input);
        return syseba_http_error(connection,
                                 400,
                                 "invalid_request",
                                 "restore path is required");
    }
    if (cJSON_IsString(strategy_json) &&
        strategy_json->valuestring != NULL) {
        strategy_name = strategy_json->valuestring;
    }
    if (strcmp(strategy_name, "rename") == 0) {
        strategy = SYSEBA_RESTORE_RENAME;
        strategy_name = "rename";
    } else if (strcmp(strategy_name, "overwrite") == 0) {
        strategy = SYSEBA_RESTORE_OVERWRITE;
        strategy_name = "overwrite";
    } else if (strcmp(strategy_name, "fail") != 0) {
        cJSON_Delete(input);
        return syseba_http_error(connection,
                                 400,
                                 "invalid_request",
                                 "Invalid restore strategy");
    } else {
        strategy_name = "fail";
    }
    result = syseba_restore_item(app,
                                 path->valuestring,
                                 strategy,
                                 destination,
                                 sizeof(destination),
                                 error,
                                 sizeof(error));
    cJSON_Delete(input);
    if (result != SYSEBA_OK) {
        int status = result == SYSEBA_ERR_NOT_FOUND
                         ? 404
                         : (result == SYSEBA_ERR_EXISTS ? 409 : 400);
        return syseba_http_error(connection,
                                 status,
                                 result == SYSEBA_ERR_NOT_FOUND
                                     ? "not_found"
                                     : (result == SYSEBA_ERR_EXISTS
                                            ? "destination_exists"
                                            : "invalid_request"),
                                 error);
    }
    response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "restored_to", destination);
    cJSON_AddStringToObject(response, "strategy", strategy_name);
    cJSON_AddStringToObject(
        response,
        "message",
        syseba_tr(app->language,
                  "Elemento ripristinato correttamente.",
                  "Item restored successfully."));
    result = syseba_http_send_json(connection, 200, response, NULL);
    cJSON_Delete(response);
    return result;
}

static void syseba_safe_download_name(const char *path,
                                      char *output,
                                      size_t output_size)
{
    const char *name = strrchr(path, '/');
#ifdef _WIN32
    {
        const char *backslash = strrchr(path, '\\');
        if (backslash != NULL && (name == NULL || backslash > name)) {
            name = backslash;
        }
    }
#endif
    name = name == NULL ? path : name + 1;
    if (*name == '\0') {
        name = "download";
    }
    size_t used = 0;
    for (; *name != '\0' && used + 1u < output_size; name++) {
        unsigned char value = (unsigned char)*name;
        output[used++] = value == '"' || value == '\r' || value == '\n' ||
                                value == '/' || value == '\\' ||
                                value < 0x20
                            ? '_'
                            : (char)value;
    }
    output[used] = '\0';
}

static int syseba_web_open_regular(const char *path,
                                   FILE **file,
                                   uint64_t *size)
{
    return syseba_open_regular_read(path, file, size);
}

static int syseba_http_download(struct mg_connection *connection,
                                const struct mg_request_info *request,
                                syseba_app_t *app)
{
    char relative[SYSEBA_PATH_MAX];
    char path[SYSEBA_PATH_MAX];
    char name[512];
    char headers[1024];
    FILE *file = NULL;
    uint64_t file_size = 0;
    unsigned char buffer[1024 * 1024];

    (void)syseba_query_value(request,
                             "path",
                             "",
                             relative,
                             sizeof(relative));
    if (relative[0] == '\0' ||
        syseba_safe_join(app->config.restore,
                         relative,
                         path,
                         sizeof(path)) != SYSEBA_OK ||
        syseba_web_open_regular(path, &file, &file_size) != 0) {
        return syseba_http_error(connection,
                                 404,
                                 "not_found",
                                 "File not found");
    }
    syseba_safe_download_name(path, name, sizeof(name));
    (void)snprintf(headers,
                   sizeof(headers),
                   "Content-Disposition: attachment; filename=\"%s\"\r\n",
                   name);
    if (mg_printf(
            connection,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %" PRIu64 "\r\n"
            "Cache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "%s"
            "Connection: close\r\n\r\n",
            file_size,
            headers) < 0) {
        fclose(file);
        return 500;
    }
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0 && mg_write(connection, buffer, count) != (int)count) {
            fclose(file);
            return 500;
        }
        if (count < sizeof(buffer)) {
            break;
        }
    }
    fclose(file);
    return 200;
}

static int syseba_http_restart(struct mg_connection *connection,
                               syseba_app_t *app)
{
    char error[512] = {0};
    cJSON *response;
    int result;
    if (syseba_service_request_restart(error, sizeof(error)) != SYSEBA_OK) {
        return syseba_http_error(connection,
                                 503,
                                 "restart_unavailable",
                                 error);
    }
    syseba_log_emit(app,
                    "WARNING",
                    "SERVICE",
                    "",
                    "",
                    "",
                    "%s",
                    syseba_tr(app->language,
                              "Riavvio del servizio richiesto dalla Web UI",
                              "Service restart requested from Web UI"));
    response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(
        response,
        "message",
        syseba_tr(app->language,
                  "Riavvio del servizio richiesto.",
                  "Service restart requested."));
    result = syseba_http_send_json(connection, 200, response, NULL);
    cJSON_Delete(response);
    return result;
}

static int syseba_http_index(struct mg_connection *connection,
                             const syseba_app_t *app)
{
    static const char marker[] = "__SYSEBA_LANG__";
    const char *language = app->language == SYSEBA_LANGUAGE_EN ? "en" : "it";
    const char *html = (const char *)syseba_asset_html;
    const char *position = strstr(html, marker);
    if (position == NULL) {
        return syseba_http_send(connection,
                                200,
                                "text/html; charset=utf-8",
                                syseba_asset_html,
                                syseba_asset_html_len,
                                NULL);
    }
    {
        size_t prefix = (size_t)(position - html);
        size_t marker_length = strlen(marker);
        size_t body_size = syseba_asset_html_len - marker_length + 2u;
        char *body = (char *)malloc(body_size + 1u);
        int result;
        if (body == NULL) {
            return syseba_http_error(connection,
                                     500,
                                     "internal_error",
                                     "Out of memory");
        }
        memcpy(body, html, prefix);
        memcpy(body + prefix, language, 2);
        memcpy(body + prefix + 2u,
               position + marker_length,
               syseba_asset_html_len - prefix - marker_length);
        body[body_size] = '\0';
        result = syseba_http_send(connection,
                                  200,
                                  "text/html; charset=utf-8",
                                  body,
                                  body_size,
                                  NULL);
        free(body);
        return result;
    }
}

static int syseba_http_handler(struct mg_connection *connection,
                               void *opaque)
{
    syseba_app_t *app = (syseba_app_t *)opaque;
    const struct mg_request_info *request =
        mg_get_request_info(connection);
    const char *path = request->local_uri == NULL
                           ? request->request_uri
                           : request->local_uri;
    bool get = strcmp(request->request_method, "GET") == 0;
    bool post = strcmp(request->request_method, "POST") == 0;

    if (get && strcmp(path, "/") == 0) {
        return syseba_http_index(connection, app);
    }
    if (get && strcmp(path, "/webui.js") == 0) {
        return syseba_http_send(connection,
                                200,
                                "application/javascript; charset=utf-8",
                                syseba_asset_js,
                                syseba_asset_js_len,
                                NULL);
    }
    if (get && (strcmp(path, "/logo") == 0 ||
                strcmp(path, "/favicon.ico") == 0)) {
        return syseba_http_send(connection,
                                200,
                                "image/webp",
                                syseba_asset_logo,
                                syseba_asset_logo_len,
                                NULL);
    }
    if (get && strcmp(path, "/api/auth") == 0) {
        cJSON *root = cJSON_CreateObject();
        int result;
        cJSON_AddBoolToObject(root, "required", app->web_auth_enabled);
        result = syseba_http_send_json(connection, 200, root, NULL);
        cJSON_Delete(root);
        return result;
    }
    if (!get && !post) {
        return syseba_http_error(connection,
                                 400,
                                 "invalid_method",
                                 "Unsupported HTTP method");
    }
    {
        int unauthorized = syseba_http_require_auth(connection, app);
        if (unauthorized != 0) {
            return unauthorized;
        }
    }
    if (get && strcmp(path, "/api/status") == 0) {
        return syseba_http_status(connection, app);
    }
    if (get && strcmp(path, "/api/logs") == 0) {
        return syseba_http_logs(connection, request, app);
    }
    if (get && strcmp(path, "/api/config") == 0) {
        return syseba_http_config_get(connection, app);
    }
    if (get && strcmp(path, "/api/config/state") == 0) {
        cJSON *state = syseba_config_state_json(app);
        int result;
        if (state == NULL) {
            return syseba_http_error(connection,
                                     500,
                                     "internal_error",
                                     "Unable to build configuration state");
        }
        result = syseba_http_send_json(connection, 200, state, NULL);
        cJSON_Delete(state);
        return result;
    }
    if (post && strcmp(path, "/api/config") == 0) {
        return syseba_http_config_post(connection, request, app);
    }
    if (get && strcmp(path, "/api/restore") == 0) {
        return syseba_http_restore_list(connection, request, app);
    }
    if (get && strcmp(path, "/api/restore/info") == 0) {
        return syseba_http_restore_info(connection, request, app);
    }
    if (post && strcmp(path, "/api/restore") == 0) {
        return syseba_http_restore_post(connection, request, app);
    }
    if (get && strcmp(path, "/restore/download") == 0) {
        return syseba_http_download(connection, request, app);
    }
    if (post && strcmp(path, "/api/service/restart") == 0) {
        return syseba_http_restart(connection, app);
    }
    return syseba_http_error(connection,
                             404,
                             "not_found",
                             syseba_tr(app->language,
                                       "Risorsa non trovata.",
                                       "Not found."));
}

static char *syseba_trim_token(char *value)
{
    char *end;
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return value;
}

static bool syseba_web_token_valid(const char *token)
{
    size_t length = token == NULL ? 0 : strlen(token);
    if (length < SYSEBA_WEB_TOKEN_MIN || length >= SYSEBA_TOKEN_MAX) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char value = (unsigned char)token[index];
        if (value < 0x21 || value > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool syseba_web_host_is_loopback(const char *host)
{
    return strcmp(host, "127.0.0.1") == 0 ||
           strcmp(host, "::1") == 0 ||
           strcmp(host, "[::1]") == 0 ||
           strcmp(host, "localhost") == 0;
}

static int syseba_web_read_token(const char *path,
                                 char *token,
                                 size_t token_size)
{
    FILE *file = NULL;
    char buffer[SYSEBA_TOKEN_MAX + 32];
    char *trimmed;
    size_t count;
    if (syseba_open_regular_read(path, &file, NULL) != 0) {
        return -1;
    }
    count = fread(buffer, 1, sizeof(buffer) - 1u, file);
    if (ferror(file) || (!feof(file) && count == sizeof(buffer) - 1u)) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    fclose(file);
    buffer[count] = '\0';
    trimmed = syseba_trim_token(buffer);
    if (!syseba_web_token_valid(trimmed) ||
        snprintf(token, token_size, "%s", trimmed) >= (int)token_size) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int syseba_web_create_token(const char *path,
                                   char *token,
                                   size_t token_size)
{
    unsigned char random[32];
    char parent[SYSEBA_PATH_MAX];
    char temporary[SYSEBA_PATH_MAX];
    FILE *file = NULL;
    if (token_size < sizeof(random) * 2u + 1u ||
        syseba_random_bytes(random, sizeof(random)) != 0) {
        return -1;
    }
    for (size_t index = 0; index < sizeof(random); index++) {
        (void)snprintf(token + index * 2u,
                       token_size - index * 2u,
                       "%02x",
                       random[index]);
    }
    if (syseba_parent_dir(path, parent, sizeof(parent)) != 0 ||
        syseba_mkdirs(parent, 0750) != 0 ||
        snprintf(temporary,
                 sizeof(temporary),
                 "%s.tmp.%" PRIu64,
                 path,
                 syseba_monotonic_ns()) >= (int)sizeof(temporary)) {
        return -1;
    }
    if (syseba_open_exclusive_write(temporary,
                                    &file,
                                    0600) != 0) {
        return -1;
    }
    {
        int write_error =
            fprintf(file, "%s\n", token) < 0 ||
            fflush(file) != 0
#ifdef _WIN32
            || _commit(_fileno(file)) != 0
#else
            || fsync(fileno(file)) != 0
#endif
            ;
        if (fclose(file) != 0) {
            write_error = 1;
        }
        if (write_error) {
            (void)remove(temporary);
            return -1;
        }
    }
    if (syseba_set_private_permissions(temporary) != 0) {
        (void)remove(temporary);
        return -1;
    }
    if (syseba_atomic_replace(temporary, path) != 0) {
        (void)remove(temporary);
        return -1;
    }
    return syseba_set_private_permissions(path);
}

int syseba_web_load_token(syseba_app_t *app,
                          const syseba_options_t *options,
                          char *error,
                          size_t error_size)
{
    const char *environment;
    if (options->no_web_auth) {
        if (!syseba_web_host_is_loopback(options->web_host)) {
            (void)snprintf(
                error,
                error_size,
                "--no-web-auth is permitted only on a loopback address");
            return SYSEBA_ERR_INVALID;
        }
        app->web_auth_enabled = false;
        (void)snprintf(app->web_token_source,
                       sizeof(app->web_token_source),
                       "disabled");
        return SYSEBA_OK;
    }
    app->web_auth_enabled = true;
    if (options->explicit_token[0] != '\0') {
        if (!syseba_web_token_valid(options->explicit_token) ||
            snprintf(app->web_token,
                     sizeof(app->web_token),
                     "%s",
                     options->explicit_token) >=
                (int)sizeof(app->web_token)) {
            (void)snprintf(error,
                           error_size,
                           "Web token must contain 16-255 printable ASCII characters");
            return SYSEBA_ERR_INVALID;
        }
        (void)snprintf(app->web_token_source,
                       sizeof(app->web_token_source),
                       "argument");
        return SYSEBA_OK;
    }
    environment = getenv("SYSEBA_WEB_TOKEN");
    if (environment != NULL && *environment != '\0') {
        if (!syseba_web_token_valid(environment) ||
            snprintf(app->web_token,
                     sizeof(app->web_token),
                     "%s",
                     environment) >= (int)sizeof(app->web_token)) {
            (void)snprintf(
                error,
                error_size,
                "SYSEBA_WEB_TOKEN must contain 16-255 printable ASCII characters");
            return SYSEBA_ERR_INVALID;
        }
        (void)snprintf(app->web_token_source,
                       sizeof(app->web_token_source),
                       "environment");
        return SYSEBA_OK;
    }
    if (syseba_web_read_token(options->token_path,
                              app->web_token,
                              sizeof(app->web_token)) == 0) {
        if (syseba_set_private_permissions(options->token_path) != 0) {
            (void)snprintf(error,
                           error_size,
                           "Unable to secure web token %s: %s",
                           options->token_path,
                           strerror(errno));
            return SYSEBA_ERR_PERMISSION;
        }
        (void)snprintf(app->web_token_source,
                       sizeof(app->web_token_source),
                       "file");
        return SYSEBA_OK;
    }
    if (errno != ENOENT ||
        syseba_web_create_token(options->token_path,
                                app->web_token,
                                sizeof(app->web_token)) != 0) {
        (void)snprintf(error,
                       error_size,
                       "Unable to read/create web token %s: %s",
                       options->token_path,
                       strerror(errno));
        return SYSEBA_ERR;
    }
    (void)snprintf(app->web_token_source,
                   sizeof(app->web_token_source),
                   "generated-file");
    return SYSEBA_OK;
}

int syseba_web_start(syseba_app_t *app,
                     char *error,
                     size_t error_size)
{
    char listening[320];
    const char *options[] = {
        "listening_ports",
        listening,
        "num_threads",
        "8",
        "request_timeout_ms",
        "15000",
        "keep_alive_timeout_ms",
        "5000",
        "enable_keep_alive",
        "yes",
        NULL,
    };
    if (snprintf(listening,
                 sizeof(listening),
                 "%s:%d",
                 app->web_host,
                 app->web_port) >= (int)sizeof(listening)) {
        (void)snprintf(error, error_size, "Invalid web listen address");
        return SYSEBA_ERR_INVALID;
    }
    (void)mg_init_library(0);
    app->web_context = mg_start(NULL, app, options);
    if (app->web_context == NULL) {
        (void)snprintf(error,
                       error_size,
                       "Unable to listen on %s",
                       listening);
        (void)mg_exit_library();
        return SYSEBA_ERR;
    }
    mg_set_request_handler(app->web_context,
                           "/",
                           syseba_http_handler,
                           app);
    syseba_log_emit(app,
                    "INFO",
                    "WEB",
                    "",
                    "",
                    app->web_token_source,
                    "%s http://%s:%d (%s)",
                    syseba_tr(app->language,
                              "Web UI in ascolto su",
                              "Web UI listening on"),
                    app->web_host,
                    app->web_port,
                    app->web_auth_enabled
                        ? syseba_tr(app->language,
                                    "autenticazione attiva",
                                    "authentication enabled")
                        : syseba_tr(app->language,
                                    "autenticazione disabilitata",
                                    "authentication disabled"));
    return SYSEBA_OK;
}

void syseba_web_stop(syseba_app_t *app)
{
    if (app->web_context != NULL) {
        mg_stop(app->web_context);
        app->web_context = NULL;
        (void)mg_exit_library();
    }
}
