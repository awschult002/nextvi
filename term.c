static struct termios termios;
struct pollfd term_ufd = {STDIN_FILENO, POLLIN};
sbuf *term_sbuf;
s64 term_record;
s64 term_winch;
s64 term_resized;
s64 xrows, xcols;
u64 tibuf_pos, tibuf_cnt, tibuf_sz = 128, ticmd_pos;
unsigned char *tibuf, ticmd[4096];
u64 texec, texec_n;

void term_init(void)
{
	struct winsize win;
	struct termios newtermios;
	char *s;
	term_winch = 0;
	term_resized++;
	sbuf_make(term_sbuf, 2048)
	tcgetattr(term_ufd.fd, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~(ICANON | ISIG | ECHO);
	tcsetattr(term_ufd.fd, TCSAFLUSH, &newtermios);
	if (!ioctl(term_ufd.fd, TIOCGWINSZ, &win)) {
		xcols = win.ws_col;
		xrows = win.ws_row;
	} else {
		if ((s = getenv("LINES")))
			xrows = atoi(s);
		if ((s = getenv("COLUMNS")))
			xcols = atoi(s);
	}
	xcols = xcols ? xcols : 80;
	xrows = xrows ? xrows : 25;
}

void term_done(void)
{
	if (!term_sbuf)
		return;
	term_commit();
	sbuf_free(term_sbuf)
	term_sbuf = NULL;
	tcsetattr(term_ufd.fd, 0, &termios);
}

void term_clean(void)
{
	term_write("\x1b[2J", 4)	/* clear screen */
	term_write("\x1b[H", 3)		/* cursor topleft */
}

void term_suspend(void)
{
	if (xvis & 8)
		term_scrl()
	term_done();
	kill(0, SIGSTOP);
	term_init();
	if (xvis & 8)
		term_scrh()
}

void term_commit(void)
{
	term_write(term_sbuf->s, term_sbuf->s_n)
	sbuf_cut(term_sbuf, 0)
	term_record = 0;
}

static void term_out(char *s)
{
	if (term_record)
		sbufn_str(term_sbuf, s)
	else
		term_write(s, strlen(s))
}

void term_chr(s64 ch)
{
	char s[4] = {ch};
	term_out(s);
}

void term_kill(void)
{
	term_out("\33[K");
}

void term_room(s64 n)
{
	char cmd[64] = "\33[";
	if (!n)
		return;
	char *s = itoa(labs(n), cmd+2);
	s[0] = n < 0 ? 'M' : 'L';
	s[1] = '\0';
	term_out(cmd);
}

void term_pos(s64 r, s64 c)
{
	char buf[64] = "\r\33[", *s;
	if (r < 0) {
		memcpy(itoa(MAX(0, c), buf+3), c > 0 ? "C" : "D", 2);
		term_out(buf);
	} else {
		s = itoa(r + 1, buf+3);
		if (c > 0) {
			*s++ = ';';
			s = itoa(c + 1, s);
		}
		memcpy(s, "H", 2);
		term_out(buf+1);
	}
}

/* read s before reading from the terminal */
void term_push(char *s, u64 n)
{
	static u64 tibuf_prev;
	if (tibuf_cnt + n >= tibuf_sz || tibuf_sz - (tibuf_cnt + n) > 128) {
		tibuf_sz = tibuf_cnt + n + 128;
		tibuf = erealloc(tibuf, tibuf_sz);
	}
	if (texec) {
		if (texec == '@' && xquit > 0) {
			xquit = 0;
			texec_n = 0;
		} else if (tibuf_prev != tibuf_pos)
			texec_n = 0;
		memmove(tibuf + tibuf_pos + n + texec_n,
			tibuf + tibuf_pos + texec_n,
			tibuf_cnt - tibuf_pos - texec_n);
		memcpy(tibuf + tibuf_pos + texec_n, s, n);
		texec_n += n;
		tibuf_prev = tibuf_pos;
	} else
		memcpy(tibuf + tibuf_cnt, s, n);
	tibuf_cnt += n;
}

s64 term_read(s64 winch)
{
	static struct pollfd ufd[1 + LSP_NFDS_MAX];
	s64 cw, i, nfds;
	if (tibuf_pos >= tibuf_cnt) {
		if (texec) {
			xquit = !xquit ? 1 : xquit;
			if (texec == '&')
				goto err;
		}
		if (agent_tool) {
			agent_input_blocked = 1;
			xquit = !xquit ? 1 : xquit;
			*tibuf = TK_CTL('c');
			goto ret;
		}
		if (term_winch && winch) {
			*tibuf = winch;	/* yield until term_winch is cleared */
			goto ret;
		}
		cw = 0;
		re:
		ufd[0].fd = term_ufd.fd;
		ufd[0].events = POLLIN;
		/* the count is kept: servicing an fd below may unregister it */
		for (i = 0, nfds = lsp_nfds; i < nfds; i++) {
			ufd[i+1].fd = lsp_fds[i];
			ufd[i+1].events = POLLIN;
		}
		/* read a single input character, servicing lsp fds */
		if (xquit >= 0 && poll(ufd, 1 + nfds, -1) > 0) {
			/* POLLHUP too: a server that died must be reaped here,
			 * else its fd stays in the set and poll() never blocks */
			for (i = 0; i < nfds; i++)
				if (ufd[i+1].revents & (POLLIN | POLLHUP | POLLERR))
					lsp_process_fd(ufd[i+1].fd);
			/* only POLLIN yields input; on POLLHUP the read below
			 * fails and the usual end of input handling takes over */
			if (!(ufd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
				if (term_winch && winch) {
					*tibuf = winch;
					goto ret;
				}
				if (lsp_dirty && lsp_wake)
					goto err;	/* yield so vi redraws diagnostics */
				goto re;
			}
			if (read(term_ufd.fd, tibuf, 1) > 0) {
				if (xrr > 0) {
					static char buf[2];
					buf[0] = *tibuf;
					ex_regput(xrr, buf, 1);
				}
				goto ret;
			}
		}
		xquit = !isatty(term_ufd.fd) ? -1 : xquit;
		if (term_winch && winch && xquit >= 0) {
			*tibuf = winch;
			goto ret;
		} else if (term_winch != cw && !winch && xquit >= 0) {
			cw = term_winch;
			goto re;
		}
		err:
		*tibuf = 0;
		ret:
		tibuf_cnt = 1;
		tibuf_pos = 0;
	}
	vi_rendwait();		/* the queued frame overlaps the read above */
	if (ticmd_pos < sizeof(ticmd))
		ticmd[ticmd_pos++] = tibuf[tibuf_pos];
	return tibuf[tibuf_pos++];
}

/* return a static string that changes text attributes to att */
char *term_att(s64 att)
{
	if (att & SYN_MK)
		return "\x1b[m";
	static char buf[128] = "\x1b[";
	char *s = buf+2;
	if (att & SYN_BD)
		{*s++ = ';'; *s++ = '1';}
	if (att & SYN_IT)
		{*s++ = ';'; *s++ = '3';}
	if (att & SYN_RV)
		{*s++ = ';'; *s++ = '7';}
	if (SYN_FGSET(att)) {
		s64 fg = SYN_FG(att);
		*s++ = ';';
		if (fg < 8)
			s = itoa(30 + fg, s);
		else
			s = itoa(fg, (char*)memcpy(s, "38;5;", 5)+5);
	}
	if (SYN_BGSET(att)) {
		s64 bg = SYN_BG(att);
		*s++ = ';';
		if (bg < 8)
			s = itoa(40 + bg, s);
		else
			s = itoa(bg, (char*)memcpy(s, "48;5;", 5)+5);
	}
	s[0] = 'm';
	s[1] = '\0';
	return buf;
}

static s64 cmd_make(char **argv, s64 *ifd, s64 *ofd)
{
	s64 pid;
	int pipefds0[2] = {-1, -1};
	int pipefds1[2] = {-1, -1};
	if (ifd)
		pipe(pipefds0);
	if (ofd)
		pipe(pipefds1);
	if (!(pid = fork())) {
		if (ifd) {		/* setting up stdin */
			dup2(pipefds0[0], 0);
			close(pipefds0[1]);
			close(pipefds0[0]);
		}
		if (ofd) {		/* setting up stdout and stderr */
			dup2(pipefds1[1], 1);
			dup2(pipefds1[1], 2);
			close(pipefds1[0]);
			close(pipefds1[1]);
		}
		execvp(argv[0], argv);
		exit(1);
	}
	if (ifd)
		close(pipefds0[0]);
	if (ofd)
		close(pipefds1[1]);
	if (pid < 0) {
		if (ifd)
			close(pipefds0[1]);
		if (ofd)
			close(pipefds1[0]);
		return -1;
	}
	if (ifd)
		*ifd = pipefds0[1];
	if (ofd)
		*ofd = pipefds1[0];
	return pid;
}

char *xgetenv(char **q)
{
	char *r = NULL;
	while (*q && !r) {
		if (**q == '$')
			r = getenv(*q+1);
		else
			return *q;
		q++;
	}
	return r;
}

/* execute a command; pass in input if ibuf and process output if oproc */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, s64 oproc, s64 *status)
{
	s64 terminal = !ibuf && term_sbuf;
	if (agent_tool)
		return agent_shell(cmd, ibuf, oproc, status);
	static char *sh[] = {"$SHELL", "sh", NULL};
	struct pollfd fds[3];
	char buf[512];
	s64 ifd = -1, ofd = -1;
	s64 nw = 0;
	char *argv[5];
	argv[0] = xgetenv(sh);
	argv[1] = xish ? "-i" : argv[0];
	argv[2] = "-c";
	argv[3] = cmd;
	argv[4] = NULL;
	s64 pid = cmd_make(argv+!xish, ibuf ? &ifd : NULL, oproc ? &ofd : NULL);
	if (pid <= 0)
		return NULL;
	sbuf *sb;
	sbuf_make(sb, sizeof(buf)+1)
	if (!ibuf) {
		signal(SIGINT, SIG_IGN);
		term_done();
	} else if (ifd >= 0)
		fcntl(ifd, F_SETFL, fcntl(ifd, F_GETFL, 0) | O_NONBLOCK);
	fds[0].fd = ofd;
	fds[0].events = POLLIN;
	fds[1].fd = ifd;
	fds[1].events = POLLOUT;
	fds[2].fd = ibuf ? term_ufd.fd : -1;
	fds[2].events = POLLIN;
	while ((fds[0].fd >= 0 || fds[1].fd >= 0) && poll(fds, 3, 200) >= 0) {
		if (fds[0].revents & POLLIN) {
			s64 ret = read(fds[0].fd, buf, sizeof(buf));
			if (ret > 0 && oproc == 2)
				term_write(buf, ret)
			if (ret > 0)
				sbuf_mem(sb, buf, ret)
			else {
				close(fds[0].fd);
				fds[0].fd = -1;
			}
		} else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[0].fd);
			fds[0].fd = -1;
		}
		if (fds[1].revents & POLLOUT && ibuf) {
			s64 ret = write(fds[1].fd, ibuf->s + nw, ibuf->s_n - nw);
			if (ret > 0)
				nw += ret;
			if (ret <= 0 || nw == ibuf->s_n) {
				close(fds[1].fd);
				fds[1].fd = -1;
			}
		} else if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[1].fd);
			fds[1].fd = -1;
		}
		if (fds[2].revents & POLLIN) {
			s64 ret = read(fds[2].fd, buf, sizeof(buf));
			for (s64 i = 0; i < ret; i++)
				if ((unsigned char) buf[i] == TK_CTL('c'))
					kill(pid, SIGINT);
		} else if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[2].fd = -1;
	}
	if (fds[0].fd >= 0)
		close(ofd);
	if (fds[1].fd >= 0)
		close(ifd);
	waitpid(pid, (int*)status, 0);
	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp(term_ufd.fd, getpgrp());
	signal(SIGTTOU, SIG_DFL);
	if (!ibuf) {
		if (terminal)
			term_init();
		signal(SIGINT, SIG_DFL);
	}
	sbufn_ret(sb, sb)
}
