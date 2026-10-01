/**
 * @file lbuf.c
 * @brief Line buffer: stores the file as an array of lines, with marks and
 * undo/redo history, plus the motions that walk it (words, pairs, search).
 *
 * Each line is one allocation: [struct linfo][text]['\n'][4 NULs]. Lines are
 * handled as pointers to the text; lbuf_s()/lbuf_i() step back to the linfo
 * header. Positions are (row, off) with off a character index.
 */

/** @brief Allocate an empty line buffer. */
struct lbuf *lbuf_make(void)
{
	struct lbuf *lb = emalloc(sizeof(*lb));
	memset(lb, 0, sizeof(*lb));
	lb->mark_sb[0] = -1;
	lb->mark_se[0] = -1;
	return lb;
}

/** @brief Free a line, invalidating render caches (rstates[0..1]) that point at it. */
static void lbuf_rfree(char *ln)
{
	for (s64 i = 0; i < 2; i++)
		if (rstates[i].s == ln)
			rstates[i].s = NULL;
	free(lbuf_s(ln));
}

/**
 * @brief Free an undo record. Its lines are freed only if they are not in the
 * buffer: ref bit 2 means ins[] is live, bit 1 means del[] is live.
 */
static void lopt_done(struct lopt *lo)
{
	free(lo->mark);
	if (!(lo->ref & 2))
		for (s64 i = 0; i < lo->n_ins; i++)
			lbuf_rfree(lo->ins[i]);
	free(lo->ins);
	if (!(lo->ref & 1))
		for (s64 i = 0; i < lo->n_del; i++)
			lbuf_rfree(lo->del[i]);
	free(lo->del);
}

/* copy a (row, off) mark pair */
#define lbuf_copymark(dst, src) { dst[0] = src[0]; dst[1] = src[1]; }

/** @brief Find a mark id in an {id, row, off} triplet array, returning its row & off pair. */
static s64 *mark_find(s64 *mark, s64 n, s64 id)
{
	for (s64 i = 0; i < n * 3; i += 3)
		if (mark[i] == id)
			return mark + i + 1;
	return NULL;
}

/** @brief Set mark id, appending a triplet if it is new. */
static void mark_set(s64 **mark, s64 *n, s64 id, s64 pos, s64 off)
{
	s64 *m = mark_find(*mark, *n, id);
	if (!m) {
		*mark = erealloc(*mark, (*n + 1) * 3 * sizeof(s64));
		m = *mark + *n * 3;
		*m++ = id;
		(*n)++;
	}
	m[0] = pos;
	m[1] = off;
}

/** @brief Set mark mk; ' is an alias of `, [ and ] have their own slots. */
void lbuf_mark(struct lbuf *lb, s64 mk, s64 pos, s64 off)
{
	if (mk == '\'')
		mk = '`';
	if (mk == '[') {
		lb->mark_sb[0] = pos;
		lb->mark_sb[1] = off;
	} else if (mk == ']') {
		lb->mark_se[0] = pos;
		lb->mark_se[1] = off;
	} else
		mark_set(&lb->mark, &lb->mark_n, mk, pos, off);
}

/** @brief Get the position of mark mk; returns 1 if it is unset. */
s64 lbuf_jump(struct lbuf *lb, s64 mk, s64 *pos, s64 *off)
{
	s64 *m;
	if (mk == '\'')
		mk = '`';
	if (mk == '[')
		m = lb->mark_sb;
	else if (mk == ']')
		m = lb->mark_se;
	else
		m = mark_find(lb->mark, lb->mark_n, mk);
	if (!m || m[0] < 0)
		return 1;
	*pos = m[0];
	*off = MAX(0, m[1]);
	return 0;
}

/** @brief Free the buffer, its lines and its history. */
void lbuf_free(struct lbuf *lb)
{
	ts_forget(lb);
	s64 i;
	for (i = 0; i < lb->ln_n; i++)
		lbuf_rfree(lb->ln[i]);
	for (i = 0; i < lb->hist_n; i++)
		lopt_done(&lb->hist[i]);
	free(lb->hist);
	free(lb->mark);
	free(lb->ln);
	free(lb);
}

/** @brief Bytes of the first line of s, including its '\n'. */
static s64 linelength(char *s)
{
	s64 len = dstrlen(s, '\n');
	return s[len] == '\n' ? len + 1 : len;
}

