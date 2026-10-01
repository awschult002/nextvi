/* lsp.c - Language Server Protocol client for nextvi */
#include "jsmn.h"
#include <errno.h>

#define LSP_DIAG_MAX	64
#define LSP_SRV_MAX	8
#define LSP_RBUF_MAX	65536
#define LSP_DOCS_MAX	16
#define LSP_FILES_MAX	(LSP_SRV_MAX * 4)
#define LSP_PATH_MAX	1024
/* percent-encoding can triple a path, plus "file://" and the terminator */
#define LSP_URI_MAX	(LSP_PATH_MAX * 6 + 8)

typedef struct {
	s64 line, col, severity;
	char msg[256];
} lsp_diag;

typedef struct {
	char path[LSP_PATH_MAX];
	lsp_diag *d;
	s64 n, cap;
} lsp_diagfile;

typedef struct {
	char path[LSP_PATH_MAX];
	s64 version;
	s64 seq;		/* lbuf edseq at last sync */
} lsp_doc;

typedef struct {
	char ft[32], cmd[256];
	s64 pid, in_fd, out_fd;
	char rbuf[LSP_RBUF_MAX];
	s64 rbuf_n;
	s64 response_ready, pending_id;
	char *response_json;
	s64 initialized, init_id, next_id;
	s64 broken;		/* start or initialize failed; don't retry blindly */
	lsp_doc docs[LSP_DOCS_MAX];
	s64 ndocs;
} lsp_server;

s64 lsp_nfds = 0;
s64 lsp_fds[LSP_NFDS_MAX];
s64 lsp_dirty = 0;		/* diagnostics changed; main loop must redraw */
s64 lsp_wake = 0;		/* term_read may return 0 to report lsp_dirty */

static lsp_server lsp_srvs[LSP_SRV_MAX];
static s64 lsp_nsrvs = 0;
static lsp_diagfile lsp_diagfiles[LSP_FILES_MAX];
static s64 lsp_ndiagfiles = 0;

static void lsp_open_lb(const char *path, const char *ft, struct lbuf *lb);

static void lsp_srv_reset(lsp_server *srv);

/* open every already-loaded buffer of this filetype */
static void lsp_open_ft(const char *ft)
{
	s64 i;
	for (i = 0; i < buf_count; i++)
		if (bufs[i].ft && bufs[i].path && bufs[i].path[0] &&
				!strcmp(bufs[i].ft, ft))
			lsp_open_lb(bufs[i].path, bufs[i].ft, bufs[i].lb);
}

void lsp_register(const char *ft, const char *cmd)
{
	s64 i;
	lsp_server *srv;
	for (i = 0; i < lsp_nsrvs; i++) {
		if (!strcmp(lsp_srvs[i].ft, ft)) {
			srv = &lsp_srvs[i];
			if (strcmp(srv->cmd, cmd))
				lsp_srv_reset(srv);
			snprintf(srv->cmd, sizeof(srv->cmd), "%s", cmd);
			srv->broken = 0;	/* re-registering retries a dead cmd */
			lsp_open_ft(ft);
			return;
		}
	}
	if (lsp_nsrvs >= LSP_SRV_MAX)
		return;
	srv = &lsp_srvs[lsp_nsrvs++];
	snprintf(srv->ft, sizeof(srv->ft), "%s", ft);
	snprintf(srv->cmd, sizeof(srv->cmd), "%s", cmd);
	srv->pid = -1;
	srv->in_fd = -1;
	srv->out_fd = -1;
	lsp_open_ft(ft);
}

void lsp_list(void)
{
	s64 i;
	char buf[512];
	for (i = 0; i < lsp_nsrvs; i++) {
		snprintf(buf, sizeof(buf), "lsp %.31s %.255s [%s]",
			lsp_srvs[i].ft, lsp_srvs[i].cmd,
			lsp_srvs[i].pid > 0 ? "running" :
			lsp_srvs[i].broken ? "broken" : "stopped");
		ex_print(buf, bar_ft)
	}
	if (!lsp_nsrvs)
		ex_print("lsp: no servers registered", bar_ft)
}

static lsp_server *lsp_srv_for_ft(const char *ft)
{
	s64 i;
	for (i = 0; i < lsp_nsrvs; i++)
		if (!strcmp(lsp_srvs[i].ft, ft))
			return &lsp_srvs[i];
	return NULL;
}

static s64 lsp_doc_find(lsp_server *srv, const char *path)
{
	s64 i;
	for (i = 0; i < srv->ndocs; i++)
		if (!strcmp(srv->docs[i].path, path))
			return i;
	return -1;
}

static lsp_server *lsp_srv_for_path(const char *path)
{
	s64 i;
	for (i = 0; i < lsp_nsrvs; i++)
		if (lsp_srvs[i].pid > 0 && lsp_doc_find(&lsp_srvs[i], path) >= 0)
			return &lsp_srvs[i];
	return NULL;
}

static lsp_diagfile *lsp_diagfile_find(const char *path)
{
	s64 i;
	for (i = 0; i < lsp_ndiagfiles; i++)
		if (!strcmp(lsp_diagfiles[i].path, path))
			return &lsp_diagfiles[i];
	return NULL;
}

