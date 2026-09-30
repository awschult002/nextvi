/**
 * @file ren.c
 * @brief Rendering: maps a line's characters to screen columns (tabs, wide
 * chars, bidi reordering), and syntax highlighting by filetype.
 */
static rset *dir_rslr;	///< pattern of marks for left-to-right strings
static rset *dir_rsrl;	///< pattern of marks for right-to-left strings
static rset *dir_rsctx;	///< direction context patterns (dctxs[])

/** @brief Reverse ord[beg, end). */
static void dir_reverse(int *ord, int beg, int end)
{
	end--;
	while (beg < end) {
		int tmp = ord[beg];
		ord[beg] = ord[end];
		ord[end] = tmp;
		beg++;
		end--;
	}
}

/**
 * @brief Reorder the characters based on direction marks and characters.
 * Fills ord[] (visual position -> char index), reversing each matched group
 * whose dmarks[] dir is negative.
 * @param end  number of chars; ord[] is set to identity on the first match
 * @return nonzero if anything matched (ord[] is valid)
 */
static int dir_reorder(char *s, char *se, int *ord, int end, int dir)
{
	rset *rs = dir < 0 ? dir_rsrl : dir_rslr;
	int beg = 0, off, c_beg, c_end;
	int subs[LEN(dmarks[0].dir) * 2], found, i;
	int flg = se > s && se[-1] == '\n' ? REG_NEWLINE : 0;
	while (se > s && (found = rset_find(rs, s, subs, flg)) >= 0) {
		/* runs once: end becomes -1 after the first match */
		for (i = 0; i < end; i++)
			ord[i] = i;
		end = -1;
		c_end = 0;
		for (i = 0; i < rs->grpnsubc[found]; i += 2) {
			off = subs[i];
			if (off < 0 || dmarks[found].dir[i >> 1] >= 0)
				continue;
			c_beg = uc_off(s, off);
			c_end = c_beg + uc_off(s + off, subs[i + 1] - off);
			off = subs[i + 1];
			dir_reverse(ord, beg+c_beg, beg+c_end);
		}
		beg += c_end ? c_end : 1;
		s += c_end ? off : uc_len(s);
	}
	return end < 0;
}

/** @brief Direction context of the given line: +1 LTR, -1 RTL. */
int dir_context(char *s)
{
	int found;
	if (opt_text_dir > +1)
		return +1;
	if (opt_text_dir < -1)
		return -1;
	if (dir_rsctx && s)
		if ((found = rset_find(dir_rsctx, s, NULL, 0)) >= 0)
			return dctxs[found].dir;
	return opt_text_dir < 0 ? -1 : +1;
}

/** @brief Compile dmarks[] into the LTR/RTL mark sets and dctxs[] into dir_rsctx. */
void dir_init(void)
{
	char *relr[128];
	char *rerl[128];
	char *ctx[128];
	int i;
	for (i = 0; i < dmarkslen; i++) {
		relr[i] = dmarks[i].ctx >= 0 ? dmarks[i].pat : NULL;
		rerl[i] = dmarks[i].ctx <= 0 ? dmarks[i].pat : NULL;
	}
	dir_rslr = rset_make(i, relr, 0);
	dir_rsrl = rset_make(i, rerl, 0);
	for (i = 0; i < dctxlen; i++)
		ctx[i] = dctxs[i].pat;
	dir_rsctx = rset_make(i, ctx, 0);
}

/** @brief Screen width of the char at s drawn at column pos (tabs reach the next stop). */
static int ren_cwid(char *s, int pos)
{
	if (s[0] == '\t')
		return opt_tabstop ? opt_tabstop - (pos % opt_tabstop) : 0;
	if (s[0] == '\n')
		return 1;
	int c, l; uc_code(c, s, l)
	for (int i = 0; i < phlen; i++)
		if (c >= ph[i].cp[0] && c <= ph[i].cp[1] && l == ph[i].l)
			return ph[i].wid;
	return uc_wid(c);
}