/**
 * @brief Low-level line replacement: replace n_del rows at lo->pos with n_ins rows.
 * @param s   text to split into new lines (appended to sb as pointers),
 *            or NULL if sb->s already holds n_ins line pointers (undo/redo)
 * @return the number of inserted lines
 */
static s64 lbuf_replace(struct lbuf *lb, sbuf *sb, char *s, struct lopt *lo, s64 n_del, s64 n_ins)
{
	s64 i, pos = lo->pos;
	if (s) {
		for (; *s; n_ins++) {
			s64 l = linelength(s);
			s64 l_nonl = l - (s[l - !!l] == '\n');
			/* header + text + '\n' + 4 NUL pad; ln points just after the header */
			struct linfo *n = emalloc(l_nonl + 5 + sizeof(struct linfo));
			n->len = l_nonl;
			n->grec = 0;
			char *ln = (char*)(n + 1);
			memcpy(ln, s, l_nonl);
			memset(&ln[l_nonl + 1], 0, 4);	/* fault tolerance pad */
			ln[l_nonl] = '\n';
			sbuf_mem(sb, &ln, sizeof(s))
			s += l;
		}
	}
	ts_edit(lb, pos, n_del, (char**)sb->s, n_ins);
	if (lb->ln_n + n_ins - n_del >= lb->ln_sz) {
		s64 nsz = lb->ln_n + n_ins - n_del + 512;
		char **nln = emalloc(nsz * sizeof(lb->ln[0]));
		memcpy(nln, lb->ln, lb->ln_n * sizeof(lb->ln[0]));
		free(lb->ln);
		lb->ln = nln;
		lb->ln_sz = nsz;
	}
	if (n_ins != n_del) {
		memmove(lb->ln + pos + n_ins, lb->ln + pos + n_del,
			(lb->ln_n - pos - n_del) * sizeof(lb->ln[0]));
	}
	lb->ln_n += n_ins - n_del;
	lb->edseq++;
	for (i = 0; i < n_ins; i++)
		lb->ln[pos + i] = *((char**)sb->s + i);
	/* m = {id, row, off}: marks on deleted rows are saved in lo and moved to the
	 * last new row; marks below shift; marks saved in lo are restored (undo) */
	for (i = 0; i < lb->mark_n; i++) {	/* updating marks */
		s64 *m = lb->mark + i * 3, *lm;
		if (m[1] >= pos + n_ins && m[1] < pos + n_del) {
			mark_set(&lo->mark, &lo->mark_n, m[0], m[1], m[2]);
			m[1] = n_ins ? pos + n_ins - 1 : -1;
		} else if (m[1] >= pos + n_del) {
			m[1] += n_ins - n_del;
		} else if ((lm = mark_find(lo->mark, lo->mark_n, m[0])))
			lbuf_copymark((m + 1), lm)
	}
	return n_ins;
}

/** @brief Set the [ mark (change start), saving the old one in lo. */
void lbuf_smark(struct lbuf *lb, struct lopt *lo, s64 beg, s64 o1)
{
	lbuf_copymark(lo->mark_sb, lb->mark_sb)
	lb->mark_sb[0] = beg;
	lb->mark_sb[1] = o1;
}

/** @brief Set the ] mark (change end), saving the old one in lo; frees lo if history is off. */
void lbuf_emark(struct lbuf *lb, struct lopt *lo, s64 end, s64 o2)
{
	lbuf_copymark(lo->mark_se, lb->mark_se)
	lb->mark_se[0] = end;
	lb->mark_se[1] = o2;
	if (opt_undo_seq < 0)
		lopt_done(lo);
}

/**
 * @brief Append an undo/redo history record, discarding any redo entries.
 * It remembers the n_del rows from beg that are about to be replaced.
 * With opt_undo_seq < 0 a static scratch record is used.
 */