static lsp_diagfile *lsp_diagfile_for_path(const char *path)
{
	lsp_diagfile *df = lsp_diagfile_find(path);
	if (df)
		return df;
	if (lsp_ndiagfiles >= LSP_FILES_MAX)
		return NULL;
	snprintf(lsp_diagfiles[lsp_ndiagfiles].path,
		sizeof(lsp_diagfiles[0].path), "%s", path);
	lsp_diagfiles[lsp_ndiagfiles].d = NULL;
	lsp_diagfiles[lsp_ndiagfiles].n = 0;
	lsp_diagfiles[lsp_ndiagfiles].cap = 0;
	return &lsp_diagfiles[lsp_ndiagfiles++];
}

static lsp_server *lsp_find_srv_for_fd(s64 fd)
{
	s64 i;
	for (i = 0; i < lsp_nsrvs; i++)
		if (lsp_srvs[i].out_fd == fd)
			return &lsp_srvs[i];
	return NULL;
}

static void lsp_fd_del(s64 fd)
{
	s64 i;
	for (i = 0; i < lsp_nfds; i++) {
		if (lsp_fds[i] == fd) {
			lsp_nfds--;
			memmove(lsp_fds + i, lsp_fds + i + 1,
				(lsp_nfds - i) * sizeof(lsp_fds[0]));
			return;
		}
	}
}

/* a server killed here rarely exits before waitpid() is called, so collect
 * the leftovers on the next visit instead of leaving zombies behind */
static s64 lsp_zpids[LSP_SRV_MAX * 4];
static s64 lsp_nzpids = 0;

static void lsp_reap(void)
{
	s64 i;
	for (i = 0; i < lsp_nzpids; i++) {
		if (waitpid(lsp_zpids[i], NULL, WNOHANG) != 0) {
			lsp_nzpids--;
			memmove(lsp_zpids + i, lsp_zpids + i + 1,
				(lsp_nzpids - i) * sizeof(lsp_zpids[0]));
			i--;
		}
	}
}

/* drop every trace of a stopped server; the documents have to go as well,
 * else a later request would send didChange for a document nobody opened */
static void lsp_srv_reset(lsp_server *srv)
{
	s64 i;
	lsp_diagfile *df;
	lsp_reap();
	if (srv->out_fd >= 0) {
		lsp_fd_del(srv->out_fd);
		close(srv->out_fd);
	}
	if (srv->in_fd >= 0)
		close(srv->in_fd);
	if (srv->pid > 0) {
		kill(srv->pid, SIGTERM);
		if (!waitpid(srv->pid, NULL, WNOHANG) &&
				lsp_nzpids < LEN(lsp_zpids))
			lsp_zpids[lsp_nzpids++] = srv->pid;
	}
	/* its diagnostics are gone with it; nothing will ever refresh them */
	for (i = 0; i < srv->ndocs; i++) {
		if ((df = lsp_diagfile_find(srv->docs[i].path)) && df->n) {
			df->n = 0;
			lsp_dirty = 1;
		}
	}
	srv->in_fd = -1;
	srv->out_fd = -1;
	srv->pid = -1;
	srv->initialized = 0;
	srv->ndocs = 0;
	srv->rbuf_n = 0;
	srv->rbuf[0] = '\0';
	srv->response_ready = 0;
	srv->pending_id = 0;
	free(srv->response_json);
	srv->response_json = NULL;
}

static void lsp_json_escape(const char *src, sbuf *sb)
{
	while (*src) {
		unsigned char c = (unsigned char)*src;
		if (c == '"') {
			sbuf_mem(sb, "\\\"", 2)
		} else if (c == '\\') {
			sbuf_mem(sb, "\\\\", 2)
		} else if (c == '\n') {
			sbuf_mem(sb, "\\n", 2)
		} else if (c == '\r') {
			sbuf_mem(sb, "\\r", 2)
		} else if (c == '\t') {
			sbuf_mem(sb, "\\t", 2)
		} else if (c < 0x20) {
			char esc[7];
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			sbuf_mem(sb, esc, 6)
		} else {
			sbuf_chr(sb, *src)
		}
		src++;
	}
}

/* percent-encode everything but the unreserved set, so that the uri is
 * both a valid file uri and a valid json string without further escaping */
static void lsp_uri_encode(const char *s, char *out, s64 n)
{
	static const char hex[] = "0123456789ABCDEF";
	s64 i = 0;
	for (; *s && i + 3 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '-' || c == '.' || c == '_' ||
				c == '~' || c == '/') {
			out[i++] = c;
		} else {
			out[i++] = '%';
			out[i++] = hex[c >> 4];
			out[i++] = hex[c & 15];
		}
	}
	out[i] = '\0';
}

