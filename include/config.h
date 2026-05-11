#ifndef DEEPAGENT_CONFIG_H
#define DEEPAGENT_CONFIG_H

#define DEEPAGENT_VERSION "1.0.0"
#define DEEPAGENT_PROVIDER_NVIDIA "nvidia"
#define DEEPAGENT_PROVIDER_POLLINATIONS "pollinations"
#define DEEPAGENT_DEFAULT_MODEL "deepseek-ai/deepseek-v4-flash"
#define DEEPAGENT_DEFAULT_POLLINATIONS_MODEL "openai"
#define DEEPAGENT_DEFAULT_SYSTEM_PROMPT "You are DeepAgent, an expert AI coding assistant running in a Linux terminal.\nYou have access to a comprehensive set of tools for reading and writing files,\nexecuting shell commands, searching code, managing processes, making HTTP \nrequests, and persisting information to memory. Always prefer using tools over \nguessing. Think step by step before acting. When writing code, always read any \nrelevant existing files first. When a tool returns an error, analyze the error \nand try an alternative approach."

typedef struct {
    char *provider;
    char *model;
    char *system_prompt;
    int max_iterations;
    int max_tokens;
    double temperature;
    int auto_approve;
    int subagent_timeout;
    int color_enabled;
    int verbose;
    char *memory_file;
    int exit_after_parse;
} Config;

void config_init(Config *cfg);
void config_free(Config *cfg);
int config_parse(Config *cfg, int argc, char **argv);
void config_print_usage(const char *prog);
const Config *config_global(void);
void config_set_global(const Config *cfg);
const char *config_provider_display_name(const Config *cfg);
const char *config_provider_endpoint(const Config *cfg);
const char *config_provider_env_var(const Config *cfg);
int config_api_key_valid(const Config *cfg, const char *api_key);

#endif
