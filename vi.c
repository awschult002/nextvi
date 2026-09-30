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
#include <pthread.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/resource.h>
#include "vi.h"
#include "conf.c"
#include "ex.c"
#include "lbuf.c"
#include "led.c"
#include "regex.c"
#include "ren.c"
#include "term.c"
#include "uc.c"
#include "treesitter.c"
#include "lsp.c"

/* the frame the redraw thread paints while the input loop reads keys */
static struct vi_rend {
	pthread_mutex_t mtx;
	pthread_cond_t req;		/* a frame is queued */
	pthread_cond_t done;		/* the queued frame is painted */
	pthread_t tid;
	int on;				/* 1 threaded, -1 pthread_create() failed */
	int busy;			/* a frame is queued or being painted */
	int skip;			/* mod bits of the frames dropped so far */
	int mod;			/* vi_mod when the frame was queued */
	int otop, otopsub, oleft, orow, ooff;	/* the state the frame draws over */
	int pos;			/* the cursor column of the frame */
} vi_rend = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
		PTHREAD_COND_INITIALIZER};
static void vi_rendpost(int mod, int otop, int otopsub, int oleft,
			int orow, int ooff, int pos);

int vi_hidch;			/* show hidden chars */
int vi_lncol;			/* line numbers cursor offset */
static int vi_lnnum;		/* line numbers */
/* screen redraw - bit 1: whole screen, bit 2: current line, bit 3: update vi_col */
static int vi_mod;
static char vi_word_m[] = "\0leEwW";	/* line word navigation */
static char *vi_word = vi_word_m;
static char *_vi_word = vi_word_m;
static int vi_wsel = 1;
static int vi_rshift;			/* row shift for vi_word */
static int vi_arg;			/* numeric argument */
static char vi_charlast[5];		/* the last character searched via f, t, F, or T */
static int vi_charcmd;			/* the character finding command */
static int vi_ybuf;			/* current yank buffer, -1 if not given */
static int vi_col;			/* the column requested by | command */
static int vi_scrollud;			/* scroll amount for ^u and ^d */
static int vi_scrolley;			/* scroll amount for ^e and ^y */
static int vi_cndir = 1;		/* ^n direction */
static int vi_status;			/* permanent status bar */
static int vi_tsm;			/* type of the status message */
static int vi_nlmode;			/* new line mode for vi regions */
static int vi_visual;			/* visual mode: 0=off, 'v'=char, 'V'=line 'b'=block */
static int vi_vrow;			/* selection anchor row */
static int vi_voff;			/* selection anchor column */

static void vi_drawmsg(char *msg)
{
	syn_blockhl = -1;
	preserve(int, xtd, xtd = 2;)
	preserve(int, ftidx,)
	syn_setft(bar_ft);
	RST(2, led_crender(msg, xrows, 0, 0, xcols))
	restore(xtd)
	restore(ftidx)
}
#define vi_drawmsg_mpt(msg) { vi_drawmsg(msg); if (!xmpt) xmpt = 1; }

void lsp_show_msg(char *msg) { vi_drawmsg_mpt(msg) }

static int vi_nextcol(char *ln, int dir, int *off)
{
	int o = ren_off(ln, ren_next(ln, ren_pos(ln, *off), dir));
	if (*rstate->chrs[o] == '\n')
		return -1;
	*off = o;
	return 0;
}

/* the last rendered column of the given line, measured out of band;
 * nl keeps the trailing newline, which is drawn as a blank cell;
 * the buffer line slot is used: only 0 and 1 are cleared when a line is
 * freed or an option changes, and xlw rules out the xlim nul hole */
static int vi_lncmax(char *s, int nl)
{
	int cmax;
	ren_state *r;
	if (!s)
		return 0;
	preserve(ren_state*, rstate, rstate = rstates+1;)
	r = ren_position(s);
	cmax = MAX(0, r->cmax);
	if (!nl && r->cmax >= 0 && *r->chrs[r->col[r->cmax]] == '\n')
		cmax = MAX(0, cmax - 1);
	restore(rstate)
	return cmax;
}

/* the number of terminal rows the given line occupies */
int vi_lnrows(char *s)
{
	if (!xlw || !s)
		return 1;
	return vi_lncmax(s, 1) / ren_wrapw(vi_lncol) + 1;
}

/* the terminal row at which the given line starts;
 * lines off the screen saturate, only their side of it is meaningful */
int vi_srow(int row)
{
	int i, trow = -xtopsub;
	if (!xlw)
		return row - xtop;
	for (i = row; i < xtop && trow > -xrows; i++)
		trow -= vi_lnrows(lbuf_get(xb, i));
	for (i = xtop; i < row && trow < xrows; i++)
		trow += vi_lnrows(lbuf_get(xb, i));
	return trow;
}

/* the terminal rows between the given top position and the current one;
 * positive when the screen scrolled backward, that is content moved down */
static int vi_topdiff(int row, int sub)
{
	int i, n = sub - xtopsub, dir = row > xtop ? 1 : -1;
	if (!xlw)
		return row - xtop;
	for (i = MIN(row, xtop); i < MAX(row, xtop) && n < xrows && n > -xrows; i++)
		n += dir * vi_lnrows(lbuf_get(xb, i));
	return n;
}

/* the last line visible on the screen, even if only partially */
static int vi_botrow(void)
{
	int h, row = xtop, trow = -xtopsub, len = lbuf_len(xb);
	if (!xlw)
		return MIN(xtop + xrows, MAX(1, len)) - 1;
	while (row + 1 < len && trow + (h = vi_lnrows(lbuf_get(xb, row))) < xrows)
		trow += h, row++;
	return row;
}

/* the first line shown in full; the top line may be cut in half and,
 * when it is taller than the screen, no line below it is shown at all */
static int vi_fullrow(void)
{
	if (!xlw || !xtopsub)
		return xtop;
	return xtop + 1 < lbuf_len(xb) &&
		vi_lnrows(lbuf_get(xb, xtop)) - xtopsub < xrows ? xtop + 1 : xtop;
}

/* the last line shown in full; the bottom line may be cut in half */
static int vi_lastrow(void)
{
	int row = vi_botrow();
	if (xlw && row > xtop && vi_srow(row) + vi_lnrows(lbuf_get(xb, row)) > xrows)
		row--;
	return row;
}

/* advance the top of the screen by n terminal rows; return rows advanced */
static int vi_topadv(int n)
{
	int i = 0, h;
	if (!xlw) {
		h = MAX(0, MIN(lbuf_len(xb) - 1, xtop + n));
		i = h - xtop;
		xtop = h;
		return i < 0 ? -i : i;
	}
	for (; i < n; i++) {
		h = vi_lnrows(lbuf_get(xb, xtop));
		if (xtopsub + 1 < h)
			xtopsub++;
		else if (xtop + 1 < lbuf_len(xb))
			xtop++, xtopsub = 0;
		else
			break;
	}
	for (; i < -n; i++) {
		if (xtopsub > 0)
			xtopsub--;
		else if (xtop > 0)
			xtopsub = vi_lnrows(lbuf_get(xb, --xtop)) - 1;
		else
			break;
	}
	return i;
}

/* place the given line n terminal rows below the top of the screen */
static void vi_toprows(int row, int n)
{
	if (!xlw) {
		xtop = MAX(0, row - n);
		return;
	}
	xtop = MAX(0, MIN(row, lbuf_len(xb) - 1));
	xtopsub = 0;
	vi_topadv(-n);
}

#define vi_center(row)	vi_toprows(row, xrows / 2)
/* whether the given line is off the screen; adj rows are reserved at the bottom */
#define vi_unseen(row, adj) \
	((row) < xtop || (xlw ? vi_srow(row) >= xrows - (adj) \
			: (row) >= xtop + xrows - (adj)))

/* the terminal row of the cursor */
static int vi_crow(void)
{
	char *ln;
	if (!xlw)
		return xrow - xtop;
	ln = lbuf_get(xb, xrow);
	return vi_srow(xrow) + (ln ? ren_pos(ln, xoff) / ren_wrapw(vi_lncol) : 0);
}

/* pull the cursor into the visible segments of its own line, so that
 * screen commands never drag the screen after an off-screen segment;
 * trow is the preferred terminal row, or -1 to keep the current one */
static void vi_curseg(int trow)
{
	char *ln;
	int w, seg, srow, cmax, pos;
	if (!xlw || !(ln = lbuf_get(xb, xrow)))
		return;
	w = ren_wrapw(vi_lncol);
	srow = vi_srow(xrow);
	cmax = vi_lncmax(ln, 0);
	pos = ren_pos(ln, xoff);
	seg = trow < 0 ? pos / w : trow - srow;
	if (srow + seg < 0)
		seg = -srow;
	else if (srow + seg >= xrows)
		seg = xrows - 1 - srow;
	if (seg < 0 || seg > cmax / w)	/* the line has no visible segment */
		return;
	vi_col = MIN(seg * w + pos % w, cmax);
	xoff = ren_off(ln, vi_col);
}