struct lopt *lbuf_opt(struct lbuf *lb, s64 beg, s64 o1, s64 n_del)
{
	struct lopt *lo;
	static struct lopt slo;
	if (opt_undo_seq < 0)
		lo = &slo;
	else {
		for (s64 i = lb->hist_u; i < lb->hist_n; i++)
			lopt_done(&lb->hist[i]);
		lb->hist_n = lb->hist_u;
		if (lb->hist_n == lb->hist_sz) {
			s64 sz = lb->hist_sz + (lb->hist_sz ? lb->hist_sz : 128);
			struct lopt *hist = emalloc(sz * sizeof(hist[0]));
			memcpy(hist, lb->hist, lb->hist_n * sizeof(hist[0]));
			free(lb->hist);
			lb->hist = hist;
			lb->hist_sz = sz;
		}
		lo = &lb->hist[lb->hist_n++];
		lb->hist_u = lb->hist_n;
	}
	lo->ins = NULL;
	lo->del = n_del ? emalloc(n_del * sizeof(lo->del[0])) : NULL;
	for (s64 i = 0; i < n_del; i++)
		lo->del[i] = lb->ln[beg + i];
	lo->mark = NULL;
	lo->mark_n = 0;
	lo->mark_sb[0] = -1;
	lo->mark_se[0] = -1;
	lo->pos = beg;
	lo->pos_off = o1;
	lo->n_ins = 0;
	lo->n_del = n_del;
	lo->seq = lb->useq;
	lo->ref = 2;
	return lo;
}

/**
 * @brief Replace rows [beg, end) with buf, recording undo history.
 * @param buf     new text (lines end with '\n'), NULL only deletes
 * @param o1,o2   character offsets stored as the [ and ] marks
 */
void lbuf_edit(struct lbuf *lb, char *buf, s64 beg, s64 end, s64 o1, s64 o2)
{
	if (beg > lb->ln_n)
		beg = lb->ln_n;
	if (end > lb->ln_n)
		end = lb->ln_n;
	if (beg == end && !buf)
		return;
	struct lopt *lo = lbuf_opt(lb, beg, o1, end - beg);
	sbuf_smake(sb, sizeof(lo->ins[0])+1)
	lo->n_ins = lbuf_replace(lb, sb, buf, lo, lo->n_del, 0);
	if (lb->hist_u < 2 || lb->hist[lb->hist_u - 2].seq != lb->useq)
		lbuf_smark(lb, lo, beg, o1);
	lbuf_emark(lb, lo, beg + (lo->n_ins ? lo->n_ins - 1 : 0), o2);
	lb->modified = 1;
	if (lb->saved > lb->hist_u)
		lb->saved = -1;
	if (opt_undo_seq < 0 || !lo->n_ins)
		free(sb->s);
	else
		lo->ins = (char**)sb->s;
	if (agent_tool) {
		char msg[64];
		/* Report a zero-based, half-open span, like ex range beg/end. */
		s64 last = MAX(end, beg + lo->n_ins);
		snprintf(msg, sizeof(msg), "edited lines beg: %ld end: %ld", beg, last);
		ex_print(msg, msg_ft)
	}
	agent_sync(lb);
}

/** @brief Read all of fd and replace rows [beg, end) with it; nonzero on read error. */
s64 lbuf_rd(struct lbuf *lb, s64 fd, s64 beg, s64 end)
{
	struct stat st;
	long nr;	/* 1048575 caps at 2147481600 on 32 bit */
	s64 sz = 1048575, step = 1, n = 0;
	if (fstat(fd, &st) >= 0 && S_ISREG(st.st_mode) && st.st_size)
		sz = st.st_size >= INT64_MAX ? INT64_MAX : st.st_size + step;
	/* sz is the usable size, one byte is kept for the NUL */
	char *s = emalloc(sz--);
	while ((nr = read(fd, s + n, sz - n)) > 0) {
		n += nr;
		if (n >= sz + step) {
			if (n > INT64_MAX / 2) {
				n -= nr;
				break;
			}
			sz = n * 2;
			s = erealloc(s, sz--);
			step = 1;
		} else if (n == sz) {
			sz++;
			step = 0;
		}
	}
	s[n] = '\0';
	lbuf_edit(lb, s, beg, end, 0, 0);
	free(s);
	return nr != 0;
}

/** @brief Write rows [beg, end) to fd; negative on error. */
s64 lbuf_wr(struct lbuf *lb, s64 fd, s64 beg, s64 end)
{
	for (s64 i = beg; i < end; i++) {
		char *ln = lb->ln[i];
		long nw = 0;
		long nl = lbuf_s(ln)->len + 1;
		while (nw < nl) {
			long nc = write(fd, ln + nw, nl - nw);
			if (nc < 0)
				return nc;
			nw += nc;
		}
	}
	return 0;
}