/** Render caches: 0 = current line, 1 = all other lines,
2 = aux rendering (never lbuf backed by construction). */
ren_state rstates[3];
ren_state *rstate = rstates;	///< active cache, one of rstates[]

/**
 * @brief Specify the screen position of the characters in s; the result is
 * cached in *rstate, keyed by the pointer s.
 * On rstates[1] with opt_render_limit >= 0 only that many chars are kept:
 * the next char is overwritten with NULs (saved in nulhole) until
 * led_render() restores it.
 */
ren_state *ren_position(char *s)
{
	if (rstate->s == s)
		return rstate;
	else if (rstate->col) {
		free(rstate->col - 2);
		free(rstate->pos);
	}
	rstate->s = s;
	rstate->ctx = dir_context(s);
	unsigned int n, max, l;
	char *ss = s;
	if (opt_render_limit >= 0 && rstate == rstates+1) {
		max = (unsigned int)opt_render_limit;
		for (n = 0; n < max && (l = uc_len(ss)); n++)
			ss += l;
		rstate->holelen = uc_len(ss);
		memcpy(rstate->nulhole, ss, rstate->holelen);
		memset(ss, 0, rstate->holelen);
	} else
		for (n = 0; (l = uc_len(ss)); n++)
			ss += l;
	unsigned int b = n + 1, c = 2, i;
	int cpos = 0, wid, *col;
	/* one block of b = n + 1 entries each: pos[] screen column, off[] (the
	 * order while reordering, then widths: rstate->wid), chrs[] char pointers */
	int *pos = emalloc((b * 2 * sizeof(pos[0])) + b * sizeof(char*));
	int *off = &pos[b];
	char **chrs = (char**)&off[b];
	if (opt_reorder && dir_reorder(s, ss, off, n, rstate->ctx)) {
		for (i = 0; i < b; i++) {
			chrs[i] = s;
			s += uc_len(s);
		}
		int *wids = emalloc(n * sizeof(wids[0]));
		for (i = 0; i < n; i++) {
			wid = ren_cwid(chrs[off[i]], cpos);
			pos[off[i]] = cpos;
			wids[off[i]] = wid;
			cpos += wid;
		}
		pos[n] = cpos;
		/* col[c]: char index at screen column c; 2 leading slots (col[-2], col[-1]) hold n */
		col = emalloc((cpos + 2) * sizeof(col[0]));
		for (i = 0; i < n; i++) {
			wid = wids[off[i]];
			while (wid--)
				col[c++] = off[i];
		}
		memcpy(off, wids, n * sizeof(wids[0]));
		free(wids);
	} else {
		for (i = 0; i < n; i++) {
			chrs[i] = s;
			pos[i] = cpos;
			cpos += ren_cwid(s, cpos);
			s += uc_len(s);
		}
		chrs[n] = s;
		pos[n] = cpos;
		col = emalloc((cpos + 2) * sizeof(col[0]));
		for (i = 0; i < n; i++) {
			wid = pos[i+1] - pos[i];
			off[i] = wid;
			while (wid--)
				col[c++] = i;
		}
	}
	off[n] = 0;
	col[0] = n;
	col[1] = n;
	rstate->wid = off;
	rstate->cmax = cpos - 1;
	rstate->col = col + 2;
	rstate->pos = pos;
	rstate->chrs = chrs;
	rstate->n = n;
	return rstate;
}

/** @brief Convert character offset to visual position (screen column). */
int ren_pos(char *s, int off)
{
	ren_state *r = ren_position(s);
	return off < r->n ? r->pos[off] : 0;
}

/** @brief Convert visual position to character offset. */
int ren_off(char *s, int p)
{
	ren_state *r = ren_position(s);
	return r->col[p < r->cmax ? p : r->cmax];
}

/** @brief Adjust cursor position: last screen column of the char at column p, not past the text before '\n'. */
int ren_cursor(char *s, int p)
{
	if (!s)
		return 0;
	ren_state *r = ren_position(s);
	if (p >= r->cmax)
		p = r->cmax - (*r->chrs[r->col[r->cmax]] == '\n');
	int i = r->col[p];
	return r->pos[i] + r->wid[i] - 1;
}