/* move the cursor one wrapped segment at a time, keeping the sticky column */
static void vi_wrapstep(int *row, int cnt, int dir)
{
	int w = ren_wrapw(vi_lncol), cmax;
	for (; cnt > 0; cnt--) {
		cmax = vi_lncmax(lbuf_get(xb, *row), 0);
		if (dir > 0 && vi_col + w <= cmax) {
			vi_col += w;
		} else if (dir > 0) {
			if (*row + 1 >= lbuf_len(xb))
				break;
			(*row)++;
			vi_col %= w;
		} else if (vi_col >= w) {
			vi_col -= w;
		} else {
			if (*row <= 0)
				break;
			cmax = vi_lncmax(lbuf_get(xb, --(*row)), 0);
			vi_col = MIN(cmax / w * w + vi_col % w, cmax);
		}
	}
}

#define vi_drawnum(func) \
{ \
nrow = xrow; \
noff = xoff; \
for (i = 0, ret = 0;; i++) { \
	l1 = ren_next(c, ren_pos(c, noff), 1)-1-xleft+vi_lncol; \
	if (l1 > xcols || l1 < 0 || ret || l1 >= rstate->cmax + vi_lncol) \
		break; \
	i = i > 99 ? i % 100 : i; \
	itoa(i%10 ? i%10 : i, snum); \
	tmp[l1] = *snum; \
	ret = func; \
} } \

/* first and last screen column of the character at (row, off) */
static void vi_offspan(int row, int off, int *c1, int *c2)
{
	char *ln = lbuf_get(xb, row);
	ren_state *rs = rstate;
	*c1 = *c2 = 0;
	if (!ln)
		return;
	rstate = rstates+2;
	rstate->s = NULL;
	ren_state *r = ren_position(ln);
	off = MAX(0, MIN(off, r->n - 1));
	*c1 = r->pos[off];
	*c2 = *c1 + r->wid[off] - 1;
	rstate->s = NULL;	/* scratch slot, lbuf_rfree skips it */
	rstate = rs;
}

/* screen columns the block selection spans */
static void vi_blockcols(int *c1, int *c2)
{
	int a1, a2, b1, b2;
	vi_offspan(vi_vrow, vi_voff, &a1, &a2);
	vi_offspan(xrow, xoff, &b1, &b2);
	*c1 = MIN(a1, b1);
	*c2 = MAX(a2, b2);
}

/* ansi colours are rgb bit masks (RE 1, GR 2, BL 4; 8 only makes them bright),
   so the first primary a cell lacks is the one that cannot blend into it */
static int vi_curhue(int att)
{
	static const int hue[] = {GR, RE, BL};	/* in order of preference */
	int i, bg = SYN_BG(att);
	int under = bg ? bg : SYN_FG(att);	/* fill, else text colour */
	for (i = 0; i < LEN(hue); i++)
		if (!(under & hue[i]))
			return hue[i];
	return BL;			/* white: every primary is taken */
}

/* the cursor cell, which ext_sel leaves out of the reversed run: fill it with
   the one primary the highlight under the cursor does not already carry */
static void ext_cursor(led_ext *p, led_ctx *x)
{
	if (!led_extkey(p, x))
		return;
	int *ola = p->usr;
	int i = led_attidx(x, ola[0]);
	if (i < 0)
		return;
	int hue = vi_curhue(x->att[i]) + 8;	/* the bright variant */
	/* a background and only a background: with any flag left on, SYN_RV
	   would paint the hue as the text colour instead */
	x->att[i] = SYN_FGSET(x->att[i]) | SYN_BGMK(hue) | ola[2];
}

/* the selected span of the row: a plain merge under its own name, so that
   led_extfind() tells it from entries left on the default body */
static void ext_sel(led_ext *p, led_ctx *x)
{
	ext_attmerge(p, x);
}

/* stage the row's overlays: one entry each, re-pointed per row, so a redraw
   never grows the registry. both are transient, dropped by led_extcut() */
static void vi_visual_attrib(char *s, int row)
{
	static int sel[6], cur[3];
	led_ext *p;
	int cnt = 1;
	if (!vi_visual || !s)
		return;
	int ar = vi_vrow, ao = vi_voff;
	int cr = xrow,   co = xoff;
	if (ar > cr || (ar == cr && ao > co)) {
		swap(&ar, &cr);
		swap(&ao, &co);
	}
	if (row < ar || row > cr)
		return;
	int cb = 0, ce = 0;
	if (vi_visual == 'b')	/* before s is rendered: xlim nulls part of it */
		vi_blockcols(&cb, &ce);
	ren_state *r = ren_position(s);
	int rn1 = r->n - 1;
	int o_beg, o_end;
	if (vi_visual == 'V') {
		o_beg = 0;
		o_end = rn1;
	} else if (vi_visual == 'b') {
		if (cb >= r->cmax)	/* line ends left of the block */
			return;
		o_beg = r->col[cb];
		o_end = ce < r->cmax ? r->col[ce] : rn1 - 1;
	} else if (ar == cr) {
		o_beg = ao; o_end = co;
	} else if (row == ar) {
		o_beg = ao; o_end = lbuf_eol(xb, row, 1);
	} else if (row == cr) {
		o_beg = 0;
		o_end = co;
	} else {
		o_beg = 0;
		o_end = lbuf_eol(xb, row, 1);
	}
	sel[0] = o_beg;
	sel[1] = o_end - o_beg + 1;
	sel[2] = SYN_RV;
	if (row == xrow && xoff >= o_beg && xoff <= o_end) {
		/* the cursor cell is a hole in the run: it takes a background
		   instead, which SYN_RV would undo */
		sel[1] = xoff - o_beg;
		sel[3] = xoff + 1;
		sel[4] = o_end - xoff;
		sel[5] = SYN_RV;
		cnt = 2;
	}
	if (!(p = led_extfind(ext_sel)))
		(p = led_extnew())->ext_func = ext_sel;
	p->ln = s;
	p->usr = sel;
	p->blen = cnt * 3 * sizeof(int);
	if (row != xrow)
		return;
	cur[0] = xoff;
	cur[1] = 1;
	cur[2] = 0;		/* nothing rides on top of the hue */
	if (!(p = led_extfind(ext_cursor)))	/* pushed last, runs last */
		(p = led_extnew())->ext_func = ext_cursor;
	p->ln = s;
	p->usr = cur;
	p->blen = sizeof(cur);
}

/* render an lsp diagnostic as virtual text starting at screen column col */
static void vi_drawdiag(const char *diag, int sev, int r, int col)
{
	static const char *sevname[] = {"info", "error", "warning", "info", "hint"};
	if (col < 0 || col >= xcols)
		return;
	sbuf_smake(sb, 256)
	sbuf_str(sb, "  ")
	sbuf_str(sb, sevname[sev > 0 && sev < 5 ? sev : 1])
	sbuf_str(sb, ": ")
	sbuf_str(sb, diag)
	sbuf_chr(sb, '\n')
	sbuf_nul(sb)
	preserve(int, syn_blockhl, syn_blockhl = -1;)
	preserve(int, ftidx,)
	syn_setft(lsp_ft);
	RST(2, led_prender(sb->s, r, col, 0, xcols - col))
	restore(syn_blockhl)
	restore(ftidx)
	free(sb->s);
}

static int vi_lnwid;		/* the widest visible line number, per frame */

static void vi_lnwidset(void)
{
	vi_lnwid = xlw ? vi_botrow() + 1 : xtop + xrows;
}

/* the rows drawing is clipped to; a scroll only redraws the rows it
 * exposed, so that the block highlight sees every row exactly once */
static int vi_rowbeg, vi_rowend;

/* render a line at terminal row trow; return the rows it occupies */
static int vi_rendrow(char *s, int trow, int lncol, int source_row)
{
	int h, w, k, i, beg, lim;
	if (!xlw) {
		led_srender(s, trow, lncol, xleft, xleft + xcols - lncol,
			ts_document(xb), source_row, 0)
		return 1;
	}
	w = ren_wrapw(lncol);
	h = vi_lncmax(s, 1) / w + 1;
	beg = MAX(vi_rowbeg - trow, 0);
	lim = MIN(h, (vi_rowend ? vi_rowend : xrows) - trow);
	for (i = beg; i < lim; i++) {
		/* the block highlight scans the rows in the scroll direction */
		k = syn_scdirl < 0 ? beg + lim - 1 - i : i;
		if (k && lncol) {		/* blank gutter of continuation rows */
			term_pos(trow + k, 0);
			term_kill();
		}
		led_srender(s, trow + k, lncol, k * w, k * w + w,
			ts_document(xb), source_row, 0)
	}
	return h;
}

