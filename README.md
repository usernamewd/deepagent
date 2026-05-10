# DeepAgent

DeepAgent is a Linux CLI AI coding agent written in C11. It uses the NVIDIA NIM cloud inference API through the OpenAI-compatible chat completions endpoint and exposes a local tool system for file editing, shell execution, code search, HTTP requests, process management, system inspection, persistent memory, and one-level subagents.

## Dependencies

- GCC with C11 support
- GNU Make
- libcurl development headers (`libcurl4-openssl-dev` on Debian/Ubuntu)
- POSIX/Linux libc and headers
- cJSON, downloaded automatically from <https://github.com/DaveGamble/cJSON> by `scripts/setup.sh`

## NVIDIA NIM API key

1. Visit <https://build.nvidia.com/>.
2. Sign in with an NVIDIA account.
3. Open the API key section and create a key for NVIDIA NIM.
4. Export it before running DeepAgent:

```sh
export NVIDIA_API_KEY='nvapi-your-key-here'
```

DeepAgent exits with a clear error if `NVIDIA_API_KEY` is absent.

## Build

```sh
make
sudo make install
```

The build produces `build/deepagent`. The Makefile downloads cJSON automatically when the vendor files are missing or still contain the placeholder note.

## Usage

```sh
export NVIDIA_API_KEY='nvapi-your-key-here'
./build/deepagent
./build/deepagent --model deepseek-ai/deepseek-v4-pro --max-iterations 80
./build/deepagent --auto-approve --temperature 0.1 --memory-file ./memory.json
./build/deepagent --system "You are a cautious code reviewer."
```

Input is multi-line. Enter a single `.` on its own line to submit the prompt.

## Tool reference

| Name | Description | Parameters |
| --- | --- | --- |
| `read_file` | Read an entire file into text | `path` |
| `write_file` | Create or overwrite a file, creating parent directories | `path`, `content` |
| `append_file` | Append text to a file | `path`, `content` |
| `delete_file` | Delete a file after confirmation unless auto-approved | `path` |
| `list_directory` | List entries, optionally recursively | `path`, `recursive` |
| `stat_file` | Return JSON metadata for a path | `path` |
| `move_file` | Rename a file, copying across filesystems if needed | `src`, `dst` |
| `copy_file` | Binary copy preserving mode bits | `src`, `dst` |
| `search_files` | Recursive filename match using `fnmatch` | `directory`, `pattern` |
| `make_directory` | `mkdir -p` implementation | `path` |
| `read_file_range` | Stream selected 1-indexed line range | `path`, `start_line`, `end_line` |
| `grep_search` | POSIX extended-regex search with context | `path`, `pattern`, `context_lines` |
| `find_replace` | Replace text in a file | `path`, `old_string`, `new_string`, `replace_all` |
| `diff_files` | Unified diff between two files | `path_a`, `path_b` |
| `patch_file` | Apply a unified diff to a path | `path`, `unified_diff` |
| `count_lines` | Efficient line count | `path` |
| `truncate_file` | Keep first N lines | `path`, `keep_lines` |
| `insert_lines` | Insert content after a line number | `path`, `after_line`, `content` |
| `bash` | Run a shell command with timeout and JSON result | `command`, `timeout_seconds` |
| `background_process` | Launch a tracked background command | `command` |
| `kill_process` | Signal a tracked background process | `pid`, `signal` |
| `read_process_output` | Read available tracked process output | `pid` |
| `list_processes` | List tracked background processes | none |
| `get_process_status` | Check tracked process status | `pid` |
| `http_get` | HTTP GET with optional JSON headers | `url`, `headers_json` |
| `http_post` | HTTP POST with optional JSON headers | `url`, `body`, `content_type`, `headers_json` |
| `download_file` | Download URL to a destination path | `url`, `destination_path` |
| `url_encode` | URL-encode a string | `string` |
| `url_decode` | URL-decode a string | `string` |
| `get_env` | Read an environment variable | `variable_name` |
| `set_env` | Set an environment variable for this process and children | `variable_name`, `value` |
| `system_info` | Return OS, kernel, CPU, RAM, and disk JSON | none |
| `which` | Find an executable in `PATH` | `binary_name` |
| `get_working_directory` | Print current directory | none |
| `set_working_directory` | Change current directory | `path` |
| `list_path` | List `PATH` directories | none |
| `get_hostname` | Return system hostname | none |
| `get_username` | Return current username | none |
| `remember` | Store a key/value in persistent memory | `key`, `value` |
| `recall` | Retrieve a memory value | `key` |
| `forget` | Delete a memory key | `key` |
| `list_memory` | List all memory entries | none |
| `clear_memory` | Clear memory after confirmation unless auto-approved | none |
| `spawn_subagent` | Run a one-level child agent on a task | `task`, `context`, `model` |

## Subagents

`spawn_subagent` creates a fresh in-process agent state with its own message history and the same tool registry. Optional context is injected as a system message, the task is submitted as the first user message, and the final assistant response is returned to the parent. Subagents are limited to depth 1 to prevent recursive explosion and time out after `--subagent-timeout` seconds.

## Safety

Unless `--auto-approve` is set, DeepAgent asks for confirmation before the first shell command, every file deletion, process kill, and memory clear operation. Shell execution always blocks dangerous patterns even with auto-approval: root filesystem deletion, fork bombs, raw `/dev/sda` or `/dev/nvme` access, and `mkfs.` formatting commands.

## Supported models

| Model | Notes | Context |
| --- | --- | --- |
| `deepseek-ai/deepseek-v4-flash` | Default; coding and agentic tool calling | 1M tokens |
| `deepseek-ai/deepseek-v4-pro` | Larger DeepSeek MoE model | 1M tokens |
| `meta/llama-3.1-70b-instruct` | NIM tool-calling capable instruct model | model-dependent |
| `qwen/qwen3-next-instruct` | Agentic coding model | 256K tokens |
| `zhipuai/glm-4.7` | Multilingual coding and tool use | model-dependent |
