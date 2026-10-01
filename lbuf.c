struct lbuf *lbuf_make(void)
{
	struct lbuf *lb = emalloc(sizeof(*lb));
	memset(lb, 0, sizeof(*lb));
	lb->mark_sb[0] = -1;
	lb->mark_se[0] = -1;
	return lb;
}

static void lbuf_rfree(char *ln)
{
	for (s64 i = 0; i < 2; i++)
		if (rstates[i].s == ln)
			rstates[i].s = NULL;
	free(lbuf_s(ln));
}

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

#define lbuf_copymark(dst, src) { dst[0] = src[0]; dst[1] = src[1]; }

/* find a mark id, returning its row & off pair */
static s64 *mark_find(s64 *mark, s64 n, s64 id)
{
	for (s64 i = 0; i < n * 3; i += 3)
		if (mark[i] == id)
			return mark + i + 1;
	return NULL;
}

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

static s64 linelength(char *s)
{
	s64 len = dstrlen(s, '\n');
	return s[len] == '\n' ? len + 1 : len;
}

/* low-level line replacement */
static s64 lbuf_replace(struct lbuf *lb, sbuf *sb, char *s, struct lopt *lo, s64 n_del, s64 n_ins)
{
	s64 i, pos = lo->pos;
	if (s) {
		for (; *s; n_ins++) {
			s64 l = linelength(s);
			s64 l_nonl = l - (s[l - !!l] == '\n');
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

void lbuf_smark(struct lbuf *lb, struct lopt *lo, s64 beg, s64 o1)
{
	lbuf_copymark(lo->mark_sb, lb->mark_sb)
	lb->mark_sb[0] = beg;
	lb->mark_sb[1] = o1;
}

void lbuf_emark(struct lbuf *lb, struct lopt *lo, s64 end, s64 o2)
{
	lbuf_copymark(lo->mark_se, lb->mark_se)
	lb->mark_se[0] = end;
	lb->mark_se[1] = o2;
	if (xseq < 0)
		lopt_done(lo);
}

/* append undo/redo history */
struct lopt *lbuf_opt(struct lbuf *lb, s64 beg, s64 o1, s64 n_del)
{
	struct lopt *lo;
	static struct lopt slo;
	if (xseq < 0)
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

/* replace lines beg through end with buf */
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
	if (xseq < 0 || !lo->n_ins)
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

s64 lbuf_rd(struct lbuf *lb, s64 fd, s64 beg, s64 end)
{
	struct stat st;
	long nr;	/* 1048575 caps at 2147481600 on 32 bit */
	s64 sz = 1048575, step = 1, n = 0;
	if (fstat(fd, &st) >= 0 && S_ISREG(st.st_mode) && st.st_size)
		sz = st.st_size >= INT64_MAX ? INT64_MAX : st.st_size + step;
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

void lbuf_region(struct lbuf *lb, sbuf *sb, s64 r1, s64 o1, s64 r2, s64 o2)
{
	char *s1 = lbuf_get(lb, r1), *s2;
	_sbuf_make(sb, 1024,)
	r2 = MIN(lb->ln_n, r2);
	if (s1) {
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

/* convert (row, off) position to byte offset within region (r1,o1)-(r2,o2) */
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

/* convert byte offset within region (r1,o1)-(r2,o2) to (row, off) position */
s64 lbuf_off2pos(struct lbuf *lb, s64 r1, s64 o1, s64 r2, s64 o2, s64 boff, s64 *row, s64 *off)
{
	char *ln = lbuf_get(lb, r1);
	if (!ln)
		return 1;
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
	p = emalloc(endsz + i->s_n);
	memcpy(p, s, e - s);
	memcpy(p + (e - s), i->s, i->s_n);
	memcpy(p + (e - s) + i->s_n, se, endsz - (e - s));
	return p;
}

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

char *lbuf_get(struct lbuf *lb, s64 pos)
{
	return pos >= 0 && pos < lb->ln_n ? lb->ln[pos] : NULL;
}

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

/* mark buffer as saved and, if clear, clear the undo history */
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
		while (rstr_find(a->re, s + off, offs, flg) >= 0) {
			flg |= REG_NOTBOL;
			g1 = offs[xgrp], g2 = offs[xgrp + 1];
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
			utf8_length = _utf8_length;
			utf8_length['\n'] = 1;
			return 0;
		}
	}
	utf8_length['\n'] = 1;
	return 1;
}

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

/* move to the last character of the word */
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

/* move to the matching character */
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
