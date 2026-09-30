/**
 * @file vi.c
 * @brief Visual mode: main(), the normal-mode command loop vi(), screen
 * drawing, motions and operators.
 *
 * The editor is built as one translation unit: this file includes vi.h and
 * then every other .c file, so their statics are visible here.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vi.h"
#include "conf.c"
#include "ex.c"
#include "lbuf.c"
#include "led.c"
#include "regex.c"
#include "ren.c"
#include "term.c"
#include "uc.c"

int vi_hidch;	///< show hidden chars (V toggles)
int vi_lncol;	///< width of the line-number column, added to the cursor column
static int vi_lnnum;	///< line numbers set by #: 1 both for one command, bit 2 absolute, bit 4 relative (also at the indent), bit 8 relative
/** screen redraw - bit 1: whole screen, bit 2: current line, bit 3: update vi_col; any nonzero value recomputes vi_col */
static int vi_mod;
static char vi_word_m[] = "\0leEwW";	///< motion hint modes drawn under the cursor line (^v); "\0" = off
static char *vi_word = vi_word_m;	///< current hint mode, points into vi_word_m
static char *_vi_word = vi_word_m;	///< base of vi_word_m
static int vi_wsel = 1;	///< last selected hint mode index (1..5)
static int vi_rshift;	///< rows below the cursor are shifted down by 1 while the hint row is shown
static int vi_arg;	///< numeric count prefix, 0 if none
static char vi_charlast[5];	///< the last character searched via f, t, F, or T
static int vi_charcmd;	///< the character finding command (f, t, F or T)
static int vi_ybuf;	///< register from a "x prefix, -1 if not given
static int vi_col;	///< desired screen column: set by | and kept across j/k
static int vi_scrollud;	///< scroll amount for ^u and ^d, 0 = half screen
static int vi_scrolley;	///< scroll amount for ^e and ^y
static int vi_cndir = 1;	///< ^n buffer cycling direction
static int vi_status;	///< permanent status bar shown (takes one row from xrows)
static int vi_tsm;	///< type of the status message: 0 file info, 1 character info
static int vi_nlmode;	///< 1: w/b/e and space motions do not cross line ends (toggled by vw)

