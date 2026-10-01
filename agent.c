/* Embedded subzeroclaw, adapted from e39b51b8eccc1cfc35a209d728df8a32b312ddf1.
 *
 * MIT License
 *
 * Copyright (c) 2026 GenLayer Labs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
static cJSON *agent_messages;
static unsigned long agent_epoch, agent_serial;
static s64 agent_ready, agent_syncing, agent_child_status;
static s64 agent_logbuf = 3;
static char *agent_init_error;
static unsigned long agent_rounds;	/* tool rounds completed in the current run */
static unsigned long agent_tool_calls;	/* cumulative ex tool calls executed */
static size_t agent_capture_total;	/* full tool output, including clipped bytes */

/* Usage describes the last accepted response. The anchor describes its input;
 * subsequent messages/tool results are estimated, not provider token counts. */
static struct agent_usage {
	double input, output, bytes;
	s64 reported, anchored;
} agent_usage;
static s64 agent_packing, agent_pack_done;
static void agent_run(const char *input);
static s64 agent_autocompact(const char *input);

static const char agent_tools[] =
	"[{\"type\":\"function\",\"function\":{"
	"\"name\":\"ex\",\"description\":"
	"\"Execute an ex command in nextvi\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"command\":{\"type\":\"string\"}},"
	"\"required\":[\"command\"],"
	"\"additionalProperties\":false}}}]";

/* JSON bytes are only a fallback proxy. Include tools and message framing. */
static double agent_context_bytes(void)
{
	char *json = cJSON_PrintUnformatted(agent_messages);
	double bytes = (json ? strlen(json) : 0) + strlen(agent_tools) +
		24.0 * cJSON_GetArraySize(agent_messages);
	free(json);
	return bytes;
}

static double agent_tokens(void)
{
	double bytes = agent_context_bytes();
	if (agent_usage.anchored && bytes >= agent_usage.bytes)
		return agent_usage.input + (bytes - agent_usage.bytes + 2) / 3;
	return (bytes + 2) / 3;
}

static void agent_record_usage(cJSON *root, double bytes)
{
	cJSON *usage = cJSON_GetObjectItem(root, "usage");
	cJSON *input = cJSON_GetObjectItem(usage, "prompt_tokens");
	cJSON *output = cJSON_GetObjectItem(usage, "completion_tokens");
	/* Some compatible gateways use the Responses-style names. */
	if (!input)
		input = cJSON_GetObjectItem(usage, "input_tokens");
	if (!output)
		output = cJSON_GetObjectItem(usage, "output_tokens");
	agent_usage.reported = agent_usage.anchored =
		cJSON_IsNumber(input) && input->valuedouble >= 0 &&
		input->valuedouble < 1e15;
	agent_usage.input = agent_usage.reported ? input->valuedouble : 0;
	agent_usage.output = cJSON_IsNumber(output) &&
		output->valuedouble >= 0 && output->valuedouble < 1e15 ?
		output->valuedouble : -1;
	agent_usage.bytes = bytes;
}

static char nextvi_skill[] =
"Inside Nextvi, use the ex tool with a JSON object whose command \n"
"key holds an ex command.\n"
"Nextvi is not a standard vi/ex.\n"
"Use exspec command for a command list.\n"
"Use exspec with an argument for a topic or command specification.\n";

static char caveman_skill[] =
"Drop: articles (a/an/the), filler (just/really/basically/actually/simply), pleasantries\n"
"(sure/certainly/of course/happy to), hedging. Fragments OK. Short synonyms (big not\n"
"extensive, fix not \"implement a solution for\"). No tool-call narration, no decorative\n"
"tables/emoji, no dumping long raw error logs unless asked quote shortest decisive\n"
"line. Standard well-known tech acronyms OK (DB/API/HTTP); never invent new abbreviations\n"
"(cfg/impl/req/res/fn) tokenizer split them same as full word: zero token saved, reader\n"
"still decode. Full word cheaper AND clearer. No causal arrows (→) either own token,\n"
"save nothing. Technical terms exact. Code blocks unchanged. Errors quoted exact.\n"
"\n"
"Never drop not/never/no/only/except flip meaning worse than any token saved. Numbers,\n"
"units exact.\n"
"\n"
"Never ADD word to sound caveman. Compression only style never grow output. No inserted\n"
"pronoun or copula to fake broken grammar: \"when it not\" cost one token more than\n"
"\"when not\" and say same thing. Keep correct verb form when correct form cost same\n"
"\"sees\" one token, \"see\" one token, so mangle buy nothing and read worse. Same rule\n"
"as abbreviations and arrows: if caveman phrasing not shorter than plain phrasing,\n"
"use plain.\n"
"\n"
"Clarity register: mix ASD-STE100 Simplified Technical English into caveman, always.\n"
"One idea per sentence. Sentence short, target 20 words max. Active voice. Present\n"
"tense where true. One word one meaning: same term for same thing every time, no synonym\n"
"rotation. Instruction = imperative: \"Run X\", not \"X should be run\". Noun cluster\n"
"3 words max. Pronoun only with one clear referent, else repeat noun. Caveman cut\n"
"filler; STE keep what make meaning unambiguous. Conflict between them → clarity win.\n"
"\n"
"Tool calls: fire direct. No preamble, plan, or progress note before or between calls.\n"
"After result: next call direct or final answer never announce next call. Text before\n"
"call only to clarify, warn security/irreversible, or resolve ambiguity.\n"
"\n"
"Follow explicit reply-language instructions from the user or project. Otherwise preserve\n"
"the user's dominant language. Never switch because of example text or multilingual\n"
"context elsewhere. Compress the style, not the language. Every emitted line in that\n"
"language openings, pre-tool status lines, all not just final reply. ALWAYS keep technical\n"
"terms, code, API names, CLI commands, commit-type keywords (feat/fix/...), and exact\n"
"error strings verbatim unless user explicitly ask for translation.\n"
"\n"
"'Drop articles' = article languages only. Where small markers carry case/role (particles,\n"
"postpositions), keep them grammar, not filler; compress politeness/filler instead.\n";

static cJSON *agent_msg(const char *role, const char *content)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", role);
	cJSON_AddStringToObject(m, "content", content);
	return m;
}

static char *agent_text(struct lbuf *lb)
{
	sbuf_smake(sb, 256)
	for (s64 i = 0; i < lbuf_len(lb); i++)
		sbuf_str(sb, lb->ln[i])
	sbufn_ret(sb, sb->s)
}

