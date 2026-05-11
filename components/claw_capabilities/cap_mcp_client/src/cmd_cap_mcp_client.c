/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_mcp_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "esp_console.h"

#include "cap_mcp_client_config.h"
#include "cap_mcp_client_internal.h"

static struct {
    struct arg_lit *discover;
    struct arg_lit *list_tools;
    struct arg_lit *call_tool;
    struct arg_str *server_url;
    struct arg_str *endpoint;
    struct arg_str *cursor;
    struct arg_str *tool_name;
    struct arg_str *arguments_json;
    struct arg_str *auth_token;
    struct arg_lit *initialize;
    struct arg_str *transport;
    struct arg_int *timeout_ms;
    struct arg_lit *include_self;
    struct arg_end *end;
} mcp_client_args;

static int mcp_client_call_cap(const char *cap_name, cJSON *root)
{
    char *input_json = NULL;
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
    };

    input_json = cJSON_PrintUnformatted(root);
    if (!input_json) {
        printf("Out of memory\n");
        return 1;
    }

    output = calloc(1, 4096);
    if (!output) {
        free(input_json);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(cap_name, input_json, &ctx, output, 4096);
    if (err != ESP_OK) {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    } else {
        printf("%s\n", output);
    }

    free(output);
    free(input_json);
    return err == ESP_OK ? 0 : 1;
}

static int mcp_client_func(int argc, char **argv)
{
    cJSON *root = NULL;
    cJSON *arguments = NULL;
    const char *cap_name = NULL;
    int nerrors = arg_parse(argc, argv, (void **)&mcp_client_args);
    int operation_count;
    int rc;

    if (nerrors != 0) {
        arg_print_errors(stderr, mcp_client_args.end, argv[0]);
        return 1;
    }

    operation_count = mcp_client_args.discover->count + mcp_client_args.list_tools->count +
                      mcp_client_args.call_tool->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    root = cJSON_CreateObject();
    if (!root) {
        printf("Out of memory\n");
        return 1;
    }

    if (mcp_client_args.discover->count) {
        cap_name = "mcp_discover";
        if (mcp_client_args.timeout_ms->count) {
            cJSON_AddNumberToObject(root, "timeout_ms", mcp_client_args.timeout_ms->ival[0]);
        }
        if (mcp_client_args.include_self->count) {
            cJSON_AddBoolToObject(root, "include_self", true);
        }
    } else {
        if (!mcp_client_args.server_url->count) {
            cJSON_Delete(root);
            printf("'--server-url' is required\n");
            return 1;
        }

        cJSON_AddStringToObject(root, "server_url", mcp_client_args.server_url->sval[0]);
        if (mcp_client_args.endpoint->count) {
            cJSON_AddStringToObject(root, "endpoint", mcp_client_args.endpoint->sval[0]);
        }
        if (mcp_client_args.auth_token->count) {
            cJSON_AddStringToObject(root, "auth_token", mcp_client_args.auth_token->sval[0]);
        }
        if (mcp_client_args.transport->count) {
            cJSON_AddStringToObject(root, "transport", mcp_client_args.transport->sval[0]);
        }
        if (mcp_client_args.initialize->count) {
            cJSON_AddBoolToObject(root, "initialize", true);
        }

        if (mcp_client_args.list_tools->count) {
            cap_name = "mcp_list_tools";
            if (mcp_client_args.cursor->count) {
                cJSON_AddStringToObject(root, "cursor", mcp_client_args.cursor->sval[0]);
            }
        } else {
            cap_name = "mcp_call_tool";
            if (!mcp_client_args.tool_name->count) {
                cJSON_Delete(root);
                printf("'--call-tool' requires '--tool-name'\n");
                return 1;
            }
            cJSON_AddStringToObject(root, "tool_name", mcp_client_args.tool_name->sval[0]);

            if (mcp_client_args.arguments_json->count) {
                arguments = cJSON_Parse(mcp_client_args.arguments_json->sval[0]);
                if (!arguments || !cJSON_IsObject(arguments)) {
                    cJSON_Delete(arguments);
                    cJSON_Delete(root);
                    printf("'--arguments-json' must be a JSON object\n");
                    return 1;
                }
                cJSON_AddItemToObject(root, "arguments", arguments);
            }
        }
    }

    rc = mcp_client_call_cap(cap_name, root);
    cJSON_Delete(root);
    return rc;
}

