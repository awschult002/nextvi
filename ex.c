/**
 * @file ex.c
 * @brief Ex command mode: option and editor-state globals, buffer list,
 * registers, address parsing, and every ":" command (table excmds[]).
 *
 * Commands have the signature `void *ec_x(char *loc, char *cmd, char *arg)`:
 * loc is the address text before the command, cmd the matched command name,
 * arg the expanded argument. They return NULL on success or an error string.
 * Row ranges are half-open [beg, end); o1/o2 are character offsets (-1 = unset).
 */

int opt_left_col;	///< first visible screen column, horizontal scroll (was xleft)
int opt_startup_flags;	///< mode bits: 1 raw ex I/O without line editor (-s), 2 ex mode instead of vi (-e), 4 no file message on load (-m, cleared after startup), 8 alternate screen (-a) (was xvis)
int opt_autoindent = 1;	///< autoindent option (was xai)
int opt_ignorecase = 1;	///< case-insensitive regex search (was xic)
int opt_syntax_hl = 1;	///< 1: patterns and led extensions, other >0: extensions only, 0: off (was xhl)
int opt_hl_line;	///< highlight current line (was xhll)
int opt_hl_word;	///< highlight the word under the cursor; value is the word count (was xhlw)
int opt_hl_pair;	///< highlight {}[]() pair (was xhlp)
int opt_hl_reverse;	///< highlight text rendered in reverse direction (was xhlr)
int opt_line_editor = 1;	///< use the line editor; 0 disables all terminal output (was xled)
int opt_text_dir = +1;	///< text direction: +1/-1 default LTR/RTL with per-line detection, +2/-2 forced (was xtd)
int opt_shaping = 1;	///< perform Arabic letter shaping (was xshape)
int opt_reorder = 1;	///< bidi: reorder characters per dmarks[] (was xorder)
int opt_tabstop = 8;	///< number of spaces for tab (was xts)
int opt_interactive_shell;	///< run the shell with -i (was xish)
int opt_search_group;	///< regex group used as match position, stored as group*2 (index into offs[]) (was xgrp)
int opt_print_autocomplete;	///< print autocomplete options while inserting (was xpac)
int opt_multiline_prompt;	///< counts lines printed from vi; >1 prompts "[any key to continue]", <0 disables (was xmpt)
int opt_print_reg;	///< if >0, ex_cprint() also appends printed text to this register (was xpr)
int opt_render_limit = -1;	///< max characters rendered on non-cursor lines, -1 = no limit (was xlim)
int opt_undo_seq = 1;	///< added to lbuf useq after each command; 0 merges commands into one undo step, <0 disables history (was xseq)
int opt_error_mode = 1;	///< error handling bits: 1 print errors, 2 stop the command chain on error, 4 ex_exec() always reports success (was xerr)
int opt_find_reg;	///< if set, :f searches the text of this register instead of the buffer (was xfr)
int opt_record_reg;	///< register receiving raw keystrokes while recording, 0 = off (was xrr)

int quit_state;	///< exit if positive (value-1 is the exit code), force quit or unwind if negative, see ec_quit() (was xquit)
int cursor_row;		///< cursor row, 0-based line index in the current buffer (was xrow)
int cursor_off;		///< cursor position as a character index into the line (not bytes or screen column); indexes rstate->chrs[] (was xoff)
int view_top_row;	///< buffer row shown on the first screen line (was xtop)
int buf_count;	///< number of used entries in bufs[] (was xbufcur)
int vi_ex_depth;	///< nesting depth of vi()/ex() main loops (was xgrec)
int cur_keymap;	///< index of the active keymap in kmaps[], 0 = en (was xkmap)
int keymap_alt = 1;	///< keymap selected by ^f (was xkmap_alt)
int search_dir;	///< last search direction, +1/-1, 0 = none yet (was xkwddir)
int search_changes;	///< incremented whenever search_rset is rebuilt (was xkwdcnt)
int print_newline;	///< tracks newline from ex print and pipe stdout (was xpln)
int ex_separator = ':';	///< ex command separator (was xsep)
int ex_escape = '\\';	///< ex command arg escape character (was xesc)
int ex_exec_depth;	///< ex_exec() recursion depth (was xexec_dep)
sbuf *autocomplete_filter;	///< regex choosing autocomplete words, NULL = default word regex (was xacreg)
rset *search_rset;	///< compiled last search keyword (was xkwdrs)
sbuf **str_registers;	///< string registers indexed by register id; entries may be NULL (was xregs)
int str_registers_n;	///< allocated length of str_registers[] (was xregs_n)
int default_reg;	///< register used when none is given (was xdefreg)
struct buf *bufs;	///< main buffers, bufs_max entries, buf_count used
struct buf tempbufs[3];	///< internal buffers: 0 "/hist/" prompt history, 1 "/fm/" file list, 2 "/sc/" scratch
struct buf *cur_buf;	///< current buffer, points into bufs[] or tempbufs[] (was ex_buf)
struct buf *prev_buf;	///< previous buffer, for ^^ and %# (was ex_pbuf)
static struct buf *ex_tpbuf;	///< prev_buf saved on entering a temp buffer, restored on leaving it
static int bufs_max;	///< capacity of bufs[], set by :bx (was xbufsmax)
static int bufs_alloc = 10;	///< default capacity for :bx without argument (was xbufsalloc)
static int global_depth;	///< :g nesting as a single bit (1, 2, 4...) used to tag lines in linfo.grec (was xgdep)
static int ex_expand_char = '%';	///< ex command internal state expand character (was xexp)
static int ex_shell_char = '!';	///< ex command external command expand character (was xexe)
static char xuerr[] = "unreported error";	///< generic failure; also returned after the real message was printed
static char xserr[] = "syntax error";	///< error message
static char xgerr[] = "invalid grp";	///< error message: opt_search_group beyond the regex's groups
static char xirerr[] = "invalid range";	///< error message
static char *xrerr;	///< last address error, set by ex_region()/ex_range()
static void *ex_prev_ret;	///< previous ex command return value, NULL = success (was xpret)
static signed char *capture_status;	///< status by id recorded by :??: 1 failed, 0 succeeded (inverted by !), -1 unset (was xcid)
static unsigned int capture_n;	///< number of allocated capture ids (was xcid_n)
static int capture_keep;	///< keep capture statuses across top-level ex_exec calls (was xcid_keep)
static int quit_propagate;	///< number of ex_exec levels :q propagates (was xqprop)

/* parity rule: delim halves escapes, odd keeps delim literal.
 * n = run of esc chars at p. If dtest (usually p[n] is a delimiter) emit
 * ceil(n/2) of them; if n is odd the last emitted esc is overwritten by the
 * delimiter, which is consumed as a literal. Otherwise copy all n. */
#define ex_parity(p, sb, esc, dtest) \
int n = 0, keep, d; \
for (; p[n] == esc; n++); \
keep = n; \
d = dtest; \
if (d) \
	n -= n / 2; \
sbuf_mem(sb, p, n) \
if (d && keep & 1) \
	sb->s[sb->s_n - 1] = p[keep++]; \
p += keep; \

/** @brief Read text up to an unescaped delim into sb (NUL-terminated); *src ends past the delim. */
static void ex_sread(sbuf *sb, char **src, int delim, int esc)
{
	char *s = *src;
	while (*s && *s != delim) {
		if (*s == esc) {
			ex_parity(s, sb, esc, s[n] == delim)
			continue;
		}
		sbuf_chr(sb, *s++)
	}
	*src = *s ? s + 1 : s;
	sbuf_nul(sb)
}

/** @brief Like ex_sread() but returns a new malloc'd string. */
static char *ex_se_read(char **src, int delim, int esc)
{
	sbuf_smake(sb, 256)
	ex_sread(sb, src, delim, esc);
	return sb->s;
}

/** @brief Read a regex delimited by its first char (e.g. /re/); NULL if *src is empty. */
static char *ex_re_read(char **src)
{
	int delim = **src;
	if (!delim)
		return NULL;
	++*src;
	return ex_se_read(src, delim, '\\');
}

/** @brief Compare two strings back to front; 0 only if equal and nonempty. */
static int rstrcmp(const char *s1, const char *s2, int l1, int l2)
{
	if (l1 != l2 || !l1)
		return 1;
	for (int i = l1-1; i >= 0; i--)
		if (s1[i] != s2[i])
			return 1;
	return 0;
}

/** @brief Index of the buffer whose path is path, or -1. */
static int bufs_find(const char *path, int len)
{
	for (int i = 0; i < buf_count; i++)
		if (!rstrcmp(bufs[i].path, path, bufs[i].plen, len))
			return i;
	return -1;
}

/** @brief Free the path and line buffer of bufs[idx]. */
static void bufs_free(int idx)
{
	free(bufs[idx].path);
	lbuf_free(bufs[idx].lb);
}

/** @brief File modification time, -1 if stat() fails. */
static long mtime(char *path)
{
	struct stat st;
	if (!stat(path, &st))
		return st.st_mtime;
	return -1;
}

/**
 * @brief Make bufs[idx] current, saving the cursor of the old buffer.
 * prev_buf is updated, skipping temp buffers.
 */
void bufs_switch(int idx)
{
	if (cur_buf != &bufs[idx]) {
		exbuf_save(cur_buf)
		if (istempbuf(cur_buf))
			prev_buf = &bufs[idx] == prev_buf ? ex_tpbuf : prev_buf;
		else
			prev_buf = cur_buf;
		cur_buf = &bufs[idx];
	}
	exbuf_load(cur_buf)
}

/**
 * @brief Allocate a buffer entry for path (not read yet).
 * When bufs[] is full the last entry is freed and reused.
 * @return index of the new entry
 */
static int bufs_open(const char *path, int len)
{
	int i = buf_count;
	if (i <= bufs_max - 1)
		buf_count++;
	else
		bufs_free(--i);
	bufs[i].path = uc_dup(path);
	bufs[i].lb = lbuf_make();
	bufs[i].plen = len;
	bufs[i].row = 0;
	bufs[i].off = 0;
	bufs[i].top = 0;
	bufs[i].td = +1;
	bufs[i].mtime = -1;
	return i;
}