static s64 lsp_hexval(s64 c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void lsp_uri_decode(char *s)
{
	char *w = s;
	s64 h1, h2;
	while (*s) {
		if (s[0] == '%' && (h1 = lsp_hexval(s[1])) >= 0 &&
				(h2 = lsp_hexval(s[2])) >= 0) {
			*w++ = h1 << 4 | h2;
			s += 3;
		} else
			*w++ = *s++;
	}
	*w = '\0';
}

static void lsp_uri_from_path(const char *path, char *out, s64 n)
{
	char abs[LSP_PATH_MAX * 2];
	char cwd[LSP_PATH_MAX];
	if (path[0] == '/' || !getcwd(cwd, sizeof(cwd)))
		snprintf(abs, sizeof(abs), "%s", path);
	else
		snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
	if (n < 8)
		return;
	memcpy(out, "file://", 7);
	lsp_uri_encode(abs, out + 7, n - 7);
}

static void lsp_path_from_uri(const char *uri, char *out, s64 n)
{
	if (!strncmp(uri, "file://", 7))
		uri += 7;
	snprintf(out, n, "%s", uri);
	lsp_uri_decode(out);
}

/* strip the cwd prefix so the path matches an already-open buffer */
static void lsp_relpath(char *path)
{
	char cwd[LSP_PATH_MAX];
	s64 n;
	if (path[0] != '/' || !getcwd(cwd, sizeof(cwd)))
		return;
	n = strlen(cwd);
	if (!strncmp(path, cwd, n) && path[n] == '/')
		memmove(path, path + n + 1, strlen(path + n + 1) + 1);
}

static void lsp_print_hover(const char *s)
{
	char line[512];
	s64 i = 0;
	while (*s) {
		if (s[0] == '\\' && s[1]) {
			char c;
			switch (s[1]) {
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case '\\': c = '\\'; break;
			case '"':  c = '"';  break;
			default:   c = s[1]; break;
			}
			s += 2;
			if (c == '\n' || c == '\r') {
				line[i] = '\0';
				if (i > 0)
					ex_print(line, bar_ft)
				i = 0;
			} else if (i < (s64)sizeof(line) - 1) {
				line[i++] = c;
			}
		} else {
			if (i < (s64)sizeof(line) - 1)
				line[i++] = *s;
			s++;
		}
	}
	if (i > 0) {
		line[i] = '\0';
		ex_print(line, bar_ft)
	}
}

/* a pipe write is short whenever it fills up or a signal (SIGWINCH) lands */
static s64 lsp_write(s64 fd, const char *s, s64 len)
{
	s64 n = 0, w;
	while (n < len) {
		w = write(fd, s + n, len - n);
		if (w > 0)
			n += w;
		else if (!w || errno != EINTR)
			return -1;	/* a 0 return would spin here */
	}
	return 0;
}

static void lsp_send(lsp_server *srv, const char *json, s64 len)
{
	char hdr[64];
	s64 hlen, bad;
	void (*pipe_old)(int);
	if (srv->in_fd < 0)
		return;
	hlen = snprintf(hdr, sizeof(hdr), "Content-Length: %ld\r\n\r\n", len);
	/* writing to an exited server raises SIGPIPE, which would kill the
	 * editor, so take the signal here and treat it as a dead server */
	pipe_old = signal(SIGPIPE, SIG_IGN);
	bad = lsp_write(srv->in_fd, hdr, hlen) || lsp_write(srv->in_fd, json, len);
	signal(SIGPIPE, pipe_old);
	if (bad) {
		lsp_srv_reset(srv);
		srv->broken = 1;
	}
}

static void lsp_send_sb(lsp_server *srv, sbuf *sb)
{
	sbuf_nul(sb)
	lsp_send(srv, sb->s, sb->s_n);
}

static void lsp_sbuf_int(sbuf *sb, s64 n)
{
	char s[32];
	itoa(n, s);
	sbuf_str(sb, s)
}

static void lsp_buf_content(struct lbuf *lb, sbuf *sb)
{
	s64 i;
	for (i = 0; i < lbuf_len(lb); i++) {
		char *ln = lbuf_get(lb, i);
		lsp_json_escape(ln, sb);
	}
}

static s64 lsp_fmt_init(lsp_server *srv, sbuf *sb)
{
	s64 id = ++srv->next_id;
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"id\":")
	lsp_sbuf_int(sb, id);
	sbuf_str(sb, ",\"method\":\"initialize\",\"params\":{\"processId\":")
	lsp_sbuf_int(sb, getpid());
	sbuf_str(sb, ",\"clientInfo\":{\"name\":\"nextvi\"},\"capabilities\":{")
	sbuf_str(sb, "\"textDocument\":{\"synchronization\":{\"didSave\":true},")
	sbuf_str(sb, "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},")
	sbuf_str(sb, "\"definition\":{}},")
	sbuf_str(sb, "\"general\":{\"positionEncodings\":[\"utf-8\"]}},")
	sbuf_str(sb, "\"rootUri\":null}}")
	return id;
}

static void lsp_fmt_initialized(sbuf *sb)
{
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}")
}

static void lsp_fmt_didopen(sbuf *sb, const char *path, const char *ft,
		struct lbuf *lb)
{
	char uri[LSP_URI_MAX];
	lsp_uri_from_path(path, uri, sizeof(uri));
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",")
	sbuf_str(sb, "\"params\":{\"textDocument\":{\"uri\":\"")
	sbuf_str(sb, uri)
	sbuf_str(sb, "\",\"languageId\":\"")
	sbuf_str(sb, ft)
	sbuf_str(sb, "\",\"version\":1,\"text\":\"")
	lsp_buf_content(lb, sb);
	sbuf_str(sb, "\"}}}")
}