static s64 agent_writeall(s64 fd, const char *s, size_t n)
{
	while (n) {
		ssize_t k = write(fd, s, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		s += k;
		n -= k;
	}
	return 0;
}

static void agent_output(const char *s)
{
	agent_writeall(STDOUT_FILENO, s, strlen(s));
}

static s64 agent_save(s64 i)
{
	struct buf *b = tempbufs + i;
	char *path = emalloc(strlen(b->path) + 16);
	s64 fd, ret = -1;
	sprintf(path, "%s.XXXXXX", b->path);
	fd = mkstemp(path);
	if (fd >= 0) {
		ret = lbuf_wr(b->lb, fd, 0, lbuf_len(b->lb));
		if (fsync(fd))
			ret = -1;
		if (close(fd))
			ret = -1;
		if (!ret && rename(path, b->path))
			ret = -1;
		if (!ret)
			b->mtime = mtime(b->path);
		unlink(path);
	}
	free(path);
	return ret;
}

/* Keep the old on-disk conversation in the same directory. The working
 * buffer keeps its path, so its next save creates a fresh conversation file. */
static char *agent_rotate_log(void)
{
	char *archive = emalloc(strlen(tempbufs[3].path) + 8);
	sprintf(archive, "%s.XXXXXX", tempbufs[3].path);
	s64 fd = mkstemp(archive);
	if (fd < 0) {
		free(archive);
		return NULL;
	}
	if (close(fd) || rename(tempbufs[3].path, archive)) {
		unlink(archive);
		free(archive);
		return NULL;
	}
	return archive;
}

static void agent_restore_log(char *archive)
{
	if (rename(archive, tempbufs[3].path))
		ex_print("cannot restore archived session log", msg_ft)
	else
		tempbufs[3].mtime = mtime(tempbufs[3].path);
}

static void agent_sync(struct lbuf *lb)
{
	if (!agent_ready || agent_syncing)
		return;
	for (s64 i = 3; i < 5; i++) {
		if (tempbufs[i].lb != lb)
			continue;
		if (i == 3 && agent_packing)
			agent_pack_done = 0;
		agent_syncing = 1;
		if (agent_save(i))
			ex_print("agent write failed; buffer text retained",
				msg_ft)
		agent_syncing = 0;
	}
}

static s64 agent_mkdir(char *path)
{
	struct stat st;
	for (char *p = path + 1; ; p++) {
		char c = *p;
		if (c && c != '/')
			continue;
		*p = 0;
		s64 failed = (mkdir(path, 0700) && errno != EEXIST) ||
			stat(path, &st) || !S_ISDIR(st.st_mode);
		*p = c;
		if (failed)
			return -1;
		if (!c)
			return 0;
	}
}

static void agent_history(s64 edited)
{
	agent_usage.anchored = 0;
	char *s = agent_text(tempbufs[4].lb);
	cJSON_Delete(agent_messages);
	agent_messages = cJSON_CreateArray();
	cJSON_AddItemToArray(agent_messages, agent_msg("system", s));
	free(s);
	if (edited) {
		s = agent_text(tempbufs[3].lb);
		cJSON_AddItemToArray(agent_messages, agent_msg("user", s));
		free(s);
	}
}

static void agent_init(void)
{
	const char *home = getenv("HOME");
	char *dir, *path;
	temp_open(3, "/conversation/", _ft);
	temp_open(4, "/skills/", _ft);
	lbuf_edit(tempbufs[4].lb, nextvi_skill, 0, 0, 0, 0);
	lbuf_saved(tempbufs[4].lb, 1);
	if ((!log_dir && (!home || !*home)) || (log_dir && !*log_dir)) {
		agent_init_error = "agent log directory is not configured";
		return;
	}
	dir = emalloc(strlen(log_dir ? log_dir : home) + 64);
	if (log_dir)
		strcpy(dir, log_dir);
	else
		sprintf(dir, "%s/.nextvi/logs", home);
	if (agent_mkdir(dir))
		goto fail;
	strcat(dir, "/session-XXXXXX");
	if (!mkdtemp(dir))
		goto fail;
	/* Absolute paths survive :cd during the session. */
	if (*dir != '/') {
		char *cwd = getcwd(NULL, 0);
		if (!cwd)
			goto fail;
		path = emalloc(strlen(cwd) + strlen(dir) + 2);
		sprintf(path, "%s/%s", cwd, dir);
		free(cwd);
		free(dir);
		dir = path;
	}
	for (s64 i = 3; i < 5; i++) {
		path = emalloc(strlen(dir) + 32);
		sprintf(path, "%s/%s", dir, i == 3 ? "conversation" : "skills");
		free(tempbufs[i].path);
		tempbufs[i].path = path;
		tempbufs[i].plen = strlen(path);
		if (agent_save(i))
			goto fail;
	}
	agent_ready = 1;
	agent_history(0);
	free(dir);
	return;
fail:
	free(dir);
	agent_init_error = "cannot create agent session files";
}

static void agent_log(const char *role, const char *text)
{
	struct lbuf *lb = tempbufs[agent_logbuf].lb;
	sbuf_smake(sb, 256)
	sbuf_str(sb, role)
	sbuf_chr(sb, '\n')
	sbuf_str(sb, text);
	if (sb->s[sb->s_n-1] != '\n')
		sbuf_chr(sb, '\n')
	sbuf_chr(sb, '\n')
	sbuf_nul(sb)
	agent_output(sb->s);
	lb->useq++;
	lbuf_edit(lb, sb->s, lbuf_len(lb), lbuf_len(lb), 0, 0);
	lb->useq++;
	free(sb->s);
}

static void agent_key(s64 c)
{
	if (c == TK_CTL('c'))
		agent_cancel = 1;
	if (c == 27 && !agent_cancel)
		agent_cancel = 2;
	if (c == TK_CTL('o'))
		agent_pause = 1;
	if (c == TK_CTL('l'))
		term_winch = 1;
}

static s64 agent_interrupted(void)
{
	return agent_tool && (agent_cancel || agent_pause || agent_input_blocked);
}

static s64 agent_boundary(void)
{
	if (agent_input_blocked)
		return 1;
	preserve(s64, agent_tool, agent_tool = 0;)
	while (term_inbuf_pos < term_inbuf_count)
		agent_key(term_read(0));
	while (poll(&term_ufd, 1, 0) > 0 && term_ufd.revents & POLLIN)
		agent_key(term_read(0));
	restore(agent_tool)
	return agent_cancel || agent_pause;
}

static void agent_capture_add(const char *s, s64 n)
{
	agent_capture_total += n;
	if (opt_agent_guardrails == 2)
		n = MIN(n, MAX(0, 4097 - agent_capture->s_n));
	sbuf_mem(agent_capture, s, n)
}

/* Tool output may contain NULs or malformed UTF-8. cJSON strings are
 * NUL-terminated and cJSON_Print does not validate UTF-8, so convert raw
 * bytes to readable, valid UTF-8 before adding them to history or the log. */
static char *agent_safe_text(const char *s, size_t n)
{
	sbuf_smake(text, n + 1)
	static const char hex[] = "0123456789ABCDEF";
	for (size_t i = 0; i < n; ) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
			sbuf_chr(text, c)
			i++;
			continue;
		}
		/* Accept only shortest-form UTF-8 scalar values. */
		size_t len = c >= 0xc2 && c <= 0xdf ? 2 :
			c >= 0xe0 && c <= 0xef ? 3 :
			c >= 0xf0 && c <= 0xf4 ? 4 : 0;
		if (len && len <= n - i) {
			unsigned char b1 = (unsigned char)s[i+1];
			s64 valid = b1 >= 0x80 && b1 <= 0xbf &&
				(c != 0xe0 || b1 >= 0xa0) &&
				(c != 0xed || b1 < 0xa0) &&
				(c != 0xf0 || b1 >= 0x90) &&
				(c != 0xf4 || b1 <= 0x8f);
			for (size_t j = 2; valid && j < len; j++)
				valid = (unsigned char)s[i+j] >= 0x80 &&
					(unsigned char)s[i+j] <= 0xbf;
			if (valid) {
				sbuf_mem(text, s + i, len)
				i += len;
				continue;
			}
		}
		sbuf_chr(text, '\\')
		sbuf_chr(text, 'x')
		sbuf_chr(text, hex[c >> 4])
		sbuf_chr(text, hex[c & 15])
		i++;
	}
	sbuf_nul(text)
	return text->s;
}