/**
 * @brief Copy the text from (r1, o1) up to (r2, o2) into a newly made sb.
 * r2 is inclusive; o2 < 0 takes all of r2 including '\n'; when r1 == r2 and
 * o2 < o1 the copy runs to the line end.
 */
void lbuf_region(struct lbuf *lb, sbuf *sb, s64 r1, s64 o1, s64 r2, s64 o2)
{
	char *s1 = lbuf_get(lb, r1), *s2;
	_sbuf_make(sb, 1024,)
	r2 = MIN(lb->ln_n, r2);
	if (s1) {
		/* send: one past the '\n' of row r1 */
		char *send = s1 + lbuf_s(s1)->len+1;
		if (r1 == r2) {
			s1 = uc_chr(s1, o1);
			s2 = o2 >= o1 ? uc_chr(s1, o2 - o1) : send;
			if (s2 > s1)
				sbuf_mem(sb, s1, s2 - s1)
			goto ret;
		}
		s2 = o1 >= 0 ? uc_chr(s1, o1) : send;
		if (send > s2)
			sbuf_mem(sb, s2, send - s2)
	}
	for (s64 i = r1 + 1; i < r2; i++)
		sbuf_mem(sb, lb->ln[i], lbuf_i(lb, i)->len + 1)
	if ((s2 = lbuf_get(lb, r2))) {
		s1 = o2 >= 0 ? uc_chr(s2, o2) : s2 + lbuf_s(s2)->len+1;
		if (s1 > s2)
			sbuf_mem(sb, s2, s1 - s2)
	}
	ret:
	sbuf_nul4(sb)
}

/** @brief Convert (row, off) position to byte offset within region (r1,o1)-(r2,o2); -1 if outside. */
s64 lbuf_pos2off(struct lbuf *lb, s64 r1, s64 o1, s64 r2, s64 o2, s64 row, s64 off)
{
	s64 boff = 0, sub;
	char *ln = lbuf_get(lb, r1);
	if (!ln || row < r1 || row > r2)
		return -1;
	for (s64 i = r1; ln && i <= row;) {
		if (i == row) {
			if ((i == r1 && off < o1) || (o2 >= 0 && i == r2 && off > o2))
				return -1;
			/* sub: byte offset of o1 in r1; the result counts bytes from (r1, o1) */
			sub = uc_chr(lb->ln[r1], o1) - lb->ln[r1];
			boff -= boff ? sub : 0;
			if (i != r1)
				sub = 0;
			return boff + (uc_chr(ln, off) - (ln + sub));
		}
		boff += lbuf_i(lb, i)->len + 1;
		ln = lbuf_get(lb, ++i);
	}
	return -1;
}

/** @brief Convert byte offset within region (r1,o1)-(r2,o2) to (row, off) position; nonzero if outside. */
s64 lbuf_off2pos(struct lbuf *lb, s64 r1, s64 o1, s64 r2, s64 o2, s64 boff, s64 *row, s64 *off)
{
	char *ln = lbuf_get(lb, r1);
	if (!ln)
		return 1;
	/* acc: bytes from (r1, o1) to the end of row i */
	s64 acc = -(uc_chr(ln, o1) - ln);
	for (s64 i = r1; ln && i <= r2; ln = lbuf_get(lb, ++i)) {
		acc += lbuf_i(lb, i)->len + 1;
		if (acc > boff) {
			*row = i;
			*off = uc_off(ln, boff - (acc - (lbuf_i(lb, i)->len + 1)));
			return o2 >= 0 && i == r2 && *off > o2;
		}
	}
	return 1;
}

/**
 * @brief Build r1's text before char *o1, then i, then r2's text from char *o2.
 * For r1 == r2, *o2 == -2 drops the rest of the line and *o2 <= *o1 deletes nothing.
 * *o1 (and *o2 if r1 != r2) are clamped to the line length.
 * @return malloc'd result (with the 4-NUL pad)
 */