/** @brief Initialize tempbufs[i] with an empty lbuf. */
void temp_open(int i, char *name, char *ft)
{
	tempbufs[i].path = uc_dup(name);
	tempbufs[i].lb = lbuf_make();
	tempbufs[i].row = 0;
	tempbufs[i].off = 0;
	tempbufs[i].top = 0;
	tempbufs[i].td = +1;
	tempbufs[i].mtime = -1;
	tempbufs[i].ft = ft;
}

/** @brief Set the saved cursor of tempbufs[i]; row < 0 means its last line. */
void temp_pos(int i, int row, int off, int top)
{
	if (row < 0)
		row = lbuf_len(tempbufs[i].lb)-1;
	tempbufs[i].row = row < 0 ? 0 : row;
	tempbufs[i].off = off;
	tempbufs[i].top = top;
}

/**
 * @brief Enter tempbufs[i]; if it is already current and swap is set,
 * return to the buffer that was current before.
 */
void temp_switch(int i, int swap)
{
	if (cur_buf == &tempbufs[i]) {
		if (swap) {
			exbuf_save(cur_buf)
			cur_buf = prev_buf;
			prev_buf = ex_tpbuf;
		}
	} else {
		if (!istempbuf(cur_buf)) {
			ex_tpbuf = prev_buf;
			prev_buf = cur_buf;
		}
		exbuf_save(cur_buf)
		cur_buf = &tempbufs[i];
	}
	exbuf_load(cur_buf)
	syn_setft(xb_ft);
}

/** @brief Insert str after the saved row of tempbufs[i] and advance that row. */
void temp_write(int i, char *str)
{
	if (!*str)
		return;
	struct lbuf *lb = tempbufs[i].lb;
	if (lbuf_get(lb, tempbufs[i].row))
		tempbufs[i].row++;
	lbuf_edit(lb, str, tempbufs[i].row, tempbufs[i].row, 0, 0);
}

/**
 * @brief Set the current search keyword rset if the kwd or flags changed.
 * @param kwd  regex, NULL or "" keeps the current one
 * @param dir  +-1: direction stored only when kwd changes; +-2: always set to dir/2
 */
void ex_krsset(char *kwd, int dir)
{
	sbuf *reg = ex_regget('/');
	if (kwd && *kwd && ((!reg || !search_rset || strcmp(kwd, reg->s))
			|| ((search_rset->regex->flg & REG_ICASE) != opt_ignorecase))) {
		rset_free(search_rset);
		search_rset = rset_smake(kwd, opt_ignorecase ? REG_ICASE : 0);
		search_changes++;
		ex_regput('/', kwd, 0);
		search_dir = dir;
	}
	if (dir == -2 || dir == 2)
		search_dir = dir / 2;
}

/**
 * @brief Parse one address at *num and advance *num past it.
 *
 * Forms: `.` `$` `'N` (mark) `>re>` `<re<` (search) digits, then any
 * `+N -N *N /N %N` arithmetic. `%` is "last line" only at the start (ploc).
 * @param ploc  start of the whole address list
 * @param[in,out] num  parse position
 * @param n    value of `.` (current row, or current offset if row given)
 * @param row  NULL: parse a line number; else parse a character offset in *row
 * @return the address, or -2 on error (xrerr set)
 */
static int ex_range(char *ploc, char **num, int n, int *row)
{
	int dir, off, beg, end;
	switch (**num) {
	case '.':
		++*num;
		break;
	case '%':
		if (ploc != *num)
			break;
	case '$':
		n = row ? lbuf_eol(xb, *row, 2) : lbuf_len(xb) - 1;
		++*num;
		break;
	case '\'':
		if (!uc_isdigit(*++(*num))) {
			xrerr = xserr;
			return -2;
		}
		for (off = 0; uc_isdigit(**num); ++*num)
			off = off * 10 + (**num - '0');
		if (lbuf_jump(xb, off, &n, row ? &n : &dir)) {
			xrerr = "mark not set";
			return -2;
		}
		break;
	case '>':
	case '<':
		dir = **num == '>' ? 2 : -2;
		off = row ? n : 0;
		beg = row ? *row : n + (dir > 0);
		end = row ? beg+1 : lbuf_len(xb);
		if (off < 0 || beg < 0 || beg >= lbuf_len(xb))
			return -2;
		char *e = ex_re_read(num);
		ex_krsset(e, dir);
		free(e);
		if (!search_rset) {
			xrerr = xserr;
			return -2;
		} else if (opt_search_group >= search_rset->nsubc) {
			xrerr = xgerr;
			return -2;
		}
		if (lbuf_search(xb, search_rset, search_dir, row ? beg : 0, end,
				MIN(dir, 0), !row, &beg, &off)) {
			xrerr = "range not found";
			return -2;
		}
		n = row ? off : beg;
		break;
	default:
		if (uc_isdigit(**num)) {
			n = atoi(*num);
			while (uc_isdigit(**num))
				++*num;
		}
	}
	while (**num) {
		dir = atoi(*num+1);
		if (**num == '-')
			n -= dir;
		else if (**num == '+')
			n += dir;
		else if (**num == '*')
			n *= dir;
		else if (**num == '/' && dir)
			n /= dir;
		else if (**num == '%' && dir)
			n %= dir;
		else
			break;
		for (++*num; uc_isdigit(**num);)
			++*num;
	}
	return n;
}

/** @brief ex_region() for rows only; horizontal addresses go to cursor_off. */
#define ex_vregion(loc, beg, end) ex_region(loc, beg, end, &cursor_off, &cursor_off)
/**
 * @brief Parse an address list: `a,b` rows, `;x;y` character offsets,
 * `|cmd|` ex commands run while parsing. `,#`/`;#` continue from the previous value.
 * Numeric row addresses are 1-based (adj subtracts 1).
 * @param loc  the address list text
 * @param[out] beg,end  half-open row range; defaults to the cursor row
 * @param[out] o1,o2    character offsets, untouched unless given
 * @return 0 if valid; else 1, or 2 (no row address but a |cmd| ran),
 *         or 3 (a single numeric row address out of range, e.g. 0)
 */
static int ex_region(char *loc, int *beg, int *end, int *o1, int *o2)
{
	int vaddr = *loc == '%', haddr = 0, update = 0;
	int row = cursor_row, ooff = cursor_off, ret = 1, adj = 0;
	char *ploc = loc, *cmd = NULL;
	xrerr = xirerr;
	if (vaddr)
		*beg = 0;
	while (*loc) {
		if (*loc == '|') {
			loc++;
			cmd = ex_se_read(&loc, '|', ex_escape);
			void *err = ex_exec(cmd);
			free(cmd);
			if (err) {
				xrerr = "subcommand error";
				return 1;
			}
			continue;
		} else if (*loc == ';') {
			update = loc[1] == '#';
			loc += 1 + update;
			if ((ooff = ex_range(ploc, &loc, update ? ooff : cursor_off, &row)) < 0)
				return 1;
			if (haddr++ % 2)
				*o2 = ooff;
			else
				*o1 = ooff;
		} else {
			if (*loc == ',') {
				update = loc[1] == '#';
				loc += 1 + update;
			}
			adj = uc_isdigit(*loc);
			row = ex_range(ploc, &loc, update ? row : cursor_row, NULL);
			if (vaddr++ % 2)
				*end = row + 1 - adj;
			else
				*beg = row - adj;
		}
		while (*loc && *loc != '|' && *loc != ';' && *loc != ',')
		        loc++;
	}
	if (!vaddr) {
		*beg = cursor_row;
		*end = MIN(lbuf_len(xb), *beg + 1);
		ret += cmd && !haddr;
	} else if (vaddr == 1) {
		*end = *beg + 1;
		ret += adj << 1;
	}
	return (*beg < 0 || *beg >= lbuf_len(xb) ||
		*end <= *beg || *end > lbuf_len(xb)) * ret;
}

/**
 * @brief Read one line of ex input into sb, prompting with msg.
 * In raw mode (opt_startup_flags & 1) bytes are read with term_read().
 * @return the key that ended input ('\n' on accept)
 */
static int ex_read(sbuf *sb, char *msg, ins_state *is, int ps, int flg)
{
	int n = sb->s_n, key;
	if (opt_startup_flags & 1) {
		while ((key = term_read(0)) != '\n') {
			sbuf_chr(sb, key)
			if (flg & 2 || quit_state)
				break;
		}
		sbuf_nul(sb)
		return key;
	}
	sbuf_str(sb, msg)
	key = led_prompt(sb, NULL, &cur_keymap, is, ps, flg);
	if (key == '\n' && (!*msg || strcmp(sb->s + n, msg)))
		term_chr('\n');
	return key;
}

/** Read xb_path appending to the current buffer; errchk receives lbuf_rd()'s result. */
#define readfile(errchk) \
fd = open(xb_path, O_RDONLY); \
if (fd >= 0) { \
	errchk lbuf_rd(xb, fd, 0, lbuf_len(xb)); \
	close(fd); \
} \

/**
 * @brief Switch to the buffer for path, opening and reading it if needed.
 * @return 1 if it was already open, 0 if a new buffer was created
 */
int ex_edit(const char *path, int len)
{
	int fd;
	if (path[0] == '.' && path[1] == '/') {
		path += 2;
		len -= 2;
	}
	if (path[0] && ((fd = bufs_find(path, len)) >= 0)) {
		bufs_switch(fd);
		return 1;
	}
	bufs_switch(bufs_open(path, len));
	readfile()
	return 0;
}

/** @brief `:e[!] [path]` edit a file. */
static void *ec_edit(char *loc, char *cmd, char *arg)
{
	char msg[512];
	int fd, len, rd = 0, cd = 0;
	if (arg[0] == '.' && arg[1] == '/')
		cd = 2;
	len = strlen(arg+cd);
	if (len && ((fd = bufs_find(arg+cd, len)) >= 0)) {
		bufs_switchwft(fd)
		return NULL;
	} else if (buf_count == bufs_max && !strchr(cmd, '!') &&
			bufs[bufs_max - 1].lb->modified) {
		return "last buffer modified";
	} else if (len || !buf_count || !strchr(cmd, '!')) {
		bufs_switch(bufs_open(arg+cd, len));
		cd = 3; /* XXX: quick hack to indicate new lbuf */
	}
	readfile(rd =)
	if (cd == 3 || (!rd && fd >= 0)) {
		ex_bufpostfix(cur_buf, arg[0]);
		syn_setft(xb_ft);
	}
	snprintf(msg, sizeof(msg), "\"%s\" %dL [%c]",
			*xb_path ? xb_path : "unnamed", lbuf_len(xb),
			fd < 0 || rd ? 'f' : 'r');
	if (!(opt_startup_flags & 4))
		ex_print(msg, bar_ft)
	return (fd < 0 || rd) && *arg ? xuerr : NULL;
}

