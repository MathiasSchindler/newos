# GIT

## NAME

git - inspect a local Git repository

## SYNOPSIS

```
git status [--short]
git branch --show-current
git rev-parse --show-toplevel|--git-dir|--abbrev-ref HEAD|HEAD
git ls-files [--cached]
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
- list tracked index paths with `ls-files`
- report a concise local status for modified, deleted, and untracked files
- detect executable-bit changes for regular tracked files
- honor simple root `.gitignore` and `.git/info/exclude` patterns in status
- compute Git blob object IDs with `hash-object`
- clone a clean local worktree or `file://` worktree by copying `.git` metadata
    and tracked files, preserving regular-file executable bits
- clone an ordinary `http://` or `https://` smart-HTTP repository by fetching a
    pack, writing loose objects, creating `origin`, and checking out the selected
    branch
- fetch from `origin` or an explicit HTTP(S) URL with Git upload-pack and update
    `FETCH_HEAD` plus `refs/remotes/origin/*`
- checkout a local branch, remote-tracking branch, full object ID, or `HEAD`
    from loose commit/tree/blob objects
- materialize regular files and symlink blobs during object-based checkout

## LIMITATIONS

- no push, commit, add, merge, protocol v2 negotiation, SSH transport, git://
    transport, shallow clone, partial clone, authentication, pack bitmap/index
    writing, or reflogs
- no submodules, worktrees, sparse checkout, full recursive `.gitignore`
    semantics, rename detection, or staged-vs-unstaged distinction yet
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
git ls-files
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