/* Interactive shells can put each job in a separate process group. */
static void agent_killtree(pid_t pid)
{
	struct child { pid_t pid, parent; s64 selected; } *children = NULL;
	s64 n = 0, cap = 0, changed;
	long child, parent;
	pid_t group;
	FILE *ps;
	kill(pid, SIGSTOP);
	ps = popen("ps -e -o pid= -o ppid=", "r");
	if (ps) {
		while (fscanf(ps, "%ld %ld", &child, &parent) == 2) {
			if (child <= 0 || child == getpid())
				continue;
			if (n == cap) {
				cap += 128;
				children = erealloc(children, cap * sizeof(*children));
			}
			children[n++] = (struct child){child, parent, child == pid};
		}
		pclose(ps);
		do {
			changed = 0;
			for (s64 i = 0; i < n; i++) {
				if (children[i].selected)
					continue;
				for (s64 j = 0; j < n; j++) {
					if (!children[j].selected || children[i].parent != children[j].pid)
						continue;
					children[i].selected = changed = 1;
					kill(children[i].pid, SIGSTOP);
					break;
				}
			}
		} while (changed);
		for (s64 i = 0; i < n; i++) {
			if (!children[i].selected)
				continue;
			group = getpgid(children[i].pid);
			if (group > 0 && group != getpgrp())
				kill(-group, SIGKILL);
			kill(children[i].pid, SIGKILL);
		}
	}
	free(children);
	kill(pid, SIGKILL);
}

/* Keep one excess byte to detect truncated output; continue draining the pipe. */
/* Use file-backed stdin; poll output and terminal together. */
static sbuf *agent_process(char **argv, sbuf *input, s64 *status, s64 http,
	s64 limited, sbuf **errout)
{
	FILE *in = tmpfile();
	struct pollfd fds[3];
	int output[2] = {-1, -1}, error[2] = {-1, -1}, pid, done = 0, st = 0,
		killed = 0, tidx = http ? 2 : 1, interactive = !http && opt_interactive_shell;
	void (*old_ttou)(int) = SIG_DFL, (*old_ttin)(int) = SIG_DFL;
	char buf[4097];
	sbuf *sb, *eb = NULL;
	if (errout)
		*errout = NULL;
	if (!in)
		return NULL;
	if (input && agent_writeall(fileno(in), input->s, input->s_n)) {
		fclose(in);
		return NULL;
	}
	rewind(in);
	if (pipe(output) || (http && pipe(error))) {
		fclose(in);
		if (output[0] >= 0)
			close(output[0]);
		if (output[1] >= 0)
			close(output[1]);
		if (error[0] >= 0)
			close(error[0]);
		if (error[1] >= 0)
			close(error[1]);
		return NULL;
	}
	if (interactive) {
		old_ttou = signal(SIGTTOU, SIG_IGN);
		old_ttin = signal(SIGTTIN, SIG_IGN);
	}
	pid = fork();
	if (!pid) {
		/* Interactive startup requires the editor's foreground group. */
		if (!interactive && setpgid(0, 0) < 0)
			_exit(127);
		dup2(fileno(in), STDIN_FILENO);
		dup2(output[1], STDOUT_FILENO);
		dup2(http ? error[1] : output[1], STDERR_FILENO);
		close(output[0]);
		close(output[1]);
		if (http) {
			close(error[0]);
			close(error[1]);
		}
		fclose(in);
		execvp(argv[0], argv);
		_exit(127);
	}
	fclose(in);
	close(output[1]);
	if (http)
		close(error[1]);
	if (pid < 0) {
		if (interactive) {
			signal(SIGTTOU, old_ttou);
			signal(SIGTTIN, old_ttin);
		}
		close(output[0]);
		if (http)
			close(error[0]);
		return NULL;
	}
	if (!interactive)
		setpgid(pid, pid);
	fds[0].fd = output[0];
	fds[0].events = POLLIN;
	if (http) {
		fds[1].fd = error[0];
		fds[1].events = POLLIN;
	}
	fds[tidx] = term_ufd;
	sbuf_make(sb, 4096)
	if (http)
		sbuf_make(eb, 4096)
	while (!done || fds[0].fd >= 0 || (http && fds[1].fd >= 0)) {
		if ((agent_cancel || (http && agent_pause)) && !killed) {
			if (interactive && !done)
				agent_killtree(pid);
			else if (!interactive)
				kill(-pid, SIGKILL);
			if (!done && !interactive)
				kill(pid, SIGKILL); /* also covers cancellation before setpgid */
			for (s64 i = 0; i < (http ? 2 : 1); i++) {
				if (fds[i].fd >= 0)
					close(fds[i].fd);
				fds[i].fd = -1;
			}
			killed = 1;
		}
		s64 n = poll(fds, tidx + 1, 100);
		if (n < 0 && errno != EINTR) {
			agent_cancel = 1;
			continue;
		}
		for (s64 i = 0; i < (http ? 2 : 1); i++) {
			if (fds[i].fd >= 0 &&
					fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				s64 nr = read(fds[i].fd, buf, sizeof(buf)-1);
				if (nr > 0) {
					sbuf *dest = i ? eb : sb;
					if (i == 0 && limited) {
						s64 kept = MIN(nr, MAX(0, 4097 - dest->s_n));
						if (agent_capture)
							agent_capture_total += nr - kept;
						nr = kept;
					}
					sbuf_mem(dest, buf, nr)
				} else if (!nr || errno != EINTR) {
					close(fds[i].fd);
					fds[i].fd = -1;
				}
			}
		}
		if (fds[tidx].revents & POLLIN) {
			pid_t foreground = interactive ? tcgetpgrp(term_ufd.fd) : -1;
			if (foreground > 0 && foreground != getpgrp())
				tcsetpgrp(term_ufd.fd, getpgrp());
			preserve(s64, agent_tool, agent_tool = 0;)
			agent_key(term_read(0));
			restore(agent_tool)
			if (foreground > 0 && foreground != getpgrp() && !agent_cancel)
				tcsetpgrp(term_ufd.fd, foreground);
		}
		if (fds[tidx].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			agent_cancel = 1;
			fds[tidx].fd = -1;
		}
		if (!done)
			done = waitpid(pid, &st, WNOHANG) == pid;
	}
	*status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
	if (interactive) {
		tcsetpgrp(term_ufd.fd, getpgrp());
		if (old_ttou != SIG_ERR)
			signal(SIGTTOU, old_ttou);
		if (old_ttin != SIG_ERR)
			signal(SIGTTIN, old_ttin);
	}
	if (http) {
		sbuf_nul(eb)
		if (errout)
			*errout = eb;
		else {
			free(eb->s);
			free(eb);
		}
	}
	sbufn_ret(sb, sb)
}

static sbuf *agent_shell(char *cmd, sbuf *input, s64 oproc, s64 *status)
{
	char *sh = getenv("SHELL"),
	     *argv[] = {sh && *sh ? sh : "sh",
		opt_interactive_shell ? "-i" : "-c", opt_interactive_shell ? "-c" : cmd,
		opt_interactive_shell ? cmd : NULL, NULL};
	s64 st;
	sbuf *out = agent_process(argv, input, &st, 0, !oproc && opt_agent_guardrails == 2,
		NULL);
	if (!out) {
		agent_child_status = 1;
		return NULL;
	}
	if (status)
		*status = st;
	if (st)
		agent_child_status = st;
	if (agent_cancel) {
		sbuf_free(out)
		return NULL;
	}
	if (agent_capture && !oproc)
		agent_capture_add(out->s, out->s_n);
	return out;
}

static void agent_http_failure(char *body, char *stderr_text)
{
	cJSON *root = body ? cJSON_ParseWithOpts(body, NULL, 0) : NULL;
	cJSON *api_error = cJSON_GetObjectItem(root, "error");
	cJSON *message = cJSON_GetObjectItem(api_error, "message");
	if (cJSON_IsString(message) && *message->valuestring)
		agent_log("RESULT", message->valuestring);
	else if (stderr_text && *stderr_text)
		agent_log("RESULT", stderr_text);
	else
		agent_log("RESULT", "HTTP request failed");
	cJSON_Delete(root);
}

