#include "platform.h"
#include "runtime.h"

void *newos_wasm_alloc(size_t size);
void newos_wasm_free(void *ptr);
void newos_wasm_reset(void);
int newos_wasm_write_file(const char *path, const unsigned char *data, size_t size);
int newos_wasm_read_file(const char *path, unsigned char *buffer, size_t capacity);
int newos_wasm_file_size(const char *path);
unsigned char *newos_wasm_stdout_ptr(void);
size_t newos_wasm_stdout_size(void);
unsigned char *newos_wasm_stderr_ptr(void);
size_t newos_wasm_stderr_size(void);
void newos_wasm_begin_command(void);

#define main git_tool_main
#include "../../../src/tools/git.c"
#undef main

#define WASM_ARG_CAPACITY 64U
#define WASM_ARG_TEXT_CAPACITY 8192U

static int wasm_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int wasm_parse_line(char *line, int *argc_out, char **argv_out, size_t max_args) {
    size_t argc = 0U;
    char *cursor = line;

    while (*cursor != '\0') {
        char quote = '\0';

        while (wasm_is_space(*cursor)) cursor += 1;
        if (*cursor == '\0') break;
        if (argc >= max_args) return -1;
        argv_out[argc++] = cursor;
        while (*cursor != '\0') {
            if (quote != '\0') {
                if (*cursor == quote) {
                    char *tail = cursor;
                    while (tail[1] != '\0') {
                        *tail = tail[1];
                        tail += 1;
                    }
                    *tail = '\0';
                    quote = '\0';
                    continue;
                }
            } else if (*cursor == '\'' || *cursor == '"') {
                quote = *cursor;
                {
                    char *tail = cursor;
                    while (tail[1] != '\0') {
                        *tail = tail[1];
                        tail += 1;
                    }
                    *tail = '\0';
                    continue;
                }
            } else if (wasm_is_space(*cursor)) {
                *cursor++ = '\0';
                break;
            }
            cursor += 1;
        }
        if (quote != '\0') return -1;
    }
    *argc_out = (int)argc;
    return 0;
}

int newos_git_run_line(const char *command_line) {
    char text[WASM_ARG_TEXT_CAPACITY];
    char *argv[WASM_ARG_CAPACITY];
    int argc = 0;
    size_t length;

    if (command_line == 0) return 1;
    length = rt_strlen(command_line);
    if (length + 1U > sizeof(text)) return 1;
    memcpy(text, command_line, length + 1U);
    newos_wasm_begin_command();
    argv[0] = (char *)"git";
    if (wasm_parse_line(text, &argc, argv + 1U, WASM_ARG_CAPACITY - 1U) != 0) {
        (void)rt_write_line(2, "wasm: cannot parse command line");
        return 1;
    }
    return git_tool_main(argc + 1, argv);
}

__attribute__((export_name("newos_git_clone_from_pack")))
int newos_git_clone_from_pack(const char *remote_url, const char *destination, const char *selected_ref, const char *selected_oid_hex, const unsigned char *pack_data, size_t pack_size) {
    GitRepo repo;
    GitPack pack;
    char local_ref[GIT_REF_CAPACITY];
    const char *branch_name;
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int result = 1;

    newos_wasm_begin_command();
    rt_memset(&repo, 0, sizeof(repo));
    rt_memset(&pack, 0, sizeof(pack));
    if (remote_url == 0 || destination == 0 || selected_ref == 0 || selected_oid_hex == 0 || pack_data == 0 || pack_size == 0U) {
        tool_write_error("git", "wasm clone bridge received invalid input", 0);
        return 1;
    }
    git_progress_clone_into(destination);
    if (git_parse_oid_hex_n(selected_oid_hex, GIT_OBJECT_HEX_SIZE, selected_oid) != 0) {
        tool_write_error("git", "wasm clone bridge received invalid oid: ", selected_oid_hex);
        return 1;
    }
    if (platform_make_directory(destination, 0755U) != 0 || git_init_empty_repo_at(destination, &repo) != 0) {
        tool_write_error("git", "cannot create destination: ", destination);
        return 1;
    }
    git_progress_line("Unpacking objects...");
    if (git_parse_pack(pack_data, pack_size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0 || git_write_pack_file(&repo, pack_data, pack_size, &pack) != 0) {
        tool_write_error("git", "cannot import remote pack", 0);
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    git_progress_count_line("Received objects: ", pack.count);
    branch_name = git_branch_from_ref(selected_ref);
    if (branch_name == 0 || branch_name[0] == '\0') {
        branch_name = "main";
    }
    if (git_copy(local_ref, sizeof(local_ref), "refs/heads/") != 0 || rt_strlen(local_ref) + rt_strlen(branch_name) >= sizeof(local_ref)) {
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    rt_copy_string(local_ref + rt_strlen(local_ref), sizeof(local_ref) - rt_strlen(local_ref), branch_name);
    git_progress_line("Checking out files...");
    if (git_write_ref_oid(&repo, local_ref, selected_oid) != 0 || git_write_head_ref(&repo, local_ref) != 0 || git_write_clone_config(&repo, remote_url, branch_name) != 0 || git_write_fetch_head(&repo, remote_url, selected_ref, selected_oid) != 0 || git_checkout_commit_to_worktree(&repo, selected_oid, &pack) != 0) {
        tool_write_error("git", "checkout failed: ", destination);
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    rt_write_cstr(1, "Cloned remote repository to ");
    rt_write_line(1, destination);
    result = 0;
done:
    git_pack_destroy(&pack);
    return result;
}