void register_cap_mcp_client(void)
{
    mcp_client_args.discover = arg_lit0(NULL, "discover", "Discover MCP servers on the local network");
    mcp_client_args.list_tools = arg_lit0(NULL, "list-tools", "List tools from one remote MCP server");
    mcp_client_args.call_tool = arg_lit0(NULL, "call-tool", "Call one tool on a remote MCP server");
    mcp_client_args.server_url = arg_str0(NULL, "server-url", "<url>", "Remote MCP server base URL");
    mcp_client_args.endpoint = arg_str0(NULL, "endpoint", "<endpoint>", "Remote MCP endpoint");
    mcp_client_args.cursor = arg_str0(NULL, "cursor", "<cursor>", "Pagination cursor for list-tools");
    mcp_client_args.tool_name = arg_str0(NULL, "tool-name", "<name>", "Remote MCP tool name");
    mcp_client_args.arguments_json = arg_str0(NULL, "arguments-json", "<json>", "Remote MCP tool arguments JSON object");
    mcp_client_args.auth_token = arg_str0(NULL, "auth-token", "<token>", "Bearer token for MCP server auth");
    mcp_client_args.initialize = arg_lit0(NULL, "initialize", "Force MCP initialize handshake before request");
    mcp_client_args.transport = arg_str0(NULL, "transport", "<http|sse>", "Transport mode: http (default) or sse");
    mcp_client_args.timeout_ms = arg_int0(NULL, "timeout-ms", "<ms>", "Discovery timeout in milliseconds");
    mcp_client_args.include_self = arg_lit0(NULL, "include-self", "Include the local device in discovery");
    mcp_client_args.end = arg_end(12);

    const esp_console_cmd_t mcp_client_cmd = {
        .command = "mcp_client",
        .help = "MCP client operations.\n"
        "Examples:\n"
        " mcp_client --discover --timeout-ms 3000\n"
        " mcp_client --list-tools --server-url http://host.local:18791 --endpoint mcp_server\n"
        " mcp_client --call-tool --server-url http://host.local:18791 --tool-name device.describe\n"
        " mcp_client --list-tools --server-url http://host.local:18791 --auth-token secret-token --initialize\n"
        " mcp_client --list-tools --server-url http://cloud.example.com:3000 --endpoint /sse/device-id --transport sse --auth-token secret-token\n",
        .func = mcp_client_func,
        .argtable = &mcp_client_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&mcp_client_cmd));
}

/* ─── mcp_server helper functions ──────────────────────────────── */

static int mcp_server_list_func(int argc, char **argv)
{
    (void)argc; (void)argv;
    const mcp_server_config_t *config = cap_mcp_get_config();
    if (config->count == 0) {
        printf("No MCP servers configured.\n");
        printf("Use 'mcp_server add <name> --url <url> [--token <token>]' to add one.\n");
        return 0;
    }
    printf("+----------------+----------------------------------------------------+----------+\n");
    printf("| Name           | URL                                                | Endpoint |\n");
    printf("+----------------+----------------------------------------------------+----------+\n");
    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];
        printf("| %-14s | %-50s | %-8s |\n",
               p->name, p->url, p->endpoint[0] ? p->endpoint : "mcp");
    }
    printf("+----------------+----------------------------------------------------+----------+\n");
    printf("Total: %u server(s)\n", (unsigned)config->count);
    return 0;
}

static int mcp_server_reload_func(int argc, char **argv)
{
    (void)argc; (void)argv;
    mcp_server_config_t *config = cap_mcp_get_config_mutable();
    mcp_server_config_free(config);
    esp_err_t err = mcp_server_config_load(MCP_SERVER_CONFIG_PATH, config);
    if (err != ESP_OK) {
        printf("Error: Failed to reload config (%s)\n", esp_err_to_name(err));
        return 1;
    }
    err = mcp_server_config_validate(config);
    if (err != ESP_OK) {
        printf("Warning: Config validation failed, some servers may be invalid\n");
    }
    cap_mcp_rebuild_schemas();
    printf("Config reloaded. %u server(s) loaded.\n", (unsigned)config->count);
    return 0;
}