static int vi_drawrow(int row, int trow)
{
	const char *diag = NULL;
	int dsev = 1;
	int l1, i, i1, lnnum = vi_lnnum;
	int ola[6];
	led_ext *lwx = NULL;
	char *c, *s;
	static char ch[5] = "~";
	if (xmpt == 1 && !vi_status && trow == xrows - 1)
		return 1;
	if (*vi_word && xled && !xlw) {
		int noff, nrow, ret;
		c = lbuf_get(xb, xrow);
		if (row != xrow+1 || !c || *c == '\n') {
			vi_rshift = (row > xrow+1 && c && *c != '\n');
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
		l1 = ren_next(c, ren_pos(c, xoff), 1)-1-xleft+vi_lncol;
		if (l1 >= 0 && l1 <= xcols)
			tmp[l1] = *vi_word;
		preserve(int, xorder, xorder = 0;)
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, xtd, xtd = dir_context(c) * 2;)
		preserve(int, ftidx,)
		syn_setft(n_ft);
		RST(2, led_crender(tmp, trow, 0, 0, xcols))
		restore(xorder)
		restore(syn_blockhl)
		restore(xtd)
		restore(ftidx)
		return 1;
	}
	s = lbuf_get(xb, row);
	if (xhllw && s && vi_lnrows(s) > 1) {
		ola[0] = 0;			/* block start */
		ola[1] = 1;
		ola[2] = SYN_BGMK(8);
		/* vi_lnrows left s in the buffer line slot: no second walk */
		ola[3] = rstates[1].n - 1;	/* block end */
		ola[4] = 1;
		ola[5] = SYN_BGMK(9);
		lwx = led_extnew();
		lwx->ln = s;
		lwx->usr = ola;
		lwx->blen = sizeof(ola);
	}
	if (s && xb_path && xb_path[0])
		diag = lsp_diag_for_line(xb_path, row, &dsev);
	skip:
	rstate = rstates+1;
	if (!s)
		s = row ? ch : ch+1;
	else if (lnnum && xled) {
		char tmp[32], tmp1[32], *p;
		c = tmp, i = 0, i1 = 0;
		if (lnnum == 1 || lnnum & 2) {
			c = itoa(row+1-vi_rshift, tmp);
			*c++ = ' ';
			i = itoalen(vi_lnwid);
		}
		p = c;
		if (lnnum == 1 || lnnum & 4 || lnnum & 8) {
			c = itoa(abs(xrow-row+vi_rshift), c);
			*c++ = ' ';
			i1 = itoalen(xrows);
		}
		*c = '\0';
		l1 = (c - tmp) + (i+i1 - (strlen(tmp) - !!i - !!i1));
		vi_lncol = dir_context(s) < 0 ? 0 : l1;
		memset(c, ' ', l1 - (c - tmp));
		c[l1 - (c - tmp)] = '\0';
		vi_visual_attrib(s, row);
		i = vi_rendrow(s, trow, l1, row - vi_rshift);
		int dcol = l1 + rstate->cmax - xleft;
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, ftidx,)
		syn_setft(nn_ft);
		if ((lnnum == 1 || lnnum & 4) && !xleft && vi_lncol) {
			for (i1 = 0; i1 < rstate->cmax &&
					memchr(" \t", *rstate->chrs[ren_off(s, i1)], 2);)
				i1 = ren_next(s, i1, 1);
			i1 -= (itoa(abs(xrow-row+vi_rshift), tmp1) - tmp1)+1;
			if (i1 >= 0 && trow >= vi_rowbeg) {
				memset(p, ' ', strlen(p));
				RST(2, led_prender(tmp1, trow, l1+i1, 0, l1))
			}
		}
		if (trow >= vi_rowbeg)
			RST(2, led_prender(tmp, trow, 0, 0, l1))
		else
			rstate = rstates;
		restore(syn_blockhl)
		restore(ftidx)
		if (diag && trow >= 0)
			vi_drawdiag(diag, dsev, trow, dcol);
		goto done;
	}
	vi_visual_attrib(s, row);
	i = vi_rendrow(s, trow, 0, row - vi_rshift);
	int dcol = rstate->cmax - xleft;
	rstate = rstates;
	if (diag && trow >= 0)
		vi_drawdiag(diag, dsev, trow, dcol);
	done:
	if (lwx)			/* the markers only apply to this line */
		led_extdel(lwx);
	return i;
}

/* draw a buffer line during insertion; return the rows it occupies */
int vi_drawline(int row, int trow)
{
	return vi_drawrow(row, trow);
}

/* redraw the screen */
static void vi_drawagain(int i)
{
	int trow;
	syn_scdir(0);
	vi_lnwidset();
	if (!xlw) {
		for (; i < xtop + xrows; i++)
			vi_drawrow(i, i - xtop);
		return;
	}
	vi_rshift = 0;			/* the word overlay is not drawn when wrapping */
	for (trow = vi_srow(i); trow < xrows; i++)
		trow += vi_drawrow(i, trow);
}

/* redraw the rows that a scroll of n terminal rows left blank */
static void vi_drawscroll(int n)
{
	int h, row = xtop, trow;
	if (n <= -xrows || n >= xrows) {	/* no row of the frame survives */
		vi_drawagain(xtop);
		return;
	}
	vi_lnwidset();
	term_pos(0, 0);
	term_room(n);
	syn_scdir(n);
	vi_rshift = 0;			/* the word overlay is not drawn when wrapping */
	if (n < 0) {			/* the blank rows are at the bottom */
		vi_rowbeg = xrows + n;
		for (trow = -xtopsub;
				trow + (h = vi_lnrows(lbuf_get(xb, row))) <= vi_rowbeg;
				row++)
			trow += h;
		for (; trow < xrows; row++)
			trow += vi_drawrow(row, trow);
		vi_rowbeg = 0;
		return;
	}
	vi_rowend = n;			/* and at the top, drawn the way it scans */
	for (trow = -xtopsub; trow < n; row++)
		trow += vi_lnrows(lbuf_get(xb, row));
	while (row-- > xtop) {
		trow -= vi_lnrows(lbuf_get(xb, row));
		vi_drawrow(row, trow);
	}
	vi_rowend = 0;
}

