/* Document bytes and render character offsets are separate coordinate spaces. */
struct ts_capture {
	uint32_t beg, end, pattern, order;
	s64 att;
};
struct ts_state {
	struct ts_state *next;
	struct lbuf *lb;
	TSParser *parser;
	TSTree *tree;
	uint32_t *offset;
	s64 rows, dirty, language;
	unsigned long revision;
	struct ts_capture *captures;
	s64 capture_n, capture_size, cache_beg, cache_end;
	s64 preview, beg, end, pending_n, cursor;
	char *pending, **lines;
	char *readbuf;
};
static struct {
	TSQuery *query;
	s64 *attributes;
	s64 initialized, failed;
} ts_config[LEN(ts_languages)];
static struct ts_state *ts_states;
struct ts_state *ts_preview;
s64 ts_redraw;
static rset *ts_word;

static void ts_diagnostic(void)
{
	static s64 warned;
	if (!warned++)
		fprintf(stderr, "Tree-sitter initialization failed; using regex highlighting\n");
}

static s64 ts_configure(s64 language)
{
	uint32_t offset, count, len;
	TSQueryError error;
	if (ts_config[language].initialized)
		return !ts_config[language].failed;
	ts_config[language].initialized = 1;
	const TSLanguage *lang = ts_languages[language].language();
	TSParser *parser = ts_parser_new();
	s64 valid = ts_parser_set_language(parser, lang);
	ts_parser_delete(parser);
	TSQuery *query = valid ? ts_query_new(lang, ts_languages[language].query,
		strlen(ts_languages[language].query), &offset, &error) : NULL;
	if (query)
		for (uint32_t i = 0; i < ts_query_pattern_count(query); i++) {
			ts_query_predicates_for_pattern(query, i, &count);
			if (count)
				valid = 0;
		}
	if (!query || !valid) {
		ts_query_delete(query);
		ts_config[language].failed = 1;
		ts_diagnostic();
		return 0;
	}
	ts_config[language].query = query;
	count = ts_query_capture_count(query);
	ts_config[language].attributes = emalloc(count * sizeof(s64));
	for (uint32_t i = 0; i < count; i++) {
		const char *name = ts_query_capture_name_for_id(query, i, &len);
		ts_config[language].attributes[i] = 0;
		for (s64 j = 0; j < LEN(ts_attributes); j++)
			if (strlen(ts_attributes[j].name) == len &&
					!memcmp(name, ts_attributes[j].name, len))
				ts_config[language].attributes[i] = ts_attributes[j].att;
	}
	return 1;
}

char *ts_line(struct ts_state *s, s64 row)
{
	if (!s || row < 0 || row >= s->rows)
		return NULL;
	if (s->preview && row >= s->beg) {
		if (row < s->beg + s->pending_n)
			return s->lines[row - s->beg];
		row += s->end - s->beg - s->pending_n;
	}
	return lbuf_get(s->lb, row);
}

static const char *ts_read(void *payload, uint32_t byte, TSPoint point, uint32_t *size)
{
	struct ts_state *s = payload;
	char *line = ts_line(s, point.row);
	*size = 0;
	if (!line || point.column >= s->offset[point.row + 1] - s->offset[point.row])
		return "";
	uint32_t len = s->offset[point.row + 1] - s->offset[point.row];
	/* Slot 1 may temporarily hide bytes beyond lim. */
	for (s64 i = 0; i < 2; i++) {
		ren_state *r = rstates + i;
		if (r->s == line && r->holelen) {
			s->readbuf = erealloc(s->readbuf, len);
			memcpy(s->readbuf, line, len);
			memcpy(s->readbuf + (r->chrs[r->n] - line), r->nulhole, r->holelen);
			line = s->readbuf;
			break;
		}
	}
	*size = len - point.column;
	return line + point.column;
}

static s64 ts_parse(struct ts_state *s)
{
	if (!s->dirty)
		return s->tree != NULL;
	TSInput input = {s, ts_read, TSInputEncodingUTF8};
	TSTree *tree = ts_parser_parse(s->parser, s->tree, input);
	if (!tree)
		return 0;
	ts_tree_delete(s->tree);
	s->tree = tree;
	s->dirty = 0;
	return 1;
}