static cJSON *agent_config(void)
{
	cJSON *req = cJSON_ParseWithOpts(request_extra, NULL, 1);
	if (!cJSON_IsObject(req) || !api_key || !*api_key ||
			strpbrk(api_key, "\r\n") || !endpoint ||
			(strncmp(endpoint, "http://", 7) &&
			strncmp(endpoint, "https://", 8)) ||
			request_timeout <= 0 || max_tool_rounds <= 0) {
		cJSON_Delete(req);
		return NULL;
	}
	/* Remove duplicates so providers cannot interpret a shadow value. */
	const char *owned[] = {"messages", "tools", "stream"};
	for (s64 i = 0; i < LEN(owned); i++)
		while (cJSON_GetObjectItem(req, owned[i]))
			cJSON_DeleteItemFromObject(req, owned[i]);
	return req;
}

static char *agent_http(cJSON *req, s64 *st, char **stderr_text)
{
	char hdrpath[] = "/tmp/nextvi-header-XXXXXX", timeout[32], hdrarg[80];
	s64 fd = mkstemp(hdrpath);
	sbuf *out = NULL;
	sbuf *err = NULL;
	if (stderr_text)
		*stderr_text = NULL;
	sbuf_smake(hdr, 128)
	sbuf_str(hdr, "Authorization: Bearer ")
	sbuf_str(hdr, api_key)
	sbuf_chr(hdr, '\n')
	if (fd < 0)
		goto ret;
	if (agent_writeall(fd, hdr->s, hdr->s_n)) {
		close(fd);
		goto ret;
	}
	close(fd);
	snprintf(timeout, sizeof(timeout), "%ld", request_timeout);
	sprintf(hdrarg, "@%s", hdrpath);
	char *argv[] = {
		"curl", "--disable", "--silent", "--show-error",
		"--fail-with-body", "--max-time", timeout, "--header", hdrarg,
		"--header", "Content-Type: application/json",
		"--data-binary", "@-", "--url", endpoint, NULL
	};
	sbuf body;
	body.s = cJSON_PrintUnformatted(req);
	body.s_n = strlen(body.s);
	out = agent_process(argv, &body, st, 1, 0, &err);
	free(body.s);
ret:
	unlink(hdrpath);
	free(hdr->s);
	if (err) {
		if (stderr_text)
			*stderr_text = err->s;
		else
			free(err->s);
		free(err);
	}
	if (!out) {
		*st = -1;
		return NULL;
	}
	char *s = out->s;
	free(out);
	return s;
}

static char *agent_snapshot(char *loc)
{
	s64 beg = 0, end = lbuf_len(xb), o1 = -1, o2 = -1;
	char info[160];
	sbuf text;
	if (loc && ex_region(loc, &beg, &end, &o1, &o2)) {
		if (lbuf_len(xb) || strcmp(loc, "%"))
			return NULL;
		beg = end = 0;
	}
	s64 last = MAX(0, end-1);
	if (o1 >= 0)
		o1 = MIN(o1, lbuf_get(xb, beg) ?
			uc_slen(lbuf_get(xb, beg)) : 0);
	if (o2 >= 0)
		o2 = MIN(o2, lbuf_get(xb, last) ?
			uc_slen(lbuf_get(xb, last)) : 0);
	s64 endoff = o2 >= 0 ? o2 :
		(lbuf_get(xb, last) ? uc_slen(lbuf_get(xb, last)) : 0);
	sbuf_smake(sb, 256)
	snprintf(info, sizeof(info), "Editor snapshot\nbuffer %ld\nname ",
		istempbuf(cur_buf) ?
			(s64)(tempbufs-cur_buf-1) : (s64)(cur_buf-bufs));
	sbuf_str(sb, info)
	sbuf_str(sb, xb_path)
	snprintf(info, sizeof(info),
		"\nrange %ld;%ld,%ld;%ld\n",
		end ? beg+1 : 0, MAX(o1, 0), end, endoff);
	sbuf_str(sb, info)
	lbuf_region(xb, &text, beg, MAX(o1, 0), last, o2);
	sbuf_str(sb, text.s)
	free(text.s);
	sbufn_ret(sb, sb->s)
}

static void agent_resize(void)
{
	struct winsize win;
	if (!ioctl(term_ufd.fd, TIOCGWINSZ, &win)) {
		if (win.ws_col)
			term_cols = win.ws_col;
		if (win.ws_row)
			term_rows = win.ws_row;
	}
	term_winch = 0;
	term_resized++;
}

/* Reconstruct only a screenful, never replay the entire log to scrollback. */
static void agent_redraw(const char *draft)
{
	char *log = agent_text(tempbufs[agent_logbuf].lb);
	sbuf_smake(sb, 256)
	sbuf_str(sb, log);
	if (draft)
		sbuf_str(sb, draft);
	sbuf_nul(sb)
	s64 cap = MAX(2, term_rows), *starts = emalloc(sizeof(s64) * cap);
	s64 rows = 1, col = 0;
	starts[0] = 0;
	for (s64 i = 0; i < sb->s_n; ) {
		char *s = sb->s+i;
		s64 n, newline = *s == '\n', code;
		uc_code(code, s, n)
		col += *s == '\t' ? 8 - col % 8 : MAX(0, uc_wid(code));
		i += MAX(1, n);
		if (newline || col >= MAX(1, term_cols)) {
			starts[rows++ % cap] = i;
			col = 0;
		}
	}
	s64 start = starts[MAX(0, rows - MAX(1, term_rows-2)) % cap];
	term_clean();
	agent_output(sb->s + start);
	free(starts);
	free(log);
	free(sb->s);
}

/* Recurse from the ex-style conversation into vi, preserving caller state. */
static void agent_editor(void)
{
	preserve(s64, opt_startup_flags, opt_startup_flags = (opt_startup_flags | 2) & ~1;)
	preserve(s64, agent_tool, agent_tool = 0;)
	preserve(sbuf *, agent_capture, agent_capture = NULL;)
	preserve(size_t, agent_capture_total, agent_capture_total = 0;)
	preserve(s64, agent_cancel, agent_cancel = 0;)
	preserve(s64, agent_pause, agent_pause = 0;)
	preserve(s64, ex_escape, ex_escape = '\\';)
	preserve(s64, ex_separator, ex_separator = ':';)
	preserve(s64, ex_expand_char, ex_expand_char = '%';)
	preserve(s64, ex_shell_char, ex_shell_char = '!';)
	led_modeswap();
	restore(ex_shell_char)
	restore(ex_expand_char)
	restore(ex_separator)
	restore(ex_escape)
	restore(agent_pause)
	restore(agent_cancel)
	restore(agent_capture_total)
	restore(agent_capture)
	restore(agent_tool)
	restore(opt_startup_flags)
}

static void agent_refresh(void)
{
	char *s = agent_snapshot(NULL);
	cJSON_AddItemToArray(agent_messages, agent_msg("user", s));
	agent_log("USER", s);
	free(s);
}

static void agent_sequence(void)
{
	for (s64 i = 0; i < buf_count; i++)
		bufs[i].lb->useq += opt_undo_seq;
	for (s64 i = 0; i < LEN(tempbufs); i++)
		tempbufs[i].lb->useq += opt_undo_seq;
}

