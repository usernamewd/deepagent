/*
 * config.c
 *
 * Manual long-option parser for DeepAgent. This module owns initialization,
 * validation, cleanup, and process-wide read-only access to the active Config.
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Config *g_config;

static char *xstrdup(const char *s) {
    char *p;
    if (!s) {
        return NULL;
    }
    p = malloc(strlen(s) + 1);
    if (!p) {
        return NULL;
    }
    strcpy(p, s);
    return p;
}

void config_init(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->provider = xstrdup(DEEPAGENT_PROVIDER_NVIDIA);
    cfg->model = xstrdup(DEEPAGENT_DEFAULT_MODEL);
    cfg->system_prompt = xstrdup(DEEPAGENT_DEFAULT_SYSTEM_PROMPT);
    cfg->max_iterations = 50;
    cfg->max_tokens = 4096;
    cfg->temperature = 0.2;
    cfg->subagent_timeout = 120;
    cfg->color_enabled = 1;
}

void config_free(Config *cfg) {
    if (!cfg) {
        return;
    }
    free(cfg->provider);
    free(cfg->model);
    free(cfg->system_prompt);
    free(cfg->memory_file);
    memset(cfg, 0, sizeof(*cfg));
}

void config_print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --provider <name>        API provider: nvidia or pollinations\n");
    printf("  --model <id>             Model ID string\n");
    printf("  --system <prompt>        System prompt\n");
    printf("  --max-iterations <n>     Max ReAct iterations (default 50)\n");
    printf("  --max-tokens <n>         Max tokens per response (default 4096)\n");
    printf("  --temperature <f>        Sampling temperature (default 0.2)\n");
    printf("  --auto-approve           Skip confirmation prompts\n");
    printf("  --subagent-timeout <n>   Subagent timeout seconds (default 120)\n");
    printf("  --no-color               Disable ANSI colors\n");
    printf("  --verbose                Print raw JSON request/response\n");
    printf("  --memory-file <path>     Override memory storage path\n");
    printf("  --version                Print version and exit\n");
    printf("  --help                   Print usage and exit\n");
}

static int require_value(int argc, char **argv, int *i, const char *opt, char **out) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", opt);
        return 0;
    }
    *i += 1;
    *out = argv[*i];
    return 1;
}

static int parse_int_value(const char *s, int *out) {
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < 0 || v > 1000000L) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_double_value(const char *s, double *out) {
    char *end = NULL;
    double v;
    errno = 0;
    v = strtod(s, &end);
    if (errno || end == s || *end != '\0' || v < 0.0 || v > 2.0) {
        return 0;
    }
    *out = v;
    return 1;
}

static int set_string(char **dst, const char *value) {
    char *copy = xstrdup(value);
    if (!copy) {
        return 0;
    }
    free(*dst);
    *dst = copy;
    return 1;
}

static int set_provider(Config *cfg, const char *provider) {
    int should_use_provider_default;
    if (strcmp(provider, DEEPAGENT_PROVIDER_NVIDIA) != 0 &&
        strcmp(provider, DEEPAGENT_PROVIDER_POLLINATIONS) != 0) {
        fprintf(stderr, "Unsupported provider: %s\n", provider);
        return 0;
    }
    should_use_provider_default = strcmp(cfg->model, DEEPAGENT_DEFAULT_MODEL) == 0 ||
                                  strcmp(cfg->model, DEEPAGENT_DEFAULT_POLLINATIONS_MODEL) == 0;
    if (strcmp(provider, DEEPAGENT_PROVIDER_POLLINATIONS) == 0 &&
        should_use_provider_default) {
        if (!set_string(&cfg->model, DEEPAGENT_DEFAULT_POLLINATIONS_MODEL)) {
            return 0;
        }
    }
    if (strcmp(provider, DEEPAGENT_PROVIDER_NVIDIA) == 0 &&
        should_use_provider_default) {
        if (!set_string(&cfg->model, DEEPAGENT_DEFAULT_MODEL)) {
            return 0;
        }
    }
    return set_string(&cfg->provider, provider);
}

int config_parse(Config *cfg, int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        char *value = NULL;
        if (strcmp(argv[i], "--provider") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !set_provider(cfg, value)) return 0;
        } else if (strcmp(argv[i], "--model") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !set_string(&cfg->model, value)) return 0;
        } else if (strcmp(argv[i], "--system") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !set_string(&cfg->system_prompt, value)) return 0;
        } else if (strcmp(argv[i], "--max-iterations") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !parse_int_value(value, &cfg->max_iterations)) return 0;
        } else if (strcmp(argv[i], "--max-tokens") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !parse_int_value(value, &cfg->max_tokens)) return 0;
        } else if (strcmp(argv[i], "--temperature") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !parse_double_value(value, &cfg->temperature)) return 0;
        } else if (strcmp(argv[i], "--auto-approve") == 0) {
            cfg->auto_approve = 1;
        } else if (strcmp(argv[i], "--subagent-timeout") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !parse_int_value(value, &cfg->subagent_timeout)) return 0;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            cfg->color_enabled = 0;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;
        } else if (strcmp(argv[i], "--memory-file") == 0) {
            if (!require_value(argc, argv, &i, argv[i], &value) || !set_string(&cfg->memory_file, value)) return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("DeepAgent %s\n", DEEPAGENT_VERSION);
            cfg->exit_after_parse = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            config_print_usage(argv[0]);
            cfg->exit_after_parse = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            config_print_usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

const Config *config_global(void) {
    return g_config;
}

void config_set_global(const Config *cfg) {
    g_config = cfg;
}

const char *config_provider_display_name(const Config *cfg) {
    if (cfg && cfg->provider && strcmp(cfg->provider, DEEPAGENT_PROVIDER_POLLINATIONS) == 0) {
        return "Pollinations";
    }
    return "NVIDIA NIM";
}

const char *config_provider_endpoint(const Config *cfg) {
    if (cfg && cfg->provider && strcmp(cfg->provider, DEEPAGENT_PROVIDER_POLLINATIONS) == 0) {
        return "https://gen.pollinations.ai/v1/chat/completions";
    }
    return "https://integrate.api.nvidia.com/v1/chat/completions";
}

const char *config_provider_env_var(const Config *cfg) {
    if (cfg && cfg->provider && strcmp(cfg->provider, DEEPAGENT_PROVIDER_POLLINATIONS) == 0) {
        return "POLLINATIONS_API_KEY";
    }
    return "NVIDIA_API_KEY";
}

int config_api_key_valid(const Config *cfg, const char *api_key) {
    if (!api_key || !*api_key) {
        return 0;
    }
    if (cfg && cfg->provider && strcmp(cfg->provider, DEEPAGENT_PROVIDER_NVIDIA) == 0) {
        return strncmp(api_key, "nvapi-", 6) == 0;
    }
    return 1;
}