static struct ts_state *ts_new(struct lbuf *lb, s64 language)
{
	struct ts_state *s = emalloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	s->lb = lb;
	s->language = language;
	s->parser = ts_parser_new();
	ts_parser_set_language(s->parser, ts_languages[language].language());
	s->dirty = 1;
	s->cache_beg = -1;
	s->revision = lb->ts_revision;
	s->rows = lb->ln_n;
	s->offset = emalloc((s->rows + 1) * sizeof(*s->offset));
	s->offset[0] = 0;
	for (s64 i = 0; i < s->rows; i++)
		s->offset[i + 1] = s->offset[i] + lbuf_s(lb->ln[i])->len + 1;
	s->next = ts_states;
	ts_states = s;
	return s;
}

void ts_forget(struct lbuf *lb)
{
	for (struct ts_state *s = ts_states; s; s = s->next)
		if (s->lb == lb && s != lb->ts) {
			s->lb = NULL;
			s->rows = 0;
		}
	ts_free(lb->ts);
}

void ts_free(struct ts_state *s)
{
	if (!s)
		return;
	struct ts_state **p = &ts_states;
	while (*p && *p != s)
		p = &(*p)->next;
	if (*p)
		*p = s->next;
	ts_tree_delete(s->tree);
	ts_parser_delete(s->parser);
	free(s->offset);
	free(s->captures);
	free(s->pending);
	free(s->lines);
	free(s->readbuf);
	free(s);
}

struct ts_state *ts_document(struct lbuf *lb)
{
	s64 language = -1;
	if (opt_syntax_hl <= 1 || !lb)
		return NULL;
	for (s64 i = 0; i < LEN(ts_languages); i++)
		if (syn_getft() == ts_languages[i].ft)
			language = i;
	const char *path = cur_buf && xb == lb ? xb_path : "";
	const char *suffix = strrchr(path, '.');
	if (*path && (!suffix || (strcmp(suffix, ".c") && strcmp(suffix, ".h"))))
		language = -1;
	if (language < 0 || !ts_configure(language)) {
		ts_free(lb->ts);
		lb->ts = NULL;
		return NULL;
	}
	if (lb->ts && lb->ts->language != language) {
		ts_free(lb->ts);
		lb->ts = NULL;
	}
	if (!lb->ts)
		lb->ts = ts_new(lb, language);
	return lb->ts;
}

/* Update the old tree and index before the line array moves. */
static void ts_splice(struct ts_state *s, s64 row, s64 del, uint32_t *lengths, s64 ins)
{
	uint32_t added = 0, start = s->offset[row], oldend = s->offset[row + del];
	for (s64 i = 0; i < ins; i++)
		added += lengths[i];
	TSInputEdit edit = {start, oldend, start + added,
		{row, 0}, {row + del, 0}, {row + ins, 0}};
	if (s->tree)
		ts_tree_edit(s->tree, &edit);
	s64 rows = s->rows + ins - del;
	uint32_t *index = emalloc((rows + 1) * sizeof(*index));
	memcpy(index, s->offset, (row + 1) * sizeof(*index));
	for (s64 i = 0; i < ins; i++)
		index[row + i + 1] = index[row + i] + lengths[i];
	for (s64 i = row + del + 1; i <= s->rows; i++)
		index[i + ins - del] = s->offset[i] - oldend + start + added;
	free(s->offset);
	s->offset = index;
	s->rows = rows;
	s->dirty = 1;
	s->cache_beg = -1;
}

void ts_edit(struct lbuf *lb, s64 row, s64 del, char **lines, s64 ins)
{
	lb->ts_revision++;
	if (opt_syntax_hl > 1)
		ts_redraw = 1;
	if (!lb->ts)
		return;
	uint32_t *lengths = emalloc(MAX(1, ins) * sizeof(*lengths));
	for (s64 i = 0; i < ins; i++)
		lengths[i] = lbuf_s(lines[i])->len + 1;
	ts_splice(lb->ts, row, del, lengths, ins);
	lb->ts->revision = lb->ts_revision;
	free(lengths);
}

