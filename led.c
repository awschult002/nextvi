/**
 * @file led.c
 * @brief Line editor: renders a line to the terminal (bidi, highlighting,
 * extensions) and reads line input for prompts and insert mode, with
 * history, registers and word autocomplete.
 */
static sbuf *suggestsb;	///< current autocomplete candidates, '\n'-separated; prefix matches first
static sbuf *acsb;	///< autocomplete word list, '\n'-separated with a leading '\n'; built by ^g
static sbuf *compsb;
static sbuf *extsb;	///< array of led_ext records; the last extregn bytes are the registered ones

/**
 * @brief Fill suggestsb with acsb words containing pattern, words that start
 * with it first; words of length l (the typed word itself) are skipped.
 * @return length of suggestsb
 */
static s64 search(const char *pattern, s64 l)
{
	if (!*pattern)
		return 0;
	sbuf_cut(suggestsb, 0)
	sbuf_smake(sylsb, 1024)
	char *part = strstr(acsb->s, pattern);
	while (part) {
		/* part: match position; walk back to the start of its word */
		char *part1 = part;
		while (*part != '\n')
			part--;
		s64 len = dstrlen(++part, '\n');
		if (len++ != l) {
			if (part == part1)
				sbuf_mem(suggestsb, part, len)
			else
				sbuf_mem(sylsb, part, len)
		}
		part = strstr(part+len, pattern);
	}
	sbuf_mem(suggestsb, sylsb->s, sylsb->s_n)
	free(sylsb->s);
	sbuf_nul4(suggestsb)
	return suggestsb->s_n;
}

/**
 * @brief Add the words of buf (autocomplete_filter or a default word regex) to
 * acsb, skipping duplicates. ibuf holds, as s64, the offset just past each
 * '\n' in acsb, so a word spans [ip[-1], ip[0]-1).
 */
static void file_index(struct lbuf *buf)
{
	char reg[] = "[^\t !-/:-@[-\\]^`{-\x7f]+";
	s64 len, sidx, grp = opt_search_group;
	char **ss = buf->ln;
	s64 ln_n = lbuf_len(buf), n;
	rset *rs = rset_smake(autocomplete_filter ? autocomplete_filter->s : reg,
		opt_ignorecase ? REG_ICASE | REG_NEWLINE : REG_NEWLINE);
	if (!rs || grp >= rs->nsubc) {
		rset_free(rs);
		return;
	}
	s64 subs[rs->nsubc];
	sbuf_smake(ibuf, 1024)
	for (n = 1; n <= acsb->s_n; n++)
		if (acsb->s[n - 1] == '\n')
			sbuf_mem(ibuf, &n, sizeof(n))
	for (s64 i = 0; i < ln_n; i++) {
		sidx = 0;
		while (rset_find(rs, ss[i]+sidx, subs, sidx ? REG_NOTBOL : 0) >= 0) {
			/* if target group not found, continue with group 1
			which will always be valid, otherwise there be no match */
			if (subs[grp] < 0) {
				sidx += subs[1] > 0 ? subs[1] : uc_len(ss[i] + sidx);
				continue;
			}
			len = subs[grp + 1] - subs[grp];
			if (len > 1) {
				char *part = ss[i]+sidx+subs[grp];
				s64 *ip = (s64*)(ibuf->s+sizeof(n));
				for (n = len+1; ip < (s64*)&ibuf->s[ibuf->s_n]; ip++)
					if (*ip - ip[-1] == n &&
						!memcmp(acsb->s + ip[-1], part, len))
							goto skip;
				sbuf_mem(acsb, part, len)
				sbuf_chr(acsb, '\n')
				sbuf_mem(ibuf, &acsb->s_n, sizeof(n))
			}
			skip:
			sidx += subs[grp + 1] > 0 ? subs[grp + 1] : uc_len(ss[i] + sidx);
		}
	}
	sbuf_nul(acsb)
	free(ibuf->s);
	rset_free(rs);
}

/** @brief Map key c through keymap kmap; unmapped keys map to themselves. */
static char *kmap_map(s64 kmap, s64 c)
{
	static char cs[4];
	char **keymap = conf_kmap(kmap);
	cs[0] = c;
	return keymap[c] ? keymap[c] : cs;
}

/** @brief Map cursor screen position to terminal column (mirrored for RTL lines). */
s64 led_pos(char *s, s64 pos)
{
	if (dir_context(s) < 0)
		return opt_left_col + term_cols - pos - 1;
	return pos - opt_left_col;
}

/** @brief Map a character offset in x->s0 to its x->att index; -1 if not visible. */
s64 led_attidx(led_ctx *x, s64 off)
{
	s64 i, l, j;
	if (!x->bound)
		return (u64)off < (u64)x->alen ? off : -1;
	if (!x->alen || x->stt[0] > off || x->stt[x->alen-1] < off)
		return -1;
	i = off - x->stt[0];
	if (i < x->alen && x->stt[i] == off)
		return i;		/* text not reordered */
	for (l = 0, j = x->alen - 1; l <= j;) {
		i = l + (j - l) / 2;
		if (x->stt[i] == off)
			return i;
		else if (x->stt[i] < off)
			l = i + 1;
		else
			j = i - 1;
	}
	return -1;
}