/* update the screen */
static void vi_drawupdate(int i)
{
	int n;
	if (xlw) {
		vi_drawscroll(i);
		return;
	}
	vi_lnwidset();
	term_pos(0, 0);
	term_room(i);
	syn_scdir(i);
	if (i < 0) {
		n = MIN(-i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(xtop + xrows - n + i, xrows - n + i);
	} else {
		n = MIN(i, xrows);
		for (i = n-1; i >= 0; i--)
			vi_drawrow(xtop + i, i);
	}
}

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

static char *vi_enprompt(char *msg, char *insert, int *ret, int *mlen)
{
	int kmap = 0;
	return vi_prompt(msg, ex_ft, insert, ret, &kmap, mlen);
}

static int vi_yankbuf(int winch)
{
	int c = term_read(winch);
	if (c == '"')
		return term_read(0);
	term_dec()
	return -1;
}

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

static int vi_digit(void)
{
	int c = term_read(0);
	if (c >= '0' && c <= '9')
		return c - '0';
	return -1;
}

static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

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

/* mark the keyword matches of the visible rows, the one at the
   cursor is given conf_hlmatc instead of conf_hlmat attributes;
   the triples are staged in sb, one led_ext extension per row */
static void vi_isearchhl(sbuf *sb)
{
	int offs[xkwdrs->rs ? xkwdrs->rs->nsubc : 2];
	int cnt[xrows], ola[3];
	int row, off, beg, end, flg, i, n;
	char *s;
	for (row = xtop; row < xtop + xrows && row < lbuf_len(xb); row++) {
		s = lbuf_get(xb, row);
		off = 0;
		flg = REG_NEWLINE;
		cnt[row - xtop] = 0;
		while (s[off] && rstr_find(xkwdrs, s + off, offs, flg) >= 0) {
			flg |= REG_NOTBOL;
			beg = offs[xgrp], end = offs[xgrp + 1];
			if (beg < 0) {
				off += offs[1] > 0 ? offs[1] : 1;
				continue;
			}
			ola[0] = uc_off(s, off + beg);
			ola[1] = uc_off(s + off + beg, end - beg);
			ola[2] = row == xrow && ola[0] == xoff ?
					conf_hlmatc : conf_hlmat;
			sbuf_mem(sb, ola, sizeof(ola))
			cnt[row - xtop]++;
			off += end > 0 ? end : 1;
		}
	}
	/* the ola pointers are stable only now that sb has stopped growing */
	for (i = 0, n = 0; xtop + i < row; i++) {
		if (!cnt[i])
			continue;
		led_ext *p = led_extnew();
		p->ln = lbuf_get(xb, xtop + i);
		p->usr = (int*)sb->s + n;
		p->blen = cnt[i] * 3 * sizeof(int);
		n += cnt[i] * 3;
	}
}

/* read the search keyword, previewing matches as it is typed;
   *ret is 0 if the prompt was aborted, 1 if the keyword was
   accepted and 2 if frow/foff also hold the previewed match */
static char *vi_isearch(int cmd, int *ret, int *mlen, int *frow, int *foff)
{
	int key, row, off, len, sdir, found = 0;
	int drawn = 0, dir = cmd == '/' ? +2 : -2;
	int orow = xrow, ooff = xoff, otop = xtop, oleft = xleft;
	int srow = xrow, soff = xoff, odir = xkwddir;
	char *okwd = ex_regget('/') ? strdup(ex_regget('/')->s) : NULL;
	ins_state is;
	ins_init(is)
	sbuf_smake(sb, xcols)
	sbuf_smake(hsb, sizeof(int) * 24)
	sbuf_chr(sb, cmd)
	*mlen = sb->s_n;
	while (1) {
		term_pos(xrows, 0);
		syn_setft(vs_ft);
		len = sb->s_n;
		key = led_prompt(sb, NULL, &xkmap, &is, *mlen, 2);
		syn_setft(xb_ft);
		sbuf_nul(sb)
		/* an erase key that removed nothing ends an empty prompt */
		if (key == '\n' || TK_INT(key) || xquit
				|| (key == 127 && sb->s_n == len))
			break;
		if (!xled || (sb->s_n == *mlen && !drawn))
			continue;
		/* the step keys search on from the previewed match */
		if (key == TK_CTL('i') || key == TK_CTL('_'))
			sdir = key == TK_CTL('i') ? dir / 2 : -dir / 2;
		else
			srow = orow, soff = ooff, sdir = dir / 2;
		xrow = srow, xoff = soff, xtop = otop;
		led_extcut();
		sbuf_cut(hsb, 0)
		if (sb->s_n > *mlen) {
			row = srow, off = soff;
			ex_krsset(sb->s + *mlen, dir);
			if (xkwdrs && xgrp < (xkwdrs->rs ? xkwdrs->rs->nsubc : 2)
					&& lbuf_len(xb)) {
				found = !lbuf_search(xb, xkwdrs, sdir, 0, lbuf_len(xb),
						sdir, 1, &row, &off);
				if (found)
					srow = xrow = row, soff = xoff = off;
				if (xrow < xtop || xrow >= xtop + xrows)
					xtop = MAX(0, xrow - xrows / 2);
				vi_isearchhl(hsb);
			}
		}
		term_record = 1;
		vi_drawagain(xtop);
		term_commit();
		drawn = 1;
	}
	*ret = key == '\n';
	if (*ret && sb->s_n > *mlen) {
		if (found)	/* the step keys may have moved past the first match */
			*frow = srow, *foff = soff, *ret = 2;
		lbuf_dedup(tempbufs[0].lb, sb->s + *mlen, sb->s_n - *mlen)
		temp_pos(0, -1, 0, 0);
		temp_write(0, sb->s + *mlen);
	} else if (okwd)	/* the preview must not alter the last keyword */
		ex_krsset(okwd, *ret ? dir : odir * 2);
	else if (xkwdrs) {
		rstr_free(xkwdrs);
		xkwdrs = NULL;
	}
	free(okwd);
	led_extcut();
	free(hsb->s);
	xrow = orow, xoff = ooff, xtop = otop, xleft = oleft;
	vi_mod |= drawn;
	return sb->s;
}

static int vi_search(int cmd, int cnt, int *row, int *off, int msg)
{
	int i, dir, ret = 0;
	char vi_msg[512];
	if (cmd == '/' || cmd == '?') {
		char *kw = vi_isearch(cmd, &ret, &i, row, off);
		vi_drawmsg_mpt(kw)
		if (!ret) {
			free(kw);
			return 1;
		}
		ex_krsset(kw + i, cmd == '/' ? +2 : -2);
		free(kw);
	} else if (msg)
		ex_krsset(ex_regget('/') ? ex_regget('/')->s : NULL, xkwddir);
	if (!lbuf_len(xb) || !xkwddir)
		return 1;
	else if (!xkwdrs || xgrp >= (xkwdrs->rs ? xkwdrs->rs->nsubc : 2)) {
		vi_drawmsg_mpt(xkwdrs ? "invalid grp" : "syntax error")
		return 1;
	}
	dir = cmd == 'N' ? -xkwddir : xkwddir;
	for (i = ret > 1; i < cnt; i++) {
		if (lbuf_search(xb, xkwdrs, dir, 0, lbuf_len(xb),
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

static char *vi_curword(struct lbuf *lb, int row, int off, int n, int ex)
{
	char *ln = lbuf_get(lb, row);
	if (!ln || !n)
		return NULL;
	off = ren_noeol(ln, off);
	char **chrs = rstate->chrs;
	int cap = rstate->n;
	int end = off;
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

rstr *fsincl;
static int fspos;
static int fsdir;

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
				if (!fsincl || rstr_match(fsincl, cpath, 0)) {
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

#define fssearch() \
len = lbuf_s(path)->len; \
path[len] = '\0'; \
ret = ex_edit(path, len); \
path[len] = '\n'; \
if (ret && xrow) { \
	*row = xrow; *off = xoff; /* short circuit */ \
	if (!vi_search('n', cnt, row, off, 0)) \
		return 1; \
	++*off; \
} else { \
	*row = 0; *off = 0; \
} \
if (!vi_search(*row ? 'N' : 'n', cnt, row, off, 0)) \
	return 1; \

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

static char rep_cmd[sizeof(ticmd)];	/* the last command */
static int rep_len;
#define rep_record() memcpy(rep_cmd, ticmd, ticmd_pos); rep_len = ticmd_pos;
static __thread int redraw_thread;	/* paint thread: never measure on slot 0 */

static void vc_status(int type)
{
	int l, col;
	unsigned int cp;
	char cbuf[8] = "", vi_msg[512], *c;
	if (redraw_thread)
		rstate = rstates+1;
	col = vi_off2col(xb, xrow, xoff);
	col = ren_cursor(lbuf_get(xb, xrow), col) + 1;
	char *vs = vi_visual == 'V' ? "-- VISUAL LINE -- " :
		   vi_visual == 'b' ? "-- VISUAL BLOCK -- " :
		   vi_visual ? "-- VISUAL -- " : "";
	if (type && lbuf_get(xb, xrow)) {
		c = rstate->chrs[xoff];
		uc_code(cp, c, l)
		memcpy(cbuf, c, l);
		snprintf(vi_msg, sizeof(vi_msg), "<%s> 0x%x 0%o %u %dL %dW S%td O%d C%d %s",
			cbuf, cp, cp, cp, l, rstate->wid[xoff], c - lbuf_get(xb, xrow),
			xoff, col, vs);
	} else {
		snprintf(vi_msg, sizeof(vi_msg),
			"\"%s\"%s%dL %d%% L%d C%d B%td %s",
			xb_path[0] ? xb_path : "unnamed",
			xb->modified ? "* " : " ", lbuf_len(xb),
			xrow * 100 / MAX(1, lbuf_len(xb)-1), xrow+1, col,
			istempbuf(ex_buf) ? tempbufs - ex_buf - 1 : ex_buf - bufs, vs);
	}
	if (redraw_thread)
		rstate = rstates;
	vi_drawmsg_mpt(vi_msg)
}

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
		if (!(cs = led_read(&xkmap, term_read(0))))
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
		struct buf* tmpex_buf = istempbuf(ex_buf) ? ex_pbuf : ex_buf;
		if (mv == TK_CTL(']')) {
			if (vi_arg || lkwdcnt != xkwdcnt)
				term_exec("", 1, '&')
			lkwdcnt = xkwdcnt;
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
		if (tmpex_buf != ex_buf)
			ex_pbuf = tmpex_buf;
		bsync_ret:
		for (i = xbufcur-1; i >= 0 && bufs[i].mtime == -1; i--)
			ex_bufpostfix(&bufs[i], 1);
		syn_setft(xb_ft);
		vc_status(0);
		vi_center(*row);
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
			vi_center(*row);
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
		else if (cmd < 0 && vi_unseen(*row, !vi_status))
			vi_center(*row);
		break;
	case '\n':
	case '+':
	case 'j':
		if (xlw && mv == 'j')
			vi_wrapstep(row, cnt, 1);
		else
			*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		goto lnregion;
	case 'k':
	case '-':
		if (xlw && mv == 'k')
			vi_wrapstep(row, cnt, -1);
		else
			*row = MAX(*row - cnt, 0);
		goto lnregion;
	case 'G':
		*row = vi_arg ? cnt - 1 : lbuf_len(xb) - 1;
		goto lnregion;
	case 'H':
		*row = xlw ? MIN(vi_fullrow() + cnt - 1, vi_botrow())
			: MIN(xtop + cnt - 1, lbuf_len(xb) - 1);
		goto lnregion;
	case 'L':
		*row = xlw ? MAX(vi_fullrow(), vi_lastrow() - cnt + 1)
			: MIN(xtop + xrows - 1 - cnt + 1, lbuf_len(xb) - 1);
		goto lnregion;
	case 'M':
		if (!xlw) {
			*row = MIN(xtop + xrows / 2, lbuf_len(xb) - 1);
			goto lnregion;
		}
		preserve(int, xtop,)
		preserve(int, xtopsub,)
		vi_topadv(xrows / 2);		/* the halfway line, wrapping aside */
		*row = xtop;
		restore(xtop)
		restore(xtopsub)
		goto lnregion;
	case '\'':
	case '`':
		if (lbuf_jump(xb, term_read(0), row, &var))
			return -1;
		if (cmd < 0 && vi_unseen(*row, 0))
			vi_center(*row);
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

static void vi_yank(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf < 0 ? xdefreg : vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	xrow = r1;
	xoff = lnmode ? xoff : o1;
}

static void vi_delete(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf < 0 ? xdefreg : vi_ybuf, rsb.s, lnmode);
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
	xrow = r1;
	xoff = lnmode ? lbuf_indents(xb, xrow) : o1;
}

static int vi_indents(char *ln)
{
	if (xai <= 0 || !ln)
		ln = "";
	char *pln = ln;
	for (; *ln == ' ' || *ln == '\t'; ln++);
	return ln - pln;
}

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
	vi_regput(vi_ybuf < 0 ? xdefreg : vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	if (!xlw) {
		term_pos(r1 - xtop < 0 ? 0 : r1 - xtop, 0);
		term_room(r1 < xtop ? xtop - xrow : r1 - r2 -
				(*vi_word && ln && *ln != '\n' && r1 != r2));
	}
	xrow = r1;
	if (r1 < xtop) {
		xtop = r1;
		xtopsub = 0;
	}
	led_row = xlw ? MAX(0, vi_srow(r1)) : -1;
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, r1 - (r1 - r2), 0, &postn, r1, r2 + 1);
	if (postn + l2 != tlen || memcmp(ln + l1, sb->s + l1, tlen - l2 - l1))
		lbuf_edit(xb, sb->s, r1, r2 + 1, o1, xoff);
	free(sb->s);
	return key;
}

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
	xrow = r2;
	xoff = lnmode ? lbuf_indents(xb, r2) : o2;
}

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
	if (!xmpt)
		vi_drawmsg_mpt(cmd)
	free(cmd);
}

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
				if (xet && *ln == ' ') {
					int k;
					for (k = 0; k < xsw && *ln == ' '; k++)
						ln++;
				} else
					ln++;
			} else if (*ln != '\n' || r1 == r2) {
				if (xet) {
					for (int k = 0; k < xsw; k++)
						sbuf_chr(sb, ' ')
				} else
					sbuf_chr(sb, '\t')
			}
		}
		sbufn_str(sb, ln)
		lbuf_edit(xb, sb->s, i, i + 1, 0, 0);
		sbuf_cut(sb, 0)
	}
	xoff = lbuf_indents(xb, r1);
	free(sb->s);
}

