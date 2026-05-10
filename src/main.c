/*
 * main.c
 *
 * DeepAgent entry point. Parses CLI configuration, validates NVIDIA_API_KEY,
 * initializes curl, UI, and tool registry, then starts the interactive REPL.
 */

#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "config.h"
#include "tools.h"
#include "ui.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    Config cfg;
    AgentState agent;
    const char *api_key;
    int rc = 1;
    config_init(&cfg);
    if (!config_parse(&cfg, argc, argv)) {
        config_free(&cfg);
        return 2;
    }
    if (cfg.exit_after_parse) {
        config_free(&cfg);
        return 0;
    }
    ui_set_color_enabled(cfg.color_enabled);
    config_set_global(&cfg);
    api_key = getenv("NVIDIA_API_KEY");
    if (!api_key || strncmp(api_key, "nvapi-", 6) != 0) {
        ui_print_error("Set NVIDIA_API_KEY to a valid NVIDIA NIM key beginning with nvapi-");
        config_free(&cfg);
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    tools_register_all();
    ui_print_system("DeepAgent %s using model %s", DEEPAGENT_VERSION, cfg.model);
    if (agent_init(&agent, &cfg)) {
        rc = agent_run_interactive(&agent) ? 0 : 1;
        agent_free(&agent);
    } else {
        ui_print_error("failed to initialize agent");
    }
    tools_free_all();
    curl_global_cleanup();
    config_free(&cfg);
    return rc;
}