char *lbuf_joinsb(struct lbuf *lb, s64 r1, s64 r2, sbuf *i, s64 *o1, s64 *o2)
{
	char *s = lbuf_get(lb, r1), *e, *se, *p;
	char *es = lbuf_get(lb, r2);
	s64 endsz;
	if (!s || !es)
		return NULL;
	if (rstate->s == s) {
		*o1 = MIN(*o1, rstate->n);
		e = rstate->chrs[*o1];
	} else
		e = uc_chrn(s, *o1, o1);
	if (r1 == r2) {
		se = *o2 > *o1 ? uc_chr(e, *o2 - *o1) :
			*o2 == -2 ? s + lbuf_s(s)->len + 1 : e;	/* -2 eol, -1 point default */
		endsz = lbuf_s(s)->len + 5 - (se - e);
	} else {
		se = uc_chrn(es, *o2, o2);
		endsz = (e - s) + (lbuf_s(es)->len + 5 - (se - es));
	}
	/* e: cut point in r1, se: resume point in r2, endsz: bytes before e plus bytes from se through the pad */
	p = emalloc(endsz + i->s_n);
	memcpy(p, s, e - s);
	memcpy(p + (e - s), i->s, i->s_n);
	memcpy(p + (e - s) + i->s_n, se, endsz - (e - s));
	return p;
}

/**
 * @brief Join rows [beg, end), stripping leading blanks of joined lines.
 * @param flg  insert a space between lines (not before ')')
 * @param[out] o2  cursor offset at the last join point
 */
s64 lbuf_join(struct lbuf *lb, s64 beg, s64 end, s64 o1, s64 *o2, s64 flg)
{
	if (!lbuf_get(lb, beg) || !lbuf_get(lb, end - 1))
		return 1;
	sbuf_smake(sb, 1024)
	for (s64 i = beg; i < end; i++) {
		char *ln = lbuf_get(lb, i);
		char *lnend = ln + lbuf_s(ln)->len;
		if (i > beg) {
			while (ln[0] == ' ' || ln[0] == '\t')
				ln++;
			if (flg && sb->s_n && *ln != ')' &&
					sb->s[sb->s_n-1] != ' ') {
				sbuf_chr(sb, ' ')
				*o2 += 1;
			}
		}
		*o2 += (i+1 == end) ? 0 : uc_slen(ln) - 1;
		sbuf_mem(sb, ln, lnend - ln)
	}
	sbufn_chr(sb, '\n')
	lbuf_edit(lb, sb->s, beg, end, o1, *o2);
	free(sb->s);
	return 0;
}

/** @brief Row pos, or NULL if out of range. */
char *lbuf_get(struct lbuf *lb, s64 pos)
{
	return pos >= 0 && pos < lb->ln_n ? lb->ln[pos] : NULL;
}

/** @brief Undo all history records of the latest sequence; returns 1 if none. */
s64 lbuf_undo(struct lbuf *lb, s64 *row, s64 *off)
{
	if (!lb->hist_u)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u - 1];
	const s64 useq = lo->seq;
	sbuf sb;
	if (lb->hist_u == lb->hist_n) {
		lbuf_copymark(lb->tmp_mark, lb->mark_sb)
		lbuf_copymark((lb->tmp_mark + 2), lb->mark_se)
	}
	while (lb->hist_u && lb->hist[lb->hist_u - 1].seq == useq) {
		lo = &lb->hist[--lb->hist_u];
		lo->ref = 1;
		sb.s = (char*)lo->del;
		lbuf_replace(lb, &sb, NULL, lo, lo->n_ins, lo->n_del);
	}
	*row = lo->pos;
	*off = MAX(0, lo->pos_off);
	lbuf_copymark(lb->mark_sb, lo->mark_sb)
	lbuf_copymark(lb->mark_se, lo->mark_se)
	lb->modified = lb->hist_u != lb->saved;
	agent_sync(lb);
	return 0;
}