/** @brief malloc() that exits on failure. */
void *emalloc(size_t size)
{
	void *p;
	if (!(p = malloc(size))) {
		fprintf(stderr, "\nmalloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

/** @brief realloc() that exits on failure. */
void *erealloc(void *p, size_t size)
{
	if (!(p = realloc(p, size))) {
		fprintf(stderr, "\nrealloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

static void reverse_in_place(char *str, int len)
{
	char *p1 = str;
	char *p2 = str + len - 1;
	while (p1 < p2) {
		char tmp = *p1;
		*p1++ = *p2;
		*p2-- = tmp;
	}
}

/** @brief Write n in decimal to s; returns a pointer to the terminating NUL. */
char *itoa(int n, char s[])
{
	int i = 0, sign;
	if ((sign = n) < 0)		/* record sign */
		n = -n;			/* make n positive */
	do {				/* generate digits in reverse order */
		s[i++] = n % 10 + '0';	/* get next digit */
	} while ((n /= 10) > 0);	/* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	reverse_in_place(s, i);
	return &s[i];
}

/** @brief Draw msg on the bottom (status) row. */
static void vi_drawmsg(char *msg)
{
	syn_blockhl = -1;
	preserve(int, opt_text_dir, opt_text_dir = 2;)
	preserve(int, ftidx,)
	syn_setft(bar_ft);
	RST(2, led_crender(msg, xrows, 0, 0, xcols))
	restore(opt_text_dir)
	restore(ftidx)
}
#define vi_drawmsg_mpt(msg) { vi_drawmsg(msg); if (!opt_multiline_prompt) opt_multiline_prompt = 1; }

/** @brief Move *off to the character one screen column away in dir; -1 at the line end. */
static int vi_nextcol(char *ln, int dir, int *off)
{
	int o = ren_off(ln, ren_next(ln, ren_pos(ln, *off), dir));
	if (*rstate->chrs[o] == '\n')
		return -1;
	*off = o;
	return 0;
}

/* Draw digit hints into tmp[] at the screen column of each successive
 * position reached by the motion func (starting from the cursor). */
#define vi_drawnum(func) \
{ \
nrow = cursor_row; \
noff = cursor_off; \
for (i = 0, ret = 0;; i++) { \
	l1 = ren_next(c, ren_pos(c, noff), 1)-1-opt_left_col+vi_lncol; \
	if (l1 > xcols || l1 < 0 || ret || l1 >= rstate->cmax + vi_lncol) \
		break; \
	i = i > 99 ? i % 100 : i; \
	itoa(i%10 ? i%10 : i, snum); \
	tmp[l1] = *snum; \
	ret = func; \
} } \

/** @brief Draw buffer row on screen, with the motion hint row or line numbers if enabled. */
static void vi_drawrow(int row)
{
	int l1, i, i1, lnnum = vi_lnnum;
	char *c, *s;
	static char ch[5] = "~";
	if (opt_multiline_prompt == 1 && !vi_status && row == view_top_row + xrows - 1)
		return;
	if (*vi_word && opt_line_editor) {
		int noff, nrow, ret;
		c = lbuf_get(xb, cursor_row);
		if (row != cursor_row+1 || !c || *c == '\n') {
			vi_rshift = (row > cursor_row+1 && c && *c != '\n');
			s = lbuf_get(xb, row - vi_rshift);
			goto skip;
		}
		char tmp[xcols+3], snum[32];
		memset(tmp, ' ', xcols+1);
		tmp[xcols+1] = '\n';
		tmp[xcols+2] = '\0';
		i1 = uc_isupper(*vi_word);
		if (*vi_word == 'e' || *vi_word == 'E')
			vi_drawnum(lbuf_wordend(xb, i1, 2, &nrow, &noff))
		else if (*vi_word == 'w' || *vi_word == 'W')
			vi_drawnum(lbuf_wordbeg(xb, i1, 2, &nrow, &noff))
		if (*vi_word == 'l') {
			vi_drawnum(vi_nextcol(c, 1, &noff))
			vi_drawnum(vi_nextcol(c, -1, &noff))
		} else
			vi_drawnum(lbuf_wordend(xb, i1, -2, &nrow, &noff))
		l1 = ren_next(c, ren_pos(c, cursor_off), 1)-1-opt_left_col+vi_lncol;
		if (l1 >= 0 && l1 <= xcols)
			tmp[l1] = *vi_word;
		preserve(int, opt_reorder, opt_reorder = 0;)
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, opt_text_dir, opt_text_dir = dir_context(c) * 2;)
		preserve(int, ftidx,)
		syn_setft(n_ft);
		RST(2, led_crender(tmp, row - view_top_row, 0, 0, xcols))
		restore(opt_reorder)
		restore(syn_blockhl)
		restore(opt_text_dir)
		restore(ftidx)
		return;
	}
	s = lbuf_get(xb, row);
	skip:
	rstate += row != cursor_row;
	if (!s)
		s = row ? ch : ch+1;
	else if (lnnum && opt_line_editor) {
		char tmp[32], tmp1[32], *p;
		c = tmp, i = 0, i1 = 0;
		if (lnnum == 1 || lnnum & 2) {
			c = itoa(row+1-vi_rshift, tmp);
			*c++ = ' ';
			i = itoalen(view_top_row+xrows);
		}
		p = c;
		if (lnnum == 1 || lnnum & 4 || lnnum & 8) {
			c = itoa(abs(cursor_row-row+vi_rshift), c);
			*c++ = ' ';
			i1 = itoalen(xrows);
		}
		*c = '\0';
		/* l1: column width, padding the numbers to the digits of the widest
		 * visible absolute (i) and relative (i1) numbers */
		l1 = (c - tmp) + (i+i1 - (strlen(tmp) - !!i - !!i1));
		vi_lncol = dir_context(s) < 0 ? 0 : l1;
		memset(c, ' ', l1 - (c - tmp));
		c[l1 - (c - tmp)] = '\0';
		led_crender(s, row - view_top_row, l1, opt_left_col, opt_left_col + xcols - l1)
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, ftidx,)
		syn_setft(nn_ft);
		if ((lnnum == 1 || lnnum & 4) && !opt_left_col && vi_lncol) {
			for (i1 = 0; i1 < rstate->cmax &&
					memchr(" \t", *rstate->chrs[ren_off(s, i1)], 2);)
				i1 = ren_next(s, i1, 1);
			i1 -= (itoa(abs(cursor_row-row+vi_rshift), tmp1) - tmp1)+1;
			if (i1 >= 0) {
				memset(p, ' ', strlen(p));
				RST(2, led_prender(tmp1, row - view_top_row, l1+i1, 0, l1))
			}
		}
		RST(2, led_prender(tmp, row - view_top_row, 0, 0, l1))
		restore(syn_blockhl)
		restore(ftidx)
		return;
	}
	led_crender(s, row - view_top_row, 0, opt_left_col, opt_left_col + xcols)
	rstate = rstates;
}

/** @brief Redraw screen rows from buffer row i to the bottom. */
static void vi_drawagain(int i)
{
	syn_scdir(0);
	for (; i < view_top_row + xrows; i++)
		vi_drawrow(i);
}

/** @brief Scroll the screen by i rows (term_room) and draw only the uncovered rows. */
static void vi_drawupdate(int i)
{
	int n;
	term_pos(0, 0);
	term_room(i);
	syn_scdir(i);
	if (i < 0) {
		n = MIN(-i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(view_top_row + xrows - n + i);
	} else {
		n = MIN(i, xrows);
		for (i = n-1; i >= 0; i--)
			vi_drawrow(view_top_row + i);
	}
}

/**
 * @brief Prompt on the bottom row.
 * @param msg     prompt text shown before the input
 * @param ft      filetype used to highlight the prompt line
 * @param insert  initial text after msg, may be NULL
 * @param[out] ret   1 if accepted with Enter
 * @param[in,out] kmap  keymap index
 * @param[out] mlen  length of msg; the input starts at the returned string + mlen
 * @return malloc'd msg + input
 */
static char *vi_prompt(char *msg, char *ft, char *insert, int *ret, int *kmap, int *mlen)
{
	sbuf_smake(sb, xcols)
	sbuf_str(sb, msg)
	*mlen = sb->s_n;
	term_pos(xrows, 0);
	syn_setft(ft);
	*ret = led_prompt(sb, insert, kmap, NULL, 0, 1) == '\n';
	syn_setft(xb_ft);
	return sb->s;
}

/** @brief vi_prompt() with the ex filetype and the English keymap. */
static char *vi_enprompt(char *msg, char *insert, int *ret, int *mlen)
{
	int kmap = 0;
	return vi_prompt(msg, ex_ft, insert, ret, &kmap, mlen);
}

/** @brief Read an optional "x register prefix; -1 (key pushed back) if absent. */
static int vi_yankbuf(int winch)
{
	int c = term_read(winch);
	if (c == '"')
		return term_read(0);
	term_dec()
	return -1;
}

/** @brief Read a numeric count; 0 if none. The key after it is consumed too; callers push it back with term_dec(). */
static int vi_prefix(void)
{
	int n = 0;
	int c = term_read(0);
	if (c >= '1' && c <= '9') {
		while (c >= '0' && c <= '9') {
			n = n * 10 + c - '0';
			c = term_read(0);
		}
	}
	return n;
}

/** @brief Read one digit key; -1 if not a digit. */
static int vi_digit(void)
{
	int c = term_read(0);
	if (c >= '0' && c <= '9')
		return c - '0';
	return -1;
}

/** @brief Character offset to screen column in row. */
static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

/** @brief Screen column to character offset in row (clamped to the line). */
static int vi_col2off(struct lbuf *lb, int row, int col)
{
	char *ln = lbuf_get(lb, row);
	if (!ln)
		return 0;
	ren_state *r = ren_position(ln);
	if (col >= r->cmax)
		return r->col[r->cmax - 1];
	return r->col[col];
}

/**
 * @brief `/ ? n N` search cnt times from (*row, *off).
 * @param cmd  '/' or '?' (prompt for a pattern), 'n' or 'N'
 * @param cnt  repeat count
 * @param[in,out] row,off  start position; set to the match
 * @param msg  show messages; 0 is used for silent searches (file search)
 * @return 0 if found
 */
static int vi_search(int cmd, int cnt, int *row, int *off, int msg)
{
	int i, dir, ret;
	char vi_msg[512];
	if (cmd == '/' || cmd == '?') {
		char sign[4] = {cmd};
		char *kw = vi_prompt(sign, vs_ft, NULL, &ret, &cur_keymap, &i);
		vi_drawmsg_mpt(kw)
		if (!ret) {
			free(kw);
			return 1;
		}
		ex_krsset(kw + i, cmd == '/' ? +2 : -2);
		free(kw);
	} else if (msg)
		ex_krsset(ex_regget('/') ? ex_regget('/')->s : NULL, search_dir);
	if (!lbuf_len(xb) || !search_dir)
		return 1;
	else if (!search_rset || opt_search_group >= search_rset->nsubc) {
		vi_drawmsg_mpt(search_rset ? "invalid grp" : "syntax error")
		return 1;
	}
	dir = cmd == 'N' ? -search_dir : search_dir;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, search_rset, dir, 0, lbuf_len(xb),
				msg ? dir : -1, 1, row, off)) {
			if (msg) {
				snprintf(vi_msg, sizeof(vi_msg), "\"%s\" not found %d/%d",
						ex_regget('/') ? ex_regget('/')->s : "", i, cnt);
				vi_drawmsg_mpt(vi_msg)
			}
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Regex for the word(s) at (row, off): `\<word\>` for n <= 1,
 * else n words escaped with ex_regesc(). malloc'd, NULL if none.
 */
static char *vi_curword(struct lbuf *lb, int row, int off, int n, int ex)
{
	char *ln = lbuf_get(lb, row);
	if (!ln || !n)
		return NULL;
	off = ren_noeol(ln, off);
	char **chrs = rstate->chrs;
	int cap = rstate->n;
	int end = off;
	/* [off, end): character range of the word; chrs[] gives its bytes */
	for (int i = 0; i < n && end < cap; i++)
		while (uc_kind(chrs[end++]) == 1);
	for (; off > 0 && uc_kind(chrs[off - 1]) == 1; off--);
	if (!end || --end == off)
		return NULL;
	sbuf_smake(sb, 64)
	if (n <= 1) {
		sbuf_str(sb, "\\<")
		sbuf_mem(sb, chrs[off], chrs[end] - chrs[off])
		sbuf_str(sb, "\\>")
	} else
		ex_regesc(sb, chrs[off], chrs[end], ex);
	sbufn_ret(sb, sb->s)
}

/**
 * @brief Store s in register c like vi: linewise text shifts registers
 * 1..9, otherwise the old content moves to '0'; uppercase c appends.
 */
static void vi_regput(int c, const char *s, int lnmode)
{
	sbuf *i_s;
	if (lnmode) {
		for (int i = 8; i > 0; i--)
			if ((i_s = ex_regget('0'+i)))
				ex_regput('0' + i + 1, i_s->s, 0);
		ex_regput('1', s, 0);
	} else if ((i_s = ex_regget(c)))
		ex_regput('0', i_s->s, 0);
	ex_regput(tolower(c), s, uc_isupper(c));
}

rset *fsincl;	///< regex filter for files listed by dir_calc(), NULL = all
static int fspos;	///< next index in the file list (tempbufs[1]) for ^] / ^p search
static int fsdir;	///< direction of the last file search: 1, -1, or 0

/**
 * @brief Append all regular files under path (recursive) to tempbufs[1].
 * Subdirectories are walked with explicit stacks: dps[] open dirs,
 * ptrs[] their path buffers, plen[] their path lengths.
 */
void dir_calc(char *path)
{
	struct dirent *dirp;
	struct stat statbuf;
	int i = 0, ret;
	char *cpath, *ptrs[1024];
	int plen[1024];
	DIR *dp, *sdp, *dps[1024];
	unsigned int pathlen = strlen(path), len;
	if (!(dp = opendir(path)))
		return;
	cpath = emalloc(pathlen + 1024);
	memcpy(cpath, path, pathlen + 1);
	sbuf_smake(sb, 1024)
	temp_pos(1, -1, 0, 0);
	fspos = 0;
	for (;;) {
		while ((dirp = readdir(dp))) {
			len = strlen(dirp->d_name)+1;
			if (strcmp(dirp->d_name, ".") == 0 ||
				strcmp(dirp->d_name, "..") == 0 ||
				len > 1023)
				continue;
			cpath[pathlen] = '/';
			memcpy(&cpath[pathlen+1], dirp->d_name, len);
			ret = lstat(cpath, &statbuf);
			if (ret >= 0 && S_ISDIR(statbuf.st_mode)) {
				if (i >= LEN(ptrs) || !(sdp = opendir(cpath)))
					break;
				dps[i] = sdp;
				ptrs[i] = cpath;
				cpath = emalloc(pathlen + 1024);
				memcpy(cpath, ptrs[i], pathlen + len);
				plen[i++] = pathlen + len;
			} else if (ret >= 0 && S_ISREG(statbuf.st_mode))
				if (!fsincl || rset_match(fsincl, cpath, 0)) {
					sbuf_mem(sb, cpath, (int)(pathlen + len))
					sbuf_chr(sb, '\n')
				}
		}
		closedir(dp);
		free(cpath);
		if (i > 0) {
			dp = dps[--i];
			pathlen = plen[i];
			cpath = ptrs[i];
		} else
			break;
	}
	sbuf_nul(sb)
	if (sb->s_n > 1)
		temp_write(1, sb->s);
	free(sb->s);
}

/* Open path from the file list and search it; returns 1 from the caller on a hit. */
#define fssearch() \
len = lbuf_s(path)->len; \
path[len] = '\0'; \
ret = ex_edit(path, len); \
path[len] = '\n'; \
if (ret && cursor_row) { \
	*row = cursor_row; *off = cursor_off; /* short circuit */ \
	if (!vi_search('n', cnt, row, off, 0)) \
		return 1; \
	++*off; \
} else { \
	*row = 0; *off = 0; \
} \
if (!vi_search(*row ? 'N' : 'n', cnt, row, off, 0)) \
	return 1; \

/** @brief Search the keyword forward through the files in tempbufs[1], wrapping once. */
static int fs_search(int cnt, int *row, int *off)
{
	char *path;
	int again = 0, ret, len;
	wrap:
	while (fspos < lbuf_len(tempbufs[1].lb)) {
		path = tempbufs[1].lb->ln[fspos++];
		fssearch()
	}
	if (fspos == lbuf_len(tempbufs[1].lb) && !again) {
		fspos = 0;
		again = 1;
		goto wrap;
	}
	return 0;
}

/** @brief Search the keyword backward through the files in tempbufs[1]. */
static int fs_searchback(int cnt, int *row, int *off)
{
	char *path;
	int ret, len;
	while (--fspos >= 0) {
		path = tempbufs[1].lb->ln[fspos];
		fssearch()
	}
	return 0;
}

static char rep_cmd[sizeof(ticmd)];	///< keys of the last change, replayed by .
static int rep_len;	///< length of rep_cmd
/* save the keys of the current command (ticmd) for . */
#define rep_record() memcpy(rep_cmd, ticmd, ticmd_pos); rep_len = ticmd_pos;

/** @brief Show file info (type 0) or info on the character under the cursor (type 1). */
static void vc_status(int type)
{
	int l, col;
	unsigned int cp;
	char cbuf[8] = "", vi_msg[512], *c;
	col = vi_off2col(xb, cursor_row, cursor_off);
	col = ren_cursor(lbuf_get(xb, cursor_row), col) + 1;
	if (type && lbuf_get(xb, cursor_row)) {
		c = rstate->chrs[cursor_off];
		uc_code(cp, c, l)
		memcpy(cbuf, c, l);
		snprintf(vi_msg, sizeof(vi_msg), "<%s> 0x%x 0%o %u %dL %dW S%td O%d C%d",
			cbuf, cp, cp, cp, l, rstate->wid[cursor_off], c - lbuf_get(xb, cursor_row),
			cursor_off, col);
	} else {
		snprintf(vi_msg, sizeof(vi_msg),
			"\"%s\"%s%dL %d%% L%d C%d B%td",
			xb_path[0] ? xb_path : "unnamed",
			xb->modified ? "* " : " ", lbuf_len(xb),
			cursor_row * 100 / MAX(1, lbuf_len(xb)-1), cursor_row+1, col,
			istempbuf(cur_buf) ? tempbufs - cur_buf - 1 : cur_buf - bufs);
	}
	vi_drawmsg_mpt(vi_msg)
}

/**
 * @brief Read a motion key and move (*row, *off).
 * @param cmd  the operator key (e.g. 'd'), or -1 for plain cursor movement;
 *             repeating the operator (dd) selects whole lines
 * @param[in,out] row,off  position, moved by the motion
 * @return the motion key, 0 if the key is not a motion, -1 if it failed;
 *         *off = -1 on return means a linewise motion
 */
static int vi_region(int cmd, int *row, int *off)
{
	static sbuf *savepath[5];
	static rset *bre;
	static int srow[5], soff[5], lkwdcnt;
	static int cadir = 1;
	char *cs;
	int cnt = vi_arg ? vi_arg : 1;
	int mv, i, dir, var;

	mv = term_read(0);
	switch (mv) {
	case ',':
	case ';':
		if (!vi_charlast[0])
			return -1;
		if (mv == ',')
			mv = vi_charcmd == 'F' || vi_charcmd == 'T'
				? tolower(vi_charcmd) : toupper(vi_charcmd);
		else
			mv = vi_charcmd;
		if (lbuf_findchar(xb, vi_charlast, mv, cnt, row, off))
			return -1;
		break;
	case 'h':
	case 'l':
		if (!(cs = lbuf_get(xb, *row)))
			return -1;
		dir = dir_context(cs);
		dir = mv == 'h' ? -dir : dir;
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(cs, dir, off))
				break;
		break;
	case ' ':
	case 127:
	case TK_CTL('h'):
		dir = mv == ' ' ? +1 : -1;
		cs = lbuf_get(xb, *row);
		var = cs ? ren_position(cs)->n : 0;
		i = *off;
		*off += cnt * dir;
		if (vi_nlmode) {
			*off = *off < 0 ? 0 : *off;
			break;
		}
		if (*off < 0 || *off >= var) {
			cnt -= dir > 0 ? var - i : i;
			*off = dir > 0 ? var : 0;
			while ((cs = lbuf_get(xb, *row + dir))) {
				*row += dir;
				var = uc_slen(cs);
				if (cnt - var <= 0) {
					*off = dir < 0 ? var - cnt : cnt;
					break;
				}
				cnt -= var;
			}
		}
		if (cmd < 0 && dir > 0 && lbuf_get(xb, *row + dir)
				&& (var > 1 && *off >= var - 1)) {
			*row += dir;
			*off = 0;
		}
		break;
	case 'f':
	case 'F':
	case 't':
	case 'T':
		if (!(cs = led_read(&cur_keymap, term_read(0))))
			return -1;
		snprintf(vi_charlast, sizeof(vi_charlast), "%s", cs);
		vi_charcmd = mv;
		if (lbuf_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'b':
	case 'B':
		var = mv == 'B';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, var, -(vi_nlmode+1), row, off))
				break;
		break;
	case 'e':
	case 'E':
		var = mv == 'E';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, var, vi_nlmode+1, row, off))
				break;
		break;
	case 'w':
	case 'W':
		var = mv == 'W';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, var, vi_nlmode+1, row, off))
				break;
		break;
	case '(':
	case ')':
		dir = mv == '(' ? 1 : -1;
		if (!bre)
			bre = rset_smake("^[.?!]+['\\])]*(?:[ \t]+\n?|\n)", 0);
		int subs[2], org;
		for (i = 0; i < cnt; i++) {
			var = *row;
			org = *off;
			for (; (cs = lbuf_get(xb, *row)) && *cs == '\n'; *row += dir);
			if (*row != var) {
				*off = MAX(0, lbuf_indents(xb, *row));
				if (dir > 0)
					continue;
				*off = lbuf_eol(xb, *row, 1);
			}
			while (!lbuf_next(xb, dir, row, off)) {
				cs = rstate->chrs[*off];
				if (*off == 0 && *cs == '\n') {
					if (dir < 0 && (var - *row) > 1)
						*row += 1;
					*off = MAX(0, lbuf_indents(xb, *row));
					break;
				} else if (rset_find(bre, cs, subs, 0) >= 0) {
					if (var == *row && rstate->chrs[org] == cs + subs[1])
						continue;
					if (!cs[subs[1]]) {
						if (dir < 0 && *row + 1 == var)
							continue;
						*row += 1;
						*off = MAX(0, lbuf_indents(xb, *row));
					} else
						*off += uc_off(cs, subs[1]);
					break;
				}
			}
		}
		return mv;
	case '{':
	case '}':
	case '[':
	case ']':
		dir = mv == '{' || mv == '[' ? 1 : -1;
		var = mv == '[' || mv == ']' ? '\n' : '{';
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, dir, row, off, var))
				break;
		break;
	case TK_CTL(']'):	/* this is also ^5 on some systems */
	case TK_CTL('p'):
		#define open_saved(n) \
		if (savepath[n]) { \
			*row = srow[n]; *off = soff[n]; \
			ex_edit(savepath[n]->s, savepath[n]->s_n); \
		} \

		if (vi_arg && (cs = vi_curword(xb, *row, *off, cnt, 0))) {
			ex_krsset(cs, +1);
			free(cs);
		}
		struct buf* tmpcur_buf = istempbuf(cur_buf) ? prev_buf : cur_buf;
		if (mv == TK_CTL(']')) {
			if (vi_arg || lkwdcnt != search_changes)
				term_exec("", 1, '&')
			lkwdcnt = search_changes;
			fspos += fsdir < 0 ? 1 : 0;
			fspos = MIN(fspos, lbuf_len(tempbufs[1].lb));
			fs_search(1, row, off);
			fsdir = 1;
		} else {
			fspos -= fsdir > 0 ? 1 : 0;
			if (!fs_searchback(1, row, off)) {
				open_saved(0)
				fsdir = 0;
			} else
				fsdir = -1;
			fspos = MAX(fspos, 0);
		}
		if (tmpcur_buf != cur_buf)
			prev_buf = tmpcur_buf;
		bsync_ret:
		for (i = buf_count-1; i >= 0 && bufs[i].mtime == -1; i--)
			ex_bufpostfix(&bufs[i], 1);
		syn_setft(xb_ft);
		vc_status(0);
		view_top_row = MAX(0, *row - xrows / 2);
		vi_mod |= 1;
		break;
	case TK_CTL('t'):
		if (vi_arg >= LEN(savepath) * 2)
			break;
		if (vi_arg % 2 == 0) {
			vi_arg /= 2;
			if (!savepath[vi_arg])
				sbuf_make(savepath[vi_arg], 128)
			sbuf_cut(savepath[vi_arg], 0)
			sbufn_str(savepath[vi_arg], xb_path)
			srow[vi_arg] = *row; soff[vi_arg] = *off;
			break;
		}
		open_saved(vi_arg / 2)
		goto bsync_ret;
	case '0':
		*off = 0;
		break;
	case '^':
		*off = MAX(0, lbuf_indents(xb, *row));
		break;
	case '$':
		*off = lbuf_eol(xb, *row, 1);
		break;
	case '|':
		vi_col = cnt - 1;
		break;
	case '/':
	case '?':
	case 'n':
	case 'N':
		if (vi_search(mv, cnt, row, off, 1))
			return -1;
		if (cmd < 0)
			view_top_row = MAX(0, *row - xrows / 2);
		vi_mod |= mv == '/' || mv == '?';
		break;
	case '*':
	case TK_CTL('a'):
		if (mv == TK_CTL('a') || vi_arg) {
			if (!(cs = vi_curword(xb, *row, *off, cnt, 0)))
				return -1;
			ex_krsset(cs, +1);
			free(cs);
		}
		if (vi_search(cadir < 0 ? 'N' : 'n', 1, row, off, 1))
			cadir = -cadir;
		else if (cmd < 0 && (*row < view_top_row || *row >= view_top_row + xrows - !vi_status))
			view_top_row = MAX(0, *row - xrows / 2);
		break;
	case '\n':
	case '+':
	case 'j':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		goto lnregion;
	case 'k':
	case '-':
		*row = MAX(*row - cnt, 0);
		goto lnregion;
	case 'G':
		*row = vi_arg ? cnt - 1 : lbuf_len(xb) - 1;
		goto lnregion;
	case 'H':
		*row = MIN(view_top_row + cnt - 1, lbuf_len(xb) - 1);
		goto lnregion;
	case 'L':
		*row = MIN(view_top_row + xrows - 1 - cnt + 1, lbuf_len(xb) - 1);
		goto lnregion;
	case 'M':
		*row = MIN(view_top_row + xrows / 2, lbuf_len(xb) - 1);
		goto lnregion;
	case '\'':
	case '`':
		if (lbuf_jump(xb, term_read(0), row, &var))
			return -1;
		if (cmd < 0 && (*row < view_top_row || *row >= view_top_row + xrows))
			view_top_row = MAX(0, *row - xrows / 2);
		if (mv == '\'')
			goto lnregion;
		*off = var;
		break;
	case '%':
		if (vi_arg) {
			if (cnt > 100)
				return -1;
			*row = lbuf_len(xb) * cnt / 100;
			goto lnregion;
		} else if (lbuf_pair(xb, "()[]{}", 6, row, off))
			return -1;
		break;
	default:
		if (mv != cmd)
			return 0;
		*row = MIN(*row + cnt - 1, lbuf_len(xb) - 1);
		lnregion:
		*off = -1;
	}
	return mv;
}

