# Rename map

Old names from upstream `kyx0r/nextvi` and their new names. On this branch the base is `patched/upstream-1635521b` (upstream `1635521b` + the patches in `PATCHES.md`); the binary reference for the rename check is `f2aa0fe9…efaf`. The tables were first written against `b1c78886`. Generated from `scripts/renames-globals.tsv`; replay with `scripts/rename.sh` on a fresh upstream.

### Replaying on a fresh upstream

Apply the patches first (see `PATCHES.md`), then run the three tables in this order. `rename.sh` handles the `EO()` macro change itself, so no cherry-pick is needed.

```sh
sh scripts/rename.sh scripts/renames-globals.tsv
sh scripts/rename.sh scripts/renames-term.tsv
sh scripts/rename.sh scripts/renames-vi-lnum.tsv
sh ./cbuild.sh
```

Reruns are safe. A row whose old name is gone and whose new name is present is reported as "already applied" and skipped. If both names are present, that's a real collision, and the script stops before editing anything.

After the last table, `rename.sh` runs `scripts/unmapped.sh`. It lists every `extern` global in `vi.h` that no table renames and that isn't in `scripts/renames-keep.txt` (the names deliberately left alone). `scripts/renames-allow.txt` lists known false matches, for example treesitter's `capture_n` struct member, so they aren't flagged as collisions. String literals are never renamed. After an upstream merge, that list tells you which rows to add. The `LN_*` constants and the Doxygen comments aren't in the tables; they come over by merging their commits.

## Globals (`scripts/renames-globals.tsv`)


| Old | New | Defined in |
|---|---|---|
| `xai` | `opt_autoindent` | ex.c |
| `xic` | `opt_ignorecase` | ex.c |
| `xhl` | `opt_syntax_hl` | ex.c |
| `xhll` | `opt_hl_line` | ex.c |
| `xhlw` | `opt_hl_word` | ex.c |
| `xhlp` | `opt_hl_pair` | ex.c |
| `xhlr` | `opt_hl_reverse` | ex.c |
| `xled` | `opt_line_editor` | ex.c |
| `xtd` | `opt_text_dir` | ex.c |
| `xshape` | `opt_shaping` | ex.c |
| `xorder` | `opt_reorder` | ex.c |
| `xts` | `opt_tabstop` | ex.c |
| `xish` | `opt_interactive_shell` | ex.c |
| `xgrp` | `opt_search_group` | ex.c |
| `xpac` | `opt_print_autocomplete` | ex.c |
| `xmpt` | `opt_multiline_prompt` | ex.c |
| `xpr` | `opt_print_reg` | ex.c |
| `xlim` | `opt_render_limit` | ex.c |
| `xseq` | `opt_undo_seq` | ex.c |
| `xerr` | `opt_error_mode` | ex.c |
| `xfr` | `opt_find_reg` | ex.c |
| `xrr` | `opt_record_reg` | ex.c |
| `xvis` | `opt_startup_flags` | ex.c |
| `xleft` | `opt_left_col` | ex.c |
| `xrow` | `cursor_row` | ex.c |
| `xoff` | `cursor_off` | ex.c |
| `xtop` | `view_top_row` | ex.c |
| `xquit` | `quit_state` | ex.c |
| `xbufcur` | `buf_count` | ex.c |
| `xgrec` | `vi_ex_depth` | ex.c |
| `xexec_dep` | `ex_exec_depth` | ex.c |
| `xkmap` | `cur_keymap` | ex.c |
| `xkmap_alt` | `keymap_alt` | ex.c |
| `xkwddir` | `search_dir` | ex.c |
| `xkwdcnt` | `search_changes` | ex.c |
| `xkwdrs` | `search_rset` | ex.c |
| `xacreg` | `autocomplete_filter` | ex.c |
| `xpln` | `print_newline` | ex.c |
| `xsep` | `ex_separator` | ex.c |
| `xesc` | `ex_escape` | ex.c |
| `xregs` | `str_registers` | ex.c |
| `xregs_n` | `str_registers_n` | ex.c |
| `xdefreg` | `default_reg` | ex.c |
| `ex_buf` | `cur_buf` | ex.c |
| `ex_pbuf` | `prev_buf` | ex.c |
| `xbufsmax` | `bufs_max` | ex.c |
| `xbufsalloc` | `bufs_alloc` | ex.c |
| `xgdep` | `global_depth` | ex.c |
| `xexp` | `ex_expand_char` | ex.c |
| `xexe` | `ex_shell_char` | ex.c |
| `xcid` | `capture_status` | ex.c |
| `xcid_n` | `capture_n` | ex.c |
| `xcid_keep` | `capture_keep` | ex.c |
| `xqprop` | `quit_propagate` | ex.c |
| `xpret` | `ex_prev_ret` | ex.c |

## Terminal globals (`scripts/renames-term.tsv`)

| Old | New | Defined in |
|---|---|---|
| `xrows` | `term_rows` | term.c |
| `xcols` | `term_cols` | term.c |
| `tibuf` | `term_inbuf` | term.c |
| `tibuf_pos` | `term_inbuf_pos` | term.c |
| `tibuf_cnt` | `term_inbuf_count` | term.c |
| `tibuf_sz` | `term_inbuf_size` | term.c |
| `tibuf_prev` | `term_inbuf_prev` | term.c |
| `ticmd` | `term_cmd_keys` | term.c |
| `ticmd_pos` | `term_cmd_keys_pos` | term.c |
| `texec` | `term_exec_type` | term.c |
| `texec_n` | `term_exec_pushed` | term.c |

Notes: `ticmd*` holds the keys kept for `.` repeat. `texec` became `term_exec_type` because `term_exec` is already a function.

## Line-number globals (`scripts/renames-vi-lnum.tsv`)

| Old | New | Defined in |
|---|---|---|
| `vi_lnnum` | `lnum_mode` | vi.c |
| `vi_lncol` | `lnum_width` | vi.c |
| `vi_rshift` | `hint_row_shift` | vi.c |
| `lnnum` | `lnum_cur` | vi.c |

`lnum_cur` is the local copy in `vi_drawrow`. The bare 1/2/4/8 mode values are now `LN_ONCE`, `LN_ABS`, `LN_INDENT` and `LN_REL`; see `docs/USAGE.md`.

Other file-local statics in `vi.c`, `ren.c` and `led.c` are not renamed yet.