static int vc_insert(int cmd);

/* ci is a screen column; each line maps it to its own offset */
static int vc_block_insert(int vcmd, int r1, int r2, int ci)
{
	char *old_ln = lbuf_get(xb, r1);
	if (!old_ln)
		return 0;
	xrow = r1;
	ren_state *r = ren_position(old_ln);
	int eol = r->n - 1;			/* offset of '\n' */
	int old_nbytes = lbuf_s(old_ln)->len;
	int off = ci < r->cmax ? r->col[ci] : eol;
	if (vcmd == 'A' && off < eol)
		off++;
	int app = off >= eol;			/* inserting at the line end */
	xoff = app ? MAX(0, eol - 1) : off;
	int ins_byte = r->chrs[app ? eol : off] - old_ln;
	int key = vc_insert(app ? 'a' : 'i');
	char *new_ln = lbuf_get(xb, r1);
	int new_nbytes = new_ln ? lbuf_s(new_ln)->len : 0;
	int added = new_nbytes - old_nbytes;
	if (added <= 0)
		return key;
	char *ins_text = new_ln + ins_byte;
	int added_chars = 0;
	for (char *t = ins_text; t < ins_text + added; t += uc_len(t))
		added_chars++;
	for (int rw = r1 + 1; rw <= r2; rw++) {
		char *ln = lbuf_get(xb, rw);
		if (!ln)
			continue;
		ren_state *rs = ren_position(ln);
		int n = rs->n - 1;	/* '\n' index = content chars */
		int pos = ci < rs->cmax ? rs->col[ci] : n;
		if (vcmd == 'I') {
			if (ci > rs->cmax)	/* line too short: skip */
				continue;
		} else				/* 'A': after right edge */
			pos = MIN(pos + 1, n);
		char *p = uc_chr(ln, pos);
		char *nl_p = ln + lbuf_s(ln)->len;
		int pre_bytes = p - ln;
		int post_bytes = nl_p - p + 1;		/* p through '\n' */
		char *new_ln2 = emalloc(pre_bytes + added + post_bytes + 1);
		memcpy(new_ln2, ln, pre_bytes);
		memcpy(new_ln2 + pre_bytes, ins_text, added);
		memcpy(new_ln2 + pre_bytes + added, p, post_bytes);
		new_ln2[pre_bytes + added + post_bytes] = '\0';
		lbuf_edit(xb, new_ln2, rw, rw + 1, pos, pos + added_chars);
		free(new_ln2);
	}
	vi_mod |= r1 != r2 ? 1 : 2;
	return key;
}

/* c_left and c_right are screen columns; map them per line */
#define VCB_BOUNDS \
	char *ln = lbuf_get(xb, r); \
	if (!ln) continue; \
	ren_state *rs = ren_position(ln); \
	n = rs->n - 1; \
	left = c_left < rs->cmax ? rs->col[c_left] : n; \
	right = c_right < rs->cmax ? rs->col[c_right] : n - 1; \
	bp = uc_chr(ln, left); \
	ep = right >= left ? uc_chr(bp, right - left + 1) : bp; \
	nlp = ln + lbuf_s(ln)->len; \
	if (ep > nlp) ep = nlp

static int vc_block_op(int cmd, int r1, int r2, int c_left, int c_right)
{
	int r, n, left, right;
	char *bp, *ep, *nlp;
	if (cmd == 'y' || cmd == 'd' || cmd == 'c') {
		sbuf_smake(yb, 256)
		for (r = r1; r <= r2; r++) {
			VCB_BOUNDS;
			if (ep > bp)
				sbuf_mem(yb, bp, (int)(ep - bp))
			if ((cmd == 'd' || cmd == 'c') && left <= right && n) {
				int beg_bytes = (int)(bp - ln);
				int rest_bytes = (int)(nlp - ep + 1);
				char *new_ln = emalloc(beg_bytes + rest_bytes + 1);
				memcpy(new_ln, ln, beg_bytes);
				memcpy(new_ln + beg_bytes, ep, rest_bytes);
				new_ln[beg_bytes + rest_bytes] = '\0';
				lbuf_edit(xb, new_ln, r, r + 1, left, left);
				free(new_ln);
			}
			sbuf_chr(yb, '\n')
		}
		sbuf_nul(yb)
		vi_regput(vi_ybuf < 0 ? xdefreg : vi_ybuf, yb->s, 0);
		free(yb->s);
		if (cmd == 'd' || cmd == 'c') {
			xrow = r1;
			xoff = ren_noeol(lbuf_get(xb, r1),
					vi_col2off(xb, r1, c_left));
		}
		if (cmd == 'c')
			return vc_block_insert('I', r1, r2, c_left);
	} else if (cmd == '~' || cmd == 'u' || cmd == 'U') {
		for (r = r1; r <= r2; r++) {
			VCB_BOUNDS;
			if (left > right || !n)
				continue;
			int total = lbuf_s(ln)->len + 2;
			char *new_ln = emalloc(total);
			memcpy(new_ln, ln, total - 1);
			new_ln[total - 1] = '\0';
			char *p = new_ln + (bp - ln);
			char *pe = new_ln + (ep - ln);
			while (p < pe) {
				int ch = (unsigned char)*p;
				if (ch <= 0x7f) {
					if (cmd == 'u')
						*p = (char)tolower(ch);
					else if (cmd == 'U')
						*p = (char)toupper(ch);
					else
						*p = (char)(islower(ch) ? toupper(ch) : tolower(ch));
				}
				p += uc_len(p);
			}
			lbuf_edit(xb, new_ln, r, r + 1, left, right + 1);
			free(new_ln);
		}
	} else if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1, 1);
	vi_mod |= r1 != r2 ? 1 : 2;
	return 0;
}

