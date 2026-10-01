# How nextvi fits together

This page is for someone who wants to change the editor. Read it before opening `vi.c`.

## 1. It is a unity build

`cbuild.sh` (or `make`) compiles only `vi.c`, and `vi.c` `#include`s every other `.c` file. This branch is upstream plus the patches listed in [`PATCHES.md`](PATCHES.md), which add `agent.c`, `lsp.c` and `treesitter.c`:

```
vi.c          main(), the normal-mode loop, screen redraw (vi_drawrow, paint thread)
 ├─ cJSON.c   vendored JSON library (used by agent)
 ├─ vi.h      shared types, globals (extern), prototypes
 ├─ conf.c    compile-time config: filetypes, syntax patterns, keymaps (kmap.h), tree-sitter setup
 ├─ agent.h
 ├─ ex.c      ex (:) commands, options, buffer list, editor-state globals; help text in exspec.h (generated from README)
 ├─ agent.c   agent patch: talks to a chat endpoint over curl
 ├─ lbuf.c    line buffer: the file as an array of lines, marks, undo/redo; threaded search
 ├─ led.c     line editor: draws one line, reads a line of input (insert mode, : prompt, tab complete)
 ├─ regex.c   regular expressions
 ├─ ren.c     rendering: character index → screen column (tabs, wide chars, bidi); per-thread rstate
 ├─ term.c    terminal: raw mode, window size, output buffer, input queue, cmd_pipe
 ├─ uc.c      UTF-8 helpers
 ├─ treesitter.c  tree-sitter syntax highlighting (sources pinned in the build)
 └─ lsp.c     language-server client (uses jsmn.h)
```

What this means for you:
- **Everything shares one scope.** Every global *and every file-level `static`* can be seen by every file. A `static` in `term.c` is not private to `term.c`.
- **Names can collide across files.** A new global name can quietly be shadowed by a local that already has the same name somewhere else. Renames therefore run across the whole tree (`scripts/rename.sh`) and are built with `-Wshadow`.
- **Threads share it too.** `threaded_redraw` adds a paint thread in `vi.c` and `threaded_search` adds search threads in `lbuf.c`. Any global they touch is shared memory. Per-thread state is marked `__thread` (for example `rstate`). Treat any new global used from the redraw or search path as a possible data race.
- **Keys are code.** There is no runtime key remapping. Bindings live in the big `switch` in `vi.c`, and personal changes belong in source (see *Personal changes* below).

## 2. Where state lives

The cursor and view are globals in `ex.c`: `cursor_row`, `cursor_off` (a **character index** into the line, not bytes or a screen column), and `view_top_row`. Terminal size is `term_rows`/`term_cols` in `term.c`. The full old-to-new name map is in [`RENAMES.md`](RENAMES.md).

<!-- TODO: keypress → vi() switch → lbuf edit → vi_drawrow/led → term output walkthrough -->

## 3. Style: suckless *and* readable

Names, constants and line breaks cost the compiler nothing. The rules:

1. **Names describe what they hold.** Short is fine (`cursor_row`), cryptic is not (`xrow`). Aim for 1–3 words.
2. **Flags and magic numbers get named constants**, for example `LN_ABS` instead of `2`.
3. **Split long expressions** into named intermediate values when that makes the intent readable.
4. **Explain pointer arithmetic, don't replace it.** Add a comment saying what each pointer marks, and keep the tight code.
5. **Long functions can be split** (for example `vi_drawrow` or the key switch) into static helpers with clear names.
6. **Prove it didn't change behavior. There are two tiers:**
   - *Renames, comments, named constants:* the binary must stay identical. Build with `-O2`, strip it, drop the build-id note, and compare against the base commit.
   - *Splitting functions or the key switch:* the binary will legitimately differ, because inlining and register allocation change. Instead, run the `tests/` harness on the old and new builds. It runs `EXINIT` + `&` keystroke scripts in a fixed-size pty, renders the output through a terminal emulator (`pyte`), and diffs the final screen grid and any written files. Don't diff raw pty bytes. Also check the size of the redraw path and take the median of several timing runs, since that code runs on every keystroke.
7. **Comments follow [`DOXYGEN.md`](DOXYGEN.md).**

## 4. Personal changes and upstream

Upstream is `kyx0r/nextvi`, which is still active. **Order for an upstream merge: patch first, then rename.** The patch scripts look for the original source text, so apply them to clean upstream (see `PATCHES.md`), then replay the rename tables (see `RENAMES.md`). Keep personal tweaks (key bindings, defaults) as one small commit or patch file on top of upstream, separate from the rename work, so rebasing stays cheap. Renames are replayable with `scripts/rename.sh` and the `scripts/renames-*.tsv` tables.

**Branches on the fork.** `master` is a plain mirror of `kyx0r/nextvi`: sync it from upstream and never commit to it. `AWS` is Alex's build. It starts at the same upstream commit as `master`, then come the patches (`PATCHES.md`), then the renames and comments (`RENAMES.md`), then Alex's configuration. Branches merge into `AWS` with a merge commit, never squash or rebase, because `PATCHES.md` and this doc cite commits by SHA.

**Rebuild `AWS` when upstream changes; don't merge `master` into it.** Once the renames are in, almost every upstream commit touches a renamed line, so merging would conflict everywhere. Instead:

1. Sync `master` with `kyx0r`.
2. Run `scripts/rebuild-aws.sh master`. It works on a new local branch in its own worktree and follows `patches/aws/series`. It runs the 16 patch scripts from the pinned `patches` commit, applies the fix commits with `git am`, builds that tree and saves the binary as the **new reference** (a stored sha such as `f2aa0fe9…efaf` is only valid for one upstream commit), replays the four rename tables, applies the `LN_*` and comment edits, and checks `unmapped.sh`. After the reference step, every step that changes a `.c` or `.h` file must keep the stripped binary identical. The script stops at the first failing anchor, conflict or check and names the step. It never pushes.
3. Run `rebuild-aws.sh -t` to print the commands that tag the old `AWS` (for example `AWS-1635521b`) and move `AWS` to the new branch. Check them, then run them.

A rebuild gives every commit a new SHA. SHAs cited in the docs point into the history before the rebuild, and the old tag keeps them reachable.

**Where a rebuild needs a person:** conflicts in the `git am` files when upstream changed nearby code, updating `expect` lines in `series` and `unmapped.expected` when anchors move, and exporting any new hand edit on `AWS` into `patches/aws/` with `git format-patch` (otherwise the next rebuild drops it). To keep rebuilds cheap, put configuration in as few commits as possible, and keep it out of the files the patches touch most.