/**
 * @brief `:f [re]` / `:ef[!] [re]` interactive regex filter of lines.
 * `f` jumps to a line in the current buffer; `ef` filters the file list
 * (tempbufs[1]) and opens the chosen file.
 */
static void *ec_fuzz(char *loc, char *cmd, char *arg)
{
	rset *rs;
	char *path, *p, buf[128], trunc[128], *sret = NULL;
	int c, pos, subs[2], inst = -1, lnum = -1;
	int beg, end, max = INT_MAX, dwid1, dwid2;
	int flg = REG_NEWLINE | REG_NOCAP;
	int pflg = ((opt_startup_flags & 2) == 0) * 2;
	ins_state is;
	ins_init(is)
	if (*cmd !='f')
		temp_switch(1, 0);
	if (!*loc || ex_vregion(loc, &beg, &end)) {
		end = lbuf_len(xb);
		if (!end || *loc) {
			if (*cmd !='f')
				temp_switch(1, 1);
			return *loc ? xrerr : xirerr;
		}
		beg = 0;
		max = xrows ? xrows * 3 : end;
	}
	snprintf(trunc, sizeof(trunc), "truncated to %d lines", max);
	dwid1 = itoalen(max - 1);
	sbuf_smake(sb, 128)
	sbuf_smake(fuzz, 16)
	sbuf_smake(cmdbuf, 16)
	sbuf_str(fuzz, arg)
	syn_setft(fuzz_ft);
	while (1) {
		sbuf_nul(fuzz)
		c = 0;
		rs = rset_smake(fuzz->s, opt_ignorecase ? flg | REG_ICASE : flg);
		if (rs) {
			syn_reloadft(syn_addhl(fuzz->s, 1), rs->regex->flg);
			term_record = !!term_sbuf;
			end = MIN(end, lbuf_len(xb));
			dwid2 = itoalen(end);
			dwid1 = max == INT_MAX ? dwid2 : MIN(dwid1, dwid2);
			for (pos = beg; c < max && pos < end; pos++) {
				path = xb->ln[pos];
				if (rset_match(rs, path, 0)) {
					/* sb collects the matching row numbers as raw ints, read back below */
					sbuf_mem(sb, &pos, sizeof(pos))
					p = itoa(c++, buf);
					int z, wid = p - buf;
					for (z = dwid1 + 1 - wid; z; z--)
						*p++ = ' ';
					wid = itoalen(pos+1);
					for (z = dwid2 - wid; z; z--)
						*p++ = ' ';
					p = itoa(pos+1, p);
					ex_cprint2(buf, msg_ft, -1, 0, 0, pflg)
					ex_cprint2(path, NULL, -1, (p - buf) + 1, 0, !pflg)
				}
			}
			if (c == max && c != end)
				ex_cprint2(trunc, msg_ft, -1, 0, 0, 2)
			if (pflg && c)
				term_chr('\n');
			if (term_record)
				term_commit();
		}
		if ((inst = ex_read(fuzz, "", &is, 0, 2)) == '\n' && c) {
			if (c == 1)
				break;
			if ((inst = ex_read(cmdbuf, "", NULL, 0, 0)) == '\n') {
				inst = atoi(cmdbuf->s);
				break;
			}
		}
		if (TK_INT(inst))
			goto ret;
		if (c && c < 11 && uc_isdigit(inst)) {
			inst -= '0';
			if (inst < c) {
				fuzz->s_n--;
				break;
			}
		}
		rset_free(rs);
		sbuf_cut(sb, 0)
		if (pflg) {
			term_clean();
			term_pos(xrows, 0);
		} else if (c)
			ex_print("", NULL)
	}
	if ((inst >= 0 && inst < c) || c == 1)
		lnum = *((int*)sb->s + (c == 1 ? 0 : inst));
	ret:
	syn_setft(xb_ft);
	if (fuzz->s_n > 0) {
		sbuf_cut(cmdbuf, 0)
		sbuf_str(cmdbuf, loc)
		sbuf_str(cmdbuf, cmd)
		sbuf_chr(cmdbuf, ' ')
		sbufn_mem(cmdbuf, fuzz->s, fuzz->s_n)
		lbuf_dedup(tempbufs[0].lb, cmdbuf->s, cmdbuf->s_n)
		temp_pos(0, -1, 0, 0);
		temp_write(0, cmdbuf->s);
	}
	free(cmdbuf->s);
	free(fuzz->s);
	free(sb->s);
	path = lbuf_get(xb, lnum);
	if (*cmd == 'f' && path) {
		rset_find(rs, path, subs, 0);
		cursor_row = lnum;
		cursor_off = uc_off(path, subs[0]);
	} else if (path) {
		path[lbuf_s(path)->len] = '\0';
		sret = ec_edit(loc, cmd, path);
		path[lbuf_s(path)->len] = '\n';
	} else if (*cmd != 'f')
		temp_switch(1, 1);
	rset_free(rs);
	return sret;
}

/** @brief `:f+ :f- :f> :f<` regex search within a range (or in opt_find_reg). */
static void *ec_find(char *loc, char *cmd, char *arg)
{
	int e, pskip, nskip, dir, off, nbeg, beg, end, o1 = 0, o2 = -1;
	e = ex_region(loc, &beg, &end, &o1, &o2);
	if (e && (!opt_find_reg || (*loc && e != 2)))
		return xrerr;
	dir = cmd[1] == '+' || cmd[1] == '>' ? 2 : -2;
	if (opt_find_reg) {
		if (dir < 0)
			return "register search is forward only";
		if (cmd[1] == '+' && (!*loc || e == 2))
			return "cannot increment without range";
	}
	ex_krsset(arg, dir);
	if (!search_rset)
		return xserr;
	else if (opt_search_group >= search_rset->nsubc)
		return xgerr;
	if (opt_find_reg) {
		int offs[search_rset->nsubc];
		sbuf *sb = ex_regget(opt_find_reg);
		if (!sb)
			return "uninitialized register";
		if (!*loc || e == 2) {
			if (rset_find(search_rset, sb->s, offs, 0) < 0 || offs[opt_search_group] < 0)
				return xuerr;
			return NULL;
		}
		int pin = cursor_row < beg || cursor_row >= end || (cursor_row == beg && cursor_off < o1)
			|| (o2 >= 0 && cursor_row == end - 1 && cursor_off > o2);
		off = pin ? 0 : lbuf_pos2off(xb, beg, o1, end - 1, o2,
				cursor_row, cursor_off + (cmd[1] == '+'));
		if (off < 0 || off >= sb->s_n
				|| rset_find(search_rset, sb->s + off, offs, 0) < 0
				|| offs[opt_search_group] < 0
				|| lbuf_off2pos(xb, beg, o1, end - 1, o2,
						off + offs[opt_search_group], &cursor_row, &cursor_off))
			return xuerr;
		return NULL;
	}
	off = cursor_off;
	if (cursor_row < beg || cursor_row >= end) {
		off = dir < 0 ? lbuf_eol(xb, end - 1, 2) : 0;
		end--;
		nbeg = dir > 0 ? beg : end;
		end++;
		pskip = -1;
		nskip = 0;
	} else {
		nbeg = cursor_row;
		pskip = cmd[1] == '+' ? 1 : MIN(dir, 0);
		nskip = cmd[1] == '-';
	}
	if (lbuf_search(xb, search_rset, search_dir, beg, end,
			pskip, nskip, &nbeg, &off))
		return xuerr;
	cursor_row = nbeg;
	cursor_off = off;
	return NULL;
}

/** @brief `:b [n]` list buffers or switch to one; negative n selects tempbufs[-n-1]. */
static void *ec_buffer(char *loc, char *cmd, char *arg)
{
	int n = atoi(arg);
	if (!arg[0]) {
		char ln[512];
		for (int i = 0; i < buf_count; i++) {
			char c = cur_buf == bufs+i ? '%' : ' ';
			c = prev_buf == bufs+i ? '#' : c;
			snprintf(ln, LEN(ln), "%d %c %s", i,
				c + (char)bufs[i].lb->modified, bufs[i].path);
			ex_print(ln, msg_ft)
		}
		return NULL;
	} else if (n < 0) {
		if (-n <= LEN(tempbufs)) {
			temp_switch(-n-1, 1);
			return NULL;
		}
	} else if (n < buf_count) {
		bufs_switchwft(n)
		return NULL;
	}
	return "no such buffer";
}

/** @brief `:q[!] [code]`; a numeric loc sets how many ex_exec levels it propagates. */
static void *ec_quit(char *loc, char *cmd, char *arg)
{
	if (ex_exec_depth == 1 && vi_ex_depth == 1 && !strchr(cmd, '!') && quit_state >= 0)
		for (int i = 0; i < buf_count; i++)
			if (bufs[i].lb->modified)
				return "buffers modified";
	quit_state = !quit_state ? 1 : quit_state;
	quit_propagate = *loc ? atoi(loc) : -1;
	/* recursion unwind: 256 increments preserve the first 8 bits
	for exit code and -257 is the offset for comparisons. */
	if (*arg)
		quit_state = (abs(atoi(arg)) & 255) + 1;
	if (strchr(cmd, '!'))
		quit_state = *loc ? -quit_propagate * 256 - 257 - (abs(quit_state) - 1) : -quit_state;
	return NULL;
}

/** @brief After a load: refresh mtime and filetype, mark saved (clear drops undo history). */
void ex_bufpostfix(struct buf *p, int clear)
{
	p->mtime = mtime(p->path);
	p->ft = syn_filetype(p->path);
	lbuf_saved(p->lb, clear);
}

/** @brief `:bp path` set the current buffer's path. */
static void *ec_setpath(char *loc, char *cmd, char *arg)
{
	free(xb_path);
	xb_path = uc_dup(arg);
	cur_buf->plen = strlen(arg);
	return NULL;
}