/** @brief Default extension: usr holds {offset, length, attribute} s64 triplets merged into att. */
static void ext_attmerge(led_ext *p, led_ctx *x)
{
	if (!led_extkey(p, x))
		return;
	s64 *ola = p->usr;
	for (s64 t = 0; t < p->blen / (s64)sizeof(s64); t += 3) {
		s64 o = ola[t], end = o + ola[t+1];
		for (; o < end; o++) {
			s64 i = led_attidx(x, o);
			if (i >= 0)
				x->att[i] = syn_merge(x->att[i], ola[t+2]);
		}
	}
}

static s64 extregn;	///< bytes of registered (persistent) extensions at the end of extsb

/**
 * @brief Append a zeroed extension (ext_func = ext_attmerge).
 * The returned pointer is into extsb and is invalidated by the next push.
 */
static led_ext *led_extpush(void)
{
	led_ext la;
	memset(&la, 0, sizeof(la));
	la.ext_func = ext_attmerge;
	if (!extsb)
		sbuf_make(extsb, sizeof(la) * 2)
	sbuf_mem(extsb, &la, sizeof(la))
	return (led_ext*)&extsb->s[extsb->s_n - sizeof(la)];
}

/** @brief New temporary extension (dropped by led_extcut()); kept before the registered ones. */
led_ext *led_extnew(void)
{
	led_ext la, *p = led_extpush();
	if (extregn) {	/* registered extensions stay last */
		/* reg: first registered record; move them up and put the new record in front */
		char *reg = (char*)p - extregn;
		la = *p;
		memmove(reg + sizeof(la), reg, extregn);
		memcpy(reg, &la, sizeof(la));
		p = (led_ext*)reg;
	}
	return p;
}

/** @brief New registered extension; survives led_extcut(). */
led_ext *led_extreg(void)
{
	extregn += sizeof(led_ext);
	return led_extpush();
}

/** @brief First extension with the given function, or NULL. */
led_ext *led_extfind(void (*ext_func)(led_ext *p, led_ctx *x))
{
	if (!extsb)
		return NULL;
	for (s64 i = 0; i < extsb->s_n; i += sizeof(led_ext)) {
		led_ext *p = (led_ext*)&extsb->s[i];
		if (p->ext_func == ext_func)
			return p;
	}
	return NULL;
}

/** @brief Remove extension p from extsb. */
void led_extdel(led_ext *p)
{
	char *nxt = (char*)p + sizeof(*p);
	if ((char*)p >= &extsb->s[extsb->s_n] - extregn)
		extregn -= sizeof(*p);
	memmove(p, nxt, &extsb->s[extsb->s_n] - nxt);
	sbuf_cut(extsb, extsb->s_n - sizeof(*p))
}

/** @brief Drop unregistered extensions only, then re-add the tree-sitter extension (ts_init()). */
void led_extcut(void)
{
	if (!extsb)
		return;
	memmove(extsb->s, &extsb->s[extsb->s_n] - extregn, extregn);
	sbuf_cut(extsb, extregn)
	ts_init();
}

/* print_ch/hid_ch: emit a printable / non-printable char; variant 2 is used
 * when vi_hidch shows hidden chars ('_' space, '\\' newline, "->" tab) */
#define print_ch1(out) sbuf_mem(out, chrs[o], l)
#define print_ch2(out) sbuf_mem(out, *chrs[o] == ' ' ? "_" : chrs[o], l)

#define hid_ch1(out) sbuf_set(out, ' ', i - l)
#define hid_ch2(out) \
sbuf_set(out, *chrs[o] == '\n' ? '\\' : '-', i - l) \
if (ctx > 0 && *chrs[o] == '\t') \
	out->s[out->s_n-1] = '>'; \
else if (*chrs[o] == '\t') \
	out->s[out->s_n - (i - l)] = '<'; \

/* Emit screen cells [0, cterm): off[i] is the char in cell i (-1 = none),
 * [l, i) the cells it covers; attributes change only when they differ. */
#define led_out(out, n) \
{ \
for (i = 0; i < cterm;) { \
	s64 att_new = 0; \
	o = off[i]; \
	if (o >= 0) { \
		for (l = i; off[i] == o; i++); \
		att_new = att[bound ? ctt[atti++] : o]; \
		if (att_new != att_old) \
			sbuf_str(out, term_att(att_new)) \
		char *s = ren_translate(chrs[o], s0); \
		if (s) \
			sbuf_str(out, s) \
		else if (uc_isprint(*chrs[o])) { \
			l = uc_len(chrs[o]); \
			print_ch##n(out) \
		} else { \
			hid_ch##n(out) \
		} \
	} else { \
		if (cbeg || ctx < 0) { \
			if (att_new != att_old) \
				sbuf_mem(out, "\x1b[m", 3) \
			sbuf_chr(out, ' ') \
			i++; \
		} else \
			break; \
	} \
	att_old = att_new; \
} } \

/**
 * @brief Render and highlight line s0, screen columns [cbeg, cend), into term_sbuf.
 */