static int mcp_server_test_func(int argc, char **argv)
{
    const char *opt_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) opt_name = argv[i + 1];
    }
    if (!opt_name) {
        printf("Usage: mcp_srv_test --name <name>\n");
        return 1;
    }
    const char *name = opt_name;
    const mcp_server_config_t *config = cap_mcp_get_config();
    const mcp_server_profile_t *profile = mcp_server_config_find(config, name);
    if (!profile) {
        printf("Error: Server '%s' not found\n", name);
        return 1;
    }
    printf("Testing connection to \"%s\" (%s)...\n", profile->name, profile->url);

    /* Step 1: Initialize handshake */
    printf("  [1/3] Initialize handshake... ");
    fflush(stdout);
    {
        cJSON *input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "server_url", profile->url);
        cJSON_AddStringToObject(input, "endpoint", profile->endpoint);
        if (profile->token[0]) {
            cJSON_AddStringToObject(input, "auth_token", profile->token);
        }
        cJSON_AddBoolToObject(input, "initialize", true);
        char *json_str = cJSON_PrintUnformatted(input);
        cJSON_Delete(input);
        cJSON *result = NULL;
        esp_err_t err = cap_mcp_list_remote_tools(json_str, &result);
        free(json_str);
        if (err != ESP_OK) { printf("FAILED (%s)\n", esp_err_to_name(err)); return 1; }
        const char *err_msg = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
        if (err_msg && err_msg[0]) { printf("FAILED (%s)\n", err_msg); cJSON_Delete(result); return 1; }
        printf("OK\n");
        cJSON_Delete(result);
    }

    /* Step 2: List tools */
    printf("  [2/3] List tools... ");
    fflush(stdout);
    {
        cJSON *input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "server_url", profile->url);
        cJSON_AddStringToObject(input, "endpoint", profile->endpoint);
        if (profile->token[0]) {
            cJSON_AddStringToObject(input, "auth_token", profile->token);
        }
        char *json_str = cJSON_PrintUnformatted(input);
        cJSON_Delete(input);
        cJSON *result = NULL;
        esp_err_t err = cap_mcp_list_remote_tools(json_str, &result);
        free(json_str);
        if (err != ESP_OK) { printf("FAILED (%s)\n", esp_err_to_name(err)); return 1; }
        const char *err_msg = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
        if (err_msg && err_msg[0]) { printf("FAILED (%s)\n", err_msg); cJSON_Delete(result); return 1; }
        cJSON *tools = cJSON_GetObjectItem(result, "tools");
        size_t tool_count = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
        printf("OK (%u tool(s) found)\n", (unsigned)tool_count);
        cJSON_Delete(result);
    }

    printf("  [3/3] Cleanup... OK\n");
    printf("Connection test passed!\n");
    return 0;
}

static int mcp_server_export_func(int argc, char **argv)
{
    (void)argc; (void)argv;
    const mcp_server_config_t *config = cap_mcp_get_config();
    cJSON *root = cJSON_CreateObject();
    cJSON *servers = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "servers", servers);
    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];
        cJSON *server = cJSON_CreateObject();
        cJSON_AddStringToObject(server, "url", p->url);
        if (p->token[0]) { cJSON_AddStringToObject(server, "token", "***"); }
        if (p->endpoint[0] && strcmp(p->endpoint, CAP_MCP_DEFAULT_ENDPOINT) != 0) {
            cJSON_AddStringToObject(server, "endpoint", p->endpoint);
        }
        if (p->description[0]) { cJSON_AddStringToObject(server, "description", p->description); }
        cJSON_AddItemToObject(servers, p->name, server);
    }
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_str) { printf("%s\n", json_str); free(json_str); }
    return 0;
}

/* ─── mcp_srv_add: manual argv parsing ─────────────────────────── */