/** @brief Redo all history records of the next sequence; returns 1 if none. */
s64 lbuf_redo(struct lbuf *lb, s64 *row, s64 *off)
{
	if (lb->hist_u == lb->hist_n)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u];
	const s64 useq = lo->seq;
	sbuf sb;
	while (lb->hist_u < lb->hist_n && lb->hist[lb->hist_u].seq == useq) {
		lo = &lb->hist[lb->hist_u++];
		lo->ref = 2;
		sb.s = (char*)lo->ins;
		lbuf_replace(lb, &sb, NULL, lo, lo->n_del, lo->n_ins);
	}
	*row = lo->pos;
	*off = MAX(0, lo->pos_off);
	if (lb->hist_u < lb->hist_n) {
		lo++;
		lbuf_copymark(lb->mark_sb, lo->mark_sb)
		lbuf_copymark(lb->mark_se, lo->mark_se)
	} else {
		lbuf_copymark(lb->mark_sb, lb->tmp_mark)
		lbuf_copymark(lb->mark_se, (lb->tmp_mark + 2))
	}
	lb->modified = lb->hist_u != lb->saved;
	agent_sync(lb);
	return 0;
}

/** @brief Mark buffer as saved and, if clear, clear the undo history. */
void lbuf_saved(struct lbuf *lb, s64 clear)
{
	if (clear) {
		for (s64 i = 0; i < lb->hist_n; i++)
			lopt_done(&lb->hist[i]);
		lb->hist_n = 0;
		lb->hist_u = 0;
	}
	lb->modified = 0;
	lb->saved = lb->hist_u;
}

/** @brief Number of leading blank chars of row r; for a blank line, the offset of the char before its '\n' (-1 if empty). */
s64 lbuf_indents(struct lbuf *lb, s64 r)
{
	char *ln = lbuf_get(lb, r);
	s64 o;
	if (!ln)
		return 0;
	for (o = 0; uc_isspace(*ln); o++)
		ln += uc_len(ln);
	return *ln ? o : o - 2;
}

/** @brief f/t/F/T: move to the n-th cs in the row; returns nonzero if not found. */
s64 lbuf_findchar(struct lbuf *lb, char *cs, s64 cmd, s64 n, s64 *row, s64 *off)
{
	char *ln = lbuf_get(lb, *row);
	s64 c1, c2, l, dir = (cmd == 'f' || cmd == 't') ? +1 : -1;
	if (!ln)
		return 1;
	ren_state *r = ren_position(ln);
	uc_code(c2, cs, l)
	*off += dir + (cmd == 't') - (cmd == 'T');
	while (n > 0 && *off >= 0 && *off < r->n) {
		uc_code(c1, r->chrs[*off], l)
		if (c1 == c2)
			n--;
		if (n > 0)
			*off += dir;
	}
	if (!n && (cmd == 't' || cmd == 'T'))
		*off = MIN(MAX(0, *off - dir), r->n-1);
	return n != 0;
}

struct lsparams
{
	struct lbuf *lb;
	rstr *re;
	s64 dir;
	s64 beg;
	s64 end;
	s64 *r;
	s64 *o;
	s64 off;
	s64 nskip;
};

static void *lsearch(void *arg)
{
	struct lsparams *a = arg;
	s64 r0 = *a->r, o0 = *a->o;
	s64 offs[a->re->rs ? a->re->rs->nsubc : 2], i = r0;
	char *s;
	s64 off = a->off, g1, g2, _o, step, flg;
	for (; i >= a->beg && i < a->end; i += a->dir) {
		_o = 0;
		step = 0;
		flg = REG_NEWLINE;
		s = a->lb->ln[i];
		/* off: byte offset where the next search starts; step: byte offset of the
		 * previous match, _o: its char offset, so uc_off() counts only the new part.
		 * Backward search keeps the last match of the row. */
		while (rstr_find(a->re, s + off, offs, flg) >= 0) {
			flg |= REG_NOTBOL;
			g1 = offs[opt_search_group], g2 = offs[opt_search_group + 1];
			if (g1 < 0) {
				off += offs[1] > 0 ? offs[1] : MAX(1, uc_len(s + off));
				continue;
			}
			_o += uc_off(s + step, off + g1 - step);
			if (a->dir < 0 && r0 == i && _o > o0 - a->nskip)
				break;
			*a->o = _o;
			*a->r = i;
			step = off + g1;
			off += g2 > 0 ? g2 : MAX(1, uc_len(s + off));
			a->end = -1; /* break outer loop efficiently */
			if (a->dir > 0)
				return NULL;
		}
		off = 0;
	}
	return NULL;
}