void led_render(char *s0, s64 cbeg, s64 cend)
{
	if (!opt_line_editor)
		return;
	ren_state *r = ren_position(s0);
	s64 j, c, l, i, o, n = r->n;
	s64 att_old = 0, atti = 0, cterm = cend - cbeg;
	char *bound = NULL;
	char **chrs = r->chrs;	/* chrs[i]: the i-th character in s0 */
	s64 off[cterm+1];	/* off[i]: the character at screen position i */
	s64 att[cterm+1];	/* att[i]: the attributes of i-th character */
	s64 stt[cterm+1];	/* stt[i]: remap off indexes */
	s64 ctt[cterm+1];	/* ctt[i]: cterm bound attrs */
	s64 ctx = r->ctx;
	off[cterm] = -1;
	if (ctx < 0) {
		o = cbeg;
		for (c = cterm-1; c >= 0; c--, o++)
			off[c] = o <= r->cmax ? r->col[o] : -1;
	} else {
		for (c = cbeg; c < cend; c++)
			off[c - cbeg] = c <= r->cmax ? r->col[c] : -1;
	}
	/* line does not fit: drop wide chars cut at either edge, then build bound,
	 * the visible chars in logical order, so highlighting sees only them.
	 * att[] temporarily holds the visible char offsets in screen order and is
	 * insertion-sorted; afterwards stt[k] is the offset of bound char k and
	 * ctt[j] the bound index of the j-th visible char in screen order. */
	if (r->cmax > cterm || cbeg || n > cterm) {
		i = ctx < 0 ? cterm-1 : 0;
		o = off[i];
		if (o >= 0 && cbeg && r->pos[o] < cbeg)
			while (off[i] == o)
				off[ctx < 0 ? i-- : i++] = -1;
		i = ctx < 0 ? 0 : cterm-1;
		o = off[i];
		if (o >= 0 && r->cmax > cterm && r->pos[o] + r->wid[o] > cend)
			while (off[i] == o)
				off[ctx < 0 ? i++ : i--] = -1;
		for (i = 0, c = 0; i < cterm;) {
			if ((o = off[i++]) >= 0) {
				att[c++] = o;
				for (; off[i] == o; i++);
			}
		}
		stt[0] = 0;
		for (i = 1; i < c; i++) {
			s64 key0 = att[i];
			j = i - 1;
			while (j >= 0 && att[j] > key0) {
				att[j + 1] = att[j];
				stt[j + 1] = stt[j];
				j = j - 1;
			}
			att[j + 1] = key0;
			stt[j + 1] = i;
		}
		sbuf_smake(bsb, cterm*4);
		for (i = 0; i < c; i++) {
			ctt[stt[i]] = i;
			stt[i] = att[i];
			sbuf_mem(bsb, chrs[att[i]], uc_len(chrs[att[i]]))
		}
		sbuf_nul4(bsb)
		bound = bsb->s;
	}
	memset(att, 0, MIN(n, cterm+1) * sizeof(att[0]));
	if (opt_syntax_hl == 1)
		syn_highlight(att, bound ? bound : s0, MIN(n, cterm));
	if (extsb && extsb->s_n && opt_syntax_hl > 0) {
		led_ctx x;
		x.att = att;
		x.alen = bound ? c : MIN(n, cterm);
		x.off = off;
		x.stt = stt;
		x.ctt = ctt;
		x.cterm = cterm;
		x.n = n;
		x.s0 = s0;
		x.bound = bound;
		x.r = r;
		for (i = 0; i < extsb->s_n; i += sizeof(led_ext)) {
			led_ext *p = (led_ext*)&extsb->s[i];
			p->ext_func(p, &x);
		}
	}
	free(bound);
	/* generate term output */
	if (vi_hidch)
		led_out(term_sbuf, 2)
	else
		led_out(term_sbuf, 1)
	sbufn_mem(term_sbuf, "\x1b[m", 3)
	/* restore the bytes ren_position() cut off for opt_render_limit */
	if (r->holelen) {
		memcpy(chrs[n], r->nulhole, r->holelen);
		r->holelen = 0;
	}
}

/** @brief Byte offset of the last character of s. */
static s64 led_lastchar(char *s)
{
	char *r = *s ? strchr(s, '\0') : s;
	if (r != s)
		r = uc_beg(s, r - 1);
	return r - s;
}

static s64 led_preview_ps;
static void led_preview_draw(s64 ps);

s64 led_row = -1;		/* terminal row of the edited line, -1 = relative */
static s64 led_lw;		/* the edited line is drawn wrapped at absolute rows */
static s64 led_rowh = 1;	/* the rows the edited line occupies */
static s64 led_nextb;		/* the buffer line drawn below the edited line */

/** @brief Byte offset of the start of the last word of s (trailing blanks skipped). */
static s64 led_lastword(char *s)
{
	char *r = *s ? uc_beg(s, strchr(s, '\0') - 1) : s;
	s64 kind;
	while (r > s && uc_isspace(*r))
		r = uc_beg(s, r - 1);
	kind = r > s ? uc_kind(r) : 0;
	while (r > s && uc_kind(uc_beg(s, r - 1)) == kind)
		r = uc_beg(s, r - 1);
	return r - s;
}