/**
 * @brief `:r [file|!cmd]` insert a file or command output before the cursor row.
 * xb temporarily points at the new text so loc addresses that text.
 */
static void *ec_read(char *loc, char *cmd, char *arg)
{
	sbuf obuf, *sb;
	char msg[512];
	char *path, *ret = NULL;
	int beg, end, o1 = 0, o2 = -1;
	int row = cursor_row, off = cursor_off, fd = -1;
	struct lbuf *lb = lbuf_make(), *pxb = xb;
	path = arg[0] ? arg : xb_path;
	if (arg[0] == '!') {
		if ((sb = cmd_pipe(arg + 1, NULL, 1, NULL))) {
			lbuf_edit(lb, sb->s, 0, 0, 0, 0);
			sbuf_free(sb)
		}
	} else {
		if ((fd = open(path, O_RDONLY)) < 0) {
			ret = "open failed";
			goto err;
		}
		if (lbuf_rd(lb, fd, 0, 0)) {
			ret = "read failed";
			goto err;
		}
	}
	xb = lb;
	cursor_row = 0;
	cursor_off = 0;
	if (!*loc || ex_region(loc, &beg, &end, &o1, &o2)) {
		end = lbuf_len(xb);
		if (!end || *loc) {
			ret = *loc ? xrerr : xirerr;
			goto err;
		}
		beg = 0;
	}
	lbuf_region(lb, &obuf, beg, o1, end - 1, o2);
	lbuf_edit(pxb, obuf.s, row, row, 0, 0);
	free(obuf.s);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [r]", path, end - beg);
	ex_print(msg, bar_ft)
	err:
	lbuf_free(lb);
	cursor_row = row;
	cursor_off = off;
	xb = pxb;
	if (fd >= 0)
		close(fd);
	return ret;
}

/** @brief Run cmd with buf (may be NULL) as stdin; its output goes to the terminal. */
static void *ex_pipeout(char *cmd, sbuf *buf)
{
	int ret = 0;
	if (!(opt_startup_flags & 2) && opt_multiline_prompt >= 0 && !print_newline) {
		term_chr('\n');
		print_newline = 1;
		opt_multiline_prompt = 2;
	} else if (opt_startup_flags & 2 && print_newline == 2) {
		term_chr('\n');
		print_newline = 0;
	}
	sbuf *rsb = cmd_pipe(cmd, buf, 0, &ret);
	if (!rsb)
		return "fork failed";
	sbuf_free(rsb)
	return ret ? xuerr : NULL;
}

/** @brief `:w :wq :x [!] [file|!cmd]` write the buffer or a range. */
static void *ec_write(char *loc, char *cmd, char *arg)
{
	char msg[512], *path, *ret = NULL;
	sbuf ibuf;
	int fd, quit = quit_state;
	int beg, end, o1 = -1, o2 = -1;
	path = arg[0] ? arg : xb_path;
	if (cmd[0] == 'x' && !xb->modified)
		return ec_quit("", cmd, "");
	if (!*loc || (fd = ex_region(loc, &beg, &end, &o1, &o2))) {
		if (*loc && fd != 2)
			return xrerr;
		beg = 0;
		end = lbuf_len(xb);
	}
	if (cmd[0] == 'x' || (cmd[0] == 'w' && cmd[1] == 'q')) {
		int modified = xb->modified;
		xb->modified = 0;
		ret = ec_quit("", cmd, "");
		xb->modified = modified;
		if (quit_state < 0)
			quit = quit_state;
		swap(&quit, &quit_state);
	}
	if (arg[0] == '!') {
		if (ret)
			return ret;
		lbuf_region(xb, &ibuf, beg, MAX(0, o1), end - 1, o2);
		ret = ex_pipeout(arg + 1, &ibuf);
		free(ibuf.s);
		quit_state = quit;
		return ret;
	} else if (ret)
		return "other buffers modified";
	if (!strchr(cmd, '!')) {
		if (!strcmp(xb_path, path) && mtime(path) > cur_buf->mtime)
			return "write failed: file changed";
		if (arg[0] && mtime(path) >= 0)
			return "write failed: file exists";
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode);
	if (fd < 0)
		return "write failed: cannot create file";
	if (o1 >= 0) {
		lbuf_region(xb, &ibuf, beg, o1, end - 1, o2);
		o1 = write(fd, ibuf.s, ibuf.s_n);
		free(ibuf.s);
	} else
		o1 = lbuf_wr(xb, fd, beg, end);
	close(fd);
	if (o1 < 0)
		return "write failed";
	snprintf(msg, sizeof(msg), "\"%s\" %dL [w]",
			path, end - beg);
	ex_print(msg, bar_ft)
	if (strcmp(xb_path, path))
		ec_setpath(NULL, NULL, path);
	lbuf_saved(xb, 0);
	cur_buf->mtime = mtime(path);
	quit_state = quit;
	return NULL;
}

/** @brief `:@ keys` / `:& keys` run keys as vi input via term_exec(). */
static void *ec_termexec(char *loc, char *cmd, char *arg)
{
	if (*arg && term_sbuf)
		term_exec(arg, strlen(arg), cmd[0])
	return term_sbuf ? NULL : "unsupported command";
}

/**
 * @brief Print a line to the message area or ex output.
 * @param line  text to print
 * @param ft    filetype for highlighting, NULL keeps the current one
 * @param r,c   screen row (-1 = current) and column
 * @param left  first visible column of line
 * @param flg   nonzero: go to the bottom row first; 1 also counts toward
 *              opt_multiline_prompt, 2 always emits a newline
 */
void ex_cprint(char *line, char *ft, int r, int c, int left, int flg)
{
	if (opt_print_reg > 0) {
		ex_regput(opt_print_reg, line, 1);
		sbuf *pr = ex_regget(opt_print_reg);
		if (flg & 1 && opt_print_reg >= 'A' && opt_print_reg <= 'Z' && pr && pr->s_n &&
				pr->s[pr->s_n-1] != '\n')
			ex_regput(opt_print_reg, "\n", 1);
	}
	if (opt_startup_flags & 1) {
		term_write(line, dstrlen(line, '\n'))
		term_write("\n", 1)
		return;
	}
	syn_blockhl = -1;
	if (flg && !(opt_startup_flags & 2)) {
		term_pos(xrows, 0);
		if ((!print_newline && opt_multiline_prompt > 0) || flg == 2)
			term_chr('\n');
		opt_multiline_prompt += opt_multiline_prompt >= 0 && flg == 1;
	}
	print_newline = 0;
	preserve(int, ftidx,)
	if (ft)
		syn_setft(ft);
	led_crender(line, r, c, left, left + xcols - c)
	restore(ftidx)
	if (flg && opt_startup_flags & 2)
		term_chr('\n');
}

/**
 * @brief `:i` insert text after the addressed lines, `:c` replace them
 * (or the o1..o2 part). Text comes from arg or is read line by line.
 */
static void *ec_insert(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = -1, o2 = -1, ps = 0, key;
	sbuf _sb, *sb = &_sb;
	if (!*loc || (key = ex_region(loc, &beg, &end, &o1, &o2))) {
		if (*loc && cmd[0] != 'c' && beg == -1 && end == 0
				&& (lbuf_len(xb) || key == 3))
			beg = 0;
		else if (*loc && key != 2)
			return xrerr;
		else {
			beg = MAX(0, MIN(lbuf_len(xb), cursor_row));
			end = beg + 1;
		}
	}
	if (opt_startup_flags & 1 && *arg) {
		sb->s = arg;
		sb->s_n = 1;
		key = 127;
	} else {
		_sbuf_make(sb, 128,)
		if (*arg)
			term_push(arg, strlen(arg));
		while (1) {
			syn_setft(msg_ft);
			if ((key = ex_read(sb, "", NULL, ps, 0)) != '\n')
				break;
			if (opt_startup_flags & 1 && !strcmp(".", sb->s + ps)) {
				sb->s_n = MAX(0, sb->s_n - 2);
				break;
			}
			sbuf_chr(sb, '\n')
			ps = sb->s_n;
		}
		syn_setft(xb_ft);
		if (key == TK_CTL('c'))
			goto ret;
		if (key == 127 && sb->s_n && sb->s[sb->s_n-1] == '\n')
			sb->s_n--;
		sbuf_nul(sb)
	}
	if (cmd[0] == 'i')
		beg = end;
	if (o1 >= 0 && cmd[0] == 'c') {
		if (sb->s == arg)
			sb->s_n = strlen(arg);
		if (!sb->s_n && o2 <= o1)
			goto ret;
		char *p = lbuf_joinsb(xb, beg, end - 1, sb, &o1, &o2);
		o1 -= sb->s[0] == '\n';
		if (sb->s != arg)
			free(sb->s);
		sb->s = p;
	} else if (key != 127)
		sbufn_chr(sb, '\n')
	else if (!sb->s_n)
		goto ret;
	ps = lbuf_len(xb);
	lbuf_edit(xb, sb->s, beg, end, o1, o2);
	cursor_row = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - ps - 1);
	if (o1 >= 0)
		cursor_off = o1;
	ret:
	if (sb->s != arg)
		free(sb->s);
	return NULL;
}

/** @brief `:p [text]` print a range or text; a bare address just moves the cursor. */
static void *ec_print(char *loc, char *cmd, char *arg)
{
	int i, beg, end, o1 = -1, o2 = -1;
	char *o, *ln;
	if (!*cmd && !*loc && *arg)
		return "unknown command";
	if (*cmd && *arg) {
		ex_print(arg, msg_ft)
		return NULL;
	}
	if ((i = ex_region(loc, &beg, &end, &o1, &o2)))
		return i == 2 && !*cmd ? NULL : xrerr;
	if (o1 >= 0)
		cursor_off = o2 >= 0 ? o2 : o1;
	if (!*cmd && *loc) {
		cursor_row = MAX(beg, end - 1);
		return NULL;
	}
	rstate = rstates+1;
	rstate->s = NULL;
	for (i = beg; i < end; i++) {
		ln = lbuf_get(xb, i);
		if (o1 >= 0 && o2 >= 0 && beg == end - 1)
			o = uc_sub(ln, o1, o2);
		else if (o1 >= 0 && i == beg)
			o = uc_sub(ln, o1, -1);
		else if (o2 >= 0 && i == end - 1)
			o = uc_sub(ln, 0, o2);
		else {
			ex_cprint(ln, msg_ft, -1, 0, *loc ? 0 : opt_left_col, 1);
			continue;
		}
		ex_cprint(o, msg_ft, -1, 0, 0, 1);
		free(o);
		rstate->s = NULL;
	}
	rstate--;
	cursor_row = MAX(beg, end - (cmd[0] || loc[0]));
	return NULL;
}