/** @brief Yank the region into vi_ybuf (or default_reg). */
static void vi_yank(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf < 0 ? default_reg : vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	cursor_row = r1;
	cursor_off = lnmode ? cursor_off : o1;
}

/** @brief Yank and delete the region; lnmode deletes rows r1..r2. */
static void vi_delete(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf < 0 ? default_reg : vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	if (lnmode)
		lbuf_edit(xb, NULL, r1, r2 + 1, 0, 0);
	else {
		rsb.s = "";
		rsb.s_n = 0;
		char *s = lbuf_joinsb(xb, r1, r2, &rsb, &o1, &o2);
		lbuf_edit(xb, s, r1, r2 + 1, o1, o1);
		free(s);
	}
	cursor_row = r1;
	cursor_off = lnmode ? lbuf_indents(xb, cursor_row) : o1;
}

/** @brief Byte length of the leading blanks of ln (0 if autoindent is off). */
static int vi_indents(char *ln)
{
	if (opt_autoindent <= 0 || !ln)
		ln = "";
	char *pln = ln;
	for (; *ln == ' ' || *ln == '\t'; ln++);
	return ln - pln;
}

/** @brief Replace the region with typed text; returns the key that ended input. */
static int vi_change(int r1, int o1, int r2, int o2, int lnmode)
{
	char *post, *ln = lbuf_get(xb, r1);
	sbuf rsb;
	int key, tlen, l1, l2 = 1, postn = 1;
	sbuf_smake(sb, xcols)
	if (lnmode || !ln) {
		o1 = l1 = vi_indents(ln);
		post = "\n";
		tlen = -1;
		lbuf_region(xb, &rsb, r1, 0, r2, -1);
	} else {
		l1 = uc_chr(ln, o1) - ln;
		post = uc_chr(lbuf_get(xb, r2), o2);
		l2 = uc_chrn(post, -1, &postn) - post;
		tlen = lbuf_s(ln)->len+1;
		lbuf_region(xb, &rsb, r1, o1, r2, o2);
	}
	vi_regput(vi_ybuf < 0 ? default_reg : vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	term_pos(r1 - view_top_row < 0 ? 0 : r1 - view_top_row, 0);
	term_room(r1 < view_top_row ? view_top_row - cursor_row : r1 - r2 -
			(*vi_word && ln && *ln != '\n' && r1 != r2));
	cursor_row = r1;
	if (r1 < view_top_row)
		view_top_row = r1;
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, r1 - (r1 - r2), 0, &postn);
	/* edit only if the resulting line differs from the original */
	if (postn + l2 != tlen || memcmp(ln + l1, sb->s + l1, tlen - l2 - l1))
		lbuf_edit(xb, sb->s, r1, r2 + 1, o1, cursor_off);
	free(sb->s);
	return key;
}