/**
 * @brief Search re from (*r, *o) through rows [beg, end) in direction dir.
 * The rows are split into up to NUM_THREADS slices searched in parallel by
 * lsearch(); the first slice in search order with a match wins. While it runs
 * utf8_length['\n'] is 0 (see re_pikevm()).
 * @param pskip  start pskip chars after *o on the first row; < 0 from line start
 * @param nskip  backward: on the first row ignore matches after *o - nskip
 * @return 0 if found (position in *r, *o)
 */
s64 lbuf_search(struct lbuf *lb, rstr *re, s64 dir, s64 beg, s64 end, s64 pskip,
		s64 nskip, s64 *r, s64 *o)
{
	static pthread_attr_t lsattr;	/* musl caps threads at 128k; main()'s stack */
	static pthread_attr_t *lsattr_ok;	/* &lsattr after the one-time sizing */
	#define NUM_THREADS 4
	pthread_t threads[NUM_THREADS];
	static unsigned char fake_ulen[256]; /* novelty: for fast thread termination */
	struct lsparams data[NUM_THREADS];
	s64 rs[NUM_THREADS];
	s64 os[NUM_THREADS];
	s64 thread_step = MAX(end / NUM_THREADS, 1); /* number of lines assigned per thread */
	s64 step = 0, i = 0, off;
	char *s = lbuf_get(lb, *r);
	if (pskip >= 0 && s)
		off = rstate->s == s ? rstate->chrs[MIN(*o + pskip, rstate->n)] - s
					: uc_chr(s, *o + pskip) - s;
	else
		off = 0;
	utf8_length['\n'] = 0;
	if (!lsattr_ok) {
		struct rlimit rl;
		pthread_attr_init(&lsattr);
		if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY)
			pthread_attr_setstacksize(&lsattr, rl.rlim_cur);
		lsattr_ok = &lsattr;
	}
	for (i = 0; i < NUM_THREADS; i++) {
		if (*r + step > end || *r + step * dir < 0)
			break;
		data[i].lb = lb;
		data[i].re = re;
		data[i].dir = dir;
		data[i].off = i ? 0 : off;
		rs[i] = *r + step * dir;
		step += thread_step;
		data[i].r = &rs[i];
		data[i].beg = beg;
		data[i].nskip = nskip;
		if (i == NUM_THREADS-1)
			data[i].end = end;
		else
			data[i].end = MIN(rs[i] + thread_step, end);
		os[i] = i ? -1 : *o;
		data[i].o = &os[i];
		pthread_create(&threads[i], lsattr_ok, lsearch, (void*) &data[i]);
	}
	for (step = i, i = 0; i < step; i++) {
		pthread_join(threads[i], NULL);
		if (data[i].end < 0) {
			*r = *data[i].r;
			*o = *data[i].o;
			for (s64 z = i+1; z < step; z++)
				data[z].end = -1;
			/* force instant termination, regardless how long string is */
			utf8_length = fake_ulen;
			for (s64 z = i+1; z < step; z++)
				pthread_join(threads[z], NULL);
			utf8_length = utf8_length_default;
			utf8_length['\n'] = 1;
			return 0;
		}
	}
	utf8_length['\n'] = 1;
	return 1;
}

/** @brief Move to the next row starting with ch ('{', or '\n' for paragraph breaks). */
s64 lbuf_sectionbeg(struct lbuf *lb, s64 dir, s64 *row, s64 *off, s64 ch)
{
	if (ch == '\n')
		while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) == ch)
			*row += dir;
	else
		*row += dir;
	while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) != ch)
		*row += dir;
	*row = MAX(0, MIN(*row, lbuf_len(lb) - 1));
	*off = 0;
	return 0;
}

/**
 * @brief Offset of the row's '\n', i.e. its character count without the newline.
 * @param state  nonzero: count via ren_position(); 2: only if rstate already holds row
 */
s64 lbuf_eol(struct lbuf *lb, s64 row, s64 state)
{
	char *ln = lbuf_get(lb, row);
	if (!ln)
		return 0;
	if (state == 2)
		state = rstate->s == ln;
	state = state ? ren_position(ln)->n - 1 : uc_slen(ln) - 1;
	return state < 0 ? 0 : state;
}

/**
 * @brief Step one character in dir (+-1 may cross rows, +-2 stays on the row).
 * Leaves rstate describing the new row.
 * @return -1 at the edge
 */
