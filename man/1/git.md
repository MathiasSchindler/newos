# GIT

## NAME

git - inspect a local Git repository

## SYNOPSIS

```
git status [-s|--short|--porcelain] [--color[=WHEN]|--no-color]
git diff --stat [--color[=WHEN]|--no-color] [--] [path ...]
git branch --show-current
git rev-parse --show-toplevel|--git-dir|--abbrev-ref HEAD|HEAD
git ls-files [--cached|--others] [--exclude-standard] [--] [path ...]
git add -N|--intent-to-add [--] path ...
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

- print the current branch with `branch --show-current`
- print the repository root or Git directory with `rev-parse`
- resolve `HEAD` when it is available from a loose or packed ref
- list tracked index paths with `ls-files` or untracked paths with
    `ls-files --others --exclude-standard`
- report a concise local status for modified, deleted, and untracked files,
    with optional color for human-readable status output
- mark untracked files as intent-to-add with `add -N`, so they are no longer
    reported as untracked and appear in `diff --stat` as additions
- show a working-tree-versus-index `diff --stat` summary for tracked paths,
    including optionally colored `+` and `-` change bars
- detect executable-bit changes for regular tracked files
- honor simple root `.gitignore` and `.git/info/exclude` patterns in status
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

## LIMITATIONS

- no push, commit, content-staging add, merge, protocol v2 negotiation, SSH transport, git://
    transport, shallow clone, partial clone, authentication, pack bitmap
    writing, pack reuse during network negotiation, or reflogs
- no submodules, worktrees, sparse checkout, full recursive `.gitignore`
    semantics, rename detection, or staged-vs-unstaged distinction yet
- diff support is currently limited to `diff --stat` against the index; full
    patch output and commit-to-commit diffs are not implemented yet
- untracked files are intentionally not included in `diff --stat`; use
    `ls-files --others --exclude-standard` to list them or `add -N` to include
    them as intent-to-add paths
- index support is focused on ordinary v2/v3 entries; v4 support is partial
- status compares regular working-tree files against the index, not against HEAD
- local worktree clone refuses modified or missing tracked source files; network
    clone checks out from fetched objects
- checkout writes files from the target tree but does not remove paths that are
    absent from that tree yet
- SHA-1 repositories only

## EXAMPLES

```
git branch --show-current
git rev-parse --show-toplevel
git status --short
git --no-pager diff --stat -- Makefile man/1/git.md src/tools/git.c
git ls-files
git ls-files --others --exclude-standard -- src/tools/git
git add -N -- src/tools/git
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