static void lsp_fmt_didchange(sbuf *sb, const char *path, s64 ver,
		struct lbuf *lb)
{
	char uri[LSP_URI_MAX];
	lsp_uri_from_path(path, uri, sizeof(uri));
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",")
	sbuf_str(sb, "\"params\":{\"textDocument\":{\"uri\":\"")
	sbuf_str(sb, uri)
	sbuf_str(sb, "\",\"version\":")
	lsp_sbuf_int(sb, ver);
	sbuf_str(sb, "},\"contentChanges\":[{\"text\":\"")
	lsp_buf_content(lb, sb);
	sbuf_str(sb, "\"}]}}")
}

static void lsp_fmt_didsave(sbuf *sb, const char *path)
{
	char uri[LSP_URI_MAX];
	lsp_uri_from_path(path, uri, sizeof(uri));
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",")
	sbuf_str(sb, "\"params\":{\"textDocument\":{\"uri\":\"")
	sbuf_str(sb, uri)
	sbuf_str(sb, "\"}}}")
}

/* format a textDocument/<method> request that carries a cursor position */
static s64 lsp_fmt_pos(lsp_server *srv, sbuf *sb, const char *method,
		const char *path, s64 line, s64 col)
{
	char uri[LSP_URI_MAX];
	s64 id = ++srv->next_id;
	lsp_uri_from_path(path, uri, sizeof(uri));
	sbuf_str(sb, "{\"jsonrpc\":\"2.0\",\"id\":")
	lsp_sbuf_int(sb, id);
	sbuf_str(sb, ",\"method\":\"textDocument/")
	sbuf_str(sb, method)
	sbuf_str(sb, "\",\"params\":{\"textDocument\":{\"uri\":\"")
	sbuf_str(sb, uri)
	sbuf_str(sb, "\"},\"position\":{\"line\":")
	lsp_sbuf_int(sb, line);
	sbuf_str(sb, ",\"character\":")
	lsp_sbuf_int(sb, col);
	sbuf_str(sb, "}}}")
	return id;
}

/* jsmn helpers */

/* one shared token array, grown on demand: a fixed one silently drops the
 * large replies (a file's worth of diagnostics easily passes 512 tokens) */
static jsmntok_t *lsp_toks;
static s64 lsp_ntoks;

static s64 lsp_parse(const char *json, s64 len, jsmntok_t **toks)
{
	jsmn_parser p;
	s64 n;
	if (!lsp_toks) {
		lsp_ntoks = 512;
		lsp_toks = emalloc(lsp_ntoks * sizeof(lsp_toks[0]));
	}
	while (1) {
		jsmn_init(&p);
		n = jsmn_parse(&p, json, len, lsp_toks, lsp_ntoks);
		/* a message is capped at LSP_RBUF_MAX, so no token count
		 * beyond one per two bytes of it is reachable */
		if (n != JSMN_ERROR_NOMEM || lsp_ntoks >= LSP_RBUF_MAX)
			break;
		lsp_ntoks *= 2;
		lsp_toks = erealloc(lsp_toks, lsp_ntoks * sizeof(lsp_toks[0]));
	}
	*toks = lsp_toks;
	return n;
}

static s64 lsp_tok_eq(const char *json, jsmntok_t *tok, const char *s)
{
	s64 len = tok->end - tok->start;
	return tok->type == JSMN_STRING &&
		(s64)strlen(s) == len &&
		!strncmp(json + tok->start, s, len);
}

static void lsp_tok_str(const char *json, jsmntok_t *tok, char *out, s64 n)
{
	s64 len = tok->end - tok->start;
	if (len >= n)
		len = n - 1;
	memcpy(out, json + tok->start, len);
	out[len] = '\0';
}

static s64 lsp_tok_int(const char *json, jsmntok_t *tok)
{
	char buf[32];
	lsp_tok_str(json, tok, buf, sizeof(buf));
	return atoi(buf);
}

/* skip token at index i and all its children; returns next index */
static s64 lsp_skip(jsmntok_t *toks, s64 n, s64 i)
{
	s64 end, j;
	if (i >= n)
		return n;
	if (toks[i].type == JSMN_OBJECT || toks[i].type == JSMN_ARRAY) {
		end = i + 1;
		for (j = 0; j < toks[i].size; j++) {
			if (toks[i].type == JSMN_OBJECT)
				end = lsp_skip(toks, n, end); /* key */
			end = lsp_skip(toks, n, end); /* value */
		}
		return end;
	}
	return i + 1;
}

/* find value token index for key in object at toks[obj] */
static s64 lsp_find_key(const char *json, jsmntok_t *toks, s64 n,
		s64 obj, const char *key)
{
	s64 i, j;
	if (obj >= n || toks[obj].type != JSMN_OBJECT)
		return -1;
	i = obj + 1;
	for (j = 0; j < toks[obj].size; j++) {
		if (i >= n)
			break;
		s64 val = i + 1;
		if (lsp_tok_eq(json, &toks[i], key))
			return val < n ? val : -1;
		i = lsp_skip(toks, n, val);
	}
	return -1;
}

/* collapse runs of escaped newlines (\n / \r) in place into a single space */
static void lsp_collapse_nl(char *s)
{
	char *w = s, *start = s;
	while (*s) {
		if (s[0] == '\\' && (s[1] == 'n' || s[1] == 'r')) {
			while (s[0] == '\\' && (s[1] == 'n' || s[1] == 'r'))
				s += 2;
			if (w != start && *s)	/* no leading/trailing space */
				*w++ = ' ';
		} else {
			*w++ = *s++;
		}
	}
	*w = '\0';
}