/* Return 1 for envelope errors, 2 for tool calls the model can correct. */
static s64 agent_response_error(cJSON *root, char *error, size_t size)
{
	cJSON *choices = cJSON_GetObjectItem(root, "choices");
	cJSON *message = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
	cJSON *role = cJSON_GetObjectItem(message, "role");
	cJSON *content = cJSON_GetObjectItem(message, "content");
	cJSON *calls = cJSON_GetObjectItem(message, "tool_calls"), *tc;
	const char *reason = NULL;
	s64 index = 0;
	if (!root)
		reason = "response: invalid JSON";
	else if (!cJSON_IsObject(root))
		reason = "response: expected an object";
	else if (!cJSON_IsArray(choices) || !cJSON_GetArraySize(choices))
		reason = "choices: expected a nonempty array";
	else if (!cJSON_IsObject(message))
		reason = "choices[0].message: expected an object";
	else if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant"))
		reason = "message.role: expected assistant";
	else if (content && !cJSON_IsNull(content) && !cJSON_IsString(content))
		reason = "message.content: expected a string or null";
	else if (calls && !cJSON_IsArray(calls))
		reason = "message.tool_calls: expected an array";
	else if (!cJSON_IsString(content) && !cJSON_GetArraySize(calls))
		reason = "message: missing text and tool calls";
	if (reason) {
		snprintf(error, size, "%s", reason);
		return 1;
	}
	cJSON_ArrayForEach(tc, calls) {
		cJSON *id = cJSON_GetObjectItem(tc, "id");
		cJSON *type = cJSON_GetObjectItem(tc, "type");
		cJSON *fn = cJSON_GetObjectItem(tc, "function");
		cJSON *name = cJSON_GetObjectItem(fn, "name");
		cJSON *args = cJSON_GetObjectItem(fn, "arguments");
		cJSON *parsed = cJSON_IsString(args) ?
			cJSON_ParseWithOpts(args->valuestring, NULL, 1) : NULL;
		cJSON *command = cJSON_GetObjectItem(parsed, "command");
		if (!cJSON_IsObject(tc))
			reason = "expected an object";
		else if (!cJSON_IsString(id) || !*id->valuestring)
			reason = "id: expected a nonempty string";
		else if (!cJSON_IsString(type) || strcmp(type->valuestring, "function"))
			reason = "type: expected function";
		else if (!cJSON_IsObject(fn))
			reason = "function: expected an object";
		else if (!cJSON_IsString(name) || strcmp(name->valuestring, "ex"))
			reason = "function.name: expected ex";
		else if (!cJSON_IsString(args))
			reason = "function.arguments: expected a JSON-encoded string";
		else if (!parsed)
			reason = "function.arguments: invalid JSON";
		else if (!cJSON_IsObject(parsed))
			reason = "function.arguments: expected a JSON object";
		else if (!cJSON_IsString(command) || !*command->valuestring)
			reason = "function.arguments.command: expected a nonempty string";
		cJSON_Delete(parsed);
		for (cJSON *prev = calls->child; !reason && prev != tc; prev = prev->next) {
			cJSON *pid = cJSON_GetObjectItem(prev, "id");
			if (cJSON_IsString(pid) && !strcmp(id->valuestring, pid->valuestring))
				reason = "id: duplicate within this response";
		}
		if (reason) {
			snprintf(error, size, "tool_calls[%ld].%s", index, reason);
			return 2;
		}
		index++;
	}
	return 0;
}

