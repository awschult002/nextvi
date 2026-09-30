/**
 * @file vi.h
 * @brief Shared definitions across files: types, globals and prototypes,
 * grouped by the source file that defines them (regex.c, lbuf.c, ren.c,
 * uc.c, term.c, led.c, ex.c, conf.c, vi.c). vi.c includes every .c file,
 * so the editor is one translation unit.
 * Globals and functions are documented where they are defined.
 */

/* helper macros */
#define LEN(a)		(int)(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))
/* for debug; printf() but to file */
/* p(): printf() to the file "file" in the current directory */
#define p(s, ...)\
	{FILE *f = fopen("file", "a");\
	fprintf(f, s, ##__VA_ARGS__);\
	fclose(f);}\

/* ease up ridiculous global stuffing: preserve() saves name in tmp##name
 * and runs value; restore() puts it back (both in the same scope) */
#define preserve(type, name, value) \
type tmp##name = name; \
value \

#define restore(name) \
name = tmp##name; \

/* utility funcs */
void *emalloc(size_t size);
void *erealloc(void *p, size_t size);
int dstrlen(const char *s, char delim);
char *itoa(int n, char s[]);
static int itoalen(int n) { char s[32]; return itoa(n, s) - s; }
static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

/* sbuf: variable-sized buffer/string */
/* NEXTSZ: new size for o used + r more bytes, with 50% headroom */
#define NEXTSZ(o, r)	o + r + ((o + r) >> 1)
typedef struct sbuf {
	char *s;	///< allocated buffer
	int s_n;	///< length of the string stored in s[]
	int s_sz;	///< size of memory allocated for s[]
} sbuf;

/* sbuf macros are statements, not expressions. */
#define _sbuf_make(sb, newsz, alloc) \
{ \
	alloc \
	sb->s_sz = newsz; \
	sb->s = emalloc(newsz); \
	sb->s_n = 0; \
} \

#define sbuf_chr(sb, c) \
{ \
	if (sb->s_n + 1 >= sb->s_sz) { \
		sb->s_sz = NEXTSZ(sb->s_n, 1) + 1; \
		sb->s = erealloc(sb->s, sb->s_sz); \
	} \
	sb->s[sb->s_n++] = c; \
} \

/* sbuf_: make room for len bytes and memcpy/memset (func = cpy/set) x at
 * s + s_n, without updating s_n. If less than a quarter of the buffer is
 * used, a new buffer is allocated and only the used part copied. */
#define sbuf_(sb, x, len, func) \
if (sb->s_n + len >= sb->s_sz) { \
	if (sb->s_n < sb->s_sz >> 2) { \
		char *__s_ = sb->s; \
		sb->s_sz = NEXTSZ(sb->s_n, len) + 1; \
		sb->s = emalloc(sb->s_sz); \
		memcpy(sb->s, __s_, sb->s_n); \
		free(__s_); \
	} else { \
		sb->s_sz = NEXTSZ(sb->s_n, len) + 1; \
		sb->s = erealloc(sb->s, sb->s_sz); \
	} \
} \
mem##func(sb->s + sb->s_n, x, len); \

/* sbuf_smake: declare sb on the stack (only s is heap allocated) */
#define sbuf_smake(sb, newsz) sbuf _##sb, *sb = &_##sb; _sbuf_make(sb, newsz,)
#define sbuf_make(sb, newsz) { _sbuf_make(sb, newsz, sb = emalloc(sizeof(*sb));) }
#define sbuf_free(sb) { free(sb->s); free(sb); }
#define sbuf_set(sb, ch, len) { int __l_ = len; sbuf_(sb, ch, __l_, set) sb->s_n += __l_; }
#define sbuf_mem(sb, s, len) { int __l_ = len; sbuf_(sb, s, __l_, cpy) sb->s_n += __l_; }
#define sbuf_str(sb, s) { sbuf_mem(sb, s, strlen(s)) }
#define sbuf_cut(sb, len) { sb->s_n = len; }
/* sbuf functions that nul-terminate strings */
#define sbuf_nul(sb) { sb->s[sb->s_n] = '\0'; }
/* sbuf_nul4: append 4 NULs not counted in s_n ("fat null", see uc.c) */
#define sbuf_nul4(sb) { sbuf_(sb, '\0', 4, set) }
#define sbufn_set(sb, ch, len) { sbuf_set(sb, ch, len) sbuf_nul(sb) }
#define sbufn_mem(sb, s, len) { sbuf_mem(sb, s, len) sbuf_nul(sb) }
#define sbufn_str(sb, s) { sbuf_str(sb, s) sbuf_nul(sb) }
#define sbufn_cut(sb, len) { sbuf_cut(sb, len) sbuf_nul(sb) }
#define sbufn_chr(sb, c) { sbuf_chr(sb, c) sbuf_nul(sb) }
/* sbufn_ret: NUL-terminate sb and return str from the calling function */
#define sbufn_ret(sb, str) { sbuf_nul(sb) return str; }

/* regex.c: regular expressions */
#define REG_ICASE	0x01	///< case-insensitive (ASCII)
#define REG_NEWLINE	0x02	///< unlike posix, controls termination by '\n'
#define REG_NOTBOL	0x04	///< s is not at the beginning of a line (^ fails)
#define REG_NOTEOL	0x08	///< $ fails
#define REG_NOCAP	0x10	///< computes only the default match group
typedef struct rcode rcode;
/** A compiled regular expression (see regex.c). */
struct rcode {
	rcode **la;	///< lookahead expressions
	int laidx;	///< lookahead index
	int unilen;	///< number of integers in insts
	int len;	///< number of atoms/instructions
	int sub;	///< interim val = save count; final val = nsubs size
	int presub;	///< interim val = save count; final val = 1 rsub size
	int splits;	///< number of split insts
	int sparsesz;	///< sdense size
	int flg;	///< stored flags
	int insts[];	///< re code
};
/** Regular expression set: several patterns matched at once (rset_find()). */
typedef struct {
	rcode *regex;	///< the combined regular expression
	int *grp;	///< grp[i]: capture slot of pattern i's whole match, -2 if pattern i is NULL
	int *grpnsubc;	///< grpnsubc[i]: ints pattern i fills in rset_find()'s grps (2 per group)
	int nsubc;	///< total sub count (size needed for grps)
	int n;	///< number of regular expressions in this set
} rset;
rset *rset_make(int n, char **pat, int flg);
/** @brief rset of a single pattern. */
static rset *rset_smake(char *pat, int flg)
	{ char *ss[1] = {pat}; return rset_make(1, ss, flg); }
int rset_find(rset *re, char *s, int *grps, int flg);
int rset_match(rset *rs, char *s, int flg);
void rset_free(rset *re);

/* lbuf.c: line buffer */
/** One undo record: a replacement of n_del lines by n_ins lines at pos. */
struct lopt {
	char **ins;	///< inserted lines
	char **del;	///< deleted lines
	int *mark;	///< saved marks ({id, row, off} triplets)
	int mark_n;	///< number of saved marks
	int mark_sb[2];	///< saved [ mark row & off
	int mark_se[2];	///< saved ] mark row & off
	int pos, pos_off;	///< modification location: row and char offset
	int n_ins, n_del;	///< modification range: number of lines in ins/del
	int seq;	///< operation number; records with the same seq are undone together
	int ref;	///< ins/del ref exists on lbuf: bit 2 ins[] is in the buffer, bit 1 del[] is
};
/** Header stored just before each line's text: [linfo][text]['\n'][4 NULs]. */
struct linfo {
	int len;	///< text length in bytes, without the '\n'
	int grec;	///< :g mark bits, one per :g nesting level
};
struct lbuf {
	char **ln;	///< buffer lines (pointers to the text after each linfo)
	struct lopt *hist;	///< buffer history
	int *mark;	///< mark id, row & off triplets
	int mark_n;	///< number of marks in mark[]
	int mark_sb[2];	///< [ mark row & off
	int mark_se[2];	///< ] mark row & off
	int tmp_mark[4];	///< aux mark state: [ and ] saved when undo leaves the newest state, restored when redo gets back to it
	int ln_n;	///< number of lines in ln[]
	int ln_sz;	///< size of ln[]
	int useq;	///< current operation sequence
	int modified;	///< modification state
	int saved;	///< save state: hist_u when last saved, -1 if that state was discarded
	int hist_sz;	///< size of hist[]
	int hist_n;	///< current history head in hist[] (number of records)
	int hist_u;	///< current undo head in hist[]: [0, hist_u) undoable, [hist_u, hist_n) redoable
};
#define lbuf_len(lb) lb->ln_n
/* lbuf_s(ln): the linfo header of line text ln; lbuf_i(): of row pos */
#define lbuf_s(ln) ((struct linfo*)(ln - sizeof(struct linfo)))
#define lbuf_i(lb, pos) ((struct linfo*)(lb->ln[pos] - sizeof(struct linfo)))
struct lbuf *lbuf_make(void);
void lbuf_free(struct lbuf *lb);
int lbuf_rd(struct lbuf *lb, int fd, int beg, int end);
int lbuf_wr(struct lbuf *lb, int fd, int beg, int end);
void lbuf_edit(struct lbuf *lb, char *s, int beg, int end, int o1, int o2);
void lbuf_region(struct lbuf *lb, sbuf *sb, int r1, int o1, int r2, int o2);
int lbuf_pos2off(struct lbuf *lb, int r1, int o1, int r2, int o2, int row, int off);
int lbuf_off2pos(struct lbuf *lb, int r1, int o1, int r2, int o2, int boff, int *row, int *off);
char *lbuf_joinsb(struct lbuf *lb, int r1, int r2, sbuf *i, int *o1, int *o2);
int lbuf_join(struct lbuf *lb, int beg, int end, int o1, int *o2, int flg);
char *lbuf_get(struct lbuf *lb, int pos);
void lbuf_smark(struct lbuf *lb, struct lopt *lo, int beg, int o1);
void lbuf_emark(struct lbuf *lb, struct lopt *lo, int end, int o2);
struct lopt *lbuf_opt(struct lbuf *lb, int beg, int o1, int n_del);
void lbuf_mark(struct lbuf *lb, int mk, int pos, int off);
int lbuf_jump(struct lbuf *lb, int mk, int *pos, int *off);
int lbuf_undo(struct lbuf *lb, int *row, int *off);
int lbuf_redo(struct lbuf *lb, int *row, int *off);
void lbuf_saved(struct lbuf *lb, int clear);
int lbuf_indents(struct lbuf *lb, int r);
int lbuf_eol(struct lbuf *lb, int r, int state);
int lbuf_next(struct lbuf *lb, int dir, int *r, int *o);
int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *r, int *o);
int lbuf_search(struct lbuf *lb, rset *re, int dir, int beg, int end, int pskip,
		int nskip, int *r, int *o);