/** @brief `:d` delete a range. */
static void *ec_delete(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = -1, o2 = -1;
	sbuf sb;
	char *p = NULL;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (o1 >= 0) {
		sb.s = "";
		sb.s_n = 0;
		p = lbuf_joinsb(xb, beg, end - 1, &sb, &o1, &o2);
		cursor_off = o1;
	}
	lbuf_edit(xb, p, beg, end, o1, o2);
	free(p);
	cursor_row = MIN(beg, lbuf_len(xb) - !!lbuf_len(xb));
	return NULL;
}

/** @brief Register contents, or NULL if unset. */
sbuf *ex_regget(int id)
{
	return id >= 0 && id < str_registers_n ? str_registers[id] : NULL;
}

/** @brief Set (or append to) register c, growing str_registers[] as needed. */
void ex_regput(int c, const char *s, int append)
{
	sbuf *sb;
	if (c >= str_registers_n) {
		int o = str_registers_n;
		str_registers_n = c + 1;
		str_registers = erealloc(str_registers, str_registers_n * sizeof(str_registers[0]));
		memset(str_registers + o, 0, (str_registers_n - o) * sizeof(str_registers[0]));
	}
	sb = str_registers[c];
	if (!sb) {
		sbuf_make(sb, 64)
		str_registers[c] = sb;
	}
	if (!append)
		sbuf_cut(sb, 0)
	sbuf_str(sb, s)
	sbuf_nul4(sb)
}

/** @brief `:ya[+] [reg]` yank a range (+ appends); `:ya! reg` frees the register. */
static void *ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = 0, o2 = -1;
	int reg = atoi(arg);
	if (reg < 0)
		return xserr;
	if (cmd[2] == '!') {
		sbuf *sb = ex_regget(reg);
		if (!sb)
			return xuerr;
		sbuf_free(sb)
		str_registers[reg] = NULL;
		return NULL;
	} else if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	sbuf sb;
	lbuf_region(xb, &sb, beg, o1, end - 1, o2);
	ex_regput(reg, sb.s, cmd[2] == '+');
	free(sb.s);
	return NULL;
}

/** @brief `:pu [reg][!cmd]` put a register after the range, or pipe it to cmd. */
static void *ec_put(char *loc, char *cmd, char *arg)
{
	int beg, end, i = 0, reg = default_reg;
	sbuf *buf;
	for (; uc_isdigit(arg[i]); i++)
		reg = i ? reg * 10 + (arg[i] - '0') : arg[i] - '0';
	if (!(buf = ex_regget(reg)))
		return "uninitialized register";
	for (; arg[i] && arg[i] != '!'; i++);
	if (arg[i] == '!' && arg[i+1])
		return ex_pipeout(arg + i + 1, buf);
	int n = lbuf_len(xb), o1 = -1, o2 = -1;
	if (!*loc || (i = ex_region(loc, &beg, &end, &o1, &o2))) {
		if (*loc && i != 2 && !(beg == -1 && end == 0 && o1 < 0
				&& (lbuf_len(xb) || i == 3)))
			return xrerr;
		else if (!*loc || i == 2) {
			beg = MAX(0, MIN(lbuf_len(xb), cursor_row));
			end = beg + 1;
		}
	}
	if (o1 >= 0) {
		char *p = lbuf_joinsb(xb, end - 1, end - 1, buf, &o1, &o2);
		lbuf_edit(xb, p, end - 1, end, o1, o1);
		free(p);
	} else
		lbuf_edit(xb, buf->s, end, end, o1, o1);
	cursor_row = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	return NULL;
}

/** @brief `:= [0-3]` print the parsed range (beg, end, o1, o2); `:=?` print the last error. */
static void *ec_num(char *loc, char *cmd, char *arg)
{
	if (cmd[1] == '?') {
		ex_print(ex_prev_ret ? ex_prev_ret : "no error", msg_ft)
		return *arg ? ex_prev_ret : NULL;
	}
	char msg[128];
	int arr[4] = {0, 0, -1, -1};
	int ret = ex_region(loc, &arr[0], &arr[1], &arr[2], &arr[3]);
	int d = (unsigned char)*arg ^ '0';
	if (ret && !((*arg && arg[1]) || (*arg && d >= 4)))
		return xrerr;
	if (d < 4)
		itoa(arr[d], msg);
	else
		sprintf(msg, "%d %d %d %d", arr[0], arr[1], arr[2], arr[3]);
	ex_print(msg, msg_ft)
	return NULL;
}

/** @brief `:ud` undo, `:rd` redo. */
static void *ec_undoredo(char *loc, char *cmd, char *arg)
{
	int ref;
	return (cmd[0] == 'u' ? lbuf_undo : lbuf_redo)(xb, &ref, &ref) ?
		xuerr : NULL;
}

/** @brief `:bs [x]` mark buffer saved; an argument also clears undo history. */
static void *ec_bufsave(char *loc, char *cmd, char *arg)
{
	lbuf_saved(xb, *arg);
	return NULL;
}

/** @brief `:m ids...` set marks at range start/end alternately; `:m!` with no ids clears all. */
static void *ec_mark(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = cursor_off, o2 = cursor_off;
	if (cmd[1] == '!') {
		if (!*arg) {
			xb->mark_n = 0;
			xb->mark_sb[0] = -1;
			xb->mark_se[0] = -1;
			return NULL;
		}
		beg = -1;
		end = 0;
	} else if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	for (int i = 0; uc_isdigit(*arg); i++) {
		int mk;
		for (mk = 0; uc_isdigit(*arg); arg++)
			mk = mk * 10 + (*arg - '0');
		lbuf_mark(xb, mk, i % 2 ? end - 1 : beg, i % 2 ? o2 : o1);
		while (*arg == ' ')
			arg++;
	}
	return NULL;
}

/**
 * @brief `:s/re/rep/[flags]`. Flags: g all matches, m match the region as one
 * string, ^ anchor every search at bol (implies g), digits: operate on that register.
 * offs[] are byte offsets relative to ln.
 */
static void *ec_substitute(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = -1, o2 = -2, flg, grp, reg = -1;
	char *pat, *rep = NULL, *_rep, *p, *err = NULL;
	char *s = arg;
	rset *rs = search_rset;
	int i, first = -1, last = 0;
	struct lopt *lo;
	sbuf text, *rb;
	int e = ex_region(loc, &beg, &end, &o1, &o2);
	if (e && *loc)
		return xrerr;
	pat = ex_re_read(&s);
	if (pat && (*pat || !rs))
		rs = rset_smake(pat, opt_ignorecase ? REG_ICASE : 0);
	if (!rs || opt_search_group >= rs->nsubc) {
		if (rs != search_rset)
			rset_free(rs);
		free(pat);
		return rs ? xgerr : xserr;
	}
	if (pat && *s) {
		s--;
		rep = ex_re_read(&s);
	}
	free(pat);
	int offs[rs->nsubc];
	char *lnb, *ln, *suf = "", *fr = NULL;
	int b1 = 0, pend, rflg = REG_NEWLINE, hit = 0;
	sbuf_smake(r, 256)
	for (i = 0, flg = 0; s[i]; i++) {
		if (s[i] == 'g')
			flg |= 1;
		else if (s[i] == 'm')
			flg |= 2;
		else if (s[i] == '^')
			flg |= 5;	/* ^ anchors every search, implies g */
		else if (uc_isdigit(s[i]))
			reg = (reg < 0 ? 0 : reg * 10) + s[i] - '0';
		else		/* only flags may break up the register */
			reg = -1;
	}
	if (reg >= 0) {		/* the register is the whole region */
		if (*loc)
			err = "register takes no range";
		else if (!(rb = ex_regget(reg)))
			err = "uninitialized register";
		if (err)
			goto out;
		flg |= 2;
		ln = rb->s;
		rflg = 0;
		end = 1;
		i = 0;
		goto mltest;
	} else if (e) {
		err = xrerr;
		goto out;
	}
	if (flg & 2) { 	/* multiline */
		lbuf_region(xb, &text, beg, MAX(o1, 0), end - 1, o2);
		ln = text.s;
		fr = ln;
		rflg = 0;
		pend = end;
		end = beg+1;
		i = beg;
		goto mltest;
	}
	for (i = beg; i < end; i++) {
		lnb = lbuf_get(xb, i), ln = lnb, suf = "";
		b1 = o1 > 0 && i == beg ? uc_chr(lnb, o1) - lnb : 0;
		if (o2 >= 0 && i == end - 1)
			suf = uc_chr(lnb, o2);
		rflg = *suf ? 0 : REG_NEWLINE;
		if (*suf) {		/* line ends at an offset */
			free(fr);	/* o1 past o2 spans to the line end */
			ln = fr = uc_sub(lnb, 0, b1 && o1 > o2 ? -1 : o2);
		}
		ln += b1;
		mltest:;
		hit = 0;
		sbuf_cut(r, 0)
		lnb = ln - b1;		/* start of text not yet copied */
		while (rset_find(rs, ln, offs, rflg) >= 0) {
			if (!(flg & 4))	/* only the first search is at bol */
				rflg |= REG_NOTBOL;
			if (offs[opt_search_group] < 0) {
				ln += offs[1] > 0 ? offs[1] : uc_len(ln);
				continue;
			}
			hit = 1;
			sbuf_mem(r, lnb, ln + offs[opt_search_group] - lnb)
			if (rep) {
				for (_rep = rep; *_rep; _rep++) {
					if (*_rep != '\\' || !_rep[1] || !uc_isdigit(*++_rep)) {
						sbuf_chr(r, *_rep)
						continue;
					}
					grp = *_rep - '0';
					while (grp && uc_isdigit(_rep[1]) &&
						(grp * 10 + _rep[1] - '0') < (rs->nsubc >> 1))
						grp = grp * 10 + *++_rep - '0';
					grp *= 2;
					if (grp + 1 >= rs->nsubc)
						sbuf_chr(r, *_rep)
					else if (offs[grp] >= 0)
						sbuf_mem(r, ln + offs[grp], offs[grp + 1] - offs[grp])
				}
			}
			ln += offs[opt_search_group + 1];
			if ((offs[1] == offs[0] || !offs[opt_search_group + 1]) && *ln) {
				int l = uc_len(ln);	/* zero-length match */
				sbuf_mem(r, ln, l)
				ln += l;
			}
			lnb = ln;
			if (!*ln || !(flg & 1))
				break;
		}
		if (hit) {
			sbuf_str(r, lnb)
			sbufn_str(r, suf)	/* text after the o2 offset */
			if (reg >= 0) {
				ex_regput(reg, r->s, 0);
				first = 0;
				goto out;
			} else if (first < 0) {	/* undo marks */
				first = i;
				lo = lbuf_opt(xb, cursor_row, cursor_off, 0);
				lbuf_smark(xb, lo, i, MAX(o1, 0));
				lbuf_emark(xb, lo, 0, 0);
			}
			if (flg & 2) {
				p = o1 >= 0 ? lbuf_joinsb(xb, beg, pend - 1, r, &o1, &o2) : NULL;
				lbuf_edit(xb, p ? p : r->s, beg, pend, p ? o1 : 0, MAX(o2, 0));
				free(p);
				lbuf_jump(xb, ']', &last, &pend);	/* joined region end */
			} else {
				lbuf_edit(xb, r->s, i, i + 1, 0, 0);
				last = i;
			}
		}
	}
	if (first >= 0) {	/* redo marks */
		lo = lbuf_opt(xb, cursor_row, cursor_off, 0);
		lbuf_smark(xb, lo, first, MAX(o1, 0));
		lbuf_emark(xb, lo, last, MAX(o2, 0));
	}
	out:
	free(fr);
	free(r->s);
	if (rs != search_rset)
		rset_free(rs);
	free(rep);
	return err ? err : first < 0 ? xuerr : NULL;
}