static void agent_run(const char *input)
{
	unsigned long serial = ++agent_serial, epoch = agent_epoch;
	cJSON *req, *root, *message, *calls, *tc;
	char *body, *stderr_text;
	s64 st, retries = 0, compacted = 0;
	if (agent_packing)
		agent_pack_done = 0;
	agent_cancel = agent_pause = 0;
	agent_rounds = 0;
	cJSON_AddItemToArray(agent_messages, agent_msg("user", input));
	agent_log("USER", input);
	for (s64 round = 0; ; ) {
		/* Only complete tool batches reach this boundary. Manual/automatic
		 * pack agents must never recursively trigger autocompaction. */
		/* The threshold triggers a pack, not a hard cap on its result. */
		if (compacted && agent_tokens() < opt_autocompact_tokens)
			compacted = 0;
		if (opt_autocompact_tokens && !compacted && !agent_packing && agent_logbuf == 3 &&
				agent_tokens() >= opt_autocompact_tokens) {
			if (!agent_autocompact(input) || epoch != agent_epoch || quit_state)
				return;
			/* The pack conversation was discarded. Start a fresh tool-round
			 * budget for the resumed request, without recursing into agent_run. */
			compacted = 1;
			round = retries = 0;
			agent_rounds = 0;
			serial = agent_serial;
		}
		req = agent_config();
		if (!req) {
			agent_log("RESULT", "invalid agent configuration");
			return;
		}
		cJSON_AddItemReferenceToObject(req, "messages", agent_messages);
		cJSON_AddItemToObject(req, "tools", cJSON_Parse(agent_tools));
		cJSON_AddBoolToObject(req, "stream", 0);
		double request_bytes = agent_context_bytes();
		body = agent_http(req, &st, &stderr_text);
		cJSON_Delete(req);
		if (agent_cancel) {
			free(body);
			free(stderr_text);
			if (agent_cancel != 2)
				agent_log("RESULT", "cancelled");
			return;
		}
		if (agent_pause) {
			free(body);
			free(stderr_text);
			agent_editor();
			agent_redraw(NULL);
			agent_pause = 0;
			if (serial != agent_serial ||
					epoch != agent_epoch || quit_state)
				return;
			agent_log("RESULT",
				"response skipped after recursive editing");
			agent_refresh();
			continue;
		}
		if (st || !body) {
			agent_http_failure(body, stderr_text);
			free(body);
			free(stderr_text);
			return;
		}
		free(stderr_text);
		root = cJSON_ParseWithOpts(body, NULL, 1);
		if (cJSON_GetObjectItem(root, "error")) {
			agent_http_failure(body, NULL);
			free(body);
			cJSON_Delete(root);
			return;
		}
		char error[256], diagnostic[2048], excerpt[513];
		s64 invalid = agent_response_error(root, error, sizeof(error));
		if (invalid) {
			cJSON *finish = cJSON_GetObjectItem(cJSON_GetArrayItem(
				cJSON_GetObjectItem(root, "choices"), 0), "finish_reason");
			snprintf(excerpt, sizeof(excerpt), "%.512s", body);
			cJSON *quoted = cJSON_CreateString(excerpt);
			char *encoded = cJSON_PrintUnformatted(quoted);
			snprintf(diagnostic, sizeof(diagnostic),
				"malformed assistant response: %s\n"
				"finish_reason: %.80s; rejected response (first 512 bytes): %.1100s\n"
				"%s",
				error, cJSON_IsString(finish) ? finish->valuestring : "unavailable",
				encoded ? encoded : "(unavailable)",
				retries < 2 ? "retrying automatically" :
				"retry limit reached; history retained, submit to retry");
			agent_log("RESULT", diagnostic);
			free(encoded);
			cJSON_Delete(quoted);
			/* Invalid calls never enter history or execute, even in a mixed batch. */
			if (invalid == 2) {
				snprintf(diagnostic, sizeof(diagnostic),
					"Your previous response was rejected: %s. "
					"No commands from that response were executed. "
					"Correct and resubmit the intended calls. "
					"Use tool type function, name ex, and unique nonempty call IDs. "
					"Arguments must be a JSON-encoded object with a nonempty "
					"string command, for example {\"command\":\"1,20p\"}.", error);
				cJSON_AddItemToArray(agent_messages, agent_msg("user", diagnostic));
			}
			free(body);
			cJSON_Delete(root);
			if (retries++ < 2)
				continue;
			return;
		}
		agent_record_usage(root, request_bytes);
		free(body);
		message = cJSON_GetObjectItem(cJSON_GetArrayItem(
			cJSON_GetObjectItem(root, "choices"), 0), "message");
		calls = cJSON_GetObjectItem(message, "tool_calls");
		cJSON *content = cJSON_GetObjectItem(message, "content");
		/* Complete pairs before recursive editing or submissions. */
		cJSON_AddItemToArray(agent_messages,
			cJSON_Duplicate(message, 1));
		if (opt_agent_reasoning) {
			cJSON *reasoning = cJSON_GetObjectItem(message,
				"reasoning_content");
			if (!cJSON_IsString(reasoning) || !*reasoning->valuestring)
				reasoning = cJSON_GetObjectItem(message, "reasoning");
			if (cJSON_IsString(reasoning) && *reasoning->valuestring)
				agent_log("REASONING", reasoning->valuestring);
		}
		if (cJSON_IsString(content))
			agent_log("ASSISTANT", content->valuestring);
		s64 base = cJSON_GetArraySize(agent_messages), index = 0;
		cJSON_ArrayForEach(tc, calls) {
			cJSON *result = agent_msg("tool",
				"skipped: pending execution");
			cJSON_AddStringToObject(result, "tool_call_id",
				cJSON_GetObjectItem(tc, "id")->valuestring);
			cJSON_AddItemToArray(agent_messages, result);
		}
		cJSON_ArrayForEach(tc, calls) {
			cJSON *fn = cJSON_GetObjectItem(tc, "function");
			cJSON *name = cJSON_GetObjectItem(fn, "name"),
			      *args = cJSON_GetObjectItem(fn, "arguments");
			cJSON *parsed = cJSON_IsString(args) ?
				cJSON_ParseWithOpts(args->valuestring,
					NULL, 1) : NULL;
			cJSON *command = cJSON_GetObjectItem(parsed, "command");
			char *err = NULL;
			agent_child_status = 0;
			sbuf_smake(out, 256)
			sbuf_smake(result, 256)
			agent_boundary();
			agent_log("EX", cJSON_IsString(command) ?
				command->valuestring : "invalid tool call");
			if (agent_cancel || agent_pause ||
					round == max_tool_rounds)
				err = "skipped: execution cancelled, "
					"suspended, or round limit reached";
			else if (!cJSON_IsString(name) ||
					strcmp(name->valuestring, "ex"))
				err = "unknown tool (expected ex)";
			else if (!cJSON_IsString(command) ||
					!*command->valuestring)
				err = "ex requires a nonempty command string";
			else {
				s64 savedquit = quit_state, savedqprop = quit_propagate;
				agent_input_blocked = 0;
				agent_tool = 1;
				agent_capture = out;
				agent_capture_total = 0;
				agent_child_status = 0;
				agent_sequence();
				err = ex_exec(command->valuestring);
				agent_tool_calls++;
				agent_sequence();
				if (opt_agent_guardrails == 2 && agent_capture_total > 4096) {
					char msg[128];
					snprintf(msg, sizeof(msg),
						"output guardrail: %zu > 4096 (gr 0 to bypass once)",
						agent_capture_total);
					sbuf_cut(out, 0)
					sbufn_str(out, msg)
				} else if (opt_agent_guardrails >= 0 && opt_agent_guardrails < 2)
					opt_agent_guardrails++;
				agent_capture = NULL;
				agent_tool = 0;
				if (agent_input_blocked) {
					quit_state = savedquit;
					quit_propagate = savedqprop;
					agent_input_blocked = 0;
					err = "interactive input unavailable during agent execution";
				}
			}
			if (agent_cancel)
				sbuf_str(result, "cancelled\n")
			else if (agent_pause)
				sbuf_str(result,
					"suspended; remaining commands "
					"skipped\n")
			else {
				char status[64];
				snprintf(status, sizeof(status), "status %ld\n",
					agent_child_status ?
						agent_child_status : !!err);
				sbuf_str(result, status)
			}
			if (err && err != xuerr) {
				sbuf_str(result, err)
				sbuf_chr(result, '\n')
			}
			sbuf_mem(result, out->s, out->s_n)
			char *safe = agent_safe_text(result->s, result->s_n);
			cJSON_ReplaceItemInObject(cJSON_GetArrayItem(
				agent_messages, base + index++),
				"content", cJSON_CreateString(safe));
			if (agent_cancel != 2)
				agent_log("RESULT", safe);
			free(safe);
			cJSON_Delete(parsed);
			free(out->s);
			free(result->s);
		}
		s64 has_calls = cJSON_GetArraySize(calls);
		cJSON *finish = cJSON_GetObjectItem(cJSON_GetArrayItem(
			cJSON_GetObjectItem(root, "choices"), 0), "finish_reason");
		if (agent_packing && !has_calls && cJSON_IsString(finish) &&
				!strcmp(finish->valuestring, "stop"))
			agent_pack_done = 1;
		cJSON_Delete(root);
		if (agent_cancel)
			return;
		if (agent_pause) {
			agent_editor();
			agent_redraw(NULL);
			agent_pause = 0;
			if (serial != agent_serial ||
					epoch != agent_epoch || quit_state)
				return;
			agent_refresh();
		}
		agent_rounds = round;
		if (!has_calls)
			return;
		if (round++ == max_tool_rounds) {
			agent_log("RESULT",
				"maximum tool rounds reached; "
				"submit to continue");
			return;
		}
	}
}

static void *ec_skill(char *loc, char *cmd, char *arg)
{
	char *skill_str;
	if (strcmp(cmd, "acm") == 0)
		skill_str = caveman_skill;
	struct lbuf *lb = tempbufs[4].lb;
	char *s = agent_text(lb);
	if (s) {
		char *pos = strstr(s, skill_str);
		if (pos) {
			s64 beg = 0;
			for (char *p = s; p < pos; p++)
				if (*p == '\n') beg++;
			s64 len = 0;
			for (char *p = skill_str; *p; p++)
				if (*p == '\n') len++;
			lbuf_edit(lb, NULL, beg, beg + len, 0, 0);
		} else {
			lbuf_edit(lb, skill_str, lbuf_len(lb), lbuf_len(lb), 0, 0);
		}
		free(s);
		if (agent_tool) {
			s = agent_text(lb);
			cJSON_ReplaceItemInArray(agent_messages, 0, agent_msg("system", s));
			agent_usage.anchored = 0;
			free(s);
		} else
			agent_history(1);
	}
	return NULL;
}