static void lsp_handle_diagnostics(const char *json, jsmntok_t *toks, s64 n)
{
	s64 params, uri_tok, diags_tok, i, di;
	s64 diag_obj, range_tok, sev_tok, msg_tok, start_tok, lt, ct;
	char path[LSP_PATH_MAX], uri[LSP_URI_MAX];
	lsp_diag *d;
	lsp_diagfile *df;

	params = lsp_find_key(json, toks, n, 0, "params");
	if (params < 0)
		return;
	uri_tok = lsp_find_key(json, toks, n, params, "uri");
	if (uri_tok < 0)
		return;
	lsp_tok_str(json, &toks[uri_tok], uri, sizeof(uri));
	lsp_path_from_uri(uri, path, sizeof(path));
	lsp_relpath(path);
	diags_tok = lsp_find_key(json, toks, n, params, "diagnostics");
	if (diags_tok < 0 || toks[diags_tok].type != JSMN_ARRAY)
		return;
	df = lsp_diagfile_for_path(path);
	if (!df)
		return;
	df->n = 0;
	/* iterate diagnostics array */
	i = diags_tok + 1;
	for (di = 0; di < toks[diags_tok].size && i < n; di++) {
		if (toks[i].type != JSMN_OBJECT) {
			i = lsp_skip(toks, n, i);
			continue;
		}
		if (df->n >= LSP_DIAG_MAX)
			break;
		diag_obj = i;
		i = lsp_skip(toks, n, diag_obj);
		if (df->n >= df->cap) {
			df->cap = df->cap ? df->cap * 2 : 8;
			if (df->cap > LSP_DIAG_MAX)
				df->cap = LSP_DIAG_MAX;
			df->d = erealloc(df->d, df->cap * sizeof(df->d[0]));
		}
		d = &df->d[df->n++];
		d->line = 0;
		d->col = 0;
		d->severity = 1;
		d->msg[0] = '\0';
		range_tok = lsp_find_key(json, toks, n, diag_obj, "range");
		sev_tok = lsp_find_key(json, toks, n, diag_obj, "severity");
		msg_tok = lsp_find_key(json, toks, n, diag_obj, "message");
		if (range_tok >= 0) {
			start_tok = lsp_find_key(json, toks, n, range_tok, "start");
			if (start_tok >= 0) {
				lt = lsp_find_key(json, toks, n, start_tok, "line");
				ct = lsp_find_key(json, toks, n, start_tok, "character");
				if (lt >= 0)
					d->line = lsp_tok_int(json, &toks[lt]);
				if (ct >= 0)
					d->col = lsp_tok_int(json, &toks[ct]);
			}
		}
		if (sev_tok >= 0)
			d->severity = lsp_tok_int(json, &toks[sev_tok]);
		if (msg_tok >= 0) {
			lsp_tok_str(json, &toks[msg_tok], d->msg, sizeof(d->msg));
			lsp_collapse_nl(d->msg);
		}
	}
	lsp_dirty = 1;
}

static void lsp_handle_notification(const char *json, jsmntok_t *toks, s64 n,
		const char *method)
{
	if (!strcmp(method, "textDocument/publishDiagnostics"))
		lsp_handle_diagnostics(json, toks, n);
}

static void lsp_handle_response(lsp_server *srv, const char *json, s64 id)
{
	if (srv->pending_id == id) {
		free(srv->response_json);
		srv->response_json = strdup(json);
		srv->response_ready = 1;
		srv->pending_id = 0;
	}
}

static void lsp_handle_message(lsp_server *srv, char *json, s64 len)
{
	jsmntok_t *toks;
	s64 n, mtok, itok;
	char method[128] = "";
	s64 id = -1;

	n = lsp_parse(json, len, &toks);
	if (n < 1 || toks[0].type != JSMN_OBJECT)
		return;
	mtok = lsp_find_key(json, toks, n, 0, "method");
	if (mtok >= 0 && toks[mtok].type == JSMN_STRING)
		lsp_tok_str(json, &toks[mtok], method, sizeof(method));
	itok = lsp_find_key(json, toks, n, 0, "id");
	if (itok >= 0 && toks[itok].type == JSMN_PRIMITIVE)
		id = lsp_tok_int(json, &toks[itok]);
	if (id >= 0 && !method[0]) {
		if (!srv->initialized && id == srv->init_id) {
			sbuf_smake(sb2, 128)
			lsp_fmt_initialized(sb2);
			lsp_send_sb(srv, sb2);
			free(sb2->s);
			srv->initialized = 1;
		}
		lsp_handle_response(srv, json, id);
	} else if (method[0]) {
		lsp_handle_notification(json, toks, n, method);
	}
}