/* complete the path at the end of sb, lst gets the matches, typed gets the
length of the part already there; returns the count of the matches */
static s64 led_pathcomp(sbuf *sb, s64 pre, sbuf *lst, s64 *typed)
{
	DIR *dp;
	struct dirent *dirp;
	struct stat st;
	char *base;
	s64 i, n = 0, wo, bo, blen, mlen = 0;
	sbuf_smake(path, 128)
	for (wo = sb->s_n; wo > pre; wo--)
		if (sb->s[wo-1] == ' ' || sb->s[wo-1] == '\t')
			break;
	for (bo = wo, i = sb->s_n; i > wo; i--)
		if (sb->s[i-1] == '/') {
			bo = i;
			break;
		}
	sbuf_mem(path, sb->s + wo, bo - wo)
	sbuf_nul4(path)
	base = sb->s + bo;
	*typed = blen = sb->s_n - bo;
	if (!(dp = opendir(path->s_n ? path->s : "."))) {
		free(path->s);
		return 0;
	}
	while ((dirp = readdir(dp))) {
		if (dirp->d_name[0] == '.' && (!dirp->d_name[1] ||
				(dirp->d_name[1] == '.' && !dirp->d_name[2])))
			continue;
		if (strncmp(dirp->d_name, base, blen))
			continue;
		if (!n++) {
			sbufn_str(lst, dirp->d_name)
			mlen = lst->s_n;
		} else {
			for (i = 0; i < mlen && lst->s[i] == dirp->d_name[i]; i++);
			mlen = i;
			sbuf_chr(lst, ' ')
			sbufn_str(lst, dirp->d_name)
		}
	}
	closedir(dp);
	if (n) {
		sbuf_mem(sb, lst->s + blen, mlen - blen)
		if (n == 1) {
			sbufn_str(path, lst->s)
			if (!stat(path->s, &st) && S_ISDIR(st.st_mode))
				sbuf_chr(sb, '/')
		}
		sbuf_nul4(sb)
	}
	free(path->s);
	return n;
}

/* clear the screen and list lst from column beg, below the top row,
wrapped at word boundaries; returns the first column left out */
static s64 led_complist(sbuf *lst, s64 beg)
{
	ren_state *rp;
	s64 i, r, end, tot;
	preserve(s64, opt_syntax_hl, opt_syntax_hl = 0;)
	term_clean();
	rstate = rstates+2;
	rstate->s = NULL;
	rp = ren_position(lst->s);
	tot = rp->pos[rp->n];
	for (r = 1; r < term_rows && beg < tot; r++) {
		end = beg + term_cols;
		if (end < tot) {
			for (i = end; i > beg; i--)
				if (*rp->chrs[rp->col[i]] == ' ') {
					end = i;
					break;
				}
		} else
			end = tot;
		led_crender(lst->s, r, 0, beg, end)
		for (beg = end; beg < tot && *rp->chrs[rp->col[beg]] == ' '; beg++);
	}
	rstate->s = NULL;
	rstate = rstates;
	restore(opt_syntax_hl)
	return beg < tot ? beg : 0;
}

/**
 * @brief Draw the input line sb->s + ps followed by post and place the cursor
 * between them. With :lw (led_lw) the line wraps over led_rowh rows at
 * led_row and the buffer lines below it are redrawn.
 * @param[out] poff  cursor character offset (number of chars before post)
 */
static void led_printparts(sbuf *sb, s64 pre, s64 ps,
	char *post, s64 postn, s64 *poff)
{
	if (!opt_line_editor) {
		sbuf_nul4(sb)
		return;
	}
	s64 dir, off, pos, psn = sb->s_n;
	s64 lncol = poff == &cursor_off ? lnum_width : 0;
	sbuf_str(sb, post)
	sbuf_nul4(sb)
	if (ts_preview && poff == &cursor_off) {
		led_preview_ps = ps;
		if (ts_preview_update(ts_preview, sb->s) && !led_lw)
			led_preview_draw(ps);
	}
	/* XXX: O(n) insertion; recursive array data structure cannot be optimized.
	For correctness, rstate must be recomputed. */
	rstate += 2;
	rstate->s = NULL;
	ren_state *r = ren_position(sb->s + ps);
	/* off: cursor char index; pos: its screen column, next to the char before
	 * it on the side the text flows (checked via the two preceding chars) */
	off = r->n - postn;
	*poff = off;
	pos = ren_cursor(r->s, r->pos[MAX(0, off-1)]);
	if (off > 0) {
		s64 two = off > 1 && psn != pre;
		dir = r->pos[off-two] - r->pos[off-(two+1)];
		if (labs(dir) > r->wid[off-(two+1)])
			pos = ren_cursor(r->s, r->pos[off-two]);
		pos += dir < 0 ? -1 : 1;
	}
	syn_scdir(0);
	if (led_lw) {
		s64 w = ren_wrapw(lncol), k, trow, b;
		opt_left_col = 0;
		led_rowh = MAX(MAX(0, r->cmax), pos) / w + 1;
		k = led_row + pos / w - term_rows + 1;
		if (k > 0) {			/* scroll to keep the cursor visible */
			term_pos(0, 0);
			term_room(-k);
			led_row -= k;
		} else if (led_row + pos / w < 0) {
			k = -(led_row + pos / w);
			term_pos(0, 0);
			term_room(k);
			led_row += k;
		}
		if (ts_preview)
			led_preview_draw(ps);
		for (k = MAX(0, -led_row); k < led_rowh && led_row + k < term_rows; k++) {
			if (k && lncol) {
				term_pos(led_row + k, 0);
				term_kill();
			}
			if (ts_preview)
				led_srender(r->s, led_row + k, lncol, k * w, k * w + w,
					ts_preview, ts_preview_row(ps), 0)
			else
				led_crender(r->s, led_row + k, lncol, k * w, k * w + w);
		}
		/* the rows below shift with the edited line */
		preserve(ren_state*, rstate, rstate = rstates;)
		for (b = led_nextb, trow = led_row + led_rowh; !ts_preview && trow < term_rows; b++)
			trow += vi_drawline(b, trow);
		restore(rstate)
		term_pos(led_row + pos / w, lncol + pos % w);
		sbufn_cut(sb, psn)
		rstate -= 2;
		return;
	}
	if (pos >= opt_left_col + term_cols || pos < opt_left_col)
		opt_left_col = pos < term_cols ? 0 : pos - term_cols / 2;
	if (ts_preview && poff == &cursor_off)
		led_preview_current(r->s, ps, lncol);
	else
		led_crender(r->s, -1, lncol, opt_left_col, opt_left_col + term_cols - lncol);
	term_pos(-1, led_pos(r->s, pos) + lncol);
	sbufn_cut(sb, psn)
	rstate -= 2;
}

