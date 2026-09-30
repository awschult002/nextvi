# How nextvi fits together

This page is for someone who wants to change the editor. Read it before opening `vi.c`.

## 1. It is a unity build

`cbuild.sh` compiles only `vi.c`, and `vi.c` `#include`s every other `.c` file:

```
vi.c
 ├─ vi.h      shared types, globals (extern), prototypes
 ├─ conf.c    compile-time config: filetypes, syntax patterns, keymaps (kmap.h)
 ├─ ex.c      ex (:) commands, options, buffer list, editor-state globals
 ├─ lbuf.c    line buffer: the file as an array of lines, marks, undo/redo
 ├─ led.c     line editor: draws one line, and reads a line of input (insert mode, : prompt)
 ├─ regex.c   regular expressions
 ├─ ren.c     rendering: character index → screen column (tabs, wide chars, bidi)
 ├─ term.c    terminal: raw mode, window size, output buffer, input queue
 └─ uc.c      UTF-8 helpers
```

What this means for you:
- **Everything shares one scope.** Every global *and every file-level `static`* can be seen by every file. A `static` in `term.c` is not private to `term.c`.
- **Names can collide across files.** A new global name can quietly be shadowed by a local that already has the same name somewhere else. Renames therefore run across the whole tree (`scripts/rename.sh`) and are built with `-Wshadow`.
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

Upstream is `kyx0r/nextvi`, which is still active. Keep personal tweaks (key bindings, defaults) as one small commit or patch file on top of upstream, separate from the rename work, so rebasing stays cheap. Renames are replayable with `scripts/rename.sh` and the `scripts/renames-*.tsv` tables.