/* lbuf_dedup: delete every line whose text (without '\n') is str[0..n) */
#define lbuf_dedup(lb, str, n) \
{ for (int i = 0; i < lbuf_len(lb);) { \
	char *s = lbuf_get(lb, i); \
	if (n == lbuf_s(s)->len && !memcmp(str, s, n)) \
		lbuf_edit(lb, NULL, i, i + 1, 0, 0); \
	else \
		i++; \
}} \

/* regions */
int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off, int ch);
int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_wordend(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_pair(struct lbuf *lb, char *pairs, int plen, int *row, int *off);

/* ren.c: rendering lines */
/** Rendering of one line: character positions, widths and screen columns
 * (computed by ren_position(), cached by the line pointer s). */
typedef struct {
	char **chrs;	///< chrs[i]: pointer to character i; chrs[n] is the end
	char *s;	///< line this state is for (cache key); to prevent redundant computations, ensure pointer uniqueness
	int *wid;	///< wid[i]: screen width of character i
	int *col;	///< col[c]: index of the character at screen column c
	int *pos;	///< pos[i]: screen column of character i
	int n;	///< number of characters, including the '\n'
	int cmax;	///< last screen column
	int ctx;	///< direction context of the line (dir_context())
	int holelen;	///< bytes of the char cut at opt_render_limit (0 if none)
	char nulhole[4];	///< saved bytes of that char, overwritten with NULs
} ren_state;
extern ren_state rstates[3];
extern ren_state *rstate;
/* RST: run func with rstate = rstates[n] (cache cleared), then back to
 * rstates[0]. RST_NULL: clear the cache of the listed rstates. */
#define RST(n, func) { rstate = rstates+n; rstate->s = NULL; func; rstate -= n; }
#define RST_NULL(...) { \
	int i_[] = {__VA_ARGS__}; \
	for (int j_ = 0; j_ < LEN(i_); j_++) \
		rstates[i_[j_]].s = NULL; \
} \