/**
 * @brief Read a character starting with key c: handles ^f/^e keymap switch,
 * ^v literal, ^k digraph and utf-8 sequences.
 * @return static buffer with the (mapped) character, NULL on interrupt
 */
char *led_read(s64 *kmap, s64 c)
{
	static char buf[5];
	s64 c1, c2, i, n;
	while (!TK_INT(c)) {
		switch (c) {
		case TK_CTL('f'):
			*kmap = keymap_alt;
			break;
		case TK_CTL('e'):
			*kmap = 0;
			break;
		case TK_CTL('v'):	/* literal character */
			buf[0] = term_read(0);
			buf[1] = '\0';
			return buf;
		case TK_CTL('k'):	/* digraph */
			c1 = term_read(0);
			if (TK_INT(c1))
				return NULL;
			c2 = term_read(0);
			if (TK_INT(c2))
				return NULL;
			return conf_digraph(c1, c2);
		default:
			if ((c & 0xc0) == 0xc0) {	/* utf-8 character */
				buf[0] = c;
				n = uc_len(buf);
				for (i = 1; i < n; i++)
					buf[i] = term_read(0);
				buf[n] = '\0';
				return buf;
			}
			return kmap_map(*kmap, c);
		}
		c = term_read(0);
	}
	return NULL;
}

/* show buf highlighted after the input until the next key; pn is the number
 * of chars after the cursor (led_info() passes postn, led_infoc() also
 * counts buf, leaving the cursor in front of it) */
#define _led_info(buf, pn) \
{ \
	s64 ola[3]; \
	led_ext *la; \
	ola[0] = *poff; \
	ola[1] = uc_slen(buf); \
	ola[2] = WH1 | SYN_BD | SYN_OWR; \
	la = led_extnew(); \
	la->usr = ola; \
	la->blen = sizeof(ola); \
	sbuf_str(sb, buf) \
	led_printparts(sb, pre, ps, *post, pn, poff); \
	sbuf_cut(sb, len) \
	led_extdel(la); \
	c = term_read(TK_CTL('l')); \
	led_printparts(sb, pre, ps, *post, postn, poff); \
	goto noredraw; \
} \

/* redraw the buffer rows above the edited line */
static void led_redrawlw(s64 ctop, s64 crow)
{
	if (ts_preview) {
		led_preview_draw(led_preview_ps);
		return;
	}
	preserve(ren_state*, rstate, rstate = rstates;)
	for (s64 i = ctop, trow = vi_srow(ctop); trow < term_rows && i < crow; i++)
		trow += vi_drawline(i, trow);
	restore(rstate)
}

#define led_info(buf) _led_info(buf, postn)
/* like led_info(), except the cursor is left where it was, in front of buf */
#define led_infoc(buf) _led_info(buf, postn + uc_slen(buf))

/**
 * @brief Redraw screen rows from r down: rows for orow..crow come from the
 * lines typed so far (cs), the rest from the buffer. While a tree-sitter
 * preview is active the preview is redrawn instead (led_preview_draw()).
 */
static void led_redraw(char *cs, s64 r, s64 orow, s64 crow, s64 ctop, s64 flg)
{
	if (ts_preview) {
		led_preview_draw(led_preview_ps);
		return;
	}
	rstate++;
	for (s64 nl = 0; r < term_rows; r++) {
		if (lnum_width) {
			term_pos(r, 0);
			term_kill();
		}
		if (r >= orow-ctop && r < crow-ctop) {
			sbuf_smake(cb, 128)
			nl = dstrlen(cs, '\n');
			sbuf_mem(cb, cs, nl+!!cs[nl])
			sbuf_nul4(cb)
			rstate->s = NULL;
			led_crender(cb->s, r, lnum_width, opt_left_col, opt_left_col + term_cols - lnum_width)
			free(cb->s);
			rstate->s = NULL;
			cs += nl+!!cs[nl];
			continue;
		}
		nl = r < crow-ctop ? r+ctop : (r-(crow-orow+!!(flg & 4)))+ctop;
		led_crender(lbuf_get(xb, nl) ? lbuf_get(xb, nl) : "~", r,
			lnum_width, opt_left_col, opt_left_col + term_cols - lnum_width)
	}
	term_pos(crow - ctop, 0);
	rstate--;
}

/** @brief Switch between vi and ex (Q, ^o) in a nested loop, then restore state. */
void led_modeswap(void)
{
	preserve(struct ts_state*, ts_preview, ts_preview = NULL;)
	preserve(s64, quit_state, quit_state = 0;)
	preserve(s64, term_exec_type, term_exec_type = 0;)
	preserve(s64, opt_startup_flags, opt_startup_flags ^= 2;)
	preserve(s64, ex_exec_depth, ex_exec_depth = 0;)
	if (opt_startup_flags & 2)
		ex();
	else {
		syn_setft(xb_ft);
		vi(1);
	}
	if (quit_state > 0 || (quit_state < -256 && quit_state >= -512))
		restore(quit_state)
	else if (quit_state < -512)
		quit_state += 256;
	restore(term_exec_type)
	restore(opt_startup_flags)
	restore(ex_exec_depth)
	restore(ts_preview)
}

