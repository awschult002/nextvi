/**
 * @file vi.h
 * @brief Shared definitions across files: types, globals and prototypes,
 * grouped by the source file that defines them (regex.c, lbuf.c, ren.c,
 * uc.c, term.c, led.c, ex.c, conf.c, vi.c, lsp.c; the tree-sitter
 * interface sits with led.c). The agent's declarations are in agent.h.
 * vi.c includes every .c file, so the editor is one translation unit.
 * Globals and functions are documented where they are defined.
 */
#include <stdint.h>
typedef uint64_t u64;
typedef int64_t s64;

/* helper macros */
#define LEN(a)		(s64)(sizeof(a) / sizeof((a)[0]))
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
/** @brief malloc() that exits on failure. */
static void *emalloc(size_t size)
{
	void *p;
	if (!(p = malloc(size))) {
		fprintf(stderr, "\nmalloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

/** @brief realloc() that exits on failure. */
static void *erealloc(void *p, size_t size)
{
	if (!(p = realloc(p, size))) {
		fprintf(stderr, "\nrealloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

/** @brief Length of s up to delim or NUL. */
static s64 dstrlen(const char *s, char delim)
{
	register const char *i;
	for (i=s; *i && *i != delim; ++i);
	return i-s;
}

/** @brief Write n in decimal to s; returns a pointer to the terminating NUL. */
static char *itoa(s64 n, char s[])
{
	s64 i = 0, sign;
	if ((sign = n) < 0)		/* record sign */
		n = -n;			/* make n positive */
	do {				/* generate digits in reverse order */
		s[i++] = n % 10 + '0';	/* get next digit */
	} while ((n /= 10) > 0);	/* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	char *p1 = s;	/* reverse in place */
	char *p2 = s + i - 1;
	while (p1 < p2) {
		char tmp = *p1;
		*p1++ = *p2;
		*p2-- = tmp;
	}
	return &s[i];
}
static char *sdup(const char *s, s64 n) { n++; return memcpy(emalloc(n), s, n); }
static s64 itoalen(s64 n) { char s[32]; return itoa(n, s) - s; }
static void swap(s64 *a, s64 *b) { s64 t = *a; *a = *b; *b = t; }

/* sbuf: variable-sized buffer/string */
/* NEXTSZ: new size for o used + r more bytes, with 50% headroom */
#define NEXTSZ(o, r)	o + r + ((o + r) >> 1)
typedef struct sbuf {
	char *s;	///< allocated buffer
	s64 s_n;	///< length of the string stored in s[]
	s64 s_sz;	///< size of memory allocated for s[]
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
#define sbuf_set(sb, ch, len) { s64 __l_ = len; sbuf_(sb, ch, __l_, set) sb->s_n += __l_; }
#define sbuf_mem(sb, s, len) { s64 __l_ = len; sbuf_(sb, s, __l_, cpy) sb->s_n += __l_; }
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
	s64 laidx;	///< lookahead index
	s64 unilen;	///< number of integers in insts
	s64 len;	///< number of atoms/instructions
	s64 sub;	///< interim val = save count; final val = nsubs size
	s64 presub;	///< interim val = save count; final val = 1 rsub size
	s64 splits;	///< number of split insts
	s64 sparsesz;	///< sdense size
	s64 flg;	///< stored flags
	s64 insts[];	///< re code
};
/** Regular expression set: several patterns matched at once (rset_find()). */
typedef struct {
	rcode *regex;	///< the combined regular expression
	s64 *grp;	///< grp[i]: capture slot of pattern i's whole match, -2 if pattern i is NULL
	s64 *grpnsubc;	///< grpnsubc[i]: words pattern i fills in rset_find()'s grps (2 per group)
	s64 nsubc;	///< total sub count (size needed for grps)
	s64 n;	///< number of regular expressions in this set
} rset;
typedef struct {
	rset *rs;		/* only for regex patterns */
	char *str;		/* for simple, non-regex patterns  */
	s64 len;		/* str length */
	s64 flg;		/* flags */
	s64 lbeg, lend;		/* match line beg/end */
	s64 wbeg, wend;		/* match word beg/end */
} rstr;
rset *rset_make(s64 n, char **pat, s64 flg);
rset *rset_smake(char *pat, s64 flg);
s64 rset_find(rset *re, char *s, s64 *grps, s64 flg);
s64 rset_match(rset *rs, char *s, s64 flg);
void rset_free(rset *re);
rstr *rstr_make(char *re, s64 flg);
s64 rstr_find(rstr *rs, char *s, s64 *grps, s64 flg);
s64 rstr_match(rstr *rs, char *s, s64 flg);
void rstr_free(rstr *rs);

/* lbuf.c: line buffer */
/** One undo record: a replacement of n_del lines by n_ins lines at pos. */
struct lopt {
	char **ins;	///< inserted lines
	char **del;	///< deleted lines
	s64 *mark;	///< saved marks ({id, row, off} triplets)
	s64 mark_n;	///< number of saved marks
	s64 mark_sb[2];	///< saved [ mark row & off
	s64 mark_se[2];	///< saved ] mark row & off
	s64 pos, pos_off;	///< modification location: row and char offset
	s64 n_ins, n_del;	///< modification range: number of lines in ins/del
	s64 seq;	///< operation number; records with the same seq are undone together
	s64 ref;	///< ins/del ref exists on lbuf: bit 2 ins[] is in the buffer, bit 1 del[] is
};
/** Header stored just before each line's text: [linfo][text]['\n'][4 NULs]. */
struct linfo {
	s64 len;	///< text length in bytes, without the '\n'
	s64 grec;	///< :g mark bits, one per :g nesting level
};
struct ts_state;
struct lbuf {
	struct ts_state *ts;
	unsigned long ts_revision;
	char **ln;	///< buffer lines (pointers to the text after each linfo)
	struct lopt *hist;	///< buffer history
	s64 *mark;	///< mark id, row & off triplets
	s64 mark_n;	///< number of marks in mark[]
	s64 mark_sb[2];	///< [ mark row & off
	s64 mark_se[2];	///< ] mark row & off
	s64 tmp_mark[4];	///< aux mark state: [ and ] saved when undo leaves the newest state, restored when redo gets back to it
	s64 ln_n;	///< number of lines in ln[]
	s64 ln_sz;	///< size of ln[]
	s64 useq;	///< current operation sequence
	s64 modified;	///< modification state
	s64 saved;	///< save state: hist_u when last saved, -1 if that state was discarded
	s64 hist_sz;	///< size of hist[]
	s64 hist_n;	///< current history head in hist[] (number of records)
	s64 hist_u;	///< current undo head in hist[]: [0, hist_u) undoable, [hist_u, hist_n) redoable
	s64 edseq;	///< monotonic content mutation counter
};
#define lbuf_len(lb) lb->ln_n
/* lbuf_s(ln): the linfo header of line text ln; lbuf_i(): of row pos */
#define lbuf_s(ln) ((struct linfo*)(ln - sizeof(struct linfo)))
#define lbuf_i(lb, pos) ((struct linfo*)(lb->ln[pos] - sizeof(struct linfo)))
struct lbuf *lbuf_make(void);
void lbuf_free(struct lbuf *lb);
s64 lbuf_rd(struct lbuf *lb, s64 fd, s64 beg, s64 end);
s64 lbuf_wr(struct lbuf *lb, s64 fd, s64 beg, s64 end);
void lbuf_edit(struct lbuf *lb, char *s, s64 beg, s64 end, s64 o1, s64 o2);
void lbuf_region(struct lbuf *lb, sbuf *sb, s64 r1, s64 o1, s64 r2, s64 o2);
s64 lbuf_pos2off(struct lbuf *lb, s64 r1, s64 o1, s64 r2, s64 o2, s64 row, s64 off);
s64 lbuf_off2pos(struct lbuf *lb, s64 r1, s64 o1, s64 r2, s64 o2, s64 boff, s64 *row, s64 *off);
char *lbuf_joinsb(struct lbuf *lb, s64 r1, s64 r2, sbuf *i, s64 *o1, s64 *o2);
s64 lbuf_join(struct lbuf *lb, s64 beg, s64 end, s64 o1, s64 *o2, s64 flg);
char *lbuf_get(struct lbuf *lb, s64 pos);
void lbuf_smark(struct lbuf *lb, struct lopt *lo, s64 beg, s64 o1);
void lbuf_emark(struct lbuf *lb, struct lopt *lo, s64 end, s64 o2);
struct lopt *lbuf_opt(struct lbuf *lb, s64 beg, s64 o1, s64 n_del);
void lbuf_mark(struct lbuf *lb, s64 mk, s64 pos, s64 off);
s64 lbuf_jump(struct lbuf *lb, s64 mk, s64 *pos, s64 *off);
s64 lbuf_undo(struct lbuf *lb, s64 *row, s64 *off);
s64 lbuf_redo(struct lbuf *lb, s64 *row, s64 *off);
void lbuf_saved(struct lbuf *lb, s64 clear);
s64 lbuf_indents(struct lbuf *lb, s64 r);
s64 lbuf_eol(struct lbuf *lb, s64 r, s64 state);
s64 lbuf_next(struct lbuf *lb, s64 dir, s64 *r, s64 *o);
s64 lbuf_findchar(struct lbuf *lb, char *cs, s64 cmd, s64 n, s64 *r, s64 *o);
s64 lbuf_search(struct lbuf *lb, rstr *re, s64 dir, s64 beg, s64 end, s64 pskip,
		s64 nskip, s64 *r, s64 *o);

/* lbuf_dedup: delete every line whose text (without '\n') is str[0..n) */
#define lbuf_dedup(lb, str, n) \
{ for (s64 i = 0; i < lbuf_len(lb);) { \
	char *s = lbuf_get(lb, i); \
	if (n == lbuf_s(s)->len && !memcmp(str, s, n)) \
		lbuf_edit(lb, NULL, i, i + 1, 0, 0); \
	else \
		i++; \
}} \

/* regions */
s64 lbuf_sectionbeg(struct lbuf *lb, s64 dir, s64 *row, s64 *off, s64 ch);
s64 lbuf_wordbeg(struct lbuf *lb, s64 big, s64 dir, s64 *row, s64 *off);
s64 lbuf_wordend(struct lbuf *lb, s64 big, s64 dir, s64 *row, s64 *off);
s64 lbuf_pair(struct lbuf *lb, char *pairs, s64 plen, s64 *row, s64 *off);

/* ren.c: rendering lines */
/** Rendering of one line: character positions, widths and screen columns
 * (computed by ren_position(), cached by the line pointer s). */
typedef struct {
	char **chrs;	///< chrs[i]: pointer to character i; chrs[n] is the end
	char *s;	///< line this state is for (cache key); to prevent redundant computations, ensure pointer uniqueness
	s64 *wid;	///< wid[i]: screen width of character i
	s64 *col;	///< col[c]: index of the character at screen column c
	s64 *pos;	///< pos[i]: screen column of character i
	s64 n;	///< number of characters, including the '\n'
	s64 cmax;	///< last screen column
	s64 ctx;	///< direction context of the line (dir_context())
	s64 holelen;	///< bytes of the char cut at opt_render_limit (0 if none)
	char nulhole[4];	///< saved bytes of that char, overwritten with NULs
} ren_state;
extern ren_state rstates[3];
extern __thread ren_state *rstate;
/* RST: run func with rstate = rstates[n] (cache cleared), then back to
 * rstates[0]. RST_NULL: clear the cache of the listed rstates. */
#define RST(n, func) { rstate = rstates+n; rstate->s = NULL; func; rstate -= n; }
#define RST_NULL(...) { \
	s64 i_[] = {__VA_ARGS__}; \
	for (s64 j_ = 0; j_ < LEN(i_); j_++) \
		rstates[i_[j_]].s = NULL; \
} \

ren_state *ren_position(char *s);
s64 ren_next(char *s, s64 p, s64 dir);
s64 ren_eol(char *s, s64 dir);
s64 ren_pos(char *s, s64 off);
s64 ren_cursor(char *s, s64 pos);
s64 ren_noeol(char *s, s64 p);
s64 ren_off(char *s, s64 p);
s64 ren_wrapw(s64 lncol);
char *ren_translate(char *s, char *ln);
/* text direction */
s64 dir_context(char *s);
void dir_init(void);
/* syntax highlighting */
/* Attribute words (s64): bits 0-7 fg color, 8-15 bg color; SYN_FGMK/SYN_BGMK
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
extern s64 ftidx;
extern s64 syn_scdirl;
extern s64 syn_blockhl;
char *syn_getft(void);
char *syn_setft(char *ft);
void syn_scdir(s64 scdir);
void syn_highlight(s64 *att, char *s, s64 n);
char *syn_filetype(char *path);
s64 syn_merge(s64 old, s64 new);
void syn_reloadft(s64 hl, s64 flg);
s64 syn_findhl(s64 id);
s64 syn_addhl(char *reg, s64 id);
void syn_init(void);

/* uc.c: utf-8 helper functions */
extern unsigned char utf8_length_default[256];
extern unsigned char *utf8_length;
extern s64 zwlen, def_zwlen;
extern s64 bclen, def_bclen;
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

s64 uc_wid(s64 c);
s64 uc_slen(char *s);
char *uc_chrn(char *s, s64 off, s64 *n);
char *uc_chr(char *s, s64 off);
s64 uc_off(char *s, s64 off);
char *uc_subl(char *s, s64 beg, s64 end, s64 *rlen);
char *uc_sub(char *s, s64 beg, s64 end);
/* Byte classes; unsigned wraparound makes each range one compare
 * ((c - 9) < 5 is '\t'..'\r'). uc_isalpha counts bytes >= 0x80 as letters;
 * | 0x20 folds case. */
#define uc_isspace(c) ((unsigned char)(c) == ' ' || (unsigned char)((unsigned char)(c) - 9) < 5)
#define uc_isprint(c) ((unsigned char)(c) >= 0x20 && (unsigned char)(c) != 0x7f)
#define uc_isdigit(c) (((unsigned char)(c) ^ '0') < 10)
#define uc_isalpha(c) ((unsigned char)(c) > 0x7f || (unsigned char)(((unsigned char)(c) | 0x20) - 'a') < 26)
#define uc_isupper(c) ((unsigned char)((unsigned char)(c) - 'A') < 26)
s64 uc_kind(char *c);
s64 uc_isbell(s64 c);
s64 uc_acomb(s64 c);
char *uc_beg(char *beg, char *s);
char *uc_shape(char *beg, char *s, s64 c);

/* term.c: managing the terminal */
extern struct pollfd term_ufd;
extern sbuf *term_sbuf;
extern s64 term_record;
extern s64 term_winch;
extern s64 term_resized;
extern s64 term_rows, term_cols;
extern u64 term_inbuf_pos, term_inbuf_count, term_inbuf_size, term_cmd_keys_pos;
extern unsigned char *term_inbuf, term_cmd_keys[4096];
extern u64 term_exec_type, term_exec_pushed;
/* term_write: write to stdout unless opt_line_editor is 0 */
#define term_write(s, n) if (opt_line_editor) write(1, s, n);
void term_init(void);
void term_done(void);
void term_clean(void);
void term_suspend(void);
/* leave (scrl) / enter (scrh) the alternate screen */
#define term_scrl()	term_write("\033[?1049l", 8)
#define term_scrh()	term_write("\033[?1049h", 8)
void term_chr(s64 ch);
void term_pos(s64 r, s64 c);
void term_kill(void);
void term_room(s64 n);
s64 term_read(s64 winch);
void term_commit(void);
char *term_att(s64 att);
void term_push(char *s, u64 n);
/* term_dec: unread the last byte returned by term_read() */
#define term_dec() term_inbuf_pos--; term_cmd_keys_pos--;
/* term_exec: run s[0..n) as keyboard input in a nested vi(0); type is '@'
 * (macro) or '&' (see term_exec_type). The rest of the outer input queue and the
 * term_exec_type state are restored afterwards. */
#define term_exec(s, n, type) \
{ \
	preserve(s64, term_exec_pushed, term_exec_pushed = 0;) \
	preserve(s64, term_inbuf_count,) \
	preserve(s64, term_inbuf_pos, term_inbuf_pos = term_inbuf_count;) \
	term_push(s, n); \
	preserve(s64, term_exec_type, term_exec_type = type;) \
	vi(0); \
	restore(term_exec_type) \
	if (quit_state > 0) \
		quit_state = 0; \
	restore(term_inbuf_pos) \
	restore(term_inbuf_count) \
	restore(term_exec_pushed) \
} \

/* process management */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, s64 oproc, s64 *status);
char *xgetenv(char* q[]);

/* TK_CTL(x): control key code of x; TK_INT(c): keys that abort input (NUL, ESC, ^c) */
#define TK_ESC		TK_CTL('[')
#define TK_CTL(x)	(x & 037)
#define TK_INT(c)	(!c || c == TK_ESC || c == TK_CTL('c'))

/* led.c: line-oriented input and output */
/** led_render() state, passed to every extension. */
typedef struct {
	s64 *att;	///< highlight attribute per character (per bound entry when bound is set)
	s64 *off;	///< off[c]: character offset shown at screen cell c, -1 if none
	s64 *stt;	///< stt[k]: character offset of bound entry k
	s64 *ctt;	///< ctt[j]: bound index of the j-th visible character in screen order
	char *s0;	///< the line being rendered
	char *bound;	///< visible characters only (line too wide), or NULL
	ren_state *r;	///< rendering state of s0
	s64 alen;	///< number of valid att[] entries
	s64 cterm;	///< number of screen cells
	s64 n;	///< number of characters in s0
} led_ctx;
typedef struct led_ext led_ext;
/** A syntax highlighting extension (see led.c). */
struct led_ext {
	char *ln;	///< line key; NULL matches any line
	void *usr;	///< data interpreted by the extension
	void (*ext_func)(led_ext *p, led_ctx *x);	///< extension body defaults to ext_attmerge()
	s64 blen;	///< byte length of usr
};
led_ext *led_extnew(void);
led_ext *led_extreg(void);
led_ext *led_extfind(void (*ext_func)(led_ext *p, led_ctx *x));
void led_extdel(led_ext *p);
void led_extcut(void);
s64 led_attidx(led_ctx *x, s64 off);
/* for extensions that key themselves */
/* (pointer comparison) */
#define led_extkey(p, x) (!(p)->ln || (p)->ln == (x)->s0)
void led_modeswap(void);
/** Insert-mode state kept across led_prompt() calls. */
typedef struct {
	s64 t_row;	///< history row (in tempbufs[0]) recalled next by ^a, -2 if not set
	s64 p_reg;	///< register pasted by ^p (^] cycles to the next non-empty '0'..'9', ^\ then a key selects)
	s64 lsug;	///< sb offset where the completion is inserted
	s64 sug_pt;	///< completion start set by ^x at the cursor (^x again clears), -1 if none
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

#define LED_AGENT 8 /* return control keys to the conversation prompt */
s64 led_prompt(sbuf *sb, char *insert, s64 *kmap, ins_state *is, s64 ps, s64 flg);
s64 led_input(sbuf *sb, char *post, s64 postn, s64 row, s64 flg, s64 *pren,
	s64 source_beg, s64 source_end);
void led_render(char *s0, s64 cbeg, s64 cend);
void led_render_source(char *s0, s64 cbeg, s64 cend, struct ts_state *source,
	s64 row, s64 col);
void ts_init(void);
void ts_done(void);
void ts_free(struct ts_state *s);
void ts_forget(struct lbuf *lb);
void ts_edit(struct lbuf *lb, s64 row, s64 del, char **lines, s64 ins);
struct ts_state *ts_document(struct lbuf *lb);
struct ts_state *ts_preview_begin(struct lbuf *lb, s64 beg, s64 end);
s64 ts_preview_update(struct ts_state *s, char *text);
char *ts_line(struct ts_state *s, s64 row);
extern struct ts_state *ts_preview;
extern s64 ts_redraw;
static void led_preview_current(char *text, s64 ps, s64 lncol);
static s64 ts_preview_row(s64 ps);
#define ts_winy 0
#define ts_winx 0
#define ts_winh term_rows
#define ts_winw term_cols
#define led_srender(msg, sr, sc, beg, end, view, row, col) \
{ \
	s64 record = term_record; \
	term_record = 1; \
	term_pos(sr, sc); \
	term_kill(); \
	led_render_source(msg, beg, end, view, row, col); \
	if (!record) \
		term_commit(); \
} \

/* _led_render: draw msg's screen columns [beg, end) at (row, col), after
 * running kill (led_crender clears the rest of the line). Output is
 * buffered and committed unless the caller was already recording. */
#define _led_render(msg, row, col, beg, end, kill) \
{ \
	s64 record = term_record; \
	term_record = 1; \
	term_pos(row, col); \
	kill \
	led_render(msg, beg, end); \
	if (!record) \
		term_commit(); \
} \

#define led_prender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end,)
#define led_crender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end, term_kill();)
char *led_read(s64 *kmap, s64 c);
extern s64 led_row;
s64 led_pos(char *s, s64 pos);
void led_done(void);

/* ex.c: command mode */
struct buf {
	char *ft;	///< file type
	char *path;	///< file path
	struct lbuf *lb;	///< the buffer's lines
	s64 plen, row, off, top, topsub;	///< path length; saved cursor row, char offset, view top row and its first visible wrapped segment (:lw)
	long mtime;	///< modification time
	signed char td;	///< text direction
	s64 et;	///< expandtab - use spaces for indentation
	s64 sw;	///< shiftwidth - indentation step
	s64 ts;	///< tabspace - number of spaces for tab
};
/* ex options */
extern s64 opt_left_col;
extern s64 opt_startup_flags;
extern s64 opt_autoindent;
extern s64 opt_ignorecase;
extern s64 opt_syntax_hl;
extern s64 opt_hl_line;
extern s64 opt_hl_word;
extern s64 opt_hl_pair;
extern s64 opt_hl_reverse;
extern s64 opt_line_editor;
extern s64 opt_text_dir;
extern s64 opt_shaping;
extern s64 opt_reorder;
extern s64 opt_tabstop;
extern s64 opt_expandtab;
extern s64 opt_shiftwidth;
extern s64 opt_detect_indent_lines;
extern s64 opt_interactive_shell;
extern s64 opt_search_group;
extern s64 opt_agent_exspec;
extern s64 opt_print_autocomplete;
extern s64 opt_path_complete;
extern s64 opt_multiline_prompt;
extern s64 opt_print_reg;
extern s64 opt_render_limit;
extern s64 opt_wrap_width;
extern s64 opt_hl_wrap;
extern s64 opt_undo_seq;
extern s64 opt_error_mode;
extern s64 opt_find_reg;
extern s64 opt_record_reg;
/* global variables */
extern s64 quit_state;
extern s64 cursor_row, cursor_off, view_top_row, view_top_segment;
extern s64 buf_count;
extern s64 vi_ex_depth;
extern s64 cur_keymap;
extern s64 keymap_alt;
extern s64 search_dir;
extern s64 search_changes;
extern s64 print_newline;
extern s64 ex_separator;
extern s64 ex_escape;
extern s64 ex_exec_depth;
extern sbuf *autocomplete_filter;
extern rstr *search_rset;
extern sbuf **str_registers;
extern s64 str_registers_n;
extern s64 default_reg;
extern struct buf *bufs;
extern struct buf tempbufs[5];
extern struct buf *cur_buf;
extern struct buf *prev_buf;
/* istempbuf: whether buf is one of tempbufs[]; xb*: fields of cur_buf */
#define istempbuf(buf) (buf >= tempbufs && buf < tempbufs + LEN(tempbufs))
#define xb_path cur_buf->path
#define xb_ft cur_buf->ft
#define xb cur_buf->lb
/* exbuf_load/exbuf_save: copy cursor, view (incl. topsub), text direction and
 * the indent settings et/sw/ts from/to buf */
#define exbuf_load(buf) \
	cursor_row = buf->row; \
	cursor_off = buf->off; \
	view_top_row = buf->top; \
	view_top_segment = buf->topsub; \
	opt_text_dir = buf->td; \
	opt_expandtab = buf->et; \
	opt_shiftwidth = buf->sw; \
	opt_tabstop = buf->ts; \

#define exbuf_save(buf) \
	buf->row = cursor_row; \
	buf->off = cursor_off; \
	buf->top = view_top_row; \
	buf->topsub = view_top_segment; \
	buf->td = opt_text_dir; \
	buf->et = opt_expandtab; \
	buf->sw = opt_shiftwidth; \
	buf->ts = opt_tabstop; \

/* bufs_switchwft: switch to bufs[idx] and set its filetype */
#define bufs_switchwft(idx) \
{ if (&bufs[idx] != cur_buf) { bufs_switch(idx); syn_setft(xb_ft); } } \

void bufs_switch(s64 idx);
void temp_open(s64 i, char *name, char *ft);
void temp_switch(s64 i, s64 swap);
void temp_write(s64 i, char *str);
void temp_pos(s64 i, s64 row, s64 off, s64 top);
void ex(void);
void *ex_exec(const char *ln);
/* ex_command: run ln and store it in the ':' register */
#define ex_command(ln) { ex_exec(ln); ex_regput(':', ln, 0); }
void ex_cprint(char *line, char *ft, s64 r, s64 c, s64 left, s64 flg);
/* ex_cprint2/ex_print: print using rstates[2], keeping rstates[0] intact */
#define ex_cprint2(line, ft, r, c, left, flg) { RST(2, ex_cprint(line, ft, r, c, left, flg)); }
#define ex_print(line, ft) { RST(2, ex_cprint(line, ft, -1, 0, 0, 1)); }
void ex_init(char **files, s64 n);
void ex_bufpostfix(struct buf *p, s64 clear);
s64 ex_krs(rstr **krs, s64 *dir);
void ex_krsset(char *kwd, s64 dir);
void ex_regesc(sbuf *sb, char *beg, char *end, s64 ex);
s64 ex_edit(const char *path, s64 len);
sbuf *ex_regget(s64 id);
void ex_regput(s64 c, const char *s, s64 append);

/* conf.c: configuration variables */
extern const s64 conf_mode;
/** Map file names to file types. */
struct filetype {
	char *ft;	///< file type (a name from conf.c, compared by pointer)
	char *pat;	///< file name pattern
};
extern struct filetype fts[];
extern const s64 ftslen;
/** Syntax highlighting patterns. */
struct highlight {
	char *ft;	///< the filetype of this pattern
	char *pat;	///< regular expression
	s64 *att;	///< attributes of the matched groups (see syn_highlight())
	unsigned char set;	///< subset index: consecutive entries with the same ft and set form one rset
	unsigned char id;	///< id of this hl, for syn_findhl(): 1 hlw/fuzzy, 2 hll, 3 hlp
};
extern struct highlight hls[];
extern const s64 hlslen;
extern const s64 hlopts[];
extern const s64 hloptslen;
/** Direction context: specifies the direction of a whole line. */
struct dircontext {
	char *pat;	///< pattern tested against the line
	s64 dir;	///< -1 right-to-left, +1 left-to-right
};
extern struct dircontext dctxs[];
extern const s64 dctxlen;
/** Direction marks: the direction of patterns in a line. */
struct dirmark {
	char *pat;	///< pattern; its groups get the directions in dir[]
	s64 ctx;	///< the direction context for this mark; 0 means any
	s64 dir[8];	///< the direction of a matched text group
};
extern struct dirmark dmarks[];
extern const s64 dmarkslen;
/** Character placeholders: how to show unprintable characters. */
struct placeholder {
	s64 cp[2];	///< the source character codepoint range [cp[0], cp[1]]
	char d[8];	///< the placeholder
	s64 wid;	///< the width of the placeholder
	s64 l;	///< the length of the codepoint: UTF-8 bytes the character must have
};
extern struct placeholder _ph[];
extern struct placeholder *ph;
extern s64 phlen;
extern const s64 conf_hlrev;
extern char spell_cmd[];
struct spellft {
	char *ft;		/* the filetype */
	char *arg;		/* extra speller arguments for it */
};
extern struct spellft spell_fts[];
extern const s64 spell_ftslen;
extern const s64 conf_hl_match;
extern const s64 conf_hl_match_cursor;
char **conf_kmap(s64 id);
s64 conf_kmapfind(char *name);
char *conf_digraph(s64 c1, s64 c2);

/* vi.c: main */
void vi(s64 init);
void vi_rendwait(void);
extern s64 vi_hidch;
extern s64 lnum_width;
/* soft line wrap geometry */
s64 vi_lnrows(char *s);
s64 vi_srow(s64 row);
s64 vi_drawline(s64 row, s64 trow);
/* filesystem */
extern rstr *fsincl;
void dir_calc(char *path);

/* lsp.c */
#define LSP_NFDS_MAX	8
extern s64 lsp_nfds;
extern s64 lsp_dirty;
extern s64 lsp_wake;
extern s64 lsp_fds[LSP_NFDS_MAX];
void lsp_process_fd(s64 fd);
void lsp_register(const char *ft, const char *cmd);
void lsp_open(const char *path, const char *ft);
void lsp_save(const char *path);
void lsp_sync(const char *path, struct lbuf *lb);
void lsp_hover(const char *path, s64 row, s64 off);
void lsp_definition(const char *path, s64 row, s64 off);
const char *lsp_diag_for_line(const char *path, s64 line, s64 *sev);
void lsp_list(void);
void lsp_show_msg(char *msg);