ren_state *ren_position(char *s);
int ren_next(char *s, int p, int dir);
int ren_eol(char *s, int dir);
int ren_pos(char *s, int off);
int ren_cursor(char *s, int pos);
int ren_noeol(char *s, int p);
int ren_off(char *s, int p);
char *ren_translate(char *s, char *ln);
/* text direction */
int dir_context(char *s);
void dir_init(void);
/* syntax highlighting */
/* Attribute ints: bits 0-7 fg color, 8-15 bg color; SYN_FGMK/SYN_BGMK
 * store a color with its "set" bit. SYN_B* values below go in the att[]
 * entry that follows a SYN_BLK one. */
#define SYN_BD		0x10000	///< bold
#define SYN_IT		0x20000	///< italic
#define SYN_RV		0x40000	///< reverse video
#define SYN_FGMK(f)	(0x80000 | (f))
#define SYN_BGMK(b)	(0x100000 | (b << 8))
#define SYN_FLG		0x1f0000
#define SYN_FGSET(a)	(a & 0x800ff)
#define SYN_BGSET(a)	(a & 0x10ff00)
#define SYN_FG(a)	(a & 0xff)
#define SYN_BG(a)	((a >> 8) & 0xff)
#define SYN_IGN		0x200000	/* grp is ignored */
#define SYN_SKIP	0x400000	/* grp is skipped */
#define SYN_SATT	0x800000	/* grp inclusion check at start offset */
#define SYN_EATT	0x1000000	/* grp inclusion check at end offset */
#define SYN_ATT		0x1800000	/* grp inclusion check from start to end */
#define SYN_OATT	0x2000000	/* grp overwrite of listed attributes only */
#define SYN_BATT	0x4000000	/* listed attribute tests pending block highlight */
#define SYN_BLK		0x8000000	/* grp block highlight */
#define SYN_OWR		0x10000000	/* attribute overwrite */
#define SYN_MK		0x20000000	/* marker holds id in color field, not rendered */
#define SYN_BS		0x1		/* grp starting block highlight */
#define SYN_BE		0x2		/* grp ending block highlight */
#define SYN_BSE		0x3		/* grp self terminating block highlight */
#define SYN_BP		0x4		/* grp is a block attribute passthrough */
#define SYN_BSD		0x8		/* grp block highlight start direction up */
#define SYN_BED		0x10		/* grp block highlight end direction up */
#define SYN_BSDP	0x20		/* grp block highlight start direction pass */
#define SYN_BEDP	0x40		/* grp block highlight end direction pass */
#define SYN_BN		0x80		/* grp block highlight nests into itself */
#define SYN_SET(flg, a) (a & SYN_##flg)
extern int ftidx;
extern int syn_scdirl;
extern int syn_blockhl;
char *syn_setft(char *ft);
void syn_scdir(int scdir);
void syn_highlight(int *att, char *s, int n);
char *syn_filetype(char *path);
int syn_merge(int old, int new);
void syn_reloadft(int hl, int flg);
int syn_findhl(int id);
int syn_addhl(char *reg, int id);
void syn_init(void);