/**
 * @brief Key loop for one line of input.
 * @param sb      input; sb->s[0..pre) cannot be erased, ps is where this line starts
 * @param post    text after the cursor (postn chars); *postref owns a copy if made
 * @param ai_max  autoindent limit; < 0 for prompts
 * @param poff    receives the cursor character offset
 * @param flg     bit 2: return after one key; LED_AGENT: ^o and ^l are
 *                returned to the caller (the agent conversation prompt)
 * @return the key that ended input ('\n', an interrupt, an unhandled edit
 *         key, or ^o/^l with LED_AGENT)
 * With :et, ^t/^d/tab insert or remove xsw spaces instead of a tab; with :tc,
 * tab (or ^_) completes paths in ':' prompts.
 */
static s64 led_line(sbuf *sb, s64 pre, s64 ps, char **post, s64 postn, char **postref,
	s64 ai_max, s64 *poff, s64 *kmap, ins_state *is, s64 orow, s64 crow, s64 ctop, s64 flg)
{
	char *cs;
	s64 len, c, i;
	sbuf *reg;
	do {
		led_printparts(sb, pre, ps, *post, postn, poff);
		len = sb->s_n;
		c = term_read(TK_CTL('l'));
		noredraw:
		if ((flg & LED_AGENT) && (c == TK_CTL('o') || c == TK_CTL('l')))
			return c;
		switch (c) {
		case TK_CTL('h'):
			c = 127;
		case 127:
			if (len - pre > 0)
				sbuf_cut(sb, led_lastchar(sb->s + pre) + pre)
			else
				return c;
			break;
		case TK_CTL('u'):
			sbuf_cut(sb, is->sug_pt > pre && len > is->sug_pt ? is->sug_pt : pre)
			break;
		case TK_CTL('w'):
			if (len - pre > 0)
				sbuf_cut(sb, led_lastword(sb->s + pre) + pre)
			else if (ai_max >= 0)
				return c;
			break;
		case TK_CTL('t'):
			cs = sdup(sb->s + ps, sb->s_n - ps);
			sbuf_cut(sb, ps)
			if (xet)
				for (s64 _k = 0; _k < xsw; _k++)
					sbuf_chr(sb, ' ')
			else
				sbuf_chr(sb, '\t')
			sbuf_str(sb, cs)
			free(cs);
			pre += xet ? xsw : 1;
			break;
		case TK_CTL('d'):
			if (xet) {
				s64 _k;
				for (_k = 0; _k < xsw && sb->s[ps + _k] == ' '; _k++);
				if (_k) {
					memmove(&sb->s[ps], &sb->s[ps + _k], len - ps - _k);
					sb->s_n -= _k;
					pre = MAX(ps, pre - _k);
				}
			} else if (sb->s[ps] == ' ' || sb->s[ps] == '\t') {
				memmove(&sb->s[ps], &sb->s[ps+1], len - ps - 1);
				sb->s_n--;
				pre -= pre > ps;
			}
			break;
		case TK_CTL(']'):
		case TK_CTL('\\'):
			if (c == TK_CTL(']')) {
				if (is->p_reg < '/' || is->p_reg >= '9')
					is->p_reg = '/';
				while (is->p_reg < '9' && !ex_regget(++is->p_reg));
			} else {
				c = term_read(0);
				is->p_reg = c == TK_CTL('\\') ? 0 : c;
			}
			if (ex_regget(is->p_reg))
				led_info(ex_regget(is->p_reg)->s)
			continue;
		case TK_CTL('p'):
			if ((reg = ex_regget(is->p_reg)))
				sbuf_mem(sb, reg->s, reg->s_n)
			break;
		case TK_CTL('g'):
			if (!suggestsb) {
				sbuf_make(suggestsb, 1)
				sbuf_make(acsb, 1024)
				sbufn_chr(acsb, '\n')
			}
			file_index(xb);
			break;
		case TK_CTL('y'):
			led_done();
			suggestsb = NULL;
			break;
		case TK_CTL('r'):
			if (!suggestsb || !suggestsb->s_n)
				continue;
			if (!is->sug)
				is->sug = suggestsb->s;
			if (suggestsb->s_n == is->sug - suggestsb->s)
				is->sug--;
			for (i = 0; is->sug != suggestsb->s; is->sug--) {
				if (!*is->sug) {
					i++;
					if (i == 3) {
						is->sug++;
						goto redo_suggest;
					} else
						*is->sug = '\n';
				}
			}
			goto redo_suggest;
		case TK_CTL('z'):
			term_suspend();
			if (ai_max < 0)
				continue;
			if (led_lw)
				led_redrawlw(ctop, crow);
			else
				led_redraw(sb->s, 0, orow, crow, ctop, flg);
			continue;
		case TK_CTL('x'):
			is->sug_pt = is->sug_pt == len ? -1 : len;
			char buf[100];
			itoa(is->sug_pt, buf);
			led_info(buf)
		case TK_CTL('n'):
			if (!suggestsb)
				continue;
			is->lsug = is->sug_pt >= 0 ? is->sug_pt : led_lastword(sb->s + pre) + pre;
			if (is->_sug) {
				if (suggestsb->s_n == is->sug - suggestsb->s)
					continue;
				redo_suggest:
				if (!(is->_sug = strchr(is->sug, '\n'))) {
					is->sug = suggestsb->s;
					goto lookup;
				}
				suggest:
				*is->_sug = '\0';
				sbuf_cut(sb, is->lsug)
				sbuf_str(sb, is->sug)
				is->sug = is->_sug+1;
				continue;
			}
			lookup:
			if (search(sb->s + is->lsug, len - is->lsug)) {
				is->sug = suggestsb->s;
				if (!(is->_sug = strchr(is->sug, '\n')))
					continue;
				goto suggest;
			}
			continue;
		case TK_CTL('b'):
			if (ai_max >= 0) {
				pac:;
				if (led_lw)	/* the block has no wrapped form */
					continue;
				sbuf_nul(sb)
				s64 r = crow-ctop+1;
				if (is->sug)
					goto pac_;
				i = is->sug_pt >= 0 ? is->sug_pt : led_lastword(sb->s + pre) + pre;
				if (suggestsb && search(sb->s + i, sb->s_n - i)) {
					is->sug = suggestsb->s;
					pac_:;
					preserve(s64, opt_text_dir, opt_text_dir = 2;)
					preserve(s64, ftidx,)
					syn_setft(ac_ft);
					for (s64 left = 0; r < term_rows; r++) {
						RST(2, led_crender(is->sug, r, 0, left, left+term_cols))
						left += term_cols;
						if (left >= rstates[2].pos[rstates[2].n])
							break;
					}
					restore(opt_text_dir)
					restore(ftidx)
					r++;
				}
				led_redraw(sb->s, r, orow, crow, ctop, flg);
				continue;
			}
			temp_pos(0, -1, 0, 0);
			temp_write(0, sb->s + pre);
			preserve(struct buf*, cur_buf,)
			s64 bidx = istempbuf(cur_buf) ? -1 : cur_buf - bufs;
			s64 pidx = prev_buf - bufs;
			preserve(s64, term_exec_type, term_exec_type = 0;)
			preserve(s64, quit_state, quit_state = 0;)
			preserve(s64, ftidx,)
			preserve(s64, ex_exec_depth, if (flg & LED_AGENT) ex_exec_depth = 0;)
			preserve(s64, opt_startup_flags, if (flg & LED_AGENT) opt_startup_flags &= ~2;)
			preserve(s64, opt_multiline_prompt, if (flg & LED_AGENT) opt_multiline_prompt = 0;)
			preserve(s64, print_newline, if (flg & LED_AGENT) print_newline = 0;)
			preserve(struct ts_state*, ts_preview, ts_preview = NULL;)
			temp_switch(0, 0);
			vi(1);
			restore(print_newline)
			restore(opt_multiline_prompt)
			restore(opt_startup_flags)
			restore(ex_exec_depth)
			exbuf_save(cur_buf)
			restore(term_exec_type)
			prev_buf = pidx >= buf_count ? bufs : bufs + pidx;
			if (bidx >= 0)
				cur_buf = bidx >= buf_count ? bufs : bufs + bidx;
			else
				restore(cur_buf)
			exbuf_load(cur_buf)
			syn_setft(xb_ft);
			if (!(flg & LED_AGENT))
				vi(1); /* redraw past screen */
			restore(ftidx)
			restore(ts_preview)
			term_pos(term_rows, 0);
			if (quit_state > 0 || (quit_state < -256 && quit_state >= -512))
				restore(quit_state)
			else if (quit_state < -512)
				quit_state += 256;
			is->t_row = tempbufs[0].row;
		case TK_CTL('a'):
			is->t_row = is->t_row < -1 ? tempbufs[0].row : is->t_row;
			is->t_row += lbuf_len(tempbufs[0].lb);
			is->t_row = is->t_row % MAX(1, lbuf_len(tempbufs[0].lb));
			if ((cs = lbuf_get(tempbufs[0].lb, is->t_row--))) {
				sbuf_cut(sb, pre)
				sbuf_str(sb, cs)
				sb->s_n--;
			}
			break;
		case TK_CTL('l'):
			i = term_winch;
			term_done();
			term_init();
			if (ai_max >= 0 && led_lw)
				led_redrawlw(ctop, crow);
			else if (ai_max >= 0)
				led_redraw(sb->s, 0, orow, crow, ctop, flg);
			else if (!i)
				term_clean();
			continue;
		case TK_CTL('o'): {
			if (!*postref)
				*postref = *post = sdup(*post, strlen(*post));
			preserve(struct buf*, cur_buf,)
			s64 bidx = istempbuf(cur_buf) ? -1 : cur_buf - bufs;
			preserve(s64, ftidx,)
			led_modeswap();
			restore(ftidx)
			if (bidx < 0) {
				if (cur_buf == tmpcur_buf)
					continue;
				restore(cur_buf)
				exbuf_load(cur_buf)
			} else if (bidx != cur_buf - bufs && bidx < buf_count) {
				cur_buf = bufs + bidx;
				exbuf_load(cur_buf)
			}
			continue; }
		case TK_CTL('_'):       /* list the matches on their own screen */
		case '\t':
			if (flg & 2)    /* the caller steps the search match */
				break;
			if (xtc && ai_max < 0 && sb->s[ps] == ':') {
				s64 typed, full = c == TK_CTL('_') || xtc > 1;
				if (!compsb)
					sbuf_make(compsb, 128)
				sbuf_cut(compsb, 0)
				i = led_pathcomp(sb, pre, compsb, &typed);
				if (i > 1 && sb->s_n == len) {
					s64 page = 0;
					if (!full)
						led_infoc(compsb->s + typed)
					do {
						page = led_complist(compsb, page);
						term_pos(0, 0);
						led_printparts(sb, pre, ps,
							*post, postn, poff);
						c = term_read(TK_CTL('l'));
					} while (page && (c == '\t' || c == TK_CTL('_')));
					term_clean();
					goto noredraw;
				}
				if (i)
					break;
			}
			if (c == TK_CTL('_'))
				continue;
		default:
			if (c == '\n' || TK_INT(c))
				return c;
			if (c == '\t' && xet) {
				for (s64 _l = 0; _l < xsw; _l++)
					sbuf_chr(sb, ' ')
			} else if ((cs = led_read(kmap, c)))
				sbuf_str(sb, cs)
		}
		is->sug = NULL;
		is->_sug = NULL;
		if (ai_max >= 0 && opt_print_autocomplete)
			goto pac;
	} while (!(flg & 2));
	return c;
}