static void *ec_ast(char *loc, char *cmd, char *arg)
{
	char msg[256];
	s64 counts[4] = {0, 0, 0, 0};
	unsigned long sums[4] = {0, 0, 0, 0};
	const char *names[] = {"system", "user", "assistant", "tool"};
	cJSON *m;
	ex_print("agent status", msg_ft)
	snprintf(msg, sizeof(msg), "autocompact %s, %ld input tokens (%s)",
		opt_autocompact_tokens ? "on" : "off", opt_autocompact_tokens, opt_autocompact_browse ? "aco! browse" : "aco loaded log");
	ex_print(msg, msg_ft)
	if (!agent_ready) {
		ex_print(agent_init_error ? agent_init_error :
			"agent session is not running", msg_ft)
		return NULL;
	}
	snprintf(msg, sizeof(msg), "state      ready, run %lu, epoch %lu",
		agent_serial, agent_epoch);
	ex_print(msg, msg_ft)
	snprintf(msg, sizeof(msg), "activity   %lu tool calls, %lu rounds this run",
		agent_tool_calls, agent_rounds);
	ex_print(msg, msg_ft)
	if (agent_usage.reported) {
		snprintf(msg, sizeof(msg), "tokens     %.0f input (last response)",
			agent_usage.input);
		ex_print(msg, msg_ft)
		if (agent_usage.output >= 0) {
			snprintf(msg, sizeof(msg), "           %.0f output (last response)",
				agent_usage.output);
			ex_print(msg, msg_ft)
		}
	} else
		ex_print("tokens     unavailable from endpoint", msg_ft)
	if (agent_messages) {
		snprintf(msg, sizeof(msg), "next input ~%.0f tokens (%s)", agent_tokens(),
			agent_usage.anchored ? "reported input + estimated growth" :
			"estimated: JSON bytes / 3, including tools and framing");
		ex_print(msg, msg_ft)
		cJSON_ArrayForEach(m, agent_messages) {
			cJSON *role = cJSON_GetObjectItem(m, "role");
			cJSON *content = cJSON_GetObjectItem(m, "content");
			s64 r = 0;
			if (cJSON_IsString(role)) {
				if (!strcmp(role->valuestring, "user"))
					r = 1;
				else if (!strcmp(role->valuestring, "assistant"))
					r = 2;
				else if (!strcmp(role->valuestring, "tool"))
					r = 3;
			}
			counts[r]++;
			if (cJSON_IsString(content))
				sums[r] += strlen(content->valuestring);
		}
		char *json = cJSON_PrintUnformatted(agent_messages);
		snprintf(msg, sizeof(msg), "context    %d messages, %ld bytes payload",
			cJSON_GetArraySize(agent_messages),
			json ? (long)strlen(json) : 0);
		ex_print(msg, msg_ft)
		free(json);
		for (s64 r = 0; r < 4; r++) {
			snprintf(msg, sizeof(msg), "%-9s  %ld msgs, %ld bytes",
				names[r], counts[r], (long)sums[r]);
			ex_print(msg, msg_ft)
		}
	} else
		ex_print("context    no conversation yet", msg_ft)
	char *s = agent_text(tempbufs[3].lb);
	snprintf(msg, sizeof(msg), "log        %ld lines, %ld bytes",
		lbuf_len(tempbufs[3].lb), (long)strlen(s));
	ex_print(msg, msg_ft)
	free(s);
	s = agent_text(tempbufs[4].lb);
	snprintf(msg, sizeof(msg), "skills     %ld lines, %ld bytes",
		lbuf_len(tempbufs[4].lb), (long)strlen(s));
	ex_print(msg, msg_ft)
	free(s);
	snprintf(msg, sizeof(msg), "capture    %ld bytes in current tool output",
		agent_capture ? (long)agent_capture->s_n : 0);
	ex_print(msg, msg_ft)
	snprintf(msg, sizeof(msg), "limits     %ld rounds max, %ld sec timeout, guardrail %ld",
		max_tool_rounds, request_timeout, opt_agent_guardrails);
	ex_print(msg, msg_ft)
	snprintf(msg, sizeof(msg), "session    %s", tempbufs[3].path);
	ex_print(msg, msg_ft)
	return NULL;
}

static void *agent_session(char *loc, char *cmd, char *arg, s64 compact)
{
	char *scope = NULL;
	s64 key, prefix = 2, savedvis = opt_startup_flags, term_owned = !term_sbuf;
	cJSON *config;
	unsigned long epoch;
	if (strchr(cmd, '!'))
		exspec_reset();
	if (agent_init_error)
		return agent_init_error;
	if (!(config = agent_config()))
		return "invalid agent configuration";
	cJSON_Delete(config);
	if (*loc && !(scope = agent_snapshot(loc)))
		return xrerr;
	/* Snapshot the requested range before selecting the session log. */
	if (compact)
		temp_switch(3, 0);
	if (cmd[1]) {
		agent_epoch++;
		agent_serial++;
		if (cmd[1] == '!') {
			struct lbuf *lb = tempbufs[3].lb;
			agent_syncing = 1;
			lbuf_edit(lb, NULL, 0, lbuf_len(lb), 0, 0);
			lbuf_saved(lb, 1);
			agent_syncing = 0;
			s64 fd = open(tempbufs[3].path, O_WRONLY | O_TRUNC);
			if (fd < 0 || close(fd))
				ex_print("agent reset write failed; "
					"buffer cleared", msg_ft)
			tempbufs[3].row = tempbufs[3].off = tempbufs[3].top = 0;
			if (cur_buf == tempbufs+3) {
				exbuf_load(cur_buf)
			}
		}
		agent_history(cmd[1] == '~');
	}
	epoch = agent_epoch;
	preserve(s64, agent_logbuf, agent_logbuf = compact ? 2 : 3;)
	if (term_owned)
		term_init();
	if (!(savedvis & 2))
		agent_output("\n");
	opt_startup_flags = 3;
	preserve(s64, ex_escape, ex_escape = 0;)
	preserve(s64, ex_separator, ex_separator = 0;)
	preserve(s64, ex_expand_char, ex_expand_char = 0;)
	preserve(s64, ex_shell_char, ex_shell_char = 0;)
	sbuf_smake(draft, 128)
	sbuf_smake(line, 128)
	sbuf_str(line, "> ")
	ins_state is;
	ins_init(is)
	if (*arg)
		term_push(arg, strlen(arg));
	while (!quit_state && epoch == agent_epoch) {
		preserve(s64, ftidx,)
		preserve(s64, opt_startup_flags, opt_startup_flags = (opt_startup_flags | 2) & ~1;)
		syn_setft(_ft);
		key = led_prompt(line, NULL, &cur_keymap, &is, prefix, 2|LED_AGENT);
		restore(opt_startup_flags)
		restore(ftidx)
		if (key == TK_CTL('c') || !key)
			break;
		if (key == TK_CTL('o')) {
			agent_editor();
			if (epoch != agent_epoch)
				break;
			ins_init(is)
			agent_redraw(draft->s_n ? draft->s : NULL);
		} else if (key == TK_CTL('b')) {
			if (epoch != agent_epoch)
				break;
			agent_redraw(draft->s_n ? draft->s : NULL);
		} else if (key == TK_CTL('l')) {
			agent_resize();
			agent_redraw(draft->s_n ? draft->s : NULL);
		} else if (key == '\n' || key == 27) {
			sbuf_str(draft, line->s + prefix)
			if (key == '\n') {
				sbufn_chr(draft, '\n')
				agent_output("\n");
			} else if (draft->s_n || scope) {
				if (scope) {
					sbuf_chr(draft, '\n')
					sbuf_str(draft, scope)
					free(scope);
					scope = NULL;
				}
				sbuf_nul(draft)
				agent_output("\n");
				agent_run(draft->s);
				sbufn_cut(draft, 0)
				if (agent_cancel == 1)
					break;
				agent_cancel = 0;
				/* A reply is not proof of completed compaction; allow follow-ups. */
			}
			prefix = key == '\n' ? 0 : 2;
			sbufn_cut(line, 0)
			if (prefix)
				sbuf_str(line, "> ")
			ins_init(is)
		}
	}
	agent_output("\n");
	free(scope);
	free(draft->s);
	free(line->s);
	restore(ex_shell_char)
	restore(ex_expand_char)
	restore(ex_separator)
	restore(ex_escape)
	restore(agent_logbuf)
	opt_startup_flags = savedvis;
	if (term_owned)
		term_done();
	syn_setft(xb_ft);
	return NULL;
}