s64 lbuf_next(struct lbuf *lb, s64 dir, s64 *r, s64 *o)
{
	s64 odir = dir > 0 ? 1 : -1;
	s64 len, off = *o + odir;
	if (lbuf_get(lb, *r))
		len = ren_position(lbuf_get(lb, *r))->n;
	else
		return -1;
	if (off < 0 || off >= len) {
		if (dir % 2 == 0 || !lbuf_get(lb, *r + odir))
			return -1;
		*r += odir;
		if (odir > 0) {
			ren_position(lbuf_get(lb, *r));
			*o = 0;
		} else
			*o = lbuf_eol(lb, *r, 1);
	} else
		*o = off;
	return 0;
}

/** @brief Move to the last character of the word; kind is a uc_kind() mask (3 = any non-blank). */
static s64 lbuf_wordlast(struct lbuf *lb, s64 kind, s64 dir, s64 *row, s64 *off)
{
	if (!kind || !(uc_kind(rstate->chrs[*off]) & kind))
		return 0;
	while (uc_kind(rstate->chrs[*off]) & kind)
		if (lbuf_next(lb, dir, row, off))
			return 1;
	if (!(uc_kind(rstate->chrs[*off]) & kind))
		lbuf_next(lb, -dir, row, off);
	return 0;
}

/** @brief w/W: move to the next word start (stops at an empty line); big selects WORDs. */
s64 lbuf_wordbeg(struct lbuf *lb, s64 big, s64 dir, s64 *row, s64 *off)
{
	s64 nl;
	if (!lbuf_get(lb, *row))
		return 1;
	ren_state *r = ren_position(lbuf_get(lb, *row));
	lbuf_wordlast(lb, big ? 3 : uc_kind(r->chrs[*off]), dir, row, off);
	nl = *rstate->chrs[*off] == '\n';
	if (lbuf_next(lb, dir, row, off))
		return 1;
	while (uc_isspace(*rstate->chrs[*off])) {
		nl += *rstate->chrs[*off] == '\n';
		if (nl == 2)
			return 0;
		if (lbuf_next(lb, dir, row, off))
			return 1;
	}
	return 0;
}

/** @brief e/E (dir > 0) and b/B (dir < 0) word motions; big selects WORDs. */
s64 lbuf_wordend(struct lbuf *lb, s64 big, s64 dir, s64 *row, s64 *off)
{
	s64 nl = 0;
	if (!lbuf_get(lb, *row))
		return 1;
	ren_state *r = ren_position(lbuf_get(lb, *row));
	if (!uc_isspace(*r->chrs[*off])) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl = dir < 0 && *rstate->chrs[*off] == '\n';
	}
	nl += dir > 0 && *rstate->chrs[*off] == '\n';
	while (uc_isspace(*rstate->chrs[*off])) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl += *rstate->chrs[*off] == '\n';
		if (nl == 2) {
			if (dir < 0)
				lbuf_next(lb, -dir, row, off);
			return 0;
		}
	}
	lbuf_wordlast(lb, big ? 3 : uc_kind(rstate->chrs[*off]), dir, row, off);
	return 0;
}

/**
 * @brief Move to the matching character.
 * @param pairs  open/close pairs, e.g. "()[]{}"; the first one at or after
 *               the cursor is used, even index searches forward
 */
s64 lbuf_pair(struct lbuf *lb, char *pairs, s64 plen, s64 *row, s64 *off)
{
	s64 r = *row, o = *off;
	s64 p, c, dep = 1;
	char *m;
	if (!lbuf_get(lb, r))
		return 1;
	ren_state *rs = ren_position(lbuf_get(lb, r));
	for (; o < rs->n-1 && !memchr(pairs, *rs->chrs[o], plen); o++);
	if (!(m = memchr(pairs, *rs->chrs[o], plen)))
		return 1;
	p = m - pairs;
	while (!lbuf_next(lb, (p & 1) ? -1 : +1, &r, &o)) {
		c = *rstate->chrs[o];
		if (c == pairs[p ^ 1])
			dep--;
		if (c == pairs[p])
			dep++;
		if (!dep) {
			*row = r;
			*off = o;
			return 0;
		}
	}
	return 1;
}