/** @brief `:!cmd` run a command; with a range, filter the range through it. */
static void *ec_exec(char *loc, char *cmd, char *arg)
{
	if (!*loc)
		return ex_pipeout(arg, NULL);
	int beg, end, o1 = -1, o2 = -1, e;
	if ((e = ex_region(loc, &beg, &end, &o1, &o2))) {
		if (lbuf_len(xb) || !(e == 3 && beg == -1 && end == 0 && o1 < 0))
			return xrerr;
		beg = 0;
	}
	sbuf text;
	lbuf_region(xb, &text, beg, MAX(o1, 0), end - 1, o2);
	sbuf *rep = cmd_pipe(arg, &text, 1, NULL);
	free(text.s);
	if (!rep)
		return "fork failed";
	if (o1 >= 0) {
		char *p = lbuf_joinsb(xb, beg, end - 1, rep, &o1, &o2);
		lbuf_edit(xb, p, beg, end, o1, o2);
		free(p);
	} else
		lbuf_edit(xb, rep->s, beg, end, 0, 0);
	sbuf_free(rep)
	return NULL;
}

/** @brief `:ft [type]` set or print the filetype. */
static void *ec_ft(char *loc, char *cmd, char *arg)
{
	int i;
	for (i = 0; *arg && i < ftslen; i++)
		if (!strcmp(fts[i].ft, arg)) {
			arg = fts[i].ft;
			break;
		}
	if (!(loc = syn_setft(*arg ? arg : xb_ft)))
		return "filetype not found";
	xb_ft = loc;
	if (!*arg)
		ex_print(xb_ft, msg_ft)
	led_extcut();
	for (i = 0; i < hloptslen; i++)
		syn_reloadft(syn_findhl(hlopts[i]), 0);
	return NULL;
}

/** @brief `:cm[!] [name]` set the alternate keymap (and activate it unless !). */
static void *ec_cmap(char *loc, char *cmd, char *arg)
{
	if (arg[0])
		keymap_alt = conf_kmapfind(arg);
	else
		ex_print(conf_kmap(cur_keymap)[0], msg_ft)
	if (arg[0] && !strchr(cmd, '!'))
		cur_keymap = keymap_alt;
	return NULL;
}

/**
 * @brief `:g[!]/re/cmd` run cmd on matching (or non-matching) lines.
 * Lines in range are tagged with the global_depth bit first, so lines the
 * command inserts are skipped.
 */
static void *ec_glob(char *loc, char *cmd, char *arg)
{
	int i, beg, end, not;
	void *ret = xuerr;
	char *pat, *s = arg;
	rset *rs;
	if (!loc[0] && !global_depth)
		loc = "%";
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	not = !!strchr(cmd, '!');
	pat = ex_re_read(&s);
	if (pat && *pat)
		rs = rset_smake(pat, opt_ignorecase ? REG_ICASE : 0);
	else
		rs = rset_smake(ex_regget('/') ? ex_regget('/')->s : "", opt_ignorecase ? REG_ICASE : 0);
	free(pat);
	if (!rs)
		return xserr;
	global_depth = !global_depth ? 1 : global_depth * 2;
	for (i = beg; i < end; i++)
		lbuf_i(xb, i)->grec |= global_depth;
	for (i = beg; i < lbuf_len(xb);) {
		char *ln = lbuf_get(xb, i);
		lbuf_s(ln)->grec &= ~global_depth;
		if (rset_match(rs, ln, REG_NEWLINE) != not) {
			cursor_row = i;
			if ((ret = ex_exec(s)))
				break;
			i = MIN(i, cursor_row);
		}
		while (i < lbuf_len(xb) && !(lbuf_i(xb, i)->grec & global_depth))
			i++;
	}
	rset_free(rs);
	global_depth /= 2;
	return ret;
}

/** @brief Drop all capture statuses. */
static void xcid_free(void)
{
	free(capture_status);
	capture_status = NULL;
	capture_n = 0;
}

/** @brief `:?~` clear captures; `N:?~` unset id N; any arg toggles capture_keep. */
static void *ec_xcid(char *loc, char *cmd, char *arg)
{
	if (*arg)
		capture_keep = !capture_keep;
	if (*loc) {
		unsigned int id = atoi(loc);
		if (id < capture_n)
			capture_status[id] = -1;
	} else if (!*arg)
		xcid_free();
	return NULL;
}

/**
 * @brief Conditionals and loops.
 *  - `N:?` / `N:?!` run arg N times (`$` = forever) while it succeeds (! : fails).
 *  - `:?? cmd` run cmd if the previous command succeeded (`??!`: failed).
 *  - `id:??` record the previous command's status under id.
 *  - `ids:?? cmd` run cmd if the recorded statuses pass (`,` = and, `;` = or);
 *    `ids:???` only returns the result.
 */
static void *ec_while(char *loc, char *cmd, char *arg)
{
	int isdq = cmd[1] == '?';
	int inv = cmd[1 + isdq] == '!';
	char *ret = NULL;
	if (isdq && *loc) {
		unsigned int id = atoi(loc);
		if (!*arg && cmd[2] != '?') {
			int err = (ex_prev_ret != NULL) ^ inv;
			if (id >= INT_MAX / 2)
				return xserr;
			if (id >= capture_n) {
				unsigned int n = MAX(64, NEXTSZ(capture_n, id + 1 - capture_n));
				capture_status = erealloc(capture_status, n);
				memset(capture_status + capture_n, -1, n - capture_n);
				capture_n = n;
			}
			capture_status[id] = err;
			return ret;
		}
		int and_res = 0, or_res = 1;
		for (;;) {
			if (id >= capture_n || capture_status[id] < 0)
				return ret;
			and_res |= capture_status[id];
			for (; *loc && *loc != ',' && *loc != ';'; loc++);
			if (!*loc || *loc == ';') {
				 or_res &= and_res;
				 and_res = 0;
			}
			if (!*loc) {
				if (cmd[2] == '?')
					return or_res ? xuerr : NULL;
				return (or_res ^ inv) ? xuerr : ex_exec(arg);
			}
			id = atoi(++loc);
		}
	} else if (isdq) {
		ret = (ex_prev_ret != NULL) ^ inv ? xuerr : NULL;
		return !ret && *arg ? ex_exec(arg) : ret;
	} else if (!*arg)
		return ret;
	int count = *loc ? (*loc == '$' ? INT_MAX : atoi(loc)) : 1;
	for (; count && !ret; count--) {
		ret = ex_exec(arg);
		ret = inv ? ret ? NULL : xuerr : ret;
	}
	return ret;
}

/** @brief `:j [x]` join lines; an argument inserts spaces. */
static void *ec_join(char *loc, char *cmd, char *arg)
{
	int beg, end, o2 = 0;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	cursor_row = beg;
	return lbuf_join(xb, beg, end+1, cursor_off, &o2, arg[0]) ? xuerr : NULL;
}

/** @brief `:fp [dir]` set the default directory; `:fd [dir]` rebuild the file list. */
static void *ec_setdir(char *loc, char *cmd, char *arg)
{
	/* directory remembered by :fp */
	static char *exdir;
	if (cmd[1] == 'p') {
		free(exdir);
		exdir = *arg ? uc_dup(arg) : NULL;
	} else if (cmd[1] == 'd')
		dir_calc(*arg ? arg : (exdir ? exdir : "."));
	return NULL;
}

