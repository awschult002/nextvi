# Applied patches

This branch is upstream `kyx0r/nextvi` master `1635521b` with 16 scripts from `kyx0r`'s `patches` branch applied, one commit per patch, followed by fix commits.

**Always patch first, then rename.** The patch scripts find their place by searching for the original source text, so they miss once names have changed. After patching, replay the rename tables (see `RENAMES.md`); `scripts/unmapped.sh` then lists any globals the patches added that need new rows.

## Order that works

1. `lsp`
2. `rstr`
3. `threaded_redraw`
4. `threaded_search`
5. `visual`
6. `incsearch`
7. `detect_indent`
8. `tab_complete`
9. `linewrap_v2`
10. `filetype_shebang`
11. `treesitter`
12. `agent`
13. `alternate-w-behaviour`
14. `writeall`
15. `spell`
16. `64bit` (last)

Ordering constraints found:
- `visual` and `linewrap_v2` have compatibility sections that only apply if `lsp` and `threaded_redraw` are already in, so the small patches can't all go first.
- `detect_indent` and `tab_complete` must go before `linewrap_v2`. Otherwise `detect_indent`'s `EO()` line lands inside `linewrap_v2`'s block and the build fails.

## Anchors that reported failure

| Patch | Anchor | Result |
|---|---|---|
| `tab_complete` | `led.c:813` | The script trips over its own compatibility sections, but the final code is correct. No hand edit. |
| `treesitter` | `vi.c:170` | The line no longer exists after `linewrap_v2`, and the patch's own `linewrap_v2` section makes the same change. No hand edit. |
| `agent` | `ex.c:1707` | `EO(ar) EO(gr)` was skipped and `_EO(aco)` landed in the wrong spot. Fixed by hand (`15ac620b`). |

Note: a script can exit OK after a failed anchor, having written only part of its changes. `agent` did this in an earlier trial run and never created `agent.c`. Check each patch's diff against what its script says it changes.

## Fix commits

| Commit | Fix |
|---|---|
| `8bc4fcae` | conf.c ex-command highlight: restore `sw\|et\|idt\|` and `tc\|`, which `linewrap_v2` dropped when it replaced the whole line |
| `15ac620b` | `agent`: add `EO(ar) EO(gr)` and move `_EO(aco)` to where it belongs |
| `9f597373` | conf.c ex-command highlight: merge in names clobbered by `agent` (including master's `m!?`) |
| `58fcd006` | `64bit`: keep vendored `cJSON` at its original `int` types (it's included before `s64` is defined) |
| `e8243526` | `spell_cmp` must return `int` for `qsort` |
| `83d2a07d` | `ts_capture_cmp` must return `int` for `qsort` |
| `37490cd5` | `agent.c`: keep pipe fds, pid, wait status and signal-handler pointers `int` |
| `4a9dff40` | Pin tree-sitter v0.25.10 at `da6fe9be` and tree-sitter-c v0.24.1 at `7fa1be1b`. Cached and freshly cloned sources must both match the pin with no local changes, or the build fails. |
| `1c6d0892` | `64bit`: restore the `exspec.h` help text, which is generated from the README. `64bit` had turned `int` into `s64` in the examples and changed ex's `%d` range (delete-all) into `%ld`. |
| `22b2d5b9` | `64bit`: `term.c` `cmd_pipe` now waits into a local `int`. Before this, `waitpid` wrote only half of a 64-bit status, which worked only because callers zero it first and x86-64 is little-endian. |

## Build and verification

- `make` builds the same binary as `sh ./cbuild.sh build` (compared after `strip`), with the same flags. `scripts/rebuild-aws.sh` checks both the flag sets and the binaries.
- `cbuild.sh` builds with `-std=c99 -pthread` and the warning flags; the reference binary below is built without them (`cc vi.c <tree-sitter sources> -D_DEFAULT_SOURCE -I... -O2 -D_POSIX_C_SOURCE=200809L`), so the two binaries differ. Each is stable on its own.
- `sh ./cbuild.sh build` passes. `-Wall` leaves one warning, at `ren.c:133`. It's a false positive: it only fires for a line longer than 2^63 characters. The 32-bit code has the same shape, and `64bit` just made GCC's size check notice it.
- The first build downloads the pinned tree-sitter sources, so builds are reproducible.
- Every patch commit was built and given a quick session test.
- The final binary passed normal and ex editing, `:wqa`, tab completion, spell replace, and a substitution over 300k lines.
- The reference binary for the rename check on this branch is `/workspace/scratch/nextvi-patched-base.bin` (sha256 `f2aa0fe9…efaf`, rebuilt at `22b2d5b9`). Two separate builds are byte-identical, and the rename replay is checked against it, not `b1c78886`.

## Rebuilding

`scripts/rebuild-aws.sh <upstream-ref>` replays everything on this page, plus the renames and docs, from `patches/aws/`: the patch scripts come from the pinned `patches` commit `08a66e3c`, and the hand edits are `git format-patch` files. The known anchor reports in the table above are listed as `expect` lines in `patches/aws/series`; any other report stops the rebuild.

## Dependencies

- Build: the first build downloads tree-sitter over the network, pinned to the commits above.
- Runtime, all optional: `agent` needs `curl` and a chat endpoint, `lsp` needs a language server (for example `clangd`), and `spell` needs `aspell`.

## Reviewed

The `64bit` edits in `lsp.c`, `jsmn.h`, `treesitter.c`, `agent.c` and `term.c` were reviewed line by line. The two real bugs found are fixed above. `-Wformat=2` is clean, and the fd and pid narrowings only carry small values.

## Not tested yet

- `lsp` with a real language server
- `agent` with a real endpoint
- threaded redraw under load (threads share every global in the unity build, so watch for data races)

## Declined

Alex decided against `key_remap_cmds` (2026-09-30). Key bindings stay in source, in the key switch in `vi.c`.

For reference: `key_remap_cmds` adds `:im`, `:im!`, `:nm` and `:nm!` to map or unmap single keys per keymap at runtime, via a new `map_read()`. On top of the 16 its code applies cleanly and builds, but its conf.c highlight edit needs the same merge fix as above. Keys read inside other patches (for example tab-complete's loop) aren't remapped.
