# Rename map

Old names from upstream `kyx0r/nextvi` and their new names. On this branch the base is `patched/upstream-1635521b` (upstream `1635521b` + the patches in `PATCHES.md`); the binary reference for the rename check is `f2aa0fe9…efaf`. The tables were first written against `b1c78886`. Generated from `scripts/renames-globals.tsv`; replay with `scripts/rename.sh` on a fresh upstream.

### Replaying on a fresh upstream

Apply the patches first (see `PATCHES.md`), then run the four tables in this order. `rename.sh` handles the `EO()` macro change itself, so no cherry-pick is needed.

```sh
sh scripts/rename.sh scripts/renames-globals.tsv
sh scripts/rename.sh scripts/renames-term.tsv
sh scripts/rename.sh scripts/renames-vi-lnum.tsv
sh scripts/rename.sh scripts/renames-patches.tsv
sh ./cbuild.sh
```

Reruns are safe. A row whose old name is gone and whose new name is present is reported as "already applied" and skipped. If both names are present, that's a real collision, and the script stops before editing anything.

After the last table, `rename.sh` runs `scripts/unmapped.sh`. It lists every file-scope global (`extern`, `static` and `__thread` variables in every `.c` and `.h`, except the vendored `cJSON.*` and `jsmn.h`) that no table renames and that isn't in `scripts/renames-keep.txt` (the names deliberately left alone). Upstream statics that no table renames yet (for example `acsb`, `vi_arg`, `xserr`) are still listed; that list is the baseline to compare against after a merge. `scripts/renames-allow.txt` lists known false matches, for example treesitter's `capture_n` struct member, so they aren't flagged as collisions. String literals are never renamed. After an upstream merge, that list tells you which rows to add. The `LN_*` constants and the Doxygen comments aren't in the tables; they come over by merging their commits.

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

## Patch-added globals (`scripts/renames-patches.tsv`)

Globals added by the patches in `PATCHES.md`. Run this table after the other three. `:set` options get the `opt_` prefix.

| Old | New | Defined in | Option |
|---|---|---|---|
| `xet` | `opt_expandtab` | ex.c | `:et`: indent with spaces instead of tabs |
| `xsw` | `opt_shiftwidth` | ex.c | `:sw`: spaces per indent step when `:et` is set |
| `xidt` | `opt_detect_indent_lines` | ex.c | `:idt[500]`: lines scanned on load to detect `et`/`sw`; 0 = off |
| `xaspec` | `opt_agent_exspec` | ex.c | `:aspec[1]`: print each command's ex specification to the agent once |
| `xtc` | `opt_path_complete` | ex.c | `:tc[1]`: tab path completion in the ex prompt; 1 inline, 2 full screen |
| `xlw` | `opt_wrap_width` | ex.c | `:lw`: soft line-wrap column, 0 = off, capped by the screen |
| `xhllw` | `opt_hl_wrap` | ex.c | `:hllw[1]`: mark the start and end of a soft-wrapped line |
| `xgr` | `opt_agent_guardrails` | conf.c | `:gr[2]`: agent output protection; only 2 enables it |
| `xar` | `opt_agent_reasoning` | conf.c | `:ar[0]`: show returned agent reasoning in the session log |
| `xaco` | `opt_autocompact_tokens` | conf.c | `:aco`/`:aco!`: input-token threshold for auto-compaction; 0 = off |
| `xaco_browse` | `opt_autocompact_browse` | conf.c | set by `:aco!` (browse the log) and cleared by `:aco` |
| `xtopsub` | `view_top_segment` | ex.c | |
| `conf_hlmat` | `conf_hl_match` | conf.c | |
| `conf_hlmatc` | `conf_hl_match_cursor` | conf.c | |
| `_utf8_length` | `utf8_length_default` | uc.c | |

`view_top_segment` is the first visible wrap segment of `view_top_row`. `conf_hl_match` and `conf_hl_match_cursor` are the search-match attributes; the one under the cursor uses the second. `utf8_length_default` is the default table that the `utf8_length` pointer points to.

Patch globals that already have descriptive names are listed in `scripts/renames-keep.txt` instead, for example `led_row`, `lsp_dirty`, `lsp_ft`, `spell_cmd`, `ts_preview`, `ts_redraw` and the `agent_*`, `exspec_*` and `lsp_*` state. A few patch statics with terse names (`compsb`, the `sp*` speller state in ex.c, `vi_lnwid`, `xaerr`, `xirrmsg`) are also in the keep list, like the upstream statics no table renames yet.