/** @brief `:cd [dir]` and rewrite relative buffer paths to stay valid. */
static void *ec_chdir(char *loc, char *cmd, char *arg)
{
	char oldpath[4096];
	char newpath[4096];
	char *opath;
	int i, c, plen;
	oldpath[0] = '\0';
	oldpath[sizeof(oldpath)-1] = '\0';
	if (!getcwd(oldpath, sizeof(oldpath)))
		if ((opath = getenv("PWD")))
			strncpy(oldpath, opath, sizeof(oldpath)-1);
	plen = strlen(oldpath);
	i = plen == sizeof(oldpath)-1;
	if (chdir(*arg ? arg : oldpath))
		return "chdir error";
	if (!getcwd(newpath, sizeof(newpath)))
		return "getcwd error";
	setenv("PWD", newpath, 1);
	if (i)
		return "oldpath >= 4096";
	if (plen && oldpath[plen-1] != '/')
		oldpath[plen++] = '/';
	for (i = 0; i < buf_count; i++) {
		if (!bufs[i].path[0])
			continue;
		if (bufs[i].path[0] == '/') {
			opath = bufs[i].path;
		} else {
			opath = oldpath;
			strncpy(opath+plen, bufs[i].path, sizeof(oldpath)-plen-1);
		}
		/* c: length of the common prefix; strip it if opath lies under newpath */
		for (c = 0; opath[c] && opath[c] == newpath[c]; c++);
		if (newpath[c] || !opath[c])
			c = 0;
		else if (opath[c] == '/')
			c++;
		opath = uc_dup(opath+c);
		free(bufs[i].path);
		bufs[i].path = opath;
		bufs[i].plen = strlen(opath);
	}
	return NULL;
}

/** @brief `:inc [re]` set the file filter for the file list (fsincl). */
static void *ec_setincl(char *loc, char *cmd, char *arg)
{
	rset_free(fsincl);
	if (!*arg)
		fsincl = NULL;
	else if (!(fsincl = rset_smake(arg, opt_ignorecase ? REG_ICASE : 0)))
		return xserr;
	return NULL;
}

/** @brief `:ac [re]` set autocomplete_filter. */
static void *ec_setacreg(char *loc, char *cmd, char *arg)
{
	if (autocomplete_filter)
		sbuf_free(autocomplete_filter)
	if (*arg) {
		sbuf_make(autocomplete_filter, 128)
		sbufn_str(autocomplete_filter, arg)
	} else
		autocomplete_filter = NULL;
	return NULL;
}

/** @brief `:bx [n]` resize bufs[], keeping cur/prev pointers by index. */
static void *ec_setbufsmax(char *loc, char *cmd, char *arg)
{
	int max = *arg ? atoi(arg) : bufs_alloc;
	if (max <= 0)
		return xserr;
	bufs_max = max;
	int bufidx = cur_buf - bufs;
	int pbufidx = prev_buf - bufs;
	int tpbufidx = ex_tpbuf - bufs;
	int istemp = !cur_buf ? 0 : istempbuf(cur_buf);
	for (; buf_count > bufs_max; buf_count--)
		bufs_free(buf_count - 1);
	bufs = erealloc(bufs, sizeof(struct buf) * bufs_max);
	if (!istemp)
		cur_buf = bufidx >= bufs_max ? bufs : bufs+bufidx;
	prev_buf = pbufidx >= bufs_max ? bufs : bufs+pbufidx;
	ex_tpbuf = tpbufidx >= bufs_max ? bufs : bufs+tpbufidx;
	return NULL;
}

/** @brief `:reg` list registers; `N:reg[+] text` set/append; `N:reg` set default_reg. */
static void *ec_regprint(char *loc, char *cmd, char *arg)
{
	if (*loc) {
		int reg = atoi(loc);
		if (reg < 0)
			return xserr;
		if (*arg)
			ex_regput(reg, arg, cmd[3] == '+');
		else
			default_reg = reg;
		return NULL;
	}
	char buf[16];
	int flg = (opt_startup_flags & 2) == 0;
	int wid = itoalen(str_registers_n - 1);
	preserve(int, opt_text_dir, opt_text_dir = 2;)
	for (int i = 0; i < str_registers_n; i++) {
		if (str_registers[i] && !(opt_print_reg > 0 && i == opt_print_reg)) {
			char *e = buf;
			for (int p = itoalen(i); p < wid; p++)
				*e++ = ' ';
			e = itoa(i, e);
			*e++ = ' ';
			*e++ = i > 0 && i < 256 ? i : ' ';
			*e++ = ' ';
			*e = '\0';
			ex_cprint2(buf, msg_ft, -1, 0, 0, flg)
			ex_cprint2(str_registers[i]->s, msg_ft, -1, opt_left_col ? 0 : e - buf, opt_left_col, !flg)
		}
	}
	restore(opt_text_dir)
	return NULL;
}

/**
 * @brief `:ph cp1 cp2 wid len str` add a placeholder (no args: reset);
 * `:uz` toggle zero-width, `:ub` toggle bell chars, `:uc` toggle utf-8 decoding.
 */
static void *ec_setenc(char *loc, char *cmd, char *arg)
{
	if (cmd[0] == 'p') {
		if (!*arg) {
			if (ph != _ph)
				free(ph);
			phlen = LEN(_ph);
			ph = _ph;
			return NULL;
		} else if (ph == _ph) {
			ph = NULL;
			phlen = 0;
		}
		ph = erealloc(ph, sizeof(struct placeholder) * (phlen + 1));
		ph[phlen].cp[0] = strtol(arg, &arg, 10);
		ph[phlen].cp[1] = strtol(arg, &arg, 10);
		ph[phlen].wid = strtol(arg, &arg, 10);
		ph[phlen].wid = MAX(0, ph[phlen].wid);
		ph[phlen].l = strtol(arg, &arg, 10);
		if (*arg == ' ')
			arg++;
		int len = strlen(arg);
		if (len && len < LEN(ph[0].d))
			memcpy(ph[phlen++].d, arg, len + 1);
		return NULL;
	}
	if (cmd[1] == 'z')
		zwlen = !zwlen ? def_zwlen : 0;
	else if (cmd[1] == 'b')
		bclen = !bclen ? def_bclen : 0;
	else if (utf8_length[0xc0] == 1) {
		memset(utf8_length+0xc0, 2, 0xe0 - 0xc0);
		memset(utf8_length+0xe0, 3, 0xf0 - 0xe0);
		memset(utf8_length+0xf0, 4, 0xf8 - 0xf0);
	} else
		memset(utf8_length+1, 1, 255);
	return NULL;
}

/** @brief `:sc[!] [chars]` set the escape, separator, expand and shell characters. */
static void *ec_specials(char *loc, char *cmd, char *arg)
{
	static int *const sp[] = {&ex_escape, &ex_separator, &ex_expand_char, &ex_shell_char};
	int i = 0;
	if (*loc) {
		i = (unsigned char)*loc ^ '0';
		if (!*arg && i < LEN(sp))
			*sp[i] = cmd[2] ? 0 : "\\:%!"[i];
	} else
		for (int j = 0; j < LEN(sp); j++)
			*sp[j] = cmd[2] ? 0 : "\\:%!"[j];
	for (; *arg && i < LEN(sp); i++)
		*sp[i] = *arg++;
	return NULL;
}

/** @brief Append [beg, end) escaped as a literal regex; ex also escapes ex specials. */
void ex_regesc(sbuf *sb, char *beg, char *end, int ex)
{
	for (; beg < end; beg++) {
		if (*beg == '\\') {
			/* class form is safe in any layer */
			sbuf_str(sb, "[\\\\]")
			continue;
		}
		if (ex && (*beg == ex_separator || *beg == ex_expand_char || *beg == ex_shell_char))
			sbuf_chr(sb, ex_escape)
		else if (strchr("!%{[().?^$|*/+", *beg))
			sbuf_chr(sb, '\\')
		sbuf_chr(sb, *beg)
	}
}

/** @brief `:re [kwd]` set the search keyword from arg or from the range text. */
static void *ec_krsset(char *loc, char *cmd, char *arg)
{
	if (*arg && !*loc)
		ex_krsset(arg, +1);
	else {
		int beg, end, o1 = 0, o2 = -1;
		if (ex_region(loc, &beg, &end, &o1, &o2))
			return xrerr;
		sbuf reg;
		lbuf_region(xb, &reg, beg, o1, end - 1, o2);
		sbuf_smake(sb, 64)
		ex_regesc(sb, reg.s, reg.s + reg.s_n, 1);
		free(reg.s);
		sbuf_nul(sb)
		ex_krsset(sb->s, +1);
		free(sb->s);
	}
	return search_rset ? NULL : xserr;
}

/** @brief led extension for opt_hl_reverse: mark adjacent characters drawn in reversed order. */
static void ext_hlr(led_ext *p, led_ctx *x)
{
	ren_state *r = x->r;
	int i, j, l, o;
	for (l = 0, i = 0; i < x->cterm;) {
		o = x->off[i++];
		if (o < 0)
			continue;
		for (l++; x->off[i] == o; i++);
		if (o+1 >= x->n || r->pos[o] + r->wid[o] == r->pos[o + 1])
			continue;
		if (r->pos[o + 1] + r->wid[o + 1] != r->pos[o])
			continue;
		j = x->bound ? x->ctt[l-1] : o;
		x->att[j] = syn_merge(x->att[j], conf_hlrev);
		x->att[j+1] = syn_merge(x->att[j+1], conf_hlrev);
	}
}

/** @brief Option value: a number, else the code of the first character. */
static int eo_val(char *arg)
{
	return uc_isdigit(*arg) || (*arg == '-' && uc_isdigit(arg[1])) ?
		atoi(arg) : (unsigned char)*arg;
}

/* _EO/EO define eo_<opt>() handlers: set the variable from arg, or toggle it */
#define _EO(opt, inner) \
static void *eo_##opt(char *loc, char *cmd, char *arg) { inner }

#define EO(opt, var) \
	_EO(opt, var = *arg ? eo_val(arg) : !var; return NULL;)

EO(pac, opt_print_autocomplete) EO(pr, opt_print_reg) EO(ai, opt_autoindent) EO(err, opt_error_mode) EO(fr, opt_find_reg) EO(ish, opt_interactive_shell) EO(ic, opt_ignorecase) EO(mpt, opt_multiline_prompt)
EO(rr, opt_record_reg) EO(shape, opt_shaping) EO(seq, opt_undo_seq) EO(order, opt_reorder) EO(hll, opt_hl_line) EO(hlw, opt_hl_word)
EO(hlp, opt_hl_pair) EO(hl, opt_syntax_hl) EO(lim, opt_render_limit) EO(led, opt_line_editor) EO(vis, opt_startup_flags)

_EO(ts, opt_tabstop = *arg ? eo_val(arg) : !opt_tabstop; opt_tabstop = MAX(0, opt_tabstop); RST_NULL(0, 1, 2) return NULL;)
_EO(td, opt_text_dir = *arg ? eo_val(arg) : !opt_text_dir; RST_NULL(0, 1) return NULL;)
_EO(grp, opt_search_group = (*arg ? eo_val(arg) : !opt_search_group) * 2; opt_search_group = MAX(0, opt_search_group); return NULL;)