/* uc.c: utf-8 helper functions */
extern unsigned char utf8_length[256];
extern int zwlen, def_zwlen;
extern int bclen, def_bclen;
/* the length of a given utf-8 character */
#define uc_len(s) utf8_length[(unsigned char)(s)[0]]
/* the unicode codepoint of a given utf-8 character: dst = code, l = length (dst 0 if l is 0) */
#define uc_code(dst, s, l) \
dst = (unsigned char)s[0]; \
l = utf8_length[dst]; \
if (l == 1); \
else if (l == 2) \
	dst = ((dst & 0x1f) << 6) | (s[1] & 0x3f); \
else if (l == 3) \
	dst = ((dst & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); \
else if (l == 4) \
	dst = ((dst & 0x07) << 18) | ((s[1] & 0x3f) << 12) | \
		((s[2] & 0x3f) << 6) | (s[3] & 0x3f); \
else \
	dst = 0; \

int uc_wid(int c);
int uc_slen(char *s);
char *uc_chrn(char *s, int off, int *n);
/** @brief Pointer to character off of s (see uc_chrn()). */
static char *uc_chr(char *s, int off) { int n; return uc_chrn(s, off, &n); }
int uc_off(char *s, int off);
char *uc_subl(char *s, int beg, int end, int *rlen);
/** @brief malloc'd copy of characters [beg, end) of s. */
static char *uc_sub(char *s, int beg, int end)
	{ int l; return uc_subl(s, beg, end, &l); }
char *uc_dup(const char *s);
/* Byte classes; unsigned wraparound makes each range one compare
 * ((c - 9) < 5 is '\t'..'\r'). uc_isalpha counts bytes >= 0x80 as letters;
 * | 0x20 folds case. */
#define uc_isspace(c) ((unsigned char)(c) == ' ' || (unsigned char)((unsigned char)(c) - 9) < 5)
#define uc_isprint(c) ((unsigned char)(c) >= 0x20 && (unsigned char)(c) != 0x7f)
#define uc_isdigit(c) (((unsigned char)(c) ^ '0') < 10)
#define uc_isalpha(c) ((unsigned char)(c) > 0x7f || (unsigned char)(((unsigned char)(c) | 0x20) - 'a') < 26)
#define uc_isupper(c) ((unsigned char)((unsigned char)(c) - 'A') < 26)
int uc_kind(char *c);
int uc_isbell(int c);
int uc_acomb(int c);
char *uc_beg(char *beg, char *s);
char *uc_shape(char *beg, char *s, int c);

/* term.c: managing the terminal */
extern struct pollfd term_ufd;
extern sbuf *term_sbuf;
extern int term_record;
extern int term_winch;
extern int term_resized;
extern int xrows, xcols;
extern unsigned int tibuf_pos, tibuf_cnt, tibuf_sz, ticmd_pos;
extern unsigned char *tibuf, ticmd[4096];
extern unsigned int texec, texec_n;
/* term_write: write to stdout unless opt_line_editor is 0 */
#define term_write(s, n) if (opt_line_editor) write(1, s, n);
void term_init(void);
void term_done(void);
void term_clean(void);
void term_suspend(void);
/* leave (scrl) / enter (scrh) the alternate screen */
#define term_scrl()	term_write("\033[?1049l", 8)
#define term_scrh()	term_write("\033[?1049h", 8)
void term_chr(int ch);
void term_pos(int r, int c);
void term_kill(void);
void term_room(int n);
int term_read(int winch);
void term_commit(void);
char *term_att(int att);
void term_push(char *s, unsigned int n);
/* term_dec: unread the last byte returned by term_read() */
#define term_dec() tibuf_pos--; ticmd_pos--;
/* term_exec: run s[0..n) as keyboard input in a nested vi(0); type is '@'
 * (macro) or '&' (see texec). The rest of the outer input queue and the
 * texec state are restored afterwards. */
#define term_exec(s, n, type) \
{ \
	preserve(int, texec_n, texec_n = 0;) \
	preserve(int, tibuf_cnt,) \
	preserve(int, tibuf_pos, tibuf_pos = tibuf_cnt;) \
	term_push(s, n); \
	preserve(int, texec, texec = type;) \
	vi(0); \
	restore(texec) \
	if (quit_state > 0) \
		quit_state = 0; \
	restore(tibuf_pos) \
	restore(tibuf_cnt) \
	restore(texec_n) \
} \

/* process management */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, int oproc, int *status);
char *xgetenv(char* q[]);