/* pull every complete Content-Length framed message out of the read buffer */
static void lsp_dispatch_messages(lsp_server *srv)
{
	char *hdr_end, *cl, saved;
	s64 i, clen, hdr_len, total;
	while (1) {
		hdr_end = NULL;
		for (i = 0; i + 3 < srv->rbuf_n; i++) {
			if (srv->rbuf[i] == '\r' && srv->rbuf[i+1] == '\n' &&
					srv->rbuf[i+2] == '\r' && srv->rbuf[i+3] == '\n') {
				hdr_end = srv->rbuf + i + 4;
				break;
			}
		}
		if (!hdr_end)
			return;
		hdr_len = hdr_end - srv->rbuf;
		/* the buffer is kept nul terminated, so this is a string */
		cl = strstr(srv->rbuf, "Content-Length:");
		clen = cl && cl < hdr_end ? atoi(cl + 15) : -1;
		if (clen < 0 || hdr_len + clen >= LSP_RBUF_MAX) {
			/* nothing can be done with it and keeping it around would
			 * wedge the buffer for good, so drop the connection */
			lsp_srv_reset(srv);
			srv->broken = 1;
			lsp_show_msg("lsp: protocol error");
			return;
		}
		total = hdr_len + clen;
		if (srv->rbuf_n < total)
			return;	/* wait for more data */
		/* the body is nul terminated in place, over the first byte of
		 * whatever follows it, which is put back before moving on */
		saved = srv->rbuf[total];
		srv->rbuf[total] = '\0';
		lsp_handle_message(srv, hdr_end, clen);
		srv->rbuf[total] = saved;
		srv->rbuf_n -= total;
		memmove(srv->rbuf, srv->rbuf + total, srv->rbuf_n);
		srv->rbuf[srv->rbuf_n] = '\0';
	}
}

void lsp_process_fd(s64 fd)
{
	char buf[64];
	s64 r, space;
	lsp_server *srv = lsp_find_srv_for_fd(fd);
	if (!srv)
		return;
	lsp_reap();
	space = LSP_RBUF_MAX - srv->rbuf_n - 1;
	if (space <= 0) {
		/* a full buffer with no message in it is not going to grow one */
		lsp_srv_reset(srv);
		srv->broken = 1;
		lsp_show_msg("lsp: protocol error");
		return;
	}
	r = read(fd, srv->rbuf + srv->rbuf_n, space);
	if (r > 0) {
		srv->rbuf_n += r;
		srv->rbuf[srv->rbuf_n] = '\0';
		lsp_dispatch_messages(srv);
	} else if (!r || (errno != EAGAIN && errno != EINTR)) {
		/* the server is gone; forget it, otherwise every later request
		 * waits for a reply that cannot come and poll() spins on POLLHUP */
		snprintf(buf, sizeof(buf), "lsp: %.31s server exited", srv->ft);
		lsp_srv_reset(srv);
		srv->broken = 1;
		lsp_show_msg(buf);
	}
}

/* block until the reply to id arrives, ms milliseconds at the most; the
 * editor is unresponsive meanwhile, which is why every caller has a timeout */
static s64 lsp_wait_response(lsp_server *srv, s64 id, s64 ms)
{
	struct pollfd pfd;
	s64 elapsed = 0;
	srv->pending_id = id;
	srv->response_ready = 0;
	free(srv->response_json);
	srv->response_json = NULL;
	while (elapsed < ms) {
		if (srv->pid <= 0)	/* died while we waited */
			break;
		pfd.fd = srv->out_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 10) > 0 &&
				(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
			lsp_process_fd(pfd.fd);
		if (srv->response_ready)
			return 1;
		elapsed += 10;
	}
	srv->pending_id = 0;
	return 0;
}

static s64 lsp_srv_ensure(lsp_server *srv)
{
	char cmd_redir[sizeof(srv->cmd) + 16];
	char *argv[] = {"/bin/sh", "-c", cmd_redir, NULL};
	if (srv->pid > 0)
		return 1;
	if (srv->broken)	/* don't pay the initialize timeout again */
		return 0;
	if (lsp_nfds >= LSP_NFDS_MAX)	/* no slot to poll its output on */
		return 0;
	snprintf(cmd_redir, sizeof(cmd_redir), "%s 2>/dev/null", srv->cmd);
	srv->in_fd = -1;
	srv->out_fd = -1;
	srv->pid = cmd_make(argv, &srv->in_fd, &srv->out_fd);
	if (srv->pid <= 0) {
		srv->pid = -1;
		srv->broken = 1;
		return 0;
	}
	fcntl(srv->out_fd, F_SETFL,
		fcntl(srv->out_fd, F_GETFL, 0) | O_NONBLOCK);
	lsp_fds[lsp_nfds++] = srv->out_fd;
	srv->rbuf_n = 0;
	srv->rbuf[0] = '\0';
	srv->initialized = 0;
	srv->next_id = 0;
	srv->response_ready = 0;
	srv->pending_id = 0;
	free(srv->response_json);
	srv->response_json = NULL;
	sbuf_smake(sb, 512)
	srv->init_id = lsp_fmt_init(srv, sb);
	lsp_send_sb(srv, sb);
	free(sb->s);
	/* wait for the initialize response, which enables everything else */
	lsp_wait_response(srv, srv->init_id, 5000);
	if (!srv->initialized) {
		/* a bogus command forks fine and dies right away; remember it
		 * so the next request fails instantly instead of stalling 5s */
		lsp_srv_reset(srv);
		srv->broken = 1;
		return 0;
	}
	return 1;
}