static int vc_visual_op(int cmd)
{
	int r1 = vi_vrow, o1 = vi_voff;
	int r2 = xrow, o2 = xoff;
	if (r1 > r2 || (r1 == r2 && o1 > o2)) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	}
	int visual = vi_visual;
	int lnmode = visual == 'V';
	vi_visual = 0;
	if (visual == 'b') {
		int c1, c2;
		vi_blockcols(&c1, &c2);
		return vc_block_op(cmd, r1, r2, c1, c2);
	}
	if (!lnmode) {
		/* include the char under the cursor, as vc_motion does */
		if (o2 < lbuf_eol(xb, r2, 2))
			o2++;
		ren_state *r = (ren_state*)lbuf_get(xb, r1);
		r = r ? ren_position((char*)r) : NULL;
		o1 = r ? MAX(0, MIN(o1, r->n)) : 0;
	} else {
		o1 = 0;
		o2 = lbuf_eol(xb, r2, r1 >= r2);
	}
	int key = 0;
	int prevlen = lbuf_len(xb);
	if (cmd == 'y')
		vi_yank(r1, o1, r2, o2, lnmode);
	else if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	else if (cmd == 'c')
		key = vi_change(r1, o1, r2, o2, lnmode);
	else if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	else if (cmd == '!')
		vi_pipe(r1, r2);
	else if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1, 1);
	vi_mod |= r1 != r2 || prevlen != lbuf_len(xb) ? 1 : 2;
	return key;
}

static int vc_motion(int cmd)
{
	int r1 = xrow, r2 = xrow;	/* region rows */
	int o1 = xoff, o2;		/* visual region columns */
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

static int vc_insert(int cmd)
{
	char *post, *ln = lbuf_get(xb, xrow);
	int row, cmdo, l1, off, key, postn = 1;
	sbuf_smake(sb, xcols)
	if (cmd == 'I')
		xoff = lbuf_indents(xb, xrow);
	else if (cmd == 'A')
		xoff = lbuf_eol(xb, xrow, 1);
	else if (cmd == 'o') {
		xrow++;
		if (xlw) {
			if (vi_srow(xrow) >= xrows) {
				while (vi_srow(xrow) >= xrows && vi_topadv(1));
				vi_drawagain(xtop);
			}
		} else if (xrow - xtop == xrows)
			vi_drawagain(++xtop);
	}
	xoff = ren_noeol(ln, xoff);
	row = xrow;
	if (cmd == 'a' || cmd == 'A')
		xoff++;
	if (ln && ln[0] == '\n')
		xoff = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	if (cmdo || !ln) {
		if (cmdo && !lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0, 0, 0);
		off = l1 = vi_indents(ln);
		post = "\n";
	} else {
		off = xoff;
		l1 = rstate->chrs[off] - ln;
		postn = rstate->n - off;
		post = ln + l1;
	}
	if (xlw) {
		led_row = MAX(0, vi_srow(row));
	} else {
		led_row = -1;
		term_pos(row - xtop, 0);
		term_room(cmdo);
	}
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, row, cmdo << 2, &postn, row, MIN(row + !cmdo, lbuf_len(xb)));
	if (postn != l1 || cmdo || !ln)
		lbuf_edit(xb, sb->s, row, row + !cmdo, off, xoff);
	free(sb->s);
	return key;
}

static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg);
	int i, off;
	char *ln;
	sbuf *buf = ex_regget(vi_ybuf < 0 ? xdefreg : vi_ybuf);
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
			xrow++;
		lbuf_edit(xb, sb->s, xrow, xrow, 0, 0);
		xoff = lbuf_indents(xb, xrow);
		free(sb->s);
		return 1;
	}
	if (!(ln = lbuf_get(xb, xrow)))
		ln = "\n";
	off = ren_noeol(ln, xoff) + (ln[0] != '\n' && cmd == 'p');
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_mem(sb, buf->s, buf->s_n)
	sbufn_str(sb, rstate->chrs[off])
	xoff = off + uc_slen(buf->s) * cnt - 1;
	lbuf_edit(xb, sb->s, xrow, xrow + 1, off, xoff);
	free(sb->s);
	return 1;
}

static void vc_join(int spc, int cnt)
{
	int o2 = 0;
	if (lbuf_join(xb, xrow, xrow + cnt, xoff, &o2, spc))
		return;
	xoff = o2;
}

static void vi_scrollforward(int cnt)
{
	vi_topadv(cnt);
	xrow = MAX(xrow, vi_fullrow());
}

static void vi_scrollbackward(int cnt)
{
	vi_topadv(-cnt);
	xrow = MIN(xrow, vi_lastrow());
}

static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg);
	char *cs = led_read(&xkmap, term_read(0));
	char *ln = lbuf_get(xb, xrow);
	int off, i;
	if (!ln || !cs)
		return 0;
	off = ren_noeol(ln, xoff);
	if (off + cnt >= rstate->n)
		return 0;
	sbuf_smake(sb, lbuf_s(ln)->len)
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs)
	sbufn_str(sb, rstate->chrs[off+cnt])
	xoff = (off + cnt - 1) * (cs[0] != '\n');
	lbuf_edit(xb, sb->s, xrow, xrow + 1, off, xoff);
	xrow += cnt * (cs[0] == '\n');
	free(sb->s);
	rep_record()
	return cs[0] == '\n' ? 1 : 2;
}

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

static void vi_argcmd(int arg, char cmd)
{
	char str[32];
	char *cs = itoa(arg, str);
	*cs = cmd;
	term_push(str, cs - str + 1);
}

#define topfix() \
if (xrow < 0 || xrow >= lbuf_len(xb)) \
	xrow = lbuf_len(xb) ? lbuf_len(xb) - 1 : 0; \
if (xrow < xtop) { \
	xtop = xrow; \
	xtopsub = 0; \
} else if (!xlw && xrow >= xtop + xrows) \
	xtop = xrow - xrows + 1; \
if (xlw) { \
	xtopsub = MIN(xtopsub, vi_lnrows(lbuf_get(xb, xtop)) - 1); \
	if (xrow - xtop >= xrows) \
		vi_toprows(xrow, xrows - 1); \
	while (vi_crow() < 0 && xtopsub > 0) \
		xtopsub--; \
	while (vi_crow() >= xrows && vi_topadv(1)); \
} \

/* place the terminal cursor at rendered position pos of the cursor line */
#define vi_curpos(pos, lncol) \
{ \
	if (xlw) { \
		int w = ren_wrapw(vi_lncol); \
		term_pos(vi_srow(xrow) + (pos) / w, (lncol) + (pos) % w); \
	} else \
		term_pos(xrow - xtop, (pos) + (lncol)); \
} \