/* TK_CTL(x): control key code of x; TK_INT(c): keys that abort input (NUL, ESC, ^c) */
#define TK_ESC		TK_CTL('[')
#define TK_CTL(x)	(x & 037)
#define TK_INT(c)	(!c || c == TK_ESC || c == TK_CTL('c'))

/* led.c: line-oriented input and output */
/** led_render() state, passed to every extension. */
typedef struct {
	int *att;	///< highlight attribute per character (per bound entry when bound is set)
	int *off;	///< off[c]: character offset shown at screen cell c, -1 if none
	int *stt;	///< stt[k]: character offset of bound entry k
	int *ctt;	///< ctt[j]: bound index of the j-th visible character in screen order
	char *s0;	///< the line being rendered
	char *bound;	///< visible characters only (line too wide), or NULL
	ren_state *r;	///< rendering state of s0
	int alen;	///< number of valid att[] entries
	int cterm;	///< number of screen cells
	int n;	///< number of characters in s0
} led_ctx;
typedef struct led_ext led_ext;
/** A syntax highlighting extension (see led.c). */
struct led_ext {
	char *ln;	///< line key; NULL matches any line
	void *usr;	///< data interpreted by the extension
	void (*ext_func)(led_ext *p, led_ctx *x);	///< extension body defaults to ext_attmerge()
	int blen;	///< byte length of usr
};
led_ext *led_extnew(void);
led_ext *led_extreg(void);
led_ext *led_extfind(void (*ext_func)(led_ext *p, led_ctx *x));
void led_extdel(led_ext *p);
void led_extcut(void);
int led_attidx(led_ctx *x, int off);
/* for extensions that key themselves */
/* (pointer comparison) */
#define led_extkey(p, x) (!(p)->ln || (p)->ln == (x)->s0)
void led_modeswap(void);
/** Insert-mode state kept across led_prompt() calls. */
typedef struct {
	int t_row;	///< history row (in tempbufs[0]) recalled next by ^a, -2 if not set
	int p_reg;	///< register pasted by ^p (^] cycles to the next non-empty '0'..'9', ^\ then a key selects)
	int lsug;	///< sb offset where the completion is inserted
	int sug_pt;	///< completion start set by ^x at the cursor (^x again clears), -1 if none
	char *sug;	///< next suggestion to offer
	char *_sug;	///< end of the current suggestion
} ins_state;
/* ins_init: reset an ins_state (no history row, default register, no suggestion) */
#define ins_init(is) \
is.t_row = -2; \
is.p_reg = default_reg; \
is.lsug = 0; \
is.sug_pt = -1; \
is.sug = NULL; \
is._sug = NULL; \