struct ts_state *ts_preview_begin(struct lbuf *lb, s64 beg, s64 end)
{
	struct ts_state *base = ts_document(lb);
	if (!base || !ts_parse(base))
		return NULL;
	struct ts_state *s = ts_new(lb, base->language);
	s->tree = ts_tree_copy(base->tree);
	s->beg = MIN(beg, lb->ln_n);
	s->end = MIN(end, lb->ln_n);
	return s;
}

s64 ts_preview_update(struct ts_state *s, char *text)
{
	if (!s->lb)
		return 0;
	s64 len = strlen(text), count = 0;
	char *copy = emalloc(len + 2);
	memcpy(copy, text, len);
	if (!len || copy[len - 1] != '\n')
		copy[len++] = '\n';
	copy[len] = 0;
	if (s->revision != s->lb->ts_revision) {
		/* Nested editor changed the underlying document. */
		ts_tree_delete(s->tree);
		s->tree = NULL;
		s->revision = s->lb->ts_revision;
		s->beg = MIN(s->beg, s->lb->ln_n);
		s->end = MIN(s->end, s->lb->ln_n);
		s->preview = 0;
		s->rows = s->lb->ln_n;
		s->offset = erealloc(s->offset, (s->rows + 1) * sizeof(*s->offset));
		s->offset[0] = 0;
		for (s64 i = 0; i < s->rows; i++)
			s->offset[i + 1] = s->offset[i] + lbuf_s(s->lb->ln[i])->len + 1;
	}
	if (s->preview && !memcmp(s->pending, copy, MIN(len + 1,
		(s64)(s->offset[s->beg + s->pending_n] - s->offset[s->beg]) + 1)) &&
		(uint32_t)len == s->offset[s->beg + s->pending_n] - s->offset[s->beg]) {
		free(copy);
		return 0;
	}
	for (s64 i = 0; i < len; i++)
		count += copy[i] == '\n';
	char **lines = emalloc(count * sizeof(*lines));
	uint32_t *lengths = emalloc(count * sizeof(*lengths));
	s64 row = 0, start = 0;
	for (s64 i = 0; i < len; i++)
		if (copy[i] == '\n') {
			lines[row] = copy + start;
			lengths[row++] = i + 1 - start;
			start = i + 1;
		}
	/* Common complete lines remain unchanged in the edited tree. */
	s64 oldcount = s->preview ? s->pending_n : s->end - s->beg;
	s64 first = 0, last = 0;
	while (first < MIN(count, oldcount) &&
		lengths[first] == s->offset[s->beg + first + 1] - s->offset[s->beg + first] &&
		!memcmp(lines[first], ts_line(s, s->beg + first), lengths[first]))
		first++;
	while (last < MIN(count, oldcount) - first &&
		lengths[count - last - 1] == s->offset[s->beg + oldcount - last] - s->offset[s->beg + oldcount - last - 1] &&
		!memcmp(lines[count - last - 1], ts_line(s, s->beg + oldcount - last - 1), lengths[count - last - 1]))
		last++;
	ts_splice(s, s->beg + first, oldcount - first - last,
		lengths + first, count - first - last);
	free(lengths);
	free(s->pending);
	free(s->lines);
	s->pending = copy;
	s->lines = lines;
	s->pending_n = count;
	s->preview = 1;
	return 1;
}

static int ts_capture_cmp(const void *a, const void *b)
{
	const struct ts_capture *x = a, *y = b;
	if (x->pattern != y->pattern)
		return x->pattern < y->pattern ? -1 : 1;
	return x->order < y->order ? -1 : x->order != y->order;
}