/** @brief Change ASCII case in the region: u lower, U upper, ~ toggle. */
static void vi_case(int r1, int o1, int r2, int o2, int lnmode, int cmd)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	char *s = rsb.s;
	while (uc_len(s)) {
		int c = (unsigned char) s[0];
		if (c <= 0x7f) {
			if (cmd == 'u')
				s[0] = tolower(c);
			if (cmd == 'U')
				s[0] = toupper(c);
			if (cmd == '~')
				s[0] = islower(c) ? toupper(c) : tolower(c);
		}
		s += uc_len(s);
	}
	if (lnmode) {
		lbuf_edit(xb, rsb.s, r1, r2 + 1, 0, 0);
		free(rsb.s);
	} else {
		s = lbuf_joinsb(xb, r1, r2, &rsb, &o1, &o2);
		free(rsb.s);
		lbuf_edit(xb, s, r1, r2 + 1, o1, o2);
		free(s);
	}
	cursor_row = r2;
	cursor_off = lnmode ? lbuf_indents(xb, r2) : o2;
}

/** @brief Prompt with ":r1,r2!" to filter the rows through a command. */
static void vi_pipe(int r1, int r2)
{
	int mlen, ret;
	char region[64], *p = region;
	if (!lbuf_get(xb, r1))
		*p++ = '0';
	else if (r1 == r2 && !vi_arg)
		*p++ = '.';
	else {
		p = itoa(r1+1, region);
		*p++ = ',';
		p = itoa(r2+1, p);
	}
	*p++ = '!';
	*p = '\0';
	char *cmd = vi_enprompt(":", region, &ret, &mlen);
	if (ret)
		ex_command(cmd + mlen)
	if (!opt_multiline_prompt)
		vi_drawmsg_mpt(cmd)
	free(cmd);
}