/**
 * @brief Read a prompt line into sb after its current contents.
 * @param flg  bit 1: save to history (tempbufs[0]); bit 2 and LED_AGENT: see led_line()
 * @return the key that ended input
 */
s64 led_prompt(sbuf *sb, char *insert, s64 *kmap, ins_state *is, s64 ps, s64 flg)
{
	s64 n, key, off;
	char *post = "", *postref = post;
	ins_state _is;
	if (flg & 2) {
		n = ps;
		ps = 0;
	} else
		n = sb->s_n;
	if (insert)
		sbuf_str(sb, insert)
	if (!is) {
		ins_init(_is)
		is = &_is;
	}
	preserve(s64, opt_left_col, opt_left_col = 0;)
	preserve(s64, opt_text_dir, opt_text_dir = 2;)
	preserve(s64, led_row, led_row = -1;)
	preserve(s64, led_lw, led_lw = 0;)
	key = led_line(sb, n, ps, &post, 0, &postref, -1,
			&off, kmap, is, 0, cursor_row, view_top_row, flg);
	restore(led_lw)
	restore(led_row)
	restore(opt_text_dir)
	restore(opt_left_col)
	if (key == '\n' && flg & 1) {
		lbuf_dedup(tempbufs[0].lb, sb->s + n, sb->s_n - n)
		temp_pos(0, -1, 0, 0);
		temp_write(0, sb->s + n);
	}
	return key;
}