/** @brief Clamp character offset o to the line, before its '\n' unless the line is empty. */
int ren_noeol(char *s, int o)
{
	if (!s)
		return 0;
	ren_state *r = ren_position(s);
	o = MAX(0, o >= r->n ? r->n - 1 : o);
	return o - (o > 0 && *r->chrs[o] == '\n');
}

/** @brief The visual position of the next character in dir from screen column p. */
int ren_next(char *s, int p, int dir)
{
	ren_state *r = ren_position(s);
	if (p+dir < 0 || p > r->cmax)
		return r->pos[r->col[r->cmax]];
	int i = r->col[p];
	if (r->wid[i] > 1 && dir > 0)
		return r->pos[i] + r->wid[i];
	return r->pos[i] + dir;
}

/**
 * @brief Replacement text to draw for the char at s (placeholder, combining
 * mark on a tatweel, replacement char, or shaped form); NULL draws it as is.
 * @param s   the character to draw
 * @param ln  start of the line, context for shaping
 */
char *ren_translate(char *s, char *ln)
{
	if (s[0] == '\t' || s[0] == '\n')
		return NULL;
	int c, l; uc_code(c, s, l)
	for (int i = 0; i < phlen; i++)
		if (c >= ph[i].cp[0] && c <= ph[i].cp[1] && l == ph[i].l)
			return ph[i].d;
	if (l == 1)
		return NULL;
	if (uc_acomb(c)) {
		static char buf[16] = "ـ";
		*((char*)memcpy(buf+2, s, l)+l) = '\0';
		return buf;
	}
	if (uc_isbell(c))
		return "�";
	return opt_shaping ? uc_shape(ln, s, c) : NULL;
}

/** Mapping filetypes to regular expression sets: one entry per (ft, set) group of hls[]. */
struct ftmap {
	int setbidx;	///< first hls[] index of this group
	int seteidx;	///< one past the last hls[] index
	char *ft;	///< filetype name (compared by pointer)
	rset *rs;	///< the group's patterns compiled into one set
};
static struct ftmap *ftmap;	///< built lazily by syn_setft()
static int ftmidx;	///< number of ftmap[] entries
static rset *syn_ftrs;	///< file name patterns of fts[]
static int blockatt, blockflg, blockdep;	///< active block highlight: its attribute, SYN_B* flags, and nesting depth (SYN_BN)
int ftidx;	///< ftmap[] index of the current filetype
int syn_scdirl;	///< last scroll direction given to syn_scdir(); 0 = full redraw
int syn_blockhl;	///< hls[] index of the active block highlight, -1 if none

/**
 * @brief Build ftmap[fti] from the hls[] entries at n with the same ft and set.
 * @return nonzero if another set of the same ft follows
 */
static int syn_initft(int fti, int n, char *name, int flg)
{
	if (fti >= ftmidx)
		ftmap = erealloc(ftmap, (fti + 1) * sizeof(*ftmap));
	int i = n, set = hls[i].set;
	char *pats[hlslen];
	for (; i < hlslen && hls[i].ft == name && hls[i].set == set; i++)
		pats[i - n] = hls[i].pat;
	ftmap[fti].setbidx = n;
	ftmap[fti].ft = name;
	ftmap[fti].rs = rset_make(i - n, pats, flg);
	ftmap[fti].seteidx = i;
	return i < hlslen && hls[i].ft == name && hls[i].set != set;
}

/**
 * @brief Make ft the current filetype (pointers are compared, not strings),
 * building its ftmap entries on first use. Optional patterns (hlopts) are
 * reset first.
 * @return the filetype, or NULL if unknown
 */