static void ts_query_line(struct ts_state *s, s64 row)
{
	if (s->cache_beg >= 0 && row >= s->cache_beg && row < s->cache_end)
		return;
	s->cache_beg = row / 64 * 64;
	s->cache_end = MIN(s->rows, s->cache_beg + 64);
	s->capture_n = 0;
	TSQueryCursor *cursor = ts_query_cursor_new();
	ts_query_cursor_set_byte_range(cursor, s->offset[s->cache_beg], s->offset[s->cache_end]);
	ts_query_cursor_exec(cursor, ts_config[s->language].query, ts_tree_root_node(s->tree));
	TSQueryMatch match;
	while (ts_query_cursor_next_match(cursor, &match))
		for (s64 i = 0; i < match.capture_count; i++) {
			TSQueryCapture capture = match.captures[i];
			s64 att = ts_config[s->language].attributes[capture.index];
			if (!att)
				continue;
			if (s->capture_n == s->capture_size) {
				s->capture_size = MAX(64, s->capture_size * 2);
				s->captures = erealloc(s->captures, s->capture_size * sizeof(*s->captures));
			}
			s->captures[s->capture_n] = (struct ts_capture){ts_node_start_byte(capture.node),
				ts_node_end_byte(capture.node), match.pattern_index, s->capture_n, att};
			s->capture_n++;
		}
	ts_query_cursor_delete(cursor);
	if (s->capture_n > 1)
		qsort(s->captures, s->capture_n, sizeof(*s->captures), ts_capture_cmp);
}

struct ts_source {
	struct ts_state *state;
	s64 row, col;
};

static void ts_setword(char *word)
{
	rset *rs = rset_make(1, &word, 0);
	if (rs) {
		rset_free(ts_word);
		ts_word = rs;
	}
}

static void ts_cursor(led_ctx *x, struct ts_source *source)
{
	s64 row = source->state->preview ? source->state->cursor : cursor_row;
	s64 hl = opt_hl_line && source->row == row ? syn_findhl(2) : -1;
	if (hl >= 0)
		for (s64 i = 0; i < x->alen; i++)
			x->att[i] = syn_merge(x->att[i], hls[hl].att[0]);
	hl = opt_hl_word && ts_word ? syn_findhl(1) : -1;
	if (hl < 0)
		return;
	uint32_t len;
	const char *text = ts_read(source->state, 0, (TSPoint){source->row, 0}, &len);
	char *line = emalloc(len + 1);
	memcpy(line, text, len);
	line[len] = '\0';
	s64 pos = 0, subs[ts_word->nsubc];
	while (pos < (s64)len && rset_find(ts_word, line + pos, subs, pos ? REG_NOTBOL : 0) >= 0) {
		s64 beg = pos + subs[0], end = pos + subs[1];
		if (end > (s64)len)
			break;
		for (s64 i = 0; i < x->alen; i++) {
			s64 off = x->bound ? x->stt[i] : i;
			s64 byte = source->col + (x->r->chrs[off] - x->s0);
			s64 index = led_attidx(x, off);
			if (byte >= beg && byte < end && index >= 0)
				x->att[index] = syn_merge(x->att[index], hls[hl].att[0]);
		}
		pos = end > pos ? end : pos + uc_len(line + pos);
	}
	free(line);
}

static void ext_treesitter(led_ext *p, led_ctx *x)
{
	if (opt_syntax_hl <= 1)
		return;
	struct ts_source *source = p->usr;
	struct ts_state *s = source ? source->state : NULL;
	if (!s || syn_getft() != ts_languages[s->language].ft ||
		source->row < 0 || source->row >= s->rows || !ts_parse(s)) {
		syn_highlight(x->att, x->bound ? x->bound : x->s0, MIN(x->n, x->cterm));
		return;
	}
	ts_query_line(s, source->row);
	uint32_t base = s->offset[source->row] + source->col;
	for (s64 i = 0; i < s->capture_n; i++) {
		struct ts_capture *c = s->captures + i;
		if (c->end <= base || c->beg >= s->offset[source->row + 1])
			continue;
		for (s64 j = 0; j < x->alen; j++) {
			s64 off = x->bound ? x->stt[j] : j;
			uint32_t byte = base + (x->r->chrs[off] - x->s0);
			s64 index = led_attidx(x, off);
			if (byte >= c->beg && byte < c->end && index >= 0)
				x->att[index] = syn_merge(x->att[index], c->att);
		}
	}
	ts_cursor(x, source);
}