int led_prompt(sbuf *sb, char *insert, int *kmap, ins_state *is, int ps, int flg);
int led_input(sbuf *sb, char *post, int postn, int row, int flg, int *pren);
void led_render(char *s0, int cbeg, int cend);
/* _led_render: draw msg's screen columns [beg, end) at (row, col), after
 * running kill (led_crender clears the rest of the line). Output is
 * buffered and committed unless the caller was already recording. */
#define _led_render(msg, row, col, beg, end, kill) \
{ \
	int record = term_record; \
	term_record = 1; \
	term_pos(row, col); \
	kill \
	led_render(msg, beg, end); \
	if (!record) \
		term_commit(); \
} \

#define led_prender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end,)
#define led_crender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end, term_kill();)
char *led_read(int *kmap, int c);
int led_pos(char *s, int pos);
void led_done(void);

/* ex.c: command mode */
struct buf {
	char *ft;	///< file type
	char *path;	///< file path
	struct lbuf *lb;	///< the buffer's lines
	int plen, row, off, top;	///< path length; saved cursor row, char offset and view top row
	long mtime;	///< modification time
	signed char td;	///< text direction
};
/* ex options */
extern int opt_left_col;
extern int opt_startup_flags;
extern int opt_autoindent;
extern int opt_ignorecase;
extern int opt_syntax_hl;
extern int opt_hl_line;
extern int opt_hl_word;
extern int opt_hl_pair;
extern int opt_hl_reverse;
extern int opt_line_editor;
extern int opt_text_dir;
extern int opt_shaping;
extern int opt_reorder;
extern int opt_tabstop;
extern int opt_interactive_shell;
extern int opt_search_group;
extern int opt_print_autocomplete;
extern int opt_multiline_prompt;
extern int opt_print_reg;
extern int opt_render_limit;
extern int opt_undo_seq;
extern int opt_error_mode;
extern int opt_find_reg;
extern int opt_record_reg;
/* global variables */
extern int quit_state;
extern int cursor_row, cursor_off, view_top_row;
extern int buf_count;
extern int vi_ex_depth;
extern int cur_keymap;
extern int keymap_alt;
extern int search_dir;
extern int search_changes;
extern int print_newline;
extern int ex_separator;
extern int ex_escape;
extern int ex_exec_depth;
extern sbuf *autocomplete_filter;
extern rset *search_rset;
extern sbuf **str_registers;
extern int str_registers_n;
extern int default_reg;
extern struct buf *bufs;
extern struct buf tempbufs[3];
extern struct buf *cur_buf;
extern struct buf *prev_buf;
/* istempbuf: whether buf is one of tempbufs[]; xb*: fields of cur_buf */
#define istempbuf(buf) (buf >= tempbufs && buf < tempbufs + LEN(tempbufs))
#define xb_path cur_buf->path
#define xb_ft cur_buf->ft
#define xb cur_buf->lb
/* exbuf_load/exbuf_save: copy cursor, view and text direction from/to buf */
#define exbuf_load(buf) \
	cursor_row = buf->row; \
	cursor_off = buf->off; \
	view_top_row = buf->top; \
	opt_text_dir = buf->td; \