static void lsp_open_lb(const char *path, const char *ft, struct lbuf *lb)
{
	s64 di;
	lsp_doc *doc;
	lsp_server *srv = lsp_srv_for_ft(ft);
	if (!srv || !lb || !path || !path[0])
		return;
	if (!lsp_srv_ensure(srv) || !srv->initialized)
		return;
	if ((di = lsp_doc_find(srv, path)) < 0) {
		if (srv->ndocs >= LSP_DOCS_MAX)
			return;
		doc = &srv->docs[srv->ndocs++];
		snprintf(doc->path, sizeof(doc->path), "%s", path);
		doc->version = 1;
		doc->seq = lb->edseq;
		sbuf_smake(sb, 4096)
		lsp_fmt_didopen(sb, path, ft, lb);
		lsp_send_sb(srv, sb);
		free(sb->s);
		return;
	}
	doc = &srv->docs[di];
	doc->seq = lb->edseq;	/* else lsp_sync() resends it right away */
	sbuf_smake(sb, 4096)
	lsp_fmt_didchange(sb, path, ++doc->version, lb);
	lsp_send_sb(srv, sb);
	free(sb->s);
}

void lsp_open(const char *path, const char *ft)
{
	lsp_open_lb(path, ft, cur_buf ? cur_buf->lb : NULL);
}

/* push buffer edits to the servers when the undo head changed (live diagnostics) */
void lsp_sync(const char *path, struct lbuf *lb)
{
	s64 i, di;
	lsp_doc *doc;
	lsp_server *srv;
	if (!lb || !path || !path[0])
		return;
	for (i = 0; i < lsp_nsrvs; i++) {
		srv = &lsp_srvs[i];
		if (srv->pid <= 0 || (di = lsp_doc_find(srv, path)) < 0)
			continue;
		doc = &srv->docs[di];
		if (doc->seq == lb->edseq)
			continue;
		doc->seq = lb->edseq;
		sbuf_smake(sb, 4096)
		lsp_fmt_didchange(sb, path, ++doc->version, lb);
		lsp_send_sb(srv, sb);
		free(sb->s);
	}
}

void lsp_save(const char *path)
{
	s64 i;
	lsp_server *srv;
	if (!path || !path[0])
		return;
	/* clangd lints its in-memory copy, so resync it before saving */
	lsp_sync(path, cur_buf ? cur_buf->lb : NULL);
	for (i = 0; i < lsp_nsrvs; i++) {
		srv = &lsp_srvs[i];
		if (srv->pid <= 0 || lsp_doc_find(srv, path) < 0)
			continue;
		sbuf_smake(sb, 256)
		lsp_fmt_didsave(sb, path);
		lsp_send_sb(srv, sb);
		free(sb->s);
	}
}

/* byte offset of the character at row,off, which is what the servers are
 * asked for with the utf-8 position encoding (uc_off() is the inverse) */
static s64 lsp_byte_offset(struct lbuf *lb, s64 row, s64 off)
{
	char *ln = lb ? lbuf_get(lb, row) : NULL;
	if (!ln || off <= 0)
		return 0;
	return uc_chr(ln, off) - ln;
}

/* find the server that has this file open, starting it and opening the file
 * on demand; on failure report which of the steps was the one that failed */
static lsp_server *lsp_srv_resolve(const char *path)
{
	char buf[384];
	char *ft = cur_buf ? cur_buf->ft : NULL;
	lsp_server *srv;
	if (!path || !path[0]) {
		lsp_show_msg("lsp: buffer has no file name");
		return NULL;
	}
	if ((srv = lsp_srv_for_path(path)))
		return srv;
	if (!ft || !*ft) {
		lsp_show_msg("lsp: buffer has no filetype; set one with :ft");
		return NULL;
	}
	if (!(srv = lsp_srv_for_ft(ft))) {
		snprintf(buf, sizeof(buf), "lsp: no server registered for ft %.31s", ft);
		lsp_show_msg(buf);
		return NULL;
	}
	if (srv->broken) {
		snprintf(buf, sizeof(buf), "lsp: %.31s server not running: %.255s",
			ft, srv->cmd);
		lsp_show_msg(buf);
		return NULL;
	}
	/* the file may predate the :lsp registration, or the server may have
	 * been restarted since, which empties its document list */
	lsp_open_lb(path, ft, cur_buf->lb);
	if (lsp_srv_for_path(path))
		return srv;
	if (srv->broken)
		snprintf(buf, sizeof(buf), "lsp: %.31s server failed to start: %.255s",
			ft, srv->cmd);
	else if (srv->ndocs >= LSP_DOCS_MAX)
		snprintf(buf, sizeof(buf), "lsp: %.31s server has too many files open",
			ft);
	else
		snprintf(buf, sizeof(buf), "lsp: cannot open %.255s with %.31s server",
			path, ft);
	lsp_show_msg(buf);
	return NULL;
}

/* send a position request and parse the reply; report and return -1 on error,
 * else return the "result" token index and fill json/toks/n for the caller */