/**
 * @brief Insert-mode input, possibly over several lines, with autoindent.
 * @param post  text after the insertion point (postn chars)
 * @param[out] pren  byte length of sb before post
 * @param source_beg,source_end  buffer rows [beg, end) being edited, shown
 *                    by the tree-sitter preview (ts_preview_begin())
 * @return the key that ended input
 */
s64 led_input(sbuf *sb, char *post, s64 postn, s64 row, s64 flg, s64 *pren,
	s64 source_beg, s64 source_end)
{
	preserve(struct ts_state*, ts_preview,
		ts_preview = ts_preview_begin(xb, source_beg, source_end);)
	s64 ai_max = 128 * opt_autoindent;
	s64 n, key, ps = 0, crow = cursor_row, ctop = view_top_row;
	char *postref = NULL;
	ins_state is;
	led_nextb = row + 1 - !!(flg & 4);
	led_lw = xlw && led_row >= 0;
	while (1) {
		ins_init(is)
		key = led_line(sb, sb->s_n, ps, &post, postn, &postref,
			ai_max, &cursor_off, &cur_keymap, &is, row, crow, ctop, flg);
		if (key != '\n') {
			*pren = sb->s_n;
			if (!opt_line_editor) {
				cursor_off = uc_slen(sb->s+ps);
				sbufn_str(sb, post)
			} else
				sb->s[*pren] = *post;
			free(postref);
			cursor_row = crow;
			ts_free(ts_preview);
			restore(ts_preview)
			led_row = -1;
			led_lw = 0;
			return key;
		}
		sbuf_chr(sb, key)
		led_printparts(sb, -1, ps, "", 0, &cursor_off);
		if (led_lw) {
			led_row += led_rowh;
			if (led_row >= term_rows) {	/* scroll the finished lines up */
				n = led_row - term_rows + 1;
				term_pos(0, 0);
				term_room(-n);
				led_row -= n;
			}
		} else {
			term_chr('\n');
			term_room(1);
		}
		crow++;
		n = ps;
		ps = sb->s_n;
		if (ai_max > 0) {	/* updating autoindent */
			for (; *post == ' ' || *post == '\t'; postn--)
				++post;
			s64 ai_new = n;
			while (sb->s[ai_new] == ' ' || sb->s[ai_new] == '\t')
				ai_new++;
			ai_new = ai_max > ai_new - n ? ai_new - n : ai_max;
			sbuf_mem(sb, sb->s+n, ai_new)
		}
	}
}

/** @brief Free the autocomplete and path-completion buffers. */
void led_done(void)
{
	if (suggestsb) {
		sbuf_free(suggestsb)
		sbuf_free(acsb)
	}
	if (compsb) {
		sbuf_free(compsb)
		compsb = NULL;
	}
}
