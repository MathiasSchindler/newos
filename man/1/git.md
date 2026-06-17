# GIT

## NAME

git - inspect a local Git repository

## SYNOPSIS

```
git status [-s|--short|--porcelain] [--color[=WHEN]|--no-color]
git diff [--stat] [--cached|--staged] [--color[=WHEN]|--no-color] [<rev> <rev>|<rev>..<rev>] [--] [path ...]
git branch [--show-current|NAME [START]|-d NAME]
git rev-parse --show-toplevel|--git-dir|--abbrev-ref HEAD|HEAD
git ls-files [--cached|--others] [--exclude-standard] [--] [path ...]
git add [-N|--intent-to-add] [--] path ...
git commit [-m|--message MESSAGE] [--allow-empty]
git log [--oneline] [-N|-n N|--max-count=N] [REV]
git show [--stat] [REV]
git reset [--soft|--mixed|--hard] [REV]
git restore [--staged] [--worktree] [--source REV] [--] path ...
git rm [--cached] [-r] [--] path ...
git clean [-n|--dry-run|-f|--force] [-x] [--] [path ...]
git hash-object FILE ...
git clone SOURCE [DEST]
git fetch [URL] [REF]
git checkout REF
```

## DESCRIPTION

`git` is a small, freestanding-first Git client. It focuses on repository state
that editors, build scripts, and coding agents need for safe workspace
awareness, with first-pass clone, fetch, and checkout support for simple local
and HTTPS workflows.

The tool discovers `.git` by walking up from the current directory, supports
normal `.git` directories and gitfiles, reads `HEAD`, loose refs, packed refs,
and parses common Git index files.

## CURRENT CAPABILITIES

- print the current branch, list local branches, create local branches, and
    delete loose local branches with `branch`
- print the repository root or Git directory with `rev-parse`
- resolve `HEAD` when it is available from a loose or packed ref
- list tracked index paths with `ls-files` or untracked paths with
    `ls-files --others --exclude-standard`
- report a concise local status for modified, deleted, and untracked files,
    including staged-vs-unstaged short status columns when `HEAD` can be read,
    with optional color for human-readable status output
- mark untracked files as intent-to-add with `add -N`, so they are no longer
    reported as untracked and appear in `diff --stat` as additions
- stage regular files, symlinks, directory contents, and tracked deletions with
    `add`
- create first-pass commit objects from the index with `commit -m`, writing tree
    objects and updating the current branch or detached `HEAD`
- print commit history with `log`, including compact `--oneline` output and
    bounded history with `-N`, `-n N`, or `--max-count=N`
- show a commit header and its parent diff with `show`, or a summary with
    `show --stat`
- move `HEAD` with `reset --soft`, update the index with `reset --mixed`, or
    update both index and worktree with `reset --hard`
- restore worktree paths from the index or a named commit, and restore staged
    paths from `HEAD` or `--source REV`, with `restore`
- remove tracked paths from the index and optionally the worktree with `rm` or
    `rm --cached`
- remove untracked files with `clean -f`, preview removals with `clean -n`, and
    include ignored files only when `-x` is supplied
- show working-tree-versus-index or `--cached` index-versus-HEAD diffs as
    whole-file unified patches or `--stat` summaries, including optionally
    colored `+` and `-` change bars
- show commit-to-commit diffs with `git diff A B` or `git diff A..B`
- detect executable-bit changes for regular tracked files
- honor root and nested `.gitignore` files plus `.git/info/exclude` patterns in
    status, `add`, and `ls-files --others --exclude-standard`
- compute Git blob object IDs with `hash-object`
- clone a clean local worktree or `file://` worktree by copying `.git` metadata
    and tracked files, preserving regular-file executable bits
- clone an ordinary `http://` or `https://` smart-HTTP repository by fetching a
    pack, storing it under `.git/objects/pack`, creating `origin`, and checking
    out the selected branch; remote sideband and local pack byte progress are
    streamed to stderr
- fetch from `origin` or an explicit HTTP(S) URL with Git upload-pack and update
    `FETCH_HEAD` plus `refs/remotes/origin/*`, storing the received pack
- checkout a local branch, remote-tracking branch, full object ID, or `HEAD`
    from loose objects or stored pack files
- materialize regular files and symlink blobs during object-based checkout
- remove tracked worktree paths that are absent from the target tree during
    checkout

## LIMITATIONS

- no push, merge, protocol v2 negotiation, SSH transport, git://
    transport, shallow clone, partial clone, authentication, pack bitmap
    writing, pack reuse during network negotiation, or reflogs
- no submodules, worktrees, sparse checkout, rename detection, or interactive
    commit message editing yet
- history traversal follows the first parent only; merge display and graph
    formatting are not implemented
- commit support is intentionally small: it commits the current index, supports
    `-m`/`--message`, uses simple environment-based identity defaults, and does
    not run hooks, sign commits, update reflogs, or clean up intent-to-add-only
    directory shells from tree construction yet
- `reset` does not implement pathspec reset; `restore` supports ordinary exact
    path restoration but not interactive patch mode; `rm` does not perform
    native Git's full safety checks against staged or unstaged local changes
- `clean` operates on untracked files discovered by the current walker; it does
    not implement native Git's full directory-only and ignored-only modes
- recursive `.gitignore` support covers ordinary nested pattern scopes and
    negation ordering but is not a complete byte-for-byte implementation of all
    Git ignore edge cases
- patch diff output uses simple whole-file hunks rather than Git's full hunk
    minimization algorithm
- untracked files are intentionally not included in `diff --stat`; use
    `ls-files --others --exclude-standard` to list them or `add -N` to include
    them as intent-to-add paths
- index support is focused on ordinary v2/v3 entries; v4 support is partial
- status falls back to working-tree-versus-index output when `HEAD` points to
    objects the tool cannot read
- local worktree clone refuses modified or missing tracked source files; network
    clone checks out from fetched objects
- checkout removes tracked paths absent from the target tree but does not yet
    implement native Git's full conflict-safety checks
- SHA-1 repositories only

## EXAMPLES

```
git branch --show-current
git rev-parse --show-toplevel
git status --short
git diff -- src/tools/git.c
git diff HEAD origin/main -- src/tools/git.c
git --no-pager diff --stat -- Makefile man/1/git.md src/tools/git.c
git diff --cached --stat
git ls-files
git ls-files --others --exclude-standard -- src/tools/git
git add src/tools/git.c
git add -N -- src/tools/git
git commit -m "update git tool"
git log --oneline -n 5
git show --stat HEAD
git restore --worktree src/tools/git.c
git restore --staged src/tools/git.c
git reset --hard HEAD
git rm --cached generated.txt
git clean -n
git clean -f -- tests/tmp-output.txt
git hash-object src/tools/git.c
git clone ../project-copy project-copy
git clone https://github.com/MathiasSchindler/pbf-parser.git pbf-parser
git fetch
git checkout main
```

## JSON Output

JSON mode limitation: structured JSON output is not implemented yet. Callers
should treat stdout as the documented text output.

## SEE ALSO

sha1sum, diff