static s64 lsp_request(const char *method, const char *path, s64 row, s64 off,
		char **json_out, jsmntok_t **toks_out, s64 *n_out)
{
	char buf[64], errmsg[256];
	char *json;
	jsmntok_t *toks;
	s64 id, col, n, errtok, msgtok, result;
	lsp_server *srv = lsp_srv_resolve(path);
	if (!srv)
		return -1;
	col = lsp_byte_offset(cur_buf ? cur_buf->lb : NULL, row, off);
	sbuf_smake(sb, 256)
	id = lsp_fmt_pos(srv, sb, method, path, row, col);
	lsp_send_sb(srv, sb);
	free(sb->s);
	if (!lsp_wait_response(srv, id, 2000) || !srv->response_json) {
		snprintf(buf, sizeof(buf), "lsp: %s timeout", method);
		lsp_show_msg(buf);
		return -1;
	}
	json = srv->response_json;
	n = lsp_parse(json, strlen(json), &toks);
	if (n < 1) {
		snprintf(buf, sizeof(buf), "lsp: %s parse error", method);
		lsp_show_msg(buf);
		return -1;
	}
	errtok = lsp_find_key(json, toks, n, 0, "error");
	if (errtok >= 0) {
		snprintf(errmsg, sizeof(errmsg), "lsp: error");
		msgtok = lsp_find_key(json, toks, n, errtok, "message");
		if (msgtok >= 0)
			lsp_tok_str(json, &toks[msgtok], errmsg, sizeof(errmsg));
		lsp_show_msg(errmsg);
		return -1;
	}
	result = lsp_find_key(json, toks, n, 0, "result");
	if (result < 0 || toks[result].type == JSMN_PRIMITIVE) {
		snprintf(buf, sizeof(buf), "lsp: no %s result", method);
		lsp_show_msg(buf);
		return -1;
	}
	*json_out = json;
	*toks_out = toks;
	*n_out = n;
	return result;
}

void lsp_hover(const char *path, s64 row, s64 off)
{
	char *json;
	jsmntok_t *toks;
	char msg[4096] = "";
	s64 n, contents, val;
	s64 result = lsp_request("hover", path, row, off, &json, &toks, &n);
	if (result < 0)
		return;
	contents = lsp_find_key(json, toks, n, result, "contents");
	if (contents < 0) {
		lsp_show_msg("lsp: no hover contents");
		return;
	}
	if (toks[contents].type == JSMN_ARRAY) {
		if (!toks[contents].size) {
			lsp_show_msg("lsp: empty hover");
			return;
		}
		contents++;	/* the first entry of the array stands in */
	}
	if (toks[contents].type == JSMN_STRING) {
		lsp_tok_str(json, &toks[contents], msg, sizeof(msg));
	} else if (toks[contents].type == JSMN_OBJECT) {
		val = lsp_find_key(json, toks, n, contents, "value");
		if (val >= 0)
			lsp_tok_str(json, &toks[val], msg, sizeof(msg));
	}
	if (msg[0])
		lsp_print_hover(msg);
	else
		lsp_show_msg("lsp: empty hover");
}

void lsp_definition(const char *path, s64 row, s64 off)
{
	char *json;
	jsmntok_t *toks;
	char uri[LSP_URI_MAX] = "", fpath[LSP_PATH_MAX];
	s64 n, obj, uri_tok, range_tok, start, lt, target_line = 0;
	s64 result = lsp_request("definition", path, row, off, &json, &toks, &n);
	if (result < 0)
		return;
	/* result can be object (Location), array of Location/LocationLink */
	obj = result;
	if (toks[result].type == JSMN_ARRAY) {
		if (!toks[result].size) {
			lsp_show_msg("lsp: definition not found");
			return;
		}
		obj = result + 1;
	}
	if (toks[obj].type == JSMN_OBJECT) {
		/* try Location: uri + range */
		uri_tok = lsp_find_key(json, toks, n, obj, "uri");
		if (uri_tok < 0) /* try LocationLink: targetUri + targetRange */
			uri_tok = lsp_find_key(json, toks, n, obj, "targetUri");
		if (uri_tok >= 0)
			lsp_tok_str(json, &toks[uri_tok], uri, sizeof(uri));
		range_tok = lsp_find_key(json, toks, n, obj, "range");
		if (range_tok < 0)
			range_tok = lsp_find_key(json, toks, n, obj, "targetRange");
		if (range_tok >= 0) {
			start = lsp_find_key(json, toks, n, range_tok, "start");
			if (start >= 0) {
				lt = lsp_find_key(json, toks, n, start, "line");
				if (lt >= 0)
					target_line = lsp_tok_int(json, &toks[lt]);
			}
		}
	}
	if (!uri[0]) {
		lsp_show_msg("lsp: definition not found");
		return;
	}
	lsp_path_from_uri(uri, fpath, sizeof(fpath));
	lsp_relpath(fpath);
	/* the response is dead once another buffer is loaded into cur_buf */
	if (strcmp(fpath, path) && !ex_edit(fpath, strlen(fpath))) {
		ex_bufpostfix(cur_buf, 1);
		syn_setft(xb_ft);
		if (xb_path && *xb_path && xb_ft)
			lsp_open(xb_path, xb_ft);
	}
	cursor_row = target_line < lbuf_len(xb) ? target_line : lbuf_len(xb) - 1;
	if (cursor_row < 0)
		cursor_row = 0;
	cursor_off = 0;
	view_top_row = cursor_row > xrows / 2 ? cursor_row - xrows / 2 : 0;
}

const char *lsp_diag_for_line(const char *path, s64 line, s64 *sev)
{
	s64 j;
	lsp_diagfile *df;
	if (!path || !path[0] || !(df = lsp_diagfile_find(path)))
		return NULL;
	for (j = 0; j < df->n; j++) {
		if (df->d[j].line == line) {
			if (sev)
				*sev = df->d[j].severity;
			return df->d[j].msg;
		}
	}
	return NULL;
}