#define exbuf_save(buf) \
	buf->row = cursor_row; \
	buf->off = cursor_off; \
	buf->top = view_top_row; \
	buf->td = opt_text_dir; \

/* bufs_switchwft: switch to bufs[idx] and set its filetype */
#define bufs_switchwft(idx) \
{ if (&bufs[idx] != cur_buf) { bufs_switch(idx); syn_setft(xb_ft); } } \

void bufs_switch(int idx);
void temp_open(int i, char *name, char *ft);
void temp_switch(int i, int swap);
void temp_write(int i, char *str);
void temp_pos(int i, int row, int off, int top);
void ex(void);
void *ex_exec(const char *ln);
/* ex_command: run ln and store it in the ':' register */
#define ex_command(ln) { ex_exec(ln); ex_regput(':', ln, 0); }
void ex_cprint(char *line, char *ft, int r, int c, int left, int flg);
/* ex_cprint2/ex_print: print using rstates[2], keeping rstates[0] intact */
#define ex_cprint2(line, ft, r, c, left, flg) { RST(2, ex_cprint(line, ft, r, c, left, flg)); }
#define ex_print(line, ft) { RST(2, ex_cprint(line, ft, -1, 0, 0, 1)); }
void ex_init(char **files, int n);
void ex_bufpostfix(struct buf *p, int clear);
int ex_krs(rset **krs, int *dir);
void ex_krsset(char *kwd, int dir);
void ex_regesc(sbuf *sb, char *beg, char *end, int ex);
int ex_edit(const char *path, int len);
sbuf *ex_regget(int id);
void ex_regput(int c, const char *s, int append);

/* conf.c: configuration variables */
extern const int conf_mode;
/** Map file names to file types. */
struct filetype {
	char *ft;	///< file type (a name from conf.c, compared by pointer)
	char *pat;	///< file name pattern
};
extern struct filetype fts[];
extern const int ftslen;
/** Syntax highlighting patterns. */
struct highlight {
	char *ft;	///< the filetype of this pattern
	char *pat;	///< regular expression
	int *att;	///< attributes of the matched groups (see syn_highlight())
	unsigned char set;	///< subset index: consecutive entries with the same ft and set form one rset
	unsigned char id;	///< id of this hl, for syn_findhl(): 1 hlw/fuzzy, 2 hll, 3 hlp
};
extern struct highlight hls[];
extern const int hlslen;
extern const int hlopts[];
extern const int hloptslen;
/** Direction context: specifies the direction of a whole line. */
struct dircontext {
	char *pat;	///< pattern tested against the line
	int dir;	///< -1 right-to-left, +1 left-to-right
};
extern struct dircontext dctxs[];
extern const int dctxlen;
/** Direction marks: the direction of patterns in a line. */
struct dirmark {
	char *pat;	///< pattern; its groups get the directions in dir[]
	int ctx;	///< the direction context for this mark; 0 means any
	int dir[8];	///< the direction of a matched text group
};
extern struct dirmark dmarks[];
extern const int dmarkslen;
/** Character placeholders: how to show unprintable characters. */
struct placeholder {
	int cp[2];	///< the source character codepoint range [cp[0], cp[1]]
	char d[8];	///< the placeholder
	int wid;	///< the width of the placeholder
	int l;	///< the length of the codepoint: UTF-8 bytes the character must have
};
extern struct placeholder _ph[];
extern struct placeholder *ph;
extern int phlen;
extern const int conf_hlrev;
char **conf_kmap(int id);
int conf_kmapfind(char *name);
char *conf_digraph(int c1, int c2);

/* vi.c: main */
void vi(int init);
extern int vi_hidch;
extern int vi_lncol;
/* filesystem */
extern rset *fsincl;
void dir_calc(char *path);
