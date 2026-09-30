/**
 * @file term.c
 * @brief Terminal layer: raw mode and window size, buffered output and
 * escape sequences, the input queue (keys pushed by macros, mappings and
 * term_exec are read before the terminal), and running shell commands
 * through pipes.
 */
static struct termios termios;	///< original terminal attributes, restored by term_done()
struct pollfd term_ufd = {STDIN_FILENO, POLLIN};	///< input (stdin) descriptor for poll()
sbuf *term_sbuf;	///< output buffer; NULL until the first term_init() (term_done() frees it but does not reset it)
int term_record;	///< if set, output goes to term_sbuf until term_commit()
int term_winch;	///< SIGWINCH count; reset by term_init()
int term_resized;	///< incremented by every term_init(); vi.c compares it to vi_status to take the status row out of term_rows again
int term_rows, term_cols;	///< terminal size in rows/columns: TIOCGWINSZ, else $LINES/$COLUMNS, default 25x80 (vi.c subtracts the status row from term_rows)
/* term_inbuf_pos: index of the next byte to read from term_inbuf;
 * term_inbuf_count: number of valid bytes in term_inbuf;
 * term_inbuf_size: allocated size of term_inbuf (term_push() resizes it);
 * term_cmd_keys_pos: number of bytes in term_cmd_keys. */
unsigned int term_inbuf_pos, term_inbuf_count, term_inbuf_size = 128, term_cmd_keys_pos;
/* term_inbuf: pending input queue (pushed keys, or the last byte read from the
 *        terminal); term_read() returns from it first.
 * term_cmd_keys: every byte returned by term_read() since vi.c last reset
 *        term_cmd_keys_pos (once per normal-mode command); saved for '.' repeat. */
unsigned char *term_inbuf, term_cmd_keys[4096];
/* term_exec_type: type of the running term_exec() ('@' macro or '&' ex), 0 if none;
 *        when its input runs out, term_read() sets quit_state to leave the
 *        nested vi() ('&' also returns 0 instead of reading the terminal).
 * term_exec_pushed: bytes term_push() inserted at the read position during
 *        term_exec(); later pushes go after them, so they are read in push
 *        order before the rest of the queue. */
unsigned int term_exec_type, term_exec_pushed;

/** @brief Enter raw mode (no ICANON, ISIG, ECHO) and read the terminal size. */
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
		term_cols = win.ws_col;
		term_rows = win.ws_row;
	} else {
		if ((s = getenv("LINES")))
			term_rows = atoi(s);
		if ((s = getenv("COLUMNS")))
			term_cols = atoi(s);
	}
	term_cols = term_cols ? term_cols : 80;
	term_rows = term_rows ? term_rows : 25;
}

/** @brief Flush output, free term_sbuf and restore the terminal attributes. */
void term_done(void)
{
	if (!term_sbuf)
		return;
	term_commit();
	sbuf_free(term_sbuf)
	tcsetattr(term_ufd.fd, 0, &termios);
}

/** @brief Clear the screen and home the cursor. */
void term_clean(void)
{
	term_write("\x1b[2J", 4)	/* clear screen */
	term_write("\x1b[H", 3)		/* cursor topleft */
}

/** @brief Restore the terminal and stop the process (^z); reinit on resume. */
void term_suspend(void)
{
	if (opt_startup_flags & 8)
		term_scrl()
	term_done();
	kill(0, SIGSTOP);
	term_init();
	if (opt_startup_flags & 8)
		term_scrh()
}

/** @brief Write out term_sbuf and stop recording. */
void term_commit(void)
{
	term_write(term_sbuf->s, term_sbuf->s_n)
	sbuf_cut(term_sbuf, 0)
	term_record = 0;
}

/** @brief Output s, buffered if term_record. */
static void term_out(char *s)
{
	if (term_record)
		sbufn_str(term_sbuf, s)
	else
		term_write(s, strlen(s))
}

/** @brief Output one byte. */
void term_chr(int ch)
{
	char s[4] = {ch};
	term_out(s);
}

/** @brief Clear to the end of the line. */
void term_kill(void)
{
	term_out("\33[K");
}

/** @brief Insert n lines at the cursor row (n > 0) or delete -n lines (n < 0). */
void term_room(int n)
{
	char cmd[64] = "\33[";
	if (!n)
		return;
	char *s = itoa(abs(n), cmd+2);
	s[0] = n < 0 ? 'M' : 'L';
	s[1] = '\0';
	term_out(cmd);
}

/**
 * @brief Move the cursor to row r, column c (0-based).
 * If r < 0, move to column c of the current row (CR then c right).
 */
void term_pos(int r, int c)
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

/**
 * @brief Queue n bytes of s to be read before reading from the terminal.
 * Appended to the queue normally; inside term_exec() inserted at the read
 * position (after earlier pushes of the same position, see term_exec_pushed).
 */