/** @brief Indent (dir > 0, adds tabs) or unindent (removes up to count blanks) rows r1..r2. */
static void vi_shift(int r1, int r2, int dir, int count)
{
	sbuf_smake(sb, 1024)
	char *ln;
	int i, c;
	for (i = r1; i <= r2; i++) {
		if (!(ln = lbuf_get(xb, i)))
			continue;
		for (c = 0; c < count; c++) {
			if (dir < 0) {
				if (*ln != ' ' && *ln != '\t')
					break;
				ln++;
			} else if (*ln != '\n' || r1 == r2)
				sbuf_chr(sb, '\t')
		}
		sbufn_str(sb, ln)
		lbuf_edit(xb, sb->s, i, i + 1, 0, 0);
		sbuf_cut(sb, 0)
	}
	cursor_off = lbuf_indents(xb, r1);
	free(sb->s);
}

/** @brief Operator command (d c y ! > < ~ u U ^w) followed by a motion. */
static int vc_motion(int cmd)
{
	int r1 = cursor_row, r2 = cursor_row;	/* region rows */
	int o1 = cursor_off, o2;		/* visual region columns */
	int lnmode = 0;			/* line-based region */
	int mv = vi_prefix();
	term_dec()
	if (mv)
		vi_arg = mv;
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	o2 = o1;
	if ((mv = vi_region(cmd, &r2, &o2)) <= 0)
		return 0;
	if (r2 < 0)
		r2 = 0;
	lnmode = o2 < 0;
	if (lnmode) {
		o1 = 0;
		o2 = lbuf_eol(xb, r2, r1 >= r2);
	}
	if (r1 > r2) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	} else if (r1 == r2 && o1 > o2)
		swap(&o1, &o2);
	/* r briefly holds the line pointer; clamp o1 to the line's length */
	ren_state *r = (ren_state*)lbuf_get(xb, r1);
	r = r ? ren_position((char*)r) : NULL;
	o1 = r ? MAX(0, MIN(o1, r->n)) : 0;
	if (!lnmode && strchr("fFtTeE%", mv))
		if (o2 < lbuf_eol(xb, r2, 2))
			o2++;
	if (cmd == 'y') {
		vi_yank(r1, o1, r2, o2, lnmode);
		return 0;
	}
	mv = lbuf_len(xb);
	if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	else if (cmd == 'c')
		return vi_change(r1, o1, r2, o2, lnmode);
	else if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	else if (cmd == '!')
		vi_pipe(r1, r2);
	else if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1,
			lnmode ? 1 : vi_arg ? vi_arg : 1);
	else if (cmd == TK_CTL('w'))
		vi_shift(r1, r2, -1, INT_MAX / 2);
	rep_record()
	vi_mod |= r1 != r2 || mv != lbuf_len(xb) ? 1 : 2;
	return 0;
}

/** @brief Insert commands i I a A o O; returns the key that ended input. */
static int vc_insert(int cmd)
{
	char *post, *ln = lbuf_get(xb, cursor_row);
	int row, cmdo, l1, off, key, postn = 1;
	sbuf_smake(sb, xcols)
	if (cmd == 'I')
		cursor_off = lbuf_indents(xb, cursor_row);
	else if (cmd == 'A')
		cursor_off = lbuf_eol(xb, cursor_row, 1);
	else if (cmd == 'o') {
		cursor_row++;
		if (cursor_row - view_top_row == xrows)
			vi_drawagain(++view_top_row);
	}
	cursor_off = ren_noeol(ln, cursor_off);
	row = cursor_row;
	if (cmd == 'a' || cmd == 'A')
		cursor_off++;
	if (ln && ln[0] == '\n')
		cursor_off = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	if (cmdo || !ln) {
		if (cmdo && !lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0, 0, 0);
		off = l1 = vi_indents(ln);
		post = "\n";
	} else {
		off = cursor_off;
		/* l1: byte offset of the cursor char; post: text after it, postn chars long */
		l1 = rstate->chrs[off] - ln;
		postn = rstate->n - off;
		post = ln + l1;
	}
	term_pos(row - view_top_row, 0);
	term_room(cmdo);
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, row, cmdo << 2, &postn);
	if (postn != l1 || cmdo || !ln)
		lbuf_edit(xb, sb->s, row, row + !cmdo, off, cursor_off);
	free(sb->s);
	return key;
}

/** @brief p / P: put a register after / before the cursor (linewise if it has a newline). */
static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg);
	int i, off;
	char *ln;
	sbuf *buf = ex_regget(vi_ybuf < 0 ? default_reg : vi_ybuf);
	if (!buf || !buf->s_n) {
		vi_drawmsg_mpt(buf ? "empty register" : "uninitialized register")
		return 0;
	}
	rep_record()
	sbuf_smake(sb, 1024)
	if (buf->s[buf->s_n-1] == '\n' || strchr(buf->s, '\n')) {
		for (i = 0; i < cnt; i++)
			sbufn_mem(sb, buf->s, buf->s_n)
		if (!lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0, 0, 0);
		if (cmd == 'p')
			cursor_row++;
		lbuf_edit(xb, sb->s, cursor_row, cursor_row, 0, 0);
		cursor_off = lbuf_indents(xb, cursor_row);
		free(sb->s);
		return 1;
	}
	if (!(ln = lbuf_get(xb, cursor_row)))
		ln = "\n";
	off = ren_noeol(ln, cursor_off) + (ln[0] != '\n' && cmd == 'p');
	/* new line = bytes before char off + register x cnt + rest of the line */
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_mem(sb, buf->s, buf->s_n)
	sbufn_str(sb, rstate->chrs[off])
	cursor_off = off + uc_slen(buf->s) * cnt - 1;
	lbuf_edit(xb, sb->s, cursor_row, cursor_row + 1, off, cursor_off);
	free(sb->s);
	return 1;
}

/** @brief Join cnt lines from the cursor row; spc inserts spaces. */
static void vc_join(int spc, int cnt)
{
	int o2 = 0;
	if (lbuf_join(xb, cursor_row, cursor_row + cnt, cursor_off, &o2, spc))
		return;
	cursor_off = o2;
}

/** @brief Scroll down cnt rows, keeping the cursor on screen. */
static void vi_scrollforward(int cnt)
{
	view_top_row = MIN(lbuf_len(xb) - 1, view_top_row + cnt);
	cursor_row = MAX(cursor_row, view_top_row);
}

/** @brief Scroll up cnt rows, keeping the cursor on screen. */
static void vi_scrollbackward(int cnt)
{
	view_top_row = MAX(0, view_top_row - cnt);
	cursor_row = MIN(cursor_row, view_top_row + xrows - 1);
}

/** @brief r: replace the next count characters with the typed character. */
static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg);
	char *cs = led_read(&cur_keymap, term_read(0));
	char *ln = lbuf_get(xb, cursor_row);
	int off, i;
	if (!ln || !cs)
		return 0;
	off = ren_noeol(ln, cursor_off);
	if (off + cnt >= rstate->n)
		return 0;
	sbuf_smake(sb, lbuf_s(ln)->len)
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs)
	sbufn_str(sb, rstate->chrs[off+cnt])
	cursor_off = (off + cnt - 1) * (cs[0] != '\n');
	lbuf_edit(xb, sb->s, cursor_row, cursor_row + 1, off, cursor_off);
	cursor_row += cnt * (cs[0] == '\n');
	free(sb->s);
	rep_record()
	return cs[0] == '\n' ? 1 : 2;
}

/** @brief \@x / &x: run register x as keys (: register as ex commands); repeated key reuses the last one. */
static void vc_execute(int cmd)
{
	static int exec_buf = -1;
	int c = term_read(0), i, n = MAX(1, vi_arg);
	sbuf *buf;
	if (TK_INT(c))
		return;
	if (c == cmd && exec_buf >= 0)
		c = exec_buf;
	if (!ex_regget(c)) {
		vi_drawmsg_mpt("uninitialized register")
		return;
	}
	exec_buf = c;
	if (c == ':') {
		term_pos(xrows, 0);
		for (i = 0; i < n && (buf = ex_regget(c)); i++)
			ex_exec(buf->s);
		vi_mod |= 1;
		return;
	}
	for (i = 0; i < n && (buf = ex_regget(c)); i++)
		term_exec(buf->s, buf->s_n, cmd)
}