char *syn_setft(char *ft)
{
	int i;
	if (ftmidx)
		for (i = 0; i < hloptslen; i++)
			syn_addhl(NULL, hlopts[i]);
	for (i = 0; i < ftmidx; i++)
		if (ft == ftmap[i].ft) {
			ftidx = i;
			return ftmap[ftidx].ft;
		}
	for (i = 0; i < hlslen; i++)
		if (ft == hls[i].ft) {
			default_hl:
			ftidx = ftmidx;
			while (syn_initft(ftmidx, i, hls[i].ft, 0))
				i = ftmap[ftmidx++].seteidx;
			ftmidx++;
			return ftmap[ftidx].ft;
		}
	if (ftmidx)
		return NULL;
	i = 0;
	goto default_hl;
}

/** @brief Record the scroll direction; resets block highlight state unless it continues the same way. */
void syn_scdir(int scdir)
{
	if (!scdir || abs(scdir) > xrows || (syn_scdirl > 0) != (scdir > 0)) {
		syn_scdirl = scdir;
		syn_blockhl = -1;
		blockdep = 0;
	}
}

/** @brief Merge attribute new over old (SYN_OWR in new replaces old entirely). */
int syn_merge(int old, int new)
{
	if (new & SYN_OWR)
		return new & ~SYN_OWR;
	int fg = SYN_FGSET(new) ? SYN_FG(new) : SYN_FG(old);
	int bg = SYN_BGSET(new) ? SYN_BG(new) : SYN_BG(old);
	int flg = ((old | new) & SYN_FLG) | (new & SYN_MK);
	return flg | (bg << 8) | fg;
}

/** @brief Whether *att (or the pending block attribute) has a's color bits. */
static int syn_tatt(int *att, int a, int pb)
{
	if (SYN_SET(BATT, a) && pb && (!*att || !SYN_SET(BP, blockflg)))
		att = &blockatt;
	return (*att & 0xffff) == (a & 0xffff);
}

/**
 * @brief Compute attributes att[0, n) for line s with the current filetype.
 * Each hls[].att entry is one int per group, followed by extra ints if its
 * flags ask: SYN_ATT/SYN_OATT a count and that many attributes, SYN_BLK one
 * word of SYN_B* block flags.
 */