static void *ec_agent(char *loc, char *cmd, char *arg)
{
	return agent_session(loc, cmd, arg, 0);
}

static char *agent_compact_task(s64 browse, char *arg, s64 automatic)
{
	sbuf_smake(task, 512)
	sbuf_str(task, "The current buffer contains a log of the session.\n")
	if (browse)
		sbuf_str(task,
		"The log has not been loaded into your context. Inspect the current\n"
		"buffer using ex searches and bounded line ranges. Do not read the\n"
		"entire buffer into context.\n")
	else
		sbuf_str(task,
		"The log is in your context. Do not read the current buffer into context.\n")
	sbuf_str(task, *arg ? arg : "Summarize identifying the key goals, decisions, changes,\n"
	"constraints, and unfinished work.\n")
	sbuf_str(task,
	"\nReplace the current buffer's content with the summary using %c\n"
	"followed by literal summary text.\n")
	if (automatic)
		sbuf_str(task,
		"This is automatic compaction, not a new user task.\n"
		"Modify only the current buffer.\n"
		"Preserve the latest user request, exact identifiers, critical tool results,\n"
		"completed actions (do not repeat them), and the next action to take.\n"
		"Make the summary substantially shorter than the log. Do not execute\n"
		"the unfinished task. After replacing the buffer, reply briefly and stop.\n")
	else
		sbuf_str(task, "Return control to the user once complete.\n\033")
	sbufn_ret(task, task->s)
}

static void *ec_compact(char *loc, char *cmd, char *arg)
{
	void *ret;
	s64 browse = !strcmp(cmd, "apack!");
	unsigned long epoch = agent_epoch;
	char *task = agent_compact_task(browse, arg, 0);
	/* Indices survive buffer-array growth during tool calls. */
	s64 savedtemp = istempbuf(cur_buf);
	s64 savedbuf = savedtemp ? cur_buf - tempbufs : cur_buf - bufs;
	char *original = agent_text(tempbufs[3].lb);
	char *archive = agent_rotate_log();
	if (!archive) {
		free(original);
		free(task);
		return "cannot archive session log";
	}
	/* Detach the prior conversation so a failed pack can restore it. */
	cJSON *history = agent_messages;
	struct agent_usage usage = agent_usage;
	s64 was_packing = agent_packing, was_done = agent_pack_done;
	agent_messages = NULL;
	agent_packing = 1;
	agent_pack_done = 0;
	/* apack! resets history without clearing or importing the log. */
	ret = agent_session(loc, browse ? cmd : "a~", task, 1);
	s64 complete = agent_pack_done;
	agent_packing = was_packing;
	agent_pack_done = was_done;
	if (agent_epoch == epoch + 1) {
		struct lbuf *lb = tempbufs[3].lb;
		char *summary = agent_text(lb);
		s64 changed = strcmp(original, summary) != 0;
		if (!ret && complete && changed && !agent_save(3)) {
			agent_history(1);
			cJSON_Delete(history);
		} else {
			if (!ret)
				ret = complete && changed ? "cannot save compacted session log" :
					"compaction incomplete; original session restored";
			if (changed) {
				agent_syncing = 1;
				lbuf_edit(lb, original, 0, lbuf_len(lb), 0, 0);
				agent_syncing = 0;
			}
			agent_restore_log(archive);
			cJSON_Delete(agent_messages);
			agent_messages = history;
			agent_usage = usage;
		}
		free(summary);
	} else if (agent_epoch == epoch) {
		/* Initialization or range validation failed before the prompt. */
		agent_restore_log(archive);
		cJSON_Delete(agent_messages);
		agent_messages = history;
		agent_usage = usage;
	} else {
		/* Recursive editing deliberately started a new session. */
		if (access(tempbufs[3].path, F_OK))
			agent_save(3);
		cJSON_Delete(history);
	}
	if (savedtemp)
		temp_switch(savedbuf, 0);
	else if (savedbuf < buf_count) {
		bufs_switchwft(savedbuf)
	}
	free(archive);
	free(original);
	free(task);
	return ret;
}

/* Run the same log-editing task as apack, without entering a prompt. Keep the
 * live conversation detached until the pack agent actually replaces the log.
 * Logging goes to b-3, as in manual apack, not into the source being summarized. */
static s64 agent_autocompact(const char *input)
{
	struct lbuf *lb = tempbufs[3].lb;
	cJSON *history = agent_messages;
	struct agent_usage usage = agent_usage;
	unsigned long epoch = agent_epoch, serial = agent_serial;
	unsigned long rounds = agent_rounds;
	s64 logbuf = agent_logbuf, browse = opt_autocompact_browse;
	/* Store indices rather than pointers: a tool can grow the buffer array. */
	s64 savedtemp = istempbuf(cur_buf);
	s64 savedbuf = savedtemp ? cur_buf - tempbufs : cur_buf - bufs;
	exbuf_save(cur_buf)
	char *original = agent_text(lb);
	char *task = agent_compact_task(browse, "", 1);
	char *archive = agent_rotate_log();
	s64 ok = 0;
	if (!archive) {
		free(original);
		free(task);
		agent_log("RESULT", "autocompact failed: cannot archive session log");
		return 0;
	}

	agent_logbuf = 2;
	agent_log("RESULT", browse ? "autocompact: browsing session log" :
		"autocompact: summarizing loaded session log");
	if (browse)
		exspec_reset();
	agent_messages = NULL;
	agent_history(!browse);
	agent_packing = 1;
	agent_pack_done = 0;
	temp_switch(3, 0);
	agent_run(task);
	agent_packing = 0;
	free(task);

	/* Recursive editing may have deliberately replaced the session. Never
	 * overwrite that newer history or log with the saved conversation. */
	if (epoch != agent_epoch || agent_serial != serial + 1) {
		cJSON_Delete(history);
		goto done;
	}
	char *summary = agent_text(lb);
	char *text = summary;
	while (isspace((unsigned char)*text))
		text++;
	if (agent_pack_done && !agent_cancel && !agent_pause && !quit_state &&
			*text && strcmp(original, summary) &&
			strlen(summary) < strlen(original) && !agent_save(3)) {
		agent_history(1);
		sbuf_smake(resume, 256)
		sbuf_str(resume, "Automatic compaction is complete. The preceding log summary\n"
			"is working memory, not a new task. Continue the request below, using\n"
			"the summary to avoid repeating completed actions or tool calls.\n\n")
		sbuf_str(resume, input)
		sbuf_nul(resume)
		cJSON_AddItemToArray(agent_messages, agent_msg("user", resume->s));
		free(resume->s);
		ok = 1;
	}
	free(summary);
	if (ok) {
		cJSON_Delete(history);
		agent_log("RESULT", "autocompact complete; resuming request");
	} else {
		cJSON_Delete(agent_messages);
		agent_messages = history;
		agent_usage = usage;
		agent_syncing = 1;
		lbuf_edit(lb, original, 0, lbuf_len(lb), 0, 0);
		agent_syncing = 0;
		agent_restore_log(archive);
		agent_log("RESULT", "autocompact failed or cancelled; "
			"original history retained. Compact manually or retry.");
	}
	done:
	/* A recursive session may have replaced the conversation while packing. */
	if (epoch != agent_epoch && access(tempbufs[3].path, F_OK))
		agent_save(3);
	if (savedtemp)
		temp_switch(savedbuf, 0);
	else if (savedbuf < buf_count) {
		bufs_switchwft(savedbuf)
	}
	agent_rounds = rounds;
	agent_logbuf = logbuf;
	free(original);
	free(archive);
	return ok;
}