void term_push(char *s, unsigned int n)
{
	static unsigned int term_inbuf_prev;	///< term_inbuf_pos at the previous push during term_exec(); if it moved, term_exec_pushed restarts
	/* grow if full, shrink if more than 128 bytes would be spare */
	if (term_inbuf_count + n >= term_inbuf_size || term_inbuf_size - (term_inbuf_count + n) > 128) {
		term_inbuf_size = term_inbuf_count + n + 128;
		term_inbuf = erealloc(term_inbuf, term_inbuf_size);
	}
	if (term_exec_type) {
		if (term_exec_type == '@' && quit_state > 0) {
			quit_state = 0;
			term_exec_pushed = 0;
		} else if (term_inbuf_prev != term_inbuf_pos)
			term_exec_pushed = 0;
		/* open an n byte gap after the term_exec_pushed bytes already pushed here */
		memmove(term_inbuf + term_inbuf_pos + n + term_exec_pushed,
			term_inbuf + term_inbuf_pos + term_exec_pushed,
			term_inbuf_count - term_inbuf_pos - term_exec_pushed);
		memcpy(term_inbuf + term_inbuf_pos + term_exec_pushed, s, n);
		term_exec_pushed += n;
		term_inbuf_prev = term_inbuf_pos;
	} else
		memcpy(term_inbuf + term_inbuf_count, s, n);
	term_inbuf_count += n;
}

/**
 * @brief Return the next input byte: from term_inbuf, else from the terminal.
 * @param winch  if nonzero, returned as the key when the window was resized
 * @return the byte, or 0 on EOF/error (or when a '&' term_exec runs out)
 * Terminal bytes are also appended to register opt_record_reg if set; every
 * returned byte is logged in term_cmd_keys.
 */
int term_read(int winch)
{
	int cw;
	if (term_inbuf_pos >= term_inbuf_count) {
		if (term_exec_type) {
			quit_state = !quit_state ? 1 : quit_state;
			if (term_exec_type == '&')
				goto err;
		}
		if (term_winch && winch) {
			*term_inbuf = winch;	/* yield until term_winch is cleared */
			goto ret;
		}
		cw = 0;
		re:
		/* read a single input character */
		if (quit_state < 0 || poll(&term_ufd, 1, -1) <= 0 ||
				read(term_ufd.fd, term_inbuf, 1) <= 0) {
			quit_state = !isatty(term_ufd.fd) ? -1 : quit_state;
			if (term_winch && winch && quit_state >= 0) {
				*term_inbuf = winch;
				goto ret;
			} else if (term_winch != cw && !winch && quit_state >= 0) {
				cw = term_winch;
				goto re;
			}
			err:
			*term_inbuf = 0;
		} else if (opt_record_reg > 0) {
			static char buf[2];
			buf[0] = *term_inbuf;
			ex_regput(opt_record_reg, buf, 1);
		}
		ret:
		term_inbuf_count = 1;
		term_inbuf_pos = 0;
	}
	if (term_cmd_keys_pos < sizeof(term_cmd_keys))
		term_cmd_keys[term_cmd_keys_pos++] = term_inbuf[term_inbuf_pos];
	return term_inbuf[term_inbuf_pos++];
}

/** @brief Return a static SGR escape string that sets text attributes att. */
char *term_att(int att)
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
		int fg = SYN_FG(att);
		*s++ = ';';
		if (fg < 8)
			s = itoa(30 + fg, s);
		else
			s = itoa(fg, (char*)memcpy(s, "38;5;", 5)+5);
	}
	if (SYN_BGSET(att)) {
		int bg = SYN_BG(att);
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

/**
 * @brief Fork and exec argv; if ifd/ofd, connect its stdin/stdout+stderr to
 * pipes and return our ends in them.
 * @return child pid or -1
 */
static int cmd_make(char **argv, int *ifd, int *ofd)
{
	int pid;
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

/**
 * @brief Return the first usable entry of the NULL-terminated list q: "$VAR"
 * entries are looked up in the environment, others are returned as is.
 */
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

/**
 * @brief Run cmd with $SHELL -c (or sh); pass in input if ibuf and process output if oproc.
 * @param cmd     command line passed to the shell
 * @param ibuf    written to the command's stdin; while it runs ^c on the
 *                terminal sends SIGINT. If NULL the command gets the
 *                terminal (term_done() before, term_init() after).
 * @param oproc   1: collect stdout+stderr; 2: also echo it to the terminal
 * @param status  if not NULL, receives the waitpid() status
 * @return the collected output (empty if !oproc), NULL if fork fails
 */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, int oproc, int *status)
{
	static char *sh[] = {"$SHELL", "sh", NULL};
	struct pollfd fds[3];
	char buf[512];
	int ifd = -1, ofd = -1;
	int nw = 0;
	char *argv[5];
	argv[0] = xgetenv(sh);
	argv[1] = opt_interactive_shell ? "-i" : argv[0];
	argv[2] = "-c";
	argv[3] = cmd;
	argv[4] = NULL;
	int pid = cmd_make(argv+!opt_interactive_shell, ibuf ? &ifd : NULL, oproc ? &ofd : NULL);
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
			int ret = read(fds[0].fd, buf, sizeof(buf));
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
			int ret = write(fds[1].fd, ibuf->s + nw, ibuf->s_n - nw);
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
			int ret = read(fds[2].fd, buf, sizeof(buf));
			for (int i = 0; i < ret; i++)
				if ((unsigned char) buf[i] == TK_CTL('c'))
					kill(pid, SIGINT);
		} else if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[2].fd = -1;
	}
	if (fds[0].fd >= 0)
		close(ofd);
	if (fds[1].fd >= 0)
		close(ifd);
	waitpid(pid, status, 0);
	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp(term_ufd.fd, getpgrp());
	signal(SIGTTOU, SIG_DFL);
	if (!ibuf) {
		if (term_sbuf)
			term_init();
		signal(SIGINT, SIG_DFL);
	}
	sbufn_ret(sb, sb)
}
