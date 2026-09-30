# Rename map

Old names from upstream `kyx0r/nextvi` (base `b1c78886`) and their new names. Generated from `scripts/renames-globals.tsv`; replay with `scripts/rename.sh` on a fresh upstream.

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

Terminal globals (`xrows`, `xcols`, `tibuf*`, `ticmd*`, `texec*`) and file-local statics are not renamed yet; they follow in the per-file passes.