_EO(hlr,
	opt_hl_reverse = *arg ? eo_val(arg) : !opt_hl_reverse;
	led_ext *p = led_extfind(ext_hlr);
	if (opt_hl_reverse && !p)
		led_extreg()->ext_func = ext_hlr;
	else if (!opt_hl_reverse && p)
		led_extdel(p);
	return NULL;
)

_EO(left,
	if (*loc)
		opt_left_col = (xcols / 2) * atoi(loc);
	else if (*arg)
		opt_left_col = atoi(arg);
	else if (lbuf_get(xb, cursor_row))
		opt_left_col = ren_position(lbuf_get(xb, cursor_row))->pos[MIN(cursor_off, rstate->n)];
	return NULL;
)

#undef EO
#define EO(opt) {#opt, eo_##opt}

/** Command table, matched by prefix in order: sort longest of its kind topmost. */
static struct excmd {
	char *name;	///< command name
	void *(*ec)(char *loc, char *cmd, char *arg);	///< handler
} excmds[] = {
	{"@", ec_termexec},
	{"&", ec_termexec},
	{"!", ec_exec},
	{"=?", ec_num},
	{"=", ec_num},
	{"???", ec_while},
	{"?""?!", ec_while},
	{"??", ec_while},
	{"?!", ec_while},
	{"?~", ec_xcid},
	{"?", ec_while},
	{"bp", ec_setpath},
	{"bs", ec_bufsave},
	{"bx", ec_setbufsmax},
	{"b", ec_buffer},
	EO(pac),
	EO(pr),
	{"pu", ec_put},
	{"ph", ec_setenc},
	{"p", ec_print},
	EO(ai),
	{"ac", ec_setacreg},
	EO(err),
	{"ef!", ec_fuzz},
	{"ef", ec_fuzz},
	{"e!", ec_edit},
	{"e", ec_edit},
	{"ft", ec_ft},
	{"fd", ec_setdir},
	{"fp", ec_setdir},
	EO(fr),
	{"f+", ec_find},
	{"f-", ec_find},
	{"f>", ec_find},
	{"f<", ec_find},
	{"f", ec_fuzz},
	EO(ish),
	{"inc", ec_setincl},
	EO(ic),
	{"i", ec_insert},
	{"d", ec_delete},
	EO(grp),
	{"g!", ec_glob},
	{"g", ec_glob},
	EO(mpt),
	{"m!", ec_mark},
	{"m", ec_mark},
	{"q!", ec_quit},
	{"q", ec_quit},
	{"reg+", ec_regprint},
	{"reg", ec_regprint},
	{"re", ec_krsset},
	{"rd", ec_undoredo},
	EO(rr),
	{"r", ec_read},
	{"wq!", ec_write},
	{"wq", ec_write},
	{"w!", ec_write},
	{"w", ec_write},
	{"uc", ec_setenc},
	{"uz", ec_setenc},
	{"ub", ec_setenc},
	{"ud", ec_undoredo},
	EO(shape),
	EO(seq),
	{"sc!", ec_specials},
	{"sc", ec_specials},
	{"s", ec_substitute},
	{"x!", ec_write},
	{"x", ec_write},
	{"ya!", ec_yank},
	{"ya+", ec_yank},
	{"ya", ec_yank},
	{"cm!", ec_cmap},
	{"cm", ec_cmap},
	{"cd", ec_chdir},
	{"c", ec_insert},
	{"j", ec_join},
	EO(ts),
	EO(td),
	EO(order),
	EO(hll),
	EO(hlw),
	EO(hlp),
	EO(hlr),
	EO(hl),
	EO(left),
	EO(lim),
	EO(led),
	EO(vis),
	{"", ec_print}, /* do not remove */
	{"", ec_print}, /* do not remove */
};

/**
 * @brief Append the argument to sb, expanding `%` (path), `%#` (previous
 * buffer), `%N` (bufs[N]), `%@N` (register N) and `!cmd!` (output).
 * @param src  ex line at the argument
 * @param sb   receives the expanded argument
 * @param[out] arg  offset of the argument in sb
 * @return src after the argument (at the separator or NUL)
 */
static const char *ex_arg(const char *src, sbuf *sb, int *arg)
{
	*arg = sb->s_n;
	while (*src && *src != ex_separator) {
		if (*src == ex_expand_char) {
			int n;
			struct buf *pbuf = cur_buf;
			src++;
			if (*src == '@') {
				src++;
				if (uc_isdigit(*src)) {
					for (n = 0; uc_isdigit(*src); src++)
						n = n * 10 + (*src - '0');
					sbuf *reg = ex_regget(n);
					if (reg)
						sbuf_mem(sb, reg->s, reg->s_n)
					pbuf = NULL;
				}
			} else if (*src == '#') {
				src++;
				pbuf = prev_buf;
			} else if (uc_isdigit(*src)) {
				for (n = 0; uc_isdigit(*src); src++)
					n = n * 10 + (*src - '0');
				pbuf = &bufs[n];
			}
			if (pbuf >= bufs && pbuf < &bufs[buf_count] && pbuf->path[0])
				sbuf_mem(sb, pbuf->path, pbuf->plen)
			if (src[-1] == '@')
				sbuf_chr(sb, '@')
			src += *src == ex_escape && src[-1] != '#' && uc_isdigit(src[1]);
		} else if (*src == ex_shell_char) {
			int n = sb->s_n;
			src++;
			ex_sread(sb, (char**)&src, ex_shell_char, ex_escape);
			sbuf_cut(sb, n)
			sbuf *str = cmd_pipe(sb->s + n, NULL, 1, NULL);
			if (str) {
				sbuf_mem(sb, str->s, str->s_n)
				sbuf_free(str)
			}
		} else if (*src == ex_escape) {
			ex_parity(src, sb, ex_escape,
				src[n] == ex_separator || src[n] == ex_expand_char || src[n] == ex_shell_char)
		} else
			sbuf_chr(sb, *src++)
	}
	sbuf_nul(sb)
	return src;
}

/**
 * @brief Copy the address prefix to sb (NUL-terminated) and find the command.
 * @param src  ex line at the address prefix
 * @param sb   receives the address prefix
 * @param[in,out] idx  excmds[] index; LEN(excmds)-1 marks an empty command
 * @return src after the command name
 */
static const char *ex_cmd(const char *src, sbuf *sb, int *idx)
{
	int i, j;
	if ((*src && *src == ex_separator) || (*idx == LEN(excmds) - 1))
		src++;
	while (memchr(" \t0123456789+-.,<>/$';%*#|", *src, 26)) {
		if (*src == '>' || *src == '<' || *src == '|') {
			int esc = 0;
			j = *src;
			i = j == '|' ? ex_escape : '\\';
			do {
				esc = *src == i && !esc;
				sbuf_chr(sb, *src++)
			} while (*src && (*src != j || esc));
			if (!*src)
				break;
		} else if (*src == ' ' || *src == '\t') {
			src++;
			continue;
		}
		sbuf_chr(sb, *src++)
	}
	sbuf_chr(sb, '\0')
	if (*src == ex_separator) {
		*idx = LEN(excmds) - 1;
		return src;
	}
	for (i = 0; i < LEN(excmds); i++) {
		for (j = 0; excmds[i].name[j]; j++)
			if (!src[j] || src[j] != excmds[i].name[j])
				break;
		if (!excmds[i].name[j]) {
			*idx = i;
			src += j;
			break;
		}
	}
	if (*src == ' ' || *src == '\t')
		src++;
	return src;
}

/** @brief Execute an ex command chain; returns NULL or an error string. */
void *ex_exec(const char *ln)
{
	int arg, idx = 0;
	char *ret = NULL;
	preserve(int, quit_state, quit_state = 0;)
	if (!ex_exec_depth)
		lbuf_mark(xb, '*', cursor_row, cursor_off);
	ex_exec_depth++;
	sbuf_smake(sb, 128)
	do {
		sbuf_cut(sb, 0)
		ln = ex_arg(ex_cmd(ln, sb, &idx), sb, &arg);
		ret = excmds[idx].ec(sb->s, excmds[idx].name, sb->s + arg);
		ex_prev_ret = ret;
		if (ret && ret != xuerr && opt_error_mode & 1) {
			ex_print(ret, msg_ft)
			ret = xuerr;
		}
		if (ret && opt_error_mode & 2)
			break;
	} while (*ln && !quit_state);
	free(sb->s);
	ex_exec_depth--;
	if ((quit_state > 0 && (ex_exec_depth || quit_propagate >= 0) && --quit_propagate < 0)
			|| tmpquit_state < -256)
		restore(quit_state)
	if (!ex_exec_depth) {
		if (capture_status && !capture_keep)
			xcid_free();
		quit_propagate = 0;
	}
	return opt_error_mode & 4 ? NULL : ret;
}

/** @brief Ex main loop: read and execute commands until quit_state is set. */
void ex(void)
{
	vi_ex_depth++;
	int esc = 0;
	sbuf_smake(sb, xcols)
	while (!quit_state) {
		syn_setft(ex_ft);
		if (ex_read(sb, ":", NULL, 0, 1) == '\n') {
			if (!strcmp(sb->s, ":") && esc) {
				print_newline = 2;
				ex_exec(ex_regget(':')->s);
			} else
				ex_command(sb->s + !(opt_startup_flags & 1))
			xb->useq += opt_undo_seq;
			esc = 1;
		} else
			esc = 0;
		sbuf_cut(sb, 0)
	}
	syn_setft(xb_ft);
	free(sb->s);
	vi_ex_depth--;
}

/** @brief Open the n files (at least one buffer) and run $EXINIT. */
void ex_init(char **files, int n)
{
	bufs_alloc = MAX(n, bufs_alloc);
	ec_setbufsmax(NULL, NULL, "");
	char *s = files[0] ? files[0] : "";
	do {
		opt_multiline_prompt = 0;
		ec_edit("", "e", s);
		s = *(++files);
	} while (--n > 0);
	opt_startup_flags &= ~4;
	if ((s = getenv("EXINIT")))
		ex_command(s)
}