void syn_highlight(int *att, char *s, int n)
{
	int fti = ftidx, blockhl = syn_blockhl, blockcont = -1;
	re:;
	rset *rs = ftmap[fti].rs;
	int subs[rs->nsubc], *catt, *iatt, sl, c;
	int cend, sidx = 0, flg = 0, hl, j, i, ii;
	while ((sl = rset_find(rs, s + sidx, subs, flg)) >= 0) {
		cend = uc_len(s + sidx);
		/* hl: matching hls[] index; sl becomes its group count, catt its att list */
		hl = sl + ftmap[fti].setbidx;
		sl = rs->grpnsubc[sl];
		catt = hls[hl].att;
		for (i = 0, ii = i; ii < sl; ii += 2) {
			int inc = 1;
			if (subs[ii] < 0 || SYN_SET(IGN, catt[i])) {
				skip:
				if (SYN_SET(ATT, catt[i]))
					inc += catt[i + 1] + 1;
				if (SYN_SET(OATT, catt[i]))
					inc += catt[i + inc] + 1;
				if (SYN_SET(BLK, catt[i]))
					inc++;
				i += inc;
				continue;
			}
			cend = MAX(cend, subs[ii + 1]);
			if (SYN_SET(SKIP, catt[i]))
				goto skip;
			/* [beg, end): matched group as character offsets into att[] */
			int beg = uc_off(s, sidx + subs[ii]);
			int end = beg + uc_off(s + sidx + subs[ii], subs[ii + 1] - subs[ii]);
			if (SYN_SET(ATT, catt[i])) {
				int pb = blockhl >= 0 && syn_blockhl >= 0;
				iatt = &catt[i + 1];
				c = *iatt;
				inc += c + 1;
				if (SYN_SET(ATT, catt[i]) == SYN_ATT) {
					for (j = beg; c && j < end; j++)
						for (c = *iatt; c && !syn_tatt(att + j, iatt[c], pb); c--);
				} else if (SYN_SET(SATT, catt[i]))
					for (; c && !syn_tatt(att + beg, iatt[c], pb); c--);
				else if (SYN_SET(EATT, catt[i]))
					for (; c && !syn_tatt(att + MAX(0, end-1), iatt[c], pb); c--);
				if (!c)
					break;
			}
			if (SYN_SET(OATT, catt[i])) {
				int pb = blockhl >= 0 && syn_blockhl >= 0;
				iatt = &catt[i + inc];
				inc += *iatt + 1;
				for (j = beg; j < end; j++) {
					for (c = *iatt; c && !syn_tatt(att + j, iatt[c], pb); c--);
					if (c)
						att[j] = syn_merge(att[j], catt[i]);
				}
			} else
				for (j = beg; j < end; j++)
					att[j] = syn_merge(att[j], catt[i]);
			if (SYN_SET(BLK, catt[i])) {
				iatt = &catt[i + inc];
				inc++;
				j = SYN_SET(BSDP, *iatt) || !!SYN_SET(BSD, *iatt) == (syn_scdirl > 0);
				c = SYN_SET(BEDP, *iatt) || !!SYN_SET(BED, *iatt) == (syn_scdirl > 0);
				if (syn_blockhl == hl && SYN_SET(BN, *iatt) && j) {
					blockdep++;
				} else if (syn_blockhl == hl && SYN_SET(BE, *iatt) && c) {
					if (blockdep) {
						blockdep--;
					} else {
						blockcont = -1;
						syn_blockhl = blockcont;
					}
				} else if (syn_blockhl < 0 && SYN_SET(BS, *iatt)) {
					if (j && blockcont <= 0) {
						blockflg = *iatt;
						blockatt = catt[0];
						syn_blockhl = hl;
					}
					blockcont = hl;
				}
			}
			i += inc;
		}
		sidx += cend;
		flg = REG_NOTBOL;
	}
	fti++;
	if (ftmidx > fti && ftmap[fti-1].ft == ftmap[fti].ft)
		goto re;
	if (syn_blockhl < 0 || blockhl < 0)
		return;
	for (j = 0; j < n; j++)
		if (!att[j] || !SYN_SET(BP, blockflg))
			att[j] = blockatt;
}

/** @brief Filetype for a file name (hls[0].ft if none matches). */
char *syn_filetype(char *path)
{
	int hl = rset_find(syn_ftrs, path, NULL, 0);
	return hl >= 0 && hl < ftslen ? fts[hl].ft : hls[0].ft;
}

/** @brief Recompile the pattern group containing hls[hl] (after its pattern changed). */
void syn_reloadft(int hl, int flg)
{
	if (hl >= 0) {
		int fti = ftidx;
		while (fti < ftmidx - 1 && hl >= ftmap[fti].seteidx)
			fti++;
		rset *rs = ftmap[fti].rs;
		syn_initft(fti, ftmap[fti].setbidx, ftmap[fti].ft, flg);
		if (!ftmap[fti].rs)
			ftmap[fti].rs = rs;
		else
			rset_free(rs);
	}
}

/** @brief hls[] index of the pattern with id in the current filetype, -1 if none. */
int syn_findhl(int id)
{
	int i = ftmap[ftidx].setbidx;
	char *name = ftmap[ftidx].ft;
	for (; i < hlslen && hls[i].ft == name; i++)
		if (hls[i].id == id)
			return i;
	return -1;
}

/** @brief Set the pattern with id (NULL disables it); returns its hls[] index or -1. */
int syn_addhl(char *reg, int id)
{
	int ret = syn_findhl(id);
	if (ret >= 0)
		hls[ret].pat = reg;
	return ret;
}

/** @brief Compile the file name patterns of fts[]. */
void syn_init(void)
{
	char *pats[ftslen];
	int i = 0;
	for (; i < ftslen; i++)
		pats[i] = fts[i].pat;
	syn_ftrs = rset_make(i, pats, 0);
}