static s64 ts_preview_row(s64 ps)
{
	s64 row = ts_preview->beg;
	for (s64 i = 0; i < ps; i++)
		row += ts_preview->pending[i] == '\n';
	return ts_preview->cursor = row;
}

static void led_preview_current(char *text, s64 ps, s64 lncol)
{
	led_srender(text, -1, lncol, opt_left_col, opt_left_col + term_cols - lncol,
		ts_preview, ts_preview_row(ps), 0)
}

/* Preview rows use the same wrap width and character positions as buffer rows. */
static s64 led_preview_height(s64 row, s64 trow, s64 paint)
{
	char *line = ts_line(ts_preview, row);
	s64 len = line ? ts_preview->offset[row + 1] - ts_preview->offset[row] : 1;
	char *copy = emalloc(len + 4);
	memcpy(copy, line ? line : "~", len);
	memset(copy + len, 0, 4);
	s64 w = ren_wrapw(lnum_width), h = MAX(0, ren_position(copy)->cmax) / w + 1;
	if (paint)
		for (s64 k = MAX(0, -trow); k < h && trow + k < ts_winh; k++)
			led_srender(copy, ts_winy + trow + k, ts_winx + lnum_width, k * w, k * w + w,
				ts_preview, row, 0)
	rstate->s = NULL;
	free(copy);
	return h;
}

static void led_preview_wrapped(s64 current)
{
	preserve(ren_state*, rstate, rstate = rstates + 1;)
	s64 trow = led_row;
	for (s64 row = view_top_row; row < current; row++)
		trow -= led_preview_height(row, 0, 0);
	for (s64 row = view_top_row; trow < ts_winh; row++)
		if (row == current)
			trow = led_row + led_rowh;
		else
			trow += led_preview_height(row, trow, 1);
	restore(rstate)
}

static void led_preview_draw(s64 ps)
{
	s64 current = ts_preview_row(ps);
	if (led_lw) {
		led_preview_wrapped(current);
		return;
	}
	preserve(ren_state*, rstate, rstate = rstates + 1;)
	for (s64 row = view_top_row; row < view_top_row + term_rows; row++) {
		if (row == current)
			continue;
		char *line = ts_line(ts_preview, row);
		/* Pending lines share an allocation, so terminate a render copy. */
		if (line && row >= ts_preview->beg && row < ts_preview->beg + ts_preview->pending_n) {
			s64 len = ts_preview->offset[row + 1] - ts_preview->offset[row];
			char *copy = emalloc(len + 4);
			memcpy(copy, line, len);
			memset(copy + len, 0, 4);
			led_srender(copy, row - view_top_row, lnum_width, opt_left_col, opt_left_col + term_cols - lnum_width,
				ts_preview, row, 0)
			rstate->s = NULL;
			free(copy);
		} else
			led_srender(line ? line : "~", row - view_top_row, lnum_width, opt_left_col,
				opt_left_col + term_cols - lnum_width, ts_preview, row, 0)
	}
	term_pos(current - view_top_row, lnum_width);
	restore(rstate)
}

void ts_init(void)
{
	if (!led_extfind(ext_treesitter))
		led_extnew()->ext_func = ext_treesitter;
}

void led_render_source(char *s0, s64 cbeg, s64 cend, struct ts_state *source,
	s64 row, s64 col)
{
	struct ts_source data = {source, row, col};
	led_ext *p = led_extfind(ext_treesitter);
	if (p) {
		p->usr = &data;
		p->blen = sizeof(data);
	}
	led_render(s0, cbeg, cend);
	if (p) {
		p->usr = NULL;
		p->blen = 0;
	}
}

void ts_done(void)
{
	rset_free(ts_word);
	ts_word = NULL;
	while (ts_states) {
		if (ts_states->lb && ts_states->lb->ts == ts_states)
			ts_states->lb->ts = NULL;
		ts_free(ts_states);
	}
	for (s64 i = 0; i < LEN(ts_config); i++) {
		ts_query_delete(ts_config[i].query);
		free(ts_config[i].attributes);
		memset(ts_config + i, 0, sizeof(ts_config[i]));
	}
}