static int mcp_server_add_func(int argc, char **argv)
{
    const char *opt_name = NULL, *opt_url = NULL, *opt_token = NULL;
    const char *opt_endpoint = NULL, *opt_desc = NULL;

    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--name") == 0) opt_name = argv[i + 1];
        else if (strcmp(argv[i], "--url") == 0) opt_url = argv[i + 1];
        else if (strcmp(argv[i], "--token") == 0) opt_token = argv[i + 1];
        else if (strcmp(argv[i], "--endpoint") == 0) opt_endpoint = argv[i + 1];
        else if (strcmp(argv[i], "--description") == 0) opt_desc = argv[i + 1];
    }
    if (!opt_name || !opt_url) {
        printf("Usage: mcp_srv_add --name <name> --url <url> [--token <token>] [--endpoint <ep>] [--description <desc>]\n");
        return 1;
    }

    mcp_server_profile_t profile = {0};
    strlcpy(profile.name, opt_name, sizeof(profile.name));
    strlcpy(profile.url, opt_url, sizeof(profile.url));
    if (opt_token) strlcpy(profile.token, opt_token, sizeof(profile.token));
    profile.endpoint[0] = '\0';  /* empty = use URL as-is, no path appended */
    if (opt_endpoint) strlcpy(profile.endpoint, opt_endpoint, sizeof(profile.endpoint));
    if (opt_desc) strlcpy(profile.description, opt_desc, sizeof(profile.description));

    if (strncmp(profile.url, "http://", 7) != 0 && strncmp(profile.url, "https://", 8) != 0) {
        printf("Error: URL must start with http:// or https://\n"); return 1;
    }

    mcp_server_config_t *config = cap_mcp_get_config_mutable();
    esp_err_t err = mcp_server_config_add(config, &profile);
    if (err != ESP_OK) { printf("Error: Failed to add server (%s)\n", esp_err_to_name(err)); return 1; }

    err = mcp_server_config_save(MCP_SERVER_CONFIG_PATH, config);
    if (err != ESP_OK) printf("Warning: Failed to save config (%s)\n", esp_err_to_name(err));

    cap_mcp_rebuild_schemas();
    printf("Server '%s' added successfully.\n", profile.name);
    return 0;
}

/* ─── mcp_srv_remove: manual argv parsing ──────────────────────── */

static int mcp_server_remove_func(int argc, char **argv)
{
    const char *opt_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) opt_name = argv[i + 1];
    }
    if (!opt_name) {
        printf("Usage: mcp_srv_remove --name <name>\n"); return 1;
    }

    mcp_server_config_t *config = cap_mcp_get_config_mutable();
    esp_err_t err = mcp_server_config_remove(config, opt_name);
    if (err != ESP_OK) { printf("Error: Server '%s' not found\n", opt_name); return 1; }

    err = mcp_server_config_save(MCP_SERVER_CONFIG_PATH, config);
    if (err != ESP_OK) printf("Warning: Failed to save config (%s)\n", esp_err_to_name(err));

    cap_mcp_rebuild_schemas();
    printf("Server '%s' removed.\n", opt_name);
    return 0;
}

void register_cap_mcp_server_commands(void)
{
    /* mcp_srv_list */
    const esp_console_cmd_t list_cmd = {
        .command = "mcp_srv_list",
        .help = "List configured MCP servers",
        .func = mcp_server_list_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_cmd));

    /* mcp_srv_add --name <name> --url <url> [--token <token>] */
    const esp_console_cmd_t add_cmd = {
        .command = "mcp_srv_add",
        .help = "Add an MCP server.\n"
        " mcp_srv_add --name <name> --url <url> [--token <token>] [--endpoint <ep>] [--description <desc>]",
        .func = mcp_server_add_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&add_cmd));

    /* mcp_srv_remove --name <name> */
    const esp_console_cmd_t remove_cmd = {
        .command = "mcp_srv_remove",
        .help = "Remove an MCP server.\n"
        " mcp_srv_remove --name <name>",
        .func = mcp_server_remove_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&remove_cmd));

    /* mcp_srv_reload */
    const esp_console_cmd_t reload_cmd = {
        .command = "mcp_srv_reload",
        .help = "Reload MCP server config from file",
        .func = mcp_server_reload_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reload_cmd));

    /* mcp_srv_test --name <name> */
    const esp_console_cmd_t test_cmd = {
        .command = "mcp_srv_test",
        .help = "Test connection to an MCP server.\n"
        " mcp_srv_test --name <name>",
        .func = mcp_server_test_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&test_cmd));

    /* mcp_srv_export */
    const esp_console_cmd_t export_cmd = {
        .command = "mcp_srv_export",
        .help = "Export MCP server config (tokens hidden)",
        .func = mcp_server_export_func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&export_cmd));
}