/** @brief Push "<arg><cmd>" as pending input. */
static void vi_argcmd(int arg, char cmd)
{
	char str[32];
	char *cs = itoa(arg, str);
	*cs = cmd;
	term_push(str, cs - str + 1);
}

/* clamp cursor_row to the buffer and scroll so it is visible */
#define topfix() \
if (cursor_row < 0 || cursor_row >= lbuf_len(xb)) \
	cursor_row = lbuf_len(xb) ? lbuf_len(xb) - 1 : 0; \
if (cursor_row < view_top_row) \
	view_top_row = cursor_row; \
else if (cursor_row >= view_top_row + xrows) \
	view_top_row = cursor_row - xrows + 1; \

/**
 * @brief Vi main loop: read and execute normal-mode commands until quit_state.
 * @param init  draw the screen before the first command
 */
void vi(int init)
{
	char *ln, *cs;
	int mv, n, k, c;
	vi_ex_depth++;
	if (init) {
		topfix()
		vi_col = vi_off2col(xb, cursor_row, cursor_off);
		vi_drawagain(view_top_row);
		term_pos(cursor_row - view_top_row, led_pos(lbuf_get(xb, cursor_row), vi_col) + vi_lncol);
	}
	while (!quit_state) {
		int nrow = cursor_row;
		int noff = cursor_off;
		int orow = nrow;
		int ooff = noff;
		int otop = view_top_row;
		int oleft = opt_left_col;
		ticmd_pos = 0;
		vi_mod = 0;
		vi_ybuf = vi_yankbuf(TK_CTL('l'));
		vi_arg = vi_prefix();
		term_dec()
		if (vi_lnnum == 1) {
			vi_lnnum = 0;
			vi_lncol = 0;
			vi_mod |= 1;
		}
		if (opt_multiline_prompt == 1) {
			opt_multiline_prompt = 0;
			if (syn_scdirl > 0)
				syn_scdir(0);
			vi_drawrow(otop + xrows - 1);
		}
		led_extcut();
		if (vi_ybuf < 0)
			vi_ybuf = vi_yankbuf(0);
		mv = vi_region(-1, &nrow, &noff);
		if (mv > 0 && nrow >= 0) {
			if (strchr("|jk", mv)) {
				noff = vi_col2off(xb, nrow, vi_col);
			} else {
				noff = noff < 0 ? lbuf_indents(xb, nrow) : noff;
				vi_mod |= 4;
			}
			if ((orow != nrow || ooff != noff) &&
					strchr("%'`GHML/?{}[]", mv))
				lbuf_mark(xb, '`', orow, ooff);
			cursor_row = nrow;
			cursor_off = noff;
		} else if (mv == 0) {
			char *cmd;
			term_dec()
			re_motion:
			c = term_read(TK_CTL('l'));
			switch (c) {
			case TK_CTL('b'):
				vi_scrollbackward(MAX(1, vi_arg) * (xrows - 1));
				cursor_off = lbuf_indents(xb, cursor_row);
				vi_mod |= 4;
				break;
			case TK_CTL('f'):
				vi_scrollforward(MAX(1, vi_arg) * (xrows - 1));
				cursor_off = lbuf_indents(xb, cursor_row);
				vi_mod |= 4;
				break;
			case TK_CTL('e'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollforward(MAX(1, vi_scrolley));
				cursor_off = vi_col2off(xb, cursor_row, vi_col);
				break;
			case TK_CTL('y'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollbackward(MAX(1, vi_scrolley));
				cursor_off = vi_col2off(xb, cursor_row, vi_col);
				break;
			case TK_CTL('u'):
				if (cursor_row == 0)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				cursor_row = MAX(0, cursor_row - n);
				if (view_top_row > 0)
					view_top_row = MAX(0, view_top_row - n);
				cursor_off = lbuf_indents(xb, cursor_row);
				vi_mod |= 4;
				break;
			case TK_CTL('d'):
				if (cursor_row == lbuf_len(xb) - 1)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				cursor_row = MIN(MAX(0, lbuf_len(xb) - 1), cursor_row + n);
				if (view_top_row < lbuf_len(xb) - xrows)
					view_top_row = MIN(lbuf_len(xb) - xrows, view_top_row + n);
				cursor_off = lbuf_indents(xb, cursor_row);
				vi_mod |= 4;
				break;
			case TK_CTL('i'): {
				if (!(ln = lbuf_get(xb, cursor_row)))
					break;
				ln = uc_chr(ln, cursor_off);
				n = strlen(ln);
				char buf[n + 4];
				memcpy(buf, ":e ", 3);
				memcpy(buf+3, ln, n);
				term_push(buf, n + 3);
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg ? -vi_cndir : vi_cndir;
				vi_arg = cur_buf - bufs + vi_cndir;
			case TK_CTL('_'):	/* this is also ^7 on some systems */
				if (vi_arg > 0)
					goto switchbuf;
				ex_exec("left0:b:mpt0");
				term_chr('\n');
				vi_arg = vi_digit();
				if (vi_arg > -1 && vi_arg < buf_count) {
					switchbuf:
					bufs_switchwft(vi_arg < buf_count ? vi_arg : 0)
					vc_status(0);
				}
				vi_mod |= 1;
				break;
			case 'u':
				undo:
				if (vi_arg >= 0 && !lbuf_undo(xb, &cursor_row, &cursor_off)) {
					vi_mod |= 1;
					vi_arg--;
					goto undo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("undo failed")
				else if (cursor_row < view_top_row || cursor_row >= view_top_row + xrows)
					view_top_row = MAX(0, cursor_row - xrows / 2);
				break;
			case TK_CTL('r'):
				redo:
				if (vi_arg >= 0 && !lbuf_redo(xb, &cursor_row, &cursor_off)) {
					vi_mod |= 1;
					vi_arg--;
					goto redo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("redo failed")
				else if (cursor_row < view_top_row || cursor_row >= view_top_row + xrows)
					view_top_row = MAX(0, cursor_row - xrows / 2);
				break;
			case TK_CTL('g'):
				vi_tsm = 0;
				status:
				if (vi_arg) {
					vi_status = vi_arg > 1 ? 0 : term_resized;
					xrows += vi_status ? -1 : 1;
				}
				vc_status(vi_tsm);
				break;
			case TK_CTL('^'):
				bufs_switchwft(prev_buf - bufs)
				vc_status(0);
				vi_mod |= 1;
				break;
			case TK_CTL('k'):;
				static struct lbuf *writexb;
				if ((cs = ex_exec("w")) && writexb && xb == writexb)
					cs = ex_exec("mpt0:w!");
				writexb = cs ? xb : NULL;
				vi_mod |= 1;
				break;
			case '#':
				if (vi_lnnum & vi_arg)
					vi_lnnum = vi_lnnum & ~vi_arg;
				else
					vi_lnnum = vi_arg ? vi_lnnum | vi_arg : !vi_lnnum;
				vi_lncol = 0;
				vi_mod |= 1;
				break;
			case 'v':
				vi_mod |= 2;
				k = term_read(0);
				switch (k) {
				case '.':
					while (vi_arg) {
						term_push("j", 1);
						term_push(rep_cmd, rep_len);
						if (strchr("iIoOaAsScC", rep_cmd[0])) {
							term_push("0", 1);
							if (noff)
								vi_argcmd(noff, 'l');
						}
						vi_arg--;
					}
					break;
				case 'w':
					vi_nlmode = !vi_nlmode;
					break;
				case 'o':
					ex_command("%s/\x0d//g:%s/[ \t]+$//g")
					vi_mod |= 1;
					break;
				case 'I':;
				case 'i':;
					char restr[100] = "%s/^\t/";
					vi_arg = MIN(vi_arg ? vi_arg : opt_tabstop, 80);
					if (k == 'I') {
						cmd = restr+6;
						while (vi_arg--)
							*cmd++ = ' ';
						memcpy(cmd, "/^", sizeof("/^"));
					} else {
						memcpy(restr, "%s/^ {", sizeof("%s/^ {"));
						memcpy(itoa(vi_arg, restr+6), "}/\t/^", sizeof("}/\t/^"));
					}
					ln = vi_enprompt(":", restr, &k, &n);
					goto do_excmd;
				case 'b':
				case 'v':
					term_push(k == 'v' ? ":\x01" : ":\x02", 2); /* ^a : ^b */
					break;
				case ';':
					ln = vi_enprompt(":", "!", &k, &n);
					goto do_excmd;
				case '/': {
					cs = vi_curword(xb, cursor_row, cursor_off, vi_arg, 1);
					n = cs ? strlen(cs) : 0;
					char buf[n + 30];
					memcpy(buf, "re ", sizeof("re "));
					if (cs)
						memcpy(buf+3, cs, n + 1);
					free(cs);
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd; }
				case 't': {
					vi_drawmsg("arg2:(0|#)");
					cs = vi_curword(xb, cursor_row, cursor_off, vi_prefix(), 1);
					n = cs ? strlen(cs) : 0;
					char buf[n + 30];
					memcpy(buf, ".,.+", sizeof(".,.+"));
					char *buf1 = itoa(vi_arg, buf+4);
					memcpy(buf1, "s/", sizeof("s/"));
					if (cs) {
						memcpy(buf1+2, cs, n);
						buf1[n + 2] = '/';
						buf1[n + 3] = '\0';
						free(cs);
					}
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd; }
				case 'r': {
					cs = vi_curword(xb, cursor_row, cursor_off, vi_arg, 1);
					n = cs ? strlen(cs) : 0;
					char buf[n + 30];
					memcpy(buf, "%s/", sizeof("%s/"));
					if (cs) {
						memcpy(buf+3, cs, n);
						buf[n + 3] = '/';
						buf[n + 4] = '\0';
						free(cs);
					}
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd; }
				default:
					term_dec()
				}
				break;
			case 'V':
				vi_hidch = !vi_hidch;
				vi_mod |= 1;
				break;
			case TK_CTL('v'):
				vi_arg = (vi_wsel % 5) + !!*vi_word;
			case TK_CTL('c'):
				if (vi_arg && vi_arg <= 5) {
					vi_wsel = vi_arg;
					vi_word = _vi_word + vi_arg;
				} else
					vi_word = _vi_word + (!*vi_word * vi_wsel);
				vi_rshift = 0;
				vi_mod |= 1;
				break;
			case ':':
				ln = vi_enprompt(":", NULL, &k, &n);
				do_excmd:
				if (k && ln[n]) {
					ex_command(ln + n)
					if (cursor_row != orow && (cursor_row < view_top_row ||
							cursor_row >= view_top_row + xrows - !vi_status))
						view_top_row = MAX(0, cursor_row - xrows / 2);
				}
				vi_mod |= 1;
				if (!opt_multiline_prompt)
					vi_drawmsg(ln);
				free(ln);
				if (quit_state) {
					opt_multiline_prompt = opt_multiline_prompt ? opt_multiline_prompt : (vi_ex_depth > 1);
					continue;
				} else if (!opt_multiline_prompt)
					opt_multiline_prompt = 1;
				break;
			case 'c':
			case 'd':
				k = term_read(0);
				if (k == 'i') {
					k = term_read(0);
					char pairs[2];
					switch(k) {
					case ')': case '(': pairs[0]='('; pairs[1]=')'; break;
					case ']': case '[': pairs[0]='['; pairs[1]=']'; break;
					case '}': case '{': pairs[0]='{'; pairs[1]='}'; break;
					case '>': case '<': pairs[0]='<'; pairs[1]='>'; break;
					default: pairs[0] = k; pairs[1] = k; break;
					}
					int r1 = cursor_row, o1 = cursor_off, r2, o2;
					if (TK_INT(pairs[0]) || !(cs = lbuf_get(xb, r1)))
						break;
					int dir = (k == pairs[1] && pairs[0] != pairs[1]) ? -1 : 1;
					int pair_found = 0;
					int skip = MAX(1, vi_arg);
					ren_position(cs);
					while (*rstate->chrs[o1] != pairs[0] || --skip)
						if (lbuf_next(xb, dir, &r1, &o1))
							goto out;
					r2 = r1;
					o2 = o1;
					if (pairs[0] == pairs[1]) {
						while (!lbuf_next(xb, 1, &r2, &o2))
							if (*rstate->chrs[o2] == pairs[1]) {
								pair_found = 1;
								break;
							}
					} else
						pair_found = !lbuf_pair(xb, pairs, 2, &r2, &o2);
					if (pair_found && !lbuf_next(xb, 1, &r1, &o1)) {
						vi_delete(r1, o1, r2, o2, 0);
						if (c == 'c') {
							c = 'i';
							goto insert;
						}
						rep_record()
						vi_mod |= 1;
					}
					out:
					break;
				}
				term_dec()
			case 'y':
			case '!':
			case '>':
			case '<':
			case TK_CTL('w'):
				k = vc_motion(c);
				if (c == 'c')
					goto insert_done;
				break;
			case 'I':
			case 'i':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				insert:
				k = vc_insert(c);
				insert_done:
				if (k == 127 || k == TK_CTL('w')) {
					if (cursor_row && !(cursor_off > 0 && lbuf_eol(xb, cursor_row, 1))) {
						cursor_row--;
						if (view_top_row > otop)
							view_top_row = otop;
						topfix()
						vc_join(0, 2);
						vi_drawagain(view_top_row);
						if (vi_status)
							vc_status(vi_tsm);
					} else if (cursor_off) {
						if (k == TK_CTL('w')) {
							noff = cursor_off;
							lbuf_wordend(xb, 0, -2, &cursor_row, &noff);
							vi_delete(cursor_row, noff, cursor_row, cursor_off, 0);
						} else
							vi_delete(cursor_row, cursor_off - 1, cursor_row, cursor_off, 0);
					}
					c = cursor_off != lbuf_eol(xb, cursor_row, 1) ? 'i' : 'a';
					xb->useq += opt_undo_seq;
					goto insert;
				}
				cursor_off--;
				rep_record()
				vi_mod |= !opt_print_autocomplete && cursor_row == orow ? 8 : 1;
				break;
			case 'J':
				vc_join(1, vi_arg <= 1 ? 2 : vi_arg);
				rep_record()
				vi_mod |= 1;
				break;
			case 'K': {
				preserve(int, opt_startup_flags, opt_startup_flags = 1;)
				do {
					ex_exec(";+1c\n:-1");
				} while (vi_arg--);
				restore(opt_startup_flags)
				rep_record()
				vi_mod |= 1;
				break; }
			case TK_CTL('z'):
			case TK_CTL('l'):
				if (c == TK_CTL('z')) {
					term_pos(xrows, 0);
					term_suspend();
				} else {
					term_done();
					term_init();
				}
				vi_mod |= 1;
				break;
			case 'm':
				lbuf_mark(xb, term_read(0), cursor_row, cursor_off);
				break;
			case 'p':
			case 'P':
				vi_mod |= vc_put(c);
				break;
			case 'z':
				k = term_read(0);
				switch (k) {
				case '\n':
					view_top_row = cursor_row;
					break;
				case '.':
					view_top_row = MAX(0, cursor_row - xrows / 2);
					break;
				case '-':
					view_top_row = MAX(0, cursor_row - xrows + 1);
					break;
				case 'l':
				case 'r':
				case 'L':
				case 'R':
					opt_text_dir = uc_isupper(k)+1;
					opt_text_dir = tolower(k) == 'r' ? -opt_text_dir : opt_text_dir;
					RST_NULL(0, 1)
					break;
				case 'e':
				case 'f':
					cur_keymap = k == 'e' ? 0 : keymap_alt;
					break;
				case '1':
				case '2':
					keymap_alt = k - '0';
					break;
				}
				vi_mod |= 1;
				break;
			case 'g':
				k = term_read(0);
				if (k == 'g')
					term_push("1G", 2);
				else if (k == 'a') {
					vi_tsm = 1;
					goto status;
				} else if (k == 'w') {
					preserve(int, opt_search_group, opt_search_group = 2;)
					preserve(int, opt_startup_flags, opt_startup_flags = 1;)
					n = vi_arg ? vi_arg : 80;
					while (1) {
						cursor_off = vi_col2off(xb, cursor_row, n);
						vi_col = vi_off2col(xb, cursor_row, cursor_off+1);
						if (vi_col <= n)
							break;
						if (ex_exec("f>[^ \t]*[ \t]+(?\\:.$|(.)):??;c\n"))
							break;
					}
					restore(opt_search_group)
					restore(opt_startup_flags)
					vi_mod |= !texec;
				} else if (k == 'q') {
					preserve(int, opt_line_editor, opt_line_editor = 0;)
					char cmd[64] = "g/./& ";
					memcpy(itoa(vi_arg, cmd+5), "gw", sizeof("gw"));
					ex_command(cmd)
					restore(opt_line_editor)
					vi_mod |= 1;
				} else if (k == '~' || k == 'u' || k == 'U')
					vc_motion(k);
				break;
			case 'x':
				term_push("d ", 2);
				goto motion;
			case 'X':
				term_push("d", 2);
				goto motion;
			case 'D':
				term_push("d$", 2);
				goto motion;
			case 'Y':
				term_push("yy", 2);
				goto motion;
			case '~':
				term_push("g~ ", 3);
				goto motion;
			case 'C':
				term_push("c$", 2);
				goto motion;
			case 's':
				term_push("c ", 2);
				goto motion;
			case 'S':
				term_push("cc", 2);
				motion:
				ticmd_pos--;
				goto re_motion;
			case 'r':
				vi_mod |= vc_replace();
				break;
			case 'R':
				ex_exec("left0:reg");
				break;
			case 'Q':
				term_pos(cursor_row - view_top_row, 0);
				opt_left_col = vi_arg ? opt_left_col : 0;
				led_modeswap();
				vi_mod |= 1;
				if (quit_state)
					continue;
				break;
			case 'Z':
				k = term_read(0);
				if (TK_INT(k))
					continue;
				if (k == 'Z') {
					ex_exec("x");
					continue;
				}
				quit_state = vi_arg ? -vi_arg * 256 - 257 : 1;
				if (k == 'z')
					term_push("\n", 1);
				else if (vi_ex_depth == 1) {
					term_clean();
					vi_ex_depth = 0;
				}
				continue;
			case '.':
				for (k = 0; k < MAX(1, vi_arg); k++)
					term_push(rep_cmd, rep_len);
				break;
			case 'q':
				if (opt_record_reg > 0) {
					sbuf *rsb = ex_regget(opt_record_reg);
					if (rsb && rsb->s_n)
						sbufn_cut(rsb, rsb->s_n - 1)
					opt_record_reg = 0;
				} else if (vi_ybuf <= 0) {
					vi_drawmsg_mpt("no record register")
				} else {
					if (!vi_arg)
						ex_regput(vi_ybuf, "", 0);
					opt_record_reg = vi_ybuf;
				}
				break;
			case '@':
			case '&':
				vc_execute(c);
				break;
			case '\\':
				if (!vi_arg)
					ex_exec("b-2");
				else if (xb != tempbufs[1].lb)
					ex_exec("b-2:%d:fd:b-2");
				else
					ex_exec("%d:fd");
				vc_status(0);
				vi_mod |= 1;
				break;
			default:
				continue;
			}
		}
		topfix()
		ln = lbuf_get(xb, cursor_row);
		cursor_off = ren_noeol(ln, cursor_off);
		/* cursor on a zero-width char: move to the nearest visible one, in the direction of travel */
		if (ln && !rstate->wid[cursor_off]) {
			for (n = cursor_off, k = n; k < rstate->n && !rstate->wid[k];) {
				if (!k)
					n = ooff+1;
				k += n > ooff ? 1 : -1;
			}
			if (k < rstate->n)
				cursor_off = k;
		}
		if (vi_mod)
			vi_col = vi_off2col(xb, cursor_row, cursor_off);
		if (vi_col >= opt_left_col + xcols || vi_col < opt_left_col)
			opt_left_col = vi_col < xcols ? 0 : vi_col - xcols / 2;
		n = led_pos(ln, ren_cursor(ln, vi_col));
		if (opt_multiline_prompt > 1) {
			if (!print_newline)
				term_chr('\n');
			vi_drawmsg("[any key to continue] ");
			term_read(0);
			opt_multiline_prompt = 0;
			vi_mod |= 1;
		}
		print_newline = 0;
		if (opt_hl_word) {
			static char *word;
			if ((cs = vi_curword(xb, cursor_row, cursor_off, opt_hl_word, 0))) {
				if (!word || strcmp(word, cs)) {
					syn_reloadft(syn_addhl(cs, 1), 0);
					vi_mod |= 1;
				}
				free(word);
				word = cs;
			}
		}
		if (opt_hl_pair && (k = syn_findhl(3)) >= 0) {
			int row = cursor_row, off = cursor_off, row1, off1;
			static int ola[2][3];
			led_ext *p;
			if (!lbuf_pair(xb, "()[]{}", 6, &row, &off)) {
				row1 = row; off1 = off;
				if (!lbuf_pair(xb, "()[]{}", 6, &row, &off)) {
					ola[0][0] = off;
					ola[0][1] = 1;
					ola[0][2] = hls[k].att[0];
					p = led_extnew();
					p->ln = ln;
					p->usr = ola[0];
					p->blen = sizeof(ola[0]);
					ola[1][0] = off1;
					ola[1][1] = 1;
					ola[1][2] = hls[k].att[0];
					p = led_extnew();
					p->ln = lbuf_get(xb, row1);
					p->usr = ola[1];
					p->blen = sizeof(ola[1]);
					vi_mod |= row1 == row && orow == cursor_row ? 2 : 1;
				}
			}
		}
		term_record = 1;
		if (vi_mod & 1 || opt_left_col != oleft
				|| (vi_lnnum && orow != cursor_row && !(vi_lnnum == 2))
				|| (*vi_word && orow != cursor_row))
			vi_drawagain(view_top_row);
		else if (*vi_word && (ooff != cursor_off || vi_mod & 2)
				&& cursor_row+1 < view_top_row + xrows)
			vi_drawrow(cursor_row+1);
		else if (view_top_row != otop)
			vi_drawupdate(otop - view_top_row);
		if (opt_hl_line) {
			syn_blockhl = -1;
			if (cursor_row != orow && orow >= view_top_row && orow < view_top_row + xrows)
				if (!(vi_mod & 1))
					vi_drawrow(orow);
			syn_blockhl = -1;
			syn_reloadft(syn_addhl("^.+", 2), 0);
			vi_drawrow(cursor_row);
			syn_reloadft(syn_addhl(NULL, 2), 0);
		} else if (vi_mod & 2 && !(vi_mod & 1)) {
			syn_blockhl = -1;
			vi_drawrow(cursor_row);
		}
		if (vi_status && opt_multiline_prompt < 1) {
			xrows -= term_resized != vi_status;
			vi_status = term_resized;
			vc_status(vi_tsm);
			if (opt_multiline_prompt > 0)
				opt_multiline_prompt = 0;
		}
		term_pos(cursor_row - view_top_row, n + vi_lncol);
		term_commit();
		xb->useq += opt_undo_seq;
	}
	if (--vi_ex_depth == 0) {
		term_pos(xrows - !vi_status, 0);
		if (opt_multiline_prompt > 0 && !print_newline)
			term_chr('\n');
		else
			term_kill();
	}
}

/** @brief SIGWINCH handler: count resizes in term_winch. */
static void sighandler(int signo)
{
	term_winch++;
}

/** @brief Install the SIGWINCH handler. */
static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGWINCH, &sa, NULL);
}

/** @brief Parse -aemsv options, open files, and run vi() or ex(). */
int main(int argc, char *argv[])
{
	int i, j;
	setup_signals();
	dir_init();
	syn_init();
	temp_open(0, "/hist/", _ft);
	temp_open(1, "/fm/", fm_ft);
	temp_open(2, "/sc/", _ft);
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == '-' && !argv[i][2]) {
			i++;
			break;
		}
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 's')
				opt_startup_flags |= 1|2;
			else if (argv[i][j] == 'e')
				opt_startup_flags |= 2;
			else if (argv[i][j] == 'm')
				opt_startup_flags |= 4;
			else if (argv[i][j] == 'a')
				opt_startup_flags |= 8;
			else if (argv[i][j] == 'v')
				opt_startup_flags = 0;
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				fprintf(stderr, "Nextvi-7.6 Usage: %s [-aemsv] [file ...]\n", argv[0]);
				return EXIT_FAILURE;
			}
		}
	}
	tibuf = emalloc(tibuf_sz);
	if (!(opt_startup_flags & 1))
		term_init();
	if (opt_startup_flags & 8)
		term_scrh()
	ex_init(argv + i, argc - i);
	if (opt_startup_flags & 2)
		ex();
	else
		vi(1);
	term_done();
	if (opt_startup_flags & 8)
		term_scrl()
	return quit_state < -256 ? (abs(quit_state) - 257) & 255 : abs(quit_state) - 1;
}