void vi(int init)
{
	char *ln, *cs;
	int mv, n, k, c;
	xgrec++;
	if (init) {
		topfix()
		vi_col = vi_off2col(xb, xrow, xoff);
		vi_drawagain(xtop);
		vi_curpos(led_pos(lbuf_get(xb, xrow), vi_col), vi_lncol)
	}
	while (!xquit) {
		int nrow = xrow;
		int noff = xoff;
		int orow = nrow;
		int ooff = noff;
		int otop = xtop;
		int otopsub = xtopsub;
		int oleft = xleft;
		ticmd_pos = 0;
		vi_mod = 0;
		lsp_wake = 1;
		vi_ybuf = vi_yankbuf(TK_CTL('l'));
		lsp_wake = 0;
		vi_arg = vi_prefix();
		term_dec()
		if (vi_lnnum == 1) {
			vi_lnnum = 0;
			vi_lncol = 0;
			vi_mod |= 1;
		}
		if (xmpt == 1) {
			xmpt = 0;
			if (syn_scdirl > 0)
				syn_scdir(0);
			if (xlw)
				vi_drawrow(vi_botrow(), vi_srow(vi_botrow()));
			else
				vi_drawrow(otop + xrows - 1, xrows - 1);
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
			xrow = nrow;
			xoff = noff;
			if (xlw && strchr("HML", mv))
				vi_curseg(mv == 'M' ? xrows / 2 : mv == 'H' ?
					vi_srow(xrow) : vi_srow(xrow) +
					vi_lnrows(lbuf_get(xb, xrow)) - 1);
		} else if (mv == 0) {
			char *cmd;
			term_dec()
			re_motion:
			c = term_read(TK_CTL('l'));
			switch (c) {
			case TK_CTL('b'):
				vi_scrollbackward(MAX(1, vi_arg) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_curseg(-1);
				vi_mod |= 4;
				break;
			case TK_CTL('f'):
				vi_scrollforward(MAX(1, vi_arg) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_curseg(-1);
				vi_mod |= 4;
				break;
			case TK_CTL('e'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollforward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				vi_curseg(-1);
				break;
			case TK_CTL('y'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollbackward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				vi_curseg(-1);
				break;
			case TK_CTL('u'):
				if (xrow == 0)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				if (xlw) {
					if (xtop > 0)
						vi_topadv(-n);
					vi_wrapstep(&xrow, n, -1);
					xrow = MIN(xrow, vi_lastrow());
				} else {
					xrow = MAX(0, xrow - n);
					if (xtop > 0)
						vi_topadv(-n);
				}
				xoff = lbuf_indents(xb, xrow);
				vi_curseg(-1);
				vi_mod |= 4;
				break;
			case TK_CTL('d'):
				if (xrow == lbuf_len(xb) - 1)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				if (xlw) {
					vi_topadv(n);
					vi_wrapstep(&xrow, n, 1);
					xrow = MAX(xrow, vi_fullrow());
				} else {
					xrow = MIN(MAX(0, lbuf_len(xb) - 1), xrow + n);
					if (xtop < lbuf_len(xb) - xrows)
						xtop = MIN(lbuf_len(xb) - xrows, xtop + n);
				}
				xoff = lbuf_indents(xb, xrow);
				vi_curseg(-1);
				vi_mod |= 4;
				break;
			case TK_CTL('i'): {
				if (!(ln = lbuf_get(xb, xrow)))
					break;
				ln = uc_chr(ln, xoff);
				n = strlen(ln);
				char buf[n + 4];
				memcpy(buf, ":e ", 3);
				memcpy(buf+3, ln, n);
				term_push(buf, n + 3);
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg ? -vi_cndir : vi_cndir;
				vi_arg = ex_buf - bufs + vi_cndir;
			case TK_CTL('_'):	/* this is also ^7 on some systems */
				if (vi_arg > 0)
					goto switchbuf;
				ex_exec("left0:b:mpt0");
				term_chr('\n');
				vi_arg = vi_digit();
				if (vi_arg > -1 && vi_arg < xbufcur) {
					switchbuf:
					bufs_switchwft(vi_arg < xbufcur ? vi_arg : 0)
					vc_status(0);
				}
				vi_mod |= 1;
				break;
			case 'u':
				if (vi_visual) {
					vc_visual_op('u');
					break;
				}
				undo:
				if (vi_arg >= 0 && !lbuf_undo(xb, &xrow, &xoff)) {
					vi_mod |= 1;
					vi_arg--;
					goto undo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("undo failed")
				else if (vi_unseen(xrow, 0))
					vi_center(xrow);
				break;
			case TK_CTL('r'):
				redo:
				if (vi_arg >= 0 && !lbuf_redo(xb, &xrow, &xoff)) {
					vi_mod |= 1;
					vi_arg--;
					goto redo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("redo failed")
				else if (vi_unseen(xrow, 0))
					vi_center(xrow);
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
				bufs_switchwft(ex_pbuf - bufs)
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
			case 'U':
				if (vi_visual)
					vc_visual_op('U');
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
					vi_arg = MIN(vi_arg ? vi_arg : xts, 80);
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
					cs = vi_curword(xb, xrow, xoff, vi_arg, 1);
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
					cs = vi_curword(xb, xrow, xoff, vi_prefix(), 1);
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
					cs = vi_curword(xb, xrow, xoff, vi_arg, 1);
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
				if (vi_visual == 'v' || vi_visual == 'V') {
					char range[128];
					int vr1 = MIN(vi_vrow, xrow), vr2 = MAX(vi_vrow, xrow);
					char *p = itoa(vr1+1, range);
					*p++ = ',';
					p = itoa(vr2+1, p);
					if (vi_visual == 'v') {
						int o1 = vi_voff, o2 = xoff;
						if (vi_vrow > xrow ||
								(vi_vrow == xrow && vi_voff > xoff))
							swap(&o1, &o2);
						*p++ = ';';
						p = itoa(o1, p);
						*p++ = ';';
						p = itoa(o2 + 1, p);
					}
					*p = '\0';
					ln = vi_enprompt(":", range, &k, &n);
					goto do_excmd;
				}
				ln = vi_enprompt(":", NULL, &k, &n);
				do_excmd:
				if (k && ln[n]) {
					ex_command(ln + n)
					if (xrow != orow && vi_unseen(xrow, !vi_status))
						vi_center(xrow);
				}
				vi_mod |= 1;
				if (!xmpt)
					vi_drawmsg(ln);
				free(ln);
				if (xquit) {
					xmpt = xmpt ? xmpt : (xgrec > 1);
					continue;
				} else if (!xmpt)
					xmpt = 1;
				break;
			case 'c':
				if (vi_visual) {
					k = vc_visual_op('c');
					goto insert_done;
				}
			case 'd':
				if (vi_visual) {
					vc_visual_op('d');
					break;
				}
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
					int r1 = xrow, o1 = xoff, r2, o2;
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
				if (vi_visual && c != TK_CTL('w')) {
					vc_visual_op(c);
					break;
				}
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
				if (vi_visual == 'b' && (c == 'I' || c == 'A')) {
					int r1b = MIN(vi_vrow, xrow), r2b = MAX(vi_vrow, xrow);
					int c_left, c_right;
					vi_blockcols(&c_left, &c_right);
					vi_visual = 0;
					k = vc_block_insert(c, r1b, r2b, c == 'I' ? c_left : c_right);
					goto insert_done;
				}
				insert:
				k = vc_insert(c);
				insert_done:
				if (k == 127 || k == TK_CTL('w')) {
					if (xrow && !(xoff > 0 && lbuf_eol(xb, xrow, 1))) {
						xrow--;
						if (xtop > otop)
							xtop = otop;
						topfix()
						vc_join(0, 2);
						vi_drawagain(xtop);
						if (vi_status)
							vc_status(vi_tsm);
					} else if (xoff) {
						if (k == TK_CTL('w')) {
							noff = xoff;
							lbuf_wordend(xb, 0, -2, &xrow, &noff);
							vi_delete(xrow, noff, xrow, xoff, 0);
						} else
							vi_delete(xrow, xoff - 1, xrow, xoff, 0);
					}
					c = xoff != lbuf_eol(xb, xrow, 1) ? 'i' : 'a';
					xb->useq += xseq;
					goto insert;
				}
				xoff--;
				rep_record()
				vi_mod |= !xlw && !xpac && xrow == orow ? 8 : 1;
				break;
			case 'J':
				vc_join(1, vi_arg <= 1 ? 2 : vi_arg);
				rep_record()
				vi_mod |= 1;
				break;
			case 'K': {
				preserve(int, xvis, xvis = 1;)
				do {
					ex_exec(";+1c\n:-1");
				} while (vi_arg--);
				restore(xvis)
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
				lbuf_mark(xb, term_read(0), xrow, xoff);
				break;
			case 'p':
			case 'P':
				vi_mod |= vc_put(c);
				break;
			case 'z':
				k = term_read(0);
				switch (k) {
				case '\n':
					vi_toprows(xrow, 0);
					break;
				case '.':
					vi_center(xrow);
					break;
				case '-':
					vi_toprows(xrow, xrows -
						vi_lnrows(lbuf_get(xb, xrow)));
					break;
				case 'l':
				case 'r':
				case 'L':
				case 'R':
					xtd = uc_isupper(k)+1;
					xtd = tolower(k) == 'r' ? -xtd : xtd;
					RST_NULL(0, 1)
					break;
				case 'e':
				case 'f':
					xkmap = k == 'e' ? 0 : xkmap_alt;
					break;
				case '1':
				case '2':
					xkmap_alt = k - '0';
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
					preserve(int, xgrp, xgrp = 2;)
					preserve(int, xvis, xvis = 1;)
					n = vi_arg ? vi_arg : 80;
					while (1) {
						xoff = vi_col2off(xb, xrow, n);
						vi_col = vi_off2col(xb, xrow, xoff+1);
						if (vi_col <= n)
							break;
						if (ex_exec("f>[^ \t]*[ \t]+(?\\:.$|(.)):??;c\n"))
							break;
					}
					restore(xgrp)
					restore(xvis)
					vi_mod |= !texec;
				} else if (k == 'q') {
					preserve(int, xled, xled = 0;)
					char cmd[64] = "g/./& ";
					memcpy(itoa(vi_arg, cmd+5), "gw", sizeof("gw"));
					ex_command(cmd)
					restore(xled)
					vi_mod |= 1;
				} else if (k == '~' || k == 'u' || k == 'U') {
					vc_motion(k);
				} else if (k == 'v' || k == 'V' || k == 'b') {
					if (!vi_visual) {		/* fresh selection */
						vi_vrow = xrow;
						vi_voff = xoff;
					}
					vi_visual = vi_visual == k ? 0 : k;
					vi_mod |= 1;
				} else if (k == 'K') {
					if (xb_path && xb_path[0])
						lsp_hover(xb_path, xrow, xoff);
				} else if (k == 'd') {
					if (xb_path && xb_path[0]) {
						lsp_definition(xb_path, xrow, xoff);
						vc_status(0);
						vi_mod |= 1;
					}
				}
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
				if (vi_visual) {
					vc_visual_op('~');
					break;
				}
				term_push("g~ ", 3);
				goto motion;
			case 'C':
			case 's':
			case 'S':
				if (vi_visual) {
					k = vc_visual_op('c');
					goto insert_done;
				}
				if (c == 'C')
					term_push("c$", 2);
				else if (c == 's')
					term_push("c ", 2);
				else
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
				term_pos(xrow - xtop, 0);
				xleft = vi_arg ? xleft : 0;
				led_modeswap();
				vi_mod |= 1;
				if (xquit)
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
				xquit = vi_arg ? -vi_arg * 256 - 257 : 1;
				if (k == 'z')
					term_push("\n", 1);
				else if (xgrec == 1) {
					term_clean();
					xgrec = 0;
				}
				continue;
			case '.':
				for (k = 0; k < MAX(1, vi_arg); k++)
					term_push(rep_cmd, rep_len);
				break;
			case 'q':
				if (xrr > 0) {
					sbuf *rsb = ex_regget(xrr);
					if (rsb && rsb->s_n)
						sbufn_cut(rsb, rsb->s_n - 1)
					xrr = 0;
				} else if (vi_ybuf <= 0) {
					vi_drawmsg_mpt("no record register")
				} else {
					if (!vi_arg)
						ex_regput(vi_ybuf, "", 0);
					xrr = vi_ybuf;
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
			case TK_ESC:
				if (vi_visual) {
					vi_visual = 0;
					vi_mod |= 1;
					break;
				}
				continue;
			case 0:	/* lsp_wake yield; redraw via lsp_dirty */
				break;
			default:
				continue;
			}
		}
		topfix()
		ln = lbuf_get(xb, xrow);
		xoff = ren_noeol(ln, xoff);
		if (ln && !rstate->wid[xoff]) {
			for (n = xoff, k = n; k < rstate->n && !rstate->wid[k];) {
				if (!k)
					n = ooff+1;
				k += n > ooff ? 1 : -1;
			}
			if (k < rstate->n)
				xoff = k;
		}
		if (vi_mod)
			vi_col = vi_off2col(xb, xrow, xoff);
		if (xlw)
			xleft = 0;
		else if (vi_col >= xleft + xcols || vi_col < xleft)
			xleft = vi_col < xcols ? 0 : vi_col - xcols / 2;
		n = led_pos(ln, ren_cursor(ln, vi_col));
		if (xmpt > 1) {
			if (!xpln)
				term_chr('\n');
			vi_drawmsg("[any key to continue] ");
			term_read(0);
			xmpt = 0;
			vi_mod |= 1;
		}
		xpln = 0;
		if (xhlw) {
			static char *word;
			static int tree;
			int use_tree = ts_document(xb) != NULL;
			if ((cs = vi_curword(xb, xrow, xoff, xhlw, 0))) {
				if (!word || strcmp(word, cs) || tree != use_tree) {
					if (use_tree)
						ts_setword(cs);
					else
						syn_reloadft(syn_addhl(cs, 1), 0);
					tree = use_tree;
					vi_mod |= 1;
				}
				free(word);
				word = cs;
			}
		}
		if (xhlp && (k = syn_findhl(3)) >= 0) {
			int row = xrow, off = xoff, row1, off1;
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
					vi_mod |= row1 == row && orow == xrow ? 2 : 1;
				}
			}
		}
		if (vi_visual)
			vi_mod |= 1;
		if (xb_path && xb_path[0])
			lsp_sync(xb_path, xb);
		if (lsp_dirty) {
			lsp_dirty = 0;
			vi_mod |= 1;
		}
		vi_rendpost(vi_mod, otop, otopsub, oleft, orow, ooff, n);
		xb->useq += xseq;
	}
	vi_rendwait();
	if (--xgrec == 0) {
		term_pos(xrows - !vi_status, 0);
		if (xmpt > 0 && !xpln)
			term_chr('\n');
		else
			term_kill();
	}
}

/* paint the queued frames; the state a frame reads is frozen until it lands */
static void *vi_rendloop(void *arg)
{
	struct vi_rend *r = arg;
	int once = r->on < 0;		/* no thread: this call paints one frame */
	if (!once) {
		redraw_thread = 1;	/* this thread paints, never slot 0 */
		sigset_t set;		/* SIGWINCH belongs to the input loop */
		sigemptyset(&set);
		sigaddset(&set, SIGWINCH);
		pthread_sigmask(SIG_BLOCK, &set, NULL);
	}
	do {
		int vi_mod, otop, otopsub, oleft, orow, ooff, n;
		pthread_mutex_lock(&r->mtx);
		while (!once && !r->busy)
			pthread_cond_wait(&r->req, &r->mtx);
		vi_mod = r->mod;
		otop = r->otop;
		otopsub = r->otopsub;
		oleft = r->oleft;
		orow = r->orow;
		ooff = r->ooff;
		n = r->pos;
		pthread_mutex_unlock(&r->mtx);
		term_record = 1;	/* one frame, one write */
		if (ts_redraw) {
			vi_mod |= 1;
			ts_redraw = 0;
		}
		if (vi_mod & 1 || xleft != oleft
				|| (vi_lnnum && orow != xrow && !(vi_lnnum == 2))
				|| (*vi_word && orow != xrow))
			vi_drawagain(xtop);
		else if (*vi_word && !xlw && (ooff != xoff || vi_mod & 2)
				&& xrow+1 < xtop + xrows)
			vi_drawrow(xrow+1, xrow+1 - xtop);
		else if (xtop != otop || xtopsub != otopsub)
			vi_drawupdate(vi_topdiff(otop, otopsub));
		if (xhll) {
			syn_blockhl = -1;
			if (xrow != orow && orow >= xtop && orow <= vi_botrow())
				if (!(vi_mod & 1))
					vi_drawrow(orow, vi_srow(orow));
			syn_blockhl = -1;
			if (!ts_document(xb))
				syn_reloadft(syn_addhl("^.+", 2), 0);
			vi_drawrow(xrow, vi_srow(xrow));
			if (!ts_document(xb))
				syn_reloadft(syn_addhl(NULL, 2), 0);
		} else if (vi_mod & 2 && !(vi_mod & 1)) {
			syn_blockhl = -1;
			vi_drawrow(xrow, vi_srow(xrow));
		}
		if (vi_status && xmpt < 1) {
			xrows -= term_resized != vi_status;
			vi_status = term_resized;
			vc_status(vi_tsm);
			if (xmpt > 0)
				xmpt = 0;
		}
		vi_curpos(n, vi_lncol)
		term_commit();
		pthread_mutex_lock(&r->mtx);
		r->busy = 0;
		pthread_cond_signal(&r->done);
		pthread_mutex_unlock(&r->mtx);
	} while (!once);
	return NULL;
}

/* wait for the queued frame; the editor state is shared until this returns */
void vi_rendwait(void)
{
	struct vi_rend *r = &vi_rend;
	if (r->on != 1)
		return;
	pthread_mutex_lock(&r->mtx);
	while (r->busy)
		pthread_cond_wait(&r->done, &r->mtx);
	pthread_mutex_unlock(&r->mtx);
}

/* whether input is waiting; a frame drawn now is stale before it lands */
static int vi_rendpend(void)
{
	return tibuf_pos < tibuf_cnt || poll(&term_ufd, 1, 0) > 0;
}

/* queue a frame and hand the input loop its thread back */
static void vi_rendpost(int mod, int otop, int otopsub, int oleft,
			int orow, int ooff, int pos)
{
	struct vi_rend *r = &vi_rend;
	mod |= r->skip;
	if (!xquit && vi_rendpend()) {
		r->skip = mod | 1;
		return;
	}
	r->skip = 0;
	if (!r->on) {
		/* musl caps new threads at 128k; give the thread main()'s stack */
		pthread_attr_t attr, *ap = NULL;
		struct rlimit rl;
		r->on = 1;
		if (!pthread_attr_init(&attr) && !getrlimit(RLIMIT_STACK, &rl)
				&& rl.rlim_cur != RLIM_INFINITY
				&& !pthread_attr_setstacksize(&attr, rl.rlim_cur))
			ap = &attr;
		if (pthread_create(&r->tid, ap, vi_rendloop, r))
			r->on = -1;
	}
	pthread_mutex_lock(&r->mtx);
	r->mod = mod;
	r->otop = otop;
	r->otopsub = otopsub;
	r->oleft = oleft;
	r->orow = orow;
	r->ooff = ooff;
	r->pos = pos;
	r->busy = r->on > 0;
	pthread_cond_signal(&r->req);
	pthread_mutex_unlock(&r->mtx);
	if (r->on < 0)
		vi_rendloop(r);
}

static void sighandler(int signo)
{
	term_winch++;
}

static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGWINCH, &sa, NULL);
}

int main(int argc, char *argv[])
{
	int i, j;
	setup_signals();
	dir_init();
	syn_init();
	ts_init();
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
				xvis |= 1|2;
			else if (argv[i][j] == 'e')
				xvis |= 2;
			else if (argv[i][j] == 'm')
				xvis |= 4;
			else if (argv[i][j] == 'a')
				xvis |= 8;
			else if (argv[i][j] == 'v')
				xvis = 0;
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				fprintf(stderr, "Nextvi-7.8 Usage: %s [-aemsv] [file ...]\n", argv[0]);
				return EXIT_FAILURE;
			}
		}
	}
	tibuf = emalloc(tibuf_sz);
	if (!(xvis & 1))
		term_init();
	if (xvis & 8)
		term_scrh()
	ex_init(argv + i, argc - i);
	if (xvis & 2)
		ex();
	else
		vi(1);
	ts_done();
	term_done();
	if (xvis & 8)
		term_scrl()
	return xquit < -256 ? (abs(xquit) - 257) & 255 : abs(xquit) - 1;
}
