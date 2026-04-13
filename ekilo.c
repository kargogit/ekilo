#define EKILO_VERSION "2.1.0"

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <regex.h>
#include <limits.h>

/* =========================== Syntax highlight types ======================= */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2
#define HL_MLCOMMENT 3
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8

#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[5];
    char multiline_comment_start[5];
    char multiline_comment_end[5];
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_oc;
} erow;

/* ============================ Undo/Redo history =========================== */
enum { HIST_EDIT = 1, HIST_ROWDEL = 2, HIST_SNAPSHOT = 3 };

typedef struct historyEntry {
    int type;
    uint64_t ts_ms;

    /* Common cursor/scroll states */
    int before_cy, before_cx, before_rowoff, before_coloff;
    int after_cy,  after_cx,  after_rowoff,  after_coloff;

    /* HIST_EDIT */
    int pos_cy, pos_cx;
    char *del;
    int del_len;
    char *ins;
    int ins_len;

    /* HIST_ROWDEL */
    int row_idx;
    char *row_text;
    int row_len;

    /* HIST_SNAPSHOT */
    char *snap_before;
    int snap_before_len;
    char *snap_after;
    int snap_after_len;
} historyEntry;

/* ================================ Tab buffer ============================== */
typedef struct editorBuffer {
    int cx, cy;         /* cursor position in chars (cx) and rows (cy) */
    int rx;             /* cursor position in render coords */
    int rowoff;         /* row scroll */
    int coloff;         /* column scroll (render coords) */
    int numrows;
    erow *row;

    int dirty;          /* boolean */
    int history_save;   /* history index corresponding to "saved" state, -1 = unknown/force dirty */
    historyEntry *hist;
    int hist_len;
    int hist_cap;
    int hist_index;     /* current position in history (0..hist_len) */

    char *filename;
    struct editorSyntax *syntax;
} editorBuffer;

/* =============================== Global state ============================= */
struct editorConfig {
    int textrows;     /* usable text rows (excluding tabbar+status+msg) */
    int screencols;
    int rawmode;
    int paste_mode;
    int last_key;

    int show_line_numbers;

    int help_active;
    int help_scroll;

    char statusmsg[160];
    time_t statusmsg_time;
    struct timeval last_char_time;

    editorBuffer *tabs;
    int numtabs;
    int curtab;

    int standalone_start;
    char *state_dir; /* session directory */
};

static struct editorConfig E;
static struct termios orig_termios;

/* =============================== Key handling ============================= */
#define CTRL_KEY(k) ((k)&0x1f)

enum KEY_ACTION {
    KEY_NULL = 0,
    ENTER = 13,
    ESC = 27,
    BACKSPACE = 127,

    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,

    KEY_F1
};

/* shortcuts */
enum {
    CTRL_A = 1,
    CTRL_C = 3,
    CTRL_E = 5,
    CTRL_F = 6,
    CTRL_G = 7,
    CTRL_H = 8,
    CTRL_L = 12,
    CTRL_N = 14,
    CTRL_O = 15,
    CTRL_P = 16,
    CTRL_Q = 17,
    CTRL_R = 18,
    CTRL_S = 19,
    CTRL_T = 20,
    CTRL_W = 23,
    CTRL_Y = 25,
    CTRL_Z = 26,
    CTRL_BACKSLASH = 28,
    CTRL_SLASH = 31
};

/* ============================ Minimal syntax DB =========================== */
/* C / C++ */
static char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };
static char *C_HL_keywords[] = {
    "auto","break","case","continue","default","do","else","enum","extern","for",
    "goto","if","register","return","sizeof","static","struct","switch","typedef",
    "union","volatile","while","NULL",
    "int|","long|","double|","float|","char|","unsigned|","signed|","void|","short|",
    "const|","bool|", NULL
};

/* Python */
static char *PY_HL_extensions[] = { ".py", ".pyw", NULL };
static char *PY_HL_keywords[] = {
    "and","as","assert","break","class","continue","def","del","elif","else","except",
    "finally","for","from","global","if","import","in","is","lambda","not","or",
    "pass","raise","return","try","while","with","yield","True","False","None",
    "print|","len|","range|","str|","int|","float|", NULL
};

static struct editorSyntax HLDB[] = {
    { C_HL_extensions, C_HL_keywords, "//", "/*", "*/", HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS },
    { PY_HL_extensions, PY_HL_keywords, "#",  "",   "",  HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS }
};
#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0]))

/* ============================ Forward declarations ======================== */
static void editorSetStatusMessage(const char *fmt, ...);
static void editorRefreshScreen(void);
static int editorReadKey(int fd);
static void updateWindowSize(void);
static void editorAtExit(void);
static void editorHelpModal(int startup);

static editorBuffer *curBuf(void) {
    if (E.numtabs <= 0) return NULL;
    if (E.curtab < 0) E.curtab = 0;
    if (E.curtab >= E.numtabs) E.curtab = E.numtabs - 1;
    return &E.tabs[E.curtab];
}

/* ============================ Utility / fatal ============================= */
static void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("malloc");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("realloc");
    return q;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static int digits10(int n) {
    int d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d;
}

/* ======================= Low level terminal handling ====================== */
static void disableRawMode(int fd) {
    if (E.rawmode) {
        tcsetattr(fd, TCSAFLUSH, &orig_termios);
        E.rawmode = 0;
    }
}

static int enableRawMode(int fd) {
    if (E.rawmode) return 0;
    if (!isatty(STDIN_FILENO)) { errno = ENOTTY; return -1; }
    if (tcgetattr(fd, &orig_termios) == -1) return -1;

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSAFLUSH, &raw) == -1) return -1;
    E.rawmode = 1;
    return 0;
}

static int editorReadKey(int fd) {
    int nread;
    char c;
    while ((nread = read(fd, &c, 1)) == 0) {}
    if (nread == -1) die("read");

    if (c == ESC) {
        char seq[4];
        if (read(fd, &seq[0], 1) == 0) return ESC;
        if (read(fd, &seq[1], 1) == 0) return ESC;

        /* xterm F1: ESCOP, sometimes ESC[11~ */
        if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                case 'P': return KEY_F1;
            }
            return ESC;
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(fd, &seq[2], 1) == 0) return ESC;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '1': return HOME_KEY;
                        case '4': return END_KEY;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[1] == '1') {
                    /* try F1: ESC[11~ */
                    if (seq[2] == '1') {
                        if (read(fd, &seq[3], 1) == 0) return ESC;
                        if (seq[3] == '~') return KEY_F1;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }

        return ESC;
    }
    return c;
}

static int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(ofd, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(ifd, buf + i, 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

static int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(ofd, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        int orig_row, orig_col;
        if (getCursorPosition(ifd, ofd, &orig_row, &orig_col) == -1) return -1;
        if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        if (getCursorPosition(ifd, ofd, rows, cols) == -1) return -1;

        char seq[32];
        snprintf(seq, sizeof(seq), "\x1b[%d;%dH", orig_row, orig_col);
        write(ofd, seq, strlen(seq));
        return 0;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* =============================== Append buffer ============================ */
struct abuf { char *b; int len; };
#define ABUF_INIT {NULL,0}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *newb = realloc(ab->b, ab->len + len);
    if (!newb) return;
    memcpy(newb + ab->len, s, len);
    ab->b = newb;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
}

/* ======================= Syntax highlight implementation ================== */
static int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];:{}<>", c) != NULL;
}

static int editorRowHasOpenComment(editorBuffer *B, erow *row) {
    if (!B || !B->syntax) return 0;
    char *mce = B->syntax->multiline_comment_end;
    int mcelen = (mce[0] && mce[1]) ? 2 : 0;
    if (!mcelen) return 0;
    if (row->hl && row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT) {
        if (row->rsize < 2) return 1;
        if (row->render[row->rsize - 2] == mce[0] && row->render[row->rsize - 1] == mce[1]) return 0;
        return 1;
    }
    return 0;
}

static void editorUpdateSyntax(editorBuffer *B, erow *row) {
    row->hl = xrealloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);
    if (!B || !B->syntax) return;

    char **keywords = B->syntax->keywords;
    char *scs = B->syntax->singleline_comment_start;
    char *mcs = B->syntax->multiline_comment_start;
    char *mce = B->syntax->multiline_comment_end;

    int scslen = (scs[0] ? (scs[1] ? 2 : 1) : 0);
    int mcslen = (mcs[0] && mcs[1]) ? 2 : 0;
    int mcelen = (mce[0] && mce[1]) ? 2 : 0;

    int i = 0;
    int prev_sep = 1;
    int in_string = 0;
    int in_comment = 0;

    if (row->idx > 0 && editorRowHasOpenComment(B, &B->row[row->idx - 1]))
        in_comment = 1;

    char *p = row->render;
    while (*p && isspace((unsigned char)*p)) { p++; i++; }

    while (*p) {
        if (scslen && !in_string && !in_comment && prev_sep && *p == scs[0] &&
            (scslen == 1 || *(p + 1) == scs[1])) {
            memset(row->hl + i, HL_COMMENT, (size_t)(row->rsize - i));
            return;
        }

        if (mcslen && mcelen && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (*p == mce[0] && *(p + 1) == mce[1]) {
                    row->hl[i + 1] = HL_MLCOMMENT;
                    p += 2; i += 2;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    p++; i++;
                    prev_sep = 0;
                    continue;
                }
            } else if (*p == mcs[0] && *(p + 1) == mcs[1]) {
                row->hl[i] = HL_MLCOMMENT;
                row->hl[i + 1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 1;
                prev_sep = 0;
                continue;
            }
        }

        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\' && *(p + 1)) {
                row->hl[i + 1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            prev_sep = 0;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }

        if (!isprint((unsigned char)*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        if ((B->syntax->flags & HL_HIGHLIGHT_NUMBERS) &&
            ((isdigit((unsigned char)*p) && (prev_sep || (i > 0 && row->hl[i - 1] == HL_NUMBER))) ||
             (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER))) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        if (prev_sep) {
            for (int j = 0; keywords && keywords[j]; j++) {
                int klen = (int)strlen(keywords[j]);
                int kw2 = (klen > 0 && keywords[j][klen - 1] == '|');
                if (kw2) klen--;
                if (klen <= 0) continue;

                if (!memcmp(p, keywords[j], (size_t)klen) && is_separator((unsigned char)*(p + klen))) {
                    memset(row->hl + i, (unsigned char)(kw2 ? HL_KEYWORD2 : HL_KEYWORD1), (size_t)klen);
                    p += klen; i += klen;
                    prev_sep = 0;
                    goto next_char;
                }
            }
        }

        prev_sep = is_separator((unsigned char)*p);
        p++; i++;
    next_char:
        ;
    }

    int oc = editorRowHasOpenComment(B, row);
    if (row->hl_oc != oc && row->idx + 1 < B->numrows)
        editorUpdateSyntax(B, &B->row[row->idx + 1]);
    row->hl_oc = oc;
}

static int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1:  return 33;
        case HL_KEYWORD2:  return 32;
        case HL_STRING:    return 35;
        case HL_NUMBER:    return 31;
        case HL_MATCH:     return 34;
        default:           return 37;
    }
}

static void editorSelectSyntaxHighlight(editorBuffer *B, const char *filename) {
    if (!B) return;
    B->syntax = NULL;
    if (!filename) return;

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        for (unsigned int i = 0; s->filematch && s->filematch[i]; i++) {
            const char *m = s->filematch[i];
            char *p = strstr(filename, m);
            if (!p) continue;
            int patlen = (int)strlen(m);
            if (m[0] != '.' || p[patlen] == '\0') {
                B->syntax = s;
                return;
            }
        }
    }
}

/* ========================= Row rendering and rx/cx ======================== */
static int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx && j < row->size; j++) {
        if (row->chars[j] == '\t') rx += (8 - (rx % 8));
        else rx++;
    }
    return rx;
}

static int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') cur_rx += (8 - (cur_rx % 8));
        else cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

static void editorUpdateRow(editorBuffer *B, erow *row) {
    (void)B;
    int tabs = 0;
    for (int j = 0; j < row->size; j++) if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = xmalloc((size_t)row->size + (size_t)tabs * 7 + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(B, row);
}

/* =========================== Buffer row operations ======================== */
static void bufferInsertRow(editorBuffer *B, int at, const char *s, size_t len) {
    if (at < 0 || at > B->numrows) return;
    B->row = xrealloc(B->row, sizeof(erow) * (size_t)(B->numrows + 1));
    memmove(&B->row[at + 1], &B->row[at], sizeof(erow) * (size_t)(B->numrows - at));

    for (int j = at + 1; j <= B->numrows; j++) B->row[j].idx++;

    erow *row = &B->row[at];
    row->idx = at;
    row->size = (int)len;
    row->chars = xmalloc(len + 1);
    if (len) memcpy(row->chars, s, len);
    row->chars[len] = '\0';
    row->render = NULL;
    row->rsize = 0;
    row->hl = NULL;
    row->hl_oc = 0;

    editorUpdateRow(B, row);
    B->numrows++;
}

static void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

static void bufferDelRow(editorBuffer *B, int at) {
    if (at < 0 || at >= B->numrows) return;
    editorFreeRow(&B->row[at]);
    memmove(&B->row[at], &B->row[at + 1], sizeof(erow) * (size_t)(B->numrows - at - 1));
    for (int j = at; j < B->numrows - 1; j++) B->row[j].idx = j;
    B->numrows--;
}

static void bufferRowInsertChar(editorBuffer *B, erow *row, int at, int c) {
    if (at < 0) at = 0;
    if (at > row->size) at = row->size;

    row->chars = xrealloc(row->chars, (size_t)row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(B, row);
}

static void bufferRowAppendString(editorBuffer *B, erow *row, const char *s, size_t len) {
    row->chars = xrealloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    editorUpdateRow(B, row);
}

static void bufferRowDelChar(editorBuffer *B, erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(B, row);
}

static char *bufferRowsToString(editorBuffer *B, int *buflen) {
    int totlen = 0;
    for (int j = 0; j < B->numrows; j++) totlen += B->row[j].size + 1;
    *buflen = totlen;

    char *buf = xmalloc((size_t)totlen + 1);
    char *p = buf;
    for (int j = 0; j < B->numrows; j++) {
        memcpy(p, B->row[j].chars, (size_t)B->row[j].size);
        p += B->row[j].size;
        *p++ = '\n';
    }
    *p = '\0';
    return buf;
}

/* =========================== Insert / delete text ========================= */
struct autopair { int open_char; int close_char; };
static struct autopair autopairs[] = {
    { '{','}' }, { '[',']' }, { '(',')' }, { '"','"' }, { '\'','\'' }, { '`','`' }
};

static int editorFindCloseChar(int open_char) {
    for (size_t i = 0; i < sizeof(autopairs)/sizeof(autopairs[0]); i++)
        if (autopairs[i].open_char == open_char) return autopairs[i].close_char;
    return 0;
}

static void bufferInsertChar(editorBuffer *B, int c) {
    if (B->cy == B->numrows) bufferInsertRow(B, B->numrows, "", 0);
    erow *row = &B->row[B->cy];
    bufferRowInsertChar(B, row, B->cx, c);
    B->cx++;
}

static void bufferInsertNewline(editorBuffer *B) {
    if (B->cx == 0) {
        bufferInsertRow(B, B->cy, "", 0);
    } else {
        erow *row = &B->row[B->cy];
        bufferInsertRow(B, B->cy + 1, &row->chars[B->cx], (size_t)(row->size - B->cx));
        row = &B->row[B->cy];
        row->size = B->cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(B, row);
    }
    B->cy++;
    B->cx = 0;
}

/* Forward delete at cursor (Del semantics). */
static void bufferDelCharForward(editorBuffer *B) {
    if (B->cy >= B->numrows) return;
    erow *row = &B->row[B->cy];

    if (B->cx < row->size) {
        bufferRowDelChar(B, row, B->cx);
        return;
    }
    /* at end of line: merge next line */
    if (B->cx == row->size && B->cy < B->numrows - 1) {
        erow *next = &B->row[B->cy + 1];
        bufferRowAppendString(B, row, next->chars, (size_t)next->size);
        bufferDelRow(B, B->cy + 1);
        return;
    }
}

/* ============================= Cursor & scroll ============================ */
static void bufferUpdateRx(editorBuffer *B) {
    if (B->cy >= B->numrows) { B->rx = 0; return; }
    B->rx = editorRowCxToRx(&B->row[B->cy], B->cx);
}

static int editorLineNumberWidth(editorBuffer *B) {
    if (!E.show_line_numbers) return 0;
    int n = (B && B->numrows > 0) ? B->numrows : 1;
    int w = digits10(n);
    return w + 2; /* "NN " */
}

static int editorTextCols(editorBuffer *B) {
    int lnw = editorLineNumberWidth(B);
    int tc = E.screencols - lnw;
    if (tc < 1) tc = 1;
    return tc;
}

static void editorScroll(void) {
    editorBuffer *B = curBuf();
    if (!B) return;

    bufferUpdateRx(B);
    int textcols = editorTextCols(B);

    if (B->cy < B->rowoff) B->rowoff = B->cy;
    if (B->cy >= B->rowoff + E.textrows) B->rowoff = B->cy - E.textrows + 1;

    if (B->rx < B->coloff) B->coloff = B->rx;
    if (B->rx >= B->coloff + textcols) B->coloff = B->rx - textcols + 1;
}

static void editorMoveCursor(int key) {
    editorBuffer *B = curBuf();
    if (!B) return;

    erow *row = (B->cy >= B->numrows) ? NULL : &B->row[B->cy];

    switch (key) {
        case ARROW_LEFT:
            if (B->cx != 0) {
                B->cx--;
            } else if (B->cy > 0) {
                B->cy--;
                B->cx = B->row[B->cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && B->cx < row->size) {
                B->cx++;
            } else if (row && B->cx == row->size) {
                B->cy++;
                B->cx = 0;
            }
            break;
        case ARROW_UP:
            if (B->cy != 0) B->cy--;
            break;
        case ARROW_DOWN:
            if (B->cy < B->numrows) B->cy++;
            break;
    }

    row = (B->cy >= B->numrows) ? NULL : &B->row[B->cy];
    int rowlen = row ? row->size : 0;
    if (B->cx > rowlen) B->cx = rowlen;
}

static void bufferClampCursor(editorBuffer *B) {
    if (B->cy < 0) B->cy = 0;
    if (B->cy > B->numrows) B->cy = B->numrows;
    if (B->cy < B->numrows) {
        int maxcx = B->row[B->cy].size;
        if (B->cx < 0) B->cx = 0;
        if (B->cx > maxcx) B->cx = maxcx;
    } else {
        B->cx = 0;
    }
}

/* ============================= History (Undo/Redo) ======================== */
static void historyFreeEntry(historyEntry *e) {
    if (!e) return;
    free(e->del);
    free(e->ins);
    free(e->row_text);
    free(e->snap_before);
    free(e->snap_after);
    memset(e, 0, sizeof(*e));
}

static void historyTruncateFrom(editorBuffer *B, int idx) {
    if (!B || idx < 0) return;
    if (idx > B->hist_len) idx = B->hist_len;
    for (int i = idx; i < B->hist_len; i++) historyFreeEntry(&B->hist[i]);
    B->hist_len = idx;
    if (B->hist_index > B->hist_len) B->hist_index = B->hist_len;
}

static void bufferUpdateDirty(editorBuffer *B) {
    if (!B) return;
    if (B->history_save < 0) {
        B->dirty = 1;
    } else {
        B->dirty = (B->hist_index != B->history_save);
    }
}

static int entryHasNewline(const char *s, int len) {
    for (int i = 0; i < len; i++) if (s[i] == '\n') return 1;
    return 0;
}

static int historyTryMergeInsert(editorBuffer *B, historyEntry *in) {
    if (!B || !in) return 0;
    if (B->hist_index != B->hist_len) return 0; /* no merge across undone branch */
    if (B->hist_len <= 0) return 0;

    historyEntry *prev = &B->hist[B->hist_len - 1];
    if (prev->type != HIST_EDIT || in->type != HIST_EDIT) return 0;

    /* Only merge pure insertions without newlines. */
    if (prev->del_len != 0 || in->del_len != 0) return 0;
    if (prev->ins_len <= 0 || in->ins_len <= 0) return 0;
    if (entryHasNewline(prev->ins, prev->ins_len)) return 0;
    if (entryHasNewline(in->ins, in->ins_len)) return 0;

    /* Time window */
    if (in->ts_ms < prev->ts_ms) return 0;
    if (in->ts_ms - prev->ts_ms > 900) return 0;

    /* Contiguity: insert happens exactly after previous insertion. */
    if (in->pos_cy != prev->pos_cy) return 0;
    if (in->pos_cx != prev->pos_cx + prev->ins_len) return 0;

    /* Cursor continuity */
    if (in->before_cy != prev->after_cy || in->before_cx != prev->after_cx) return 0;

    prev->ins = xrealloc(prev->ins, (size_t)prev->ins_len + (size_t)in->ins_len + 1);
    memcpy(prev->ins + prev->ins_len, in->ins, (size_t)in->ins_len);
    prev->ins_len += in->ins_len;
    prev->ins[prev->ins_len] = '\0';

    /* Expand after-state to the newest */
    prev->after_cy = in->after_cy;
    prev->after_cx = in->after_cx;
    prev->after_rowoff = in->after_rowoff;
    prev->after_coloff = in->after_coloff;
    prev->ts_ms = in->ts_ms;

    historyFreeEntry(in);
    return 1;
}

static void historyPush(editorBuffer *B, historyEntry *e) {
    if (!B || !e) return;

    if (B->hist_index < B->hist_len) historyTruncateFrom(B, B->hist_index);

    if (historyTryMergeInsert(B, e)) {
        B->hist_index = B->hist_len;
        bufferUpdateDirty(B);
        return;
    }

    if (B->hist_len + 1 > B->hist_cap) {
        int ncap = (B->hist_cap == 0) ? 64 : (B->hist_cap * 2);
        B->hist = xrealloc(B->hist, sizeof(historyEntry) * (size_t)ncap);
        memset(B->hist + B->hist_cap, 0, sizeof(historyEntry) * (size_t)(ncap - B->hist_cap));
        B->hist_cap = ncap;
    }
    B->hist[B->hist_len++] = *e;
    memset(e, 0, sizeof(*e));
    B->hist_index = B->hist_len;
    bufferUpdateDirty(B);
}

static void historyClear(editorBuffer *B) {
    if (!B) return;
    historyTruncateFrom(B, 0);
    free(B->hist);
    B->hist = NULL;
    B->hist_len = B->hist_cap = B->hist_index = 0;
    B->history_save = 0;
    bufferUpdateDirty(B);
}

/* Raw insert/delete strings at current cursor (no auto-pair). */
static void bufferInsertStringRaw(editorBuffer *B, const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n') bufferInsertNewline(B);
        else bufferInsertChar(B, (unsigned char)s[i]);
    }
}

static void bufferDeleteStringForwardRaw(editorBuffer *B, const char *s, int len) {
    (void)s; /* deletion uses only length; content is informational */
    for (int i = 0; i < len; i++) bufferDelCharForward(B);
}

static void bufferLoadFromString(editorBuffer *B, const char *buf, int len) {
    /* clear rows only */
    for (int j = 0; j < B->numrows; j++) editorFreeRow(&B->row[j]);
    free(B->row);
    B->row = NULL;
    B->numrows = 0;

    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            int l = i - start;
            if (l < 0) l = 0;
            bufferInsertRow(B, B->numrows, buf + start, (size_t)l);
            start = i + 1;
        }
    }
    /* bufferRowsToString always ends lines with \n; when loading that string,
       we create an extra empty last row. Remove if it was purely the trailing newline. */
    if (len > 0 && buf[len - 1] == '\n') {
        if (B->numrows > 0 && B->row[B->numrows - 1].size == 0) bufferDelRow(B, B->numrows - 1);
    }

    B->cx = B->cy = B->rx = 0;
    B->rowoff = B->coloff = 0;
}

/* Apply history entry forward (redo-direction) */
static void historyApplyRedo(editorBuffer *B, historyEntry *e) {
    if (!B || !e) return;

    if (e->type == HIST_EDIT) {
        B->cy = e->pos_cy; B->cx = e->pos_cx;
        bufferClampCursor(B);

        if (e->del_len > 0) bufferDeleteStringForwardRaw(B, e->del, e->del_len);
        if (e->ins_len > 0) bufferInsertStringRaw(B, e->ins, e->ins_len);

        B->cy = e->after_cy; B->cx = e->after_cx;
        B->rowoff = e->after_rowoff; B->coloff = e->after_coloff;
        bufferClampCursor(B);
    } else if (e->type == HIST_ROWDEL) {
        if (e->row_idx >= 0 && e->row_idx < B->numrows) bufferDelRow(B, e->row_idx);
        B->cy = e->after_cy; B->cx = e->after_cx;
        B->rowoff = e->after_rowoff; B->coloff = e->after_coloff;
        bufferClampCursor(B);
    } else if (e->type == HIST_SNAPSHOT) {
        bufferLoadFromString(B, e->snap_after, e->snap_after_len);
        B->cy = e->after_cy; B->cx = e->after_cx;
        B->rowoff = e->after_rowoff; B->coloff = e->after_coloff;
        bufferClampCursor(B);
    }
}

/* Apply history entry backward (undo-direction) */
static void historyApplyUndo(editorBuffer *B, historyEntry *e) {
    if (!B || !e) return;

    if (e->type == HIST_EDIT) {
        B->cy = e->pos_cy; B->cx = e->pos_cx;
        bufferClampCursor(B);

        if (e->ins_len > 0) bufferDeleteStringForwardRaw(B, e->ins, e->ins_len);
        if (e->del_len > 0) bufferInsertStringRaw(B, e->del, e->del_len);

        B->cy = e->before_cy; B->cx = e->before_cx;
        B->rowoff = e->before_rowoff; B->coloff = e->before_coloff;
        bufferClampCursor(B);
    } else if (e->type == HIST_ROWDEL) {
        if (e->row_idx < 0) e->row_idx = 0;
        if (e->row_idx > B->numrows) e->row_idx = B->numrows;
        bufferInsertRow(B, e->row_idx, e->row_text ? e->row_text : "", (size_t)e->row_len);

        B->cy = e->before_cy; B->cx = e->before_cx;
        B->rowoff = e->before_rowoff; B->coloff = e->before_coloff;
        bufferClampCursor(B);
    } else if (e->type == HIST_SNAPSHOT) {
        bufferLoadFromString(B, e->snap_before, e->snap_before_len);
        B->cy = e->before_cy; B->cx = e->before_cx;
        B->rowoff = e->before_rowoff; B->coloff = e->before_coloff;
        bufferClampCursor(B);
    }
}

static void editorUndo(editorBuffer *B) {
    if (!B) return;
    if (B->hist_index <= 0) {
        editorSetStatusMessage("Undo: nothing to undo");
        return;
    }
    B->hist_index--;
    historyApplyUndo(B, &B->hist[B->hist_index]);
    bufferUpdateDirty(B);
    editorSetStatusMessage("Undo");
}

static void editorRedo(editorBuffer *B) {
    if (!B) return;
    if (B->hist_index >= B->hist_len) {
        editorSetStatusMessage("Redo: nothing to redo");
        return;
    }
    historyApplyRedo(B, &B->hist[B->hist_index]);
    B->hist_index++;
    bufferUpdateDirty(B);
    editorSetStatusMessage("Redo");
}

/* =============================== File I/O ================================= */
static void bufferClearContent(editorBuffer *B) {
    for (int j = 0; j < B->numrows; j++) editorFreeRow(&B->row[j]);
    free(B->row);
    B->row = NULL;
    B->numrows = 0;

    B->cx = B->cy = B->rx = 0;
    B->rowoff = B->coloff = 0;
}

static int bufferOpenFile(editorBuffer *B, const char *filename) {
    bufferClearContent(B);
    historyClear(B);

    free(B->filename);
    B->filename = filename ? xstrdup(filename) : NULL;
    editorSelectSyntaxHighlight(B, B->filename);

    if (!filename) {
        B->history_save = 0;
        bufferUpdateDirty(B);
        return 0;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) {
            B->history_save = 0;
            bufferUpdateDirty(B);
            return 0;
        }
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &cap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        bufferInsertRow(B, B->numrows, line, (size_t)linelen);
    }
    free(line);
    fclose(fp);

    B->history_save = B->hist_index;
    bufferUpdateDirty(B);
    return 0;
}

static int bufferSaveToFilename(editorBuffer *B, const char *filename) {
    if (!filename || !*filename) return 1;

    int len;
    char *buf = bufferRowsToString(B, &len);

    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) { free(buf); return 1; }

    if (ftruncate(fd, len) == -1) { close(fd); free(buf); return 1; }
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    free(buf);
    if (w != len) return 1;

    free(B->filename);
    B->filename = xstrdup(filename);
    editorSelectSyntaxHighlight(B, B->filename);

    B->history_save = B->hist_index;
    bufferUpdateDirty(B);

    editorSetStatusMessage("%d bytes written", len);
    return 0;
}

/* =============================== Prompt =================================== */
static int editorIsHelpKey(int c) {
    return (c == KEY_F1 || c == CTRL_SLASH);
}

static char *editorPrompt(const char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = xmalloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey(STDIN_FILENO);

        if (editorIsHelpKey(c)) {
            editorHelpModal(0);
            continue;
        }

        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (buflen) buf[--buflen] = '\0';
        } else if (c == ESC) {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == ENTER) {
            if (callback) callback(buf, c);
            editorSetStatusMessage("");
            return buf;
        } else if (!iscntrl(c) && c < 128) {
            if (buflen + 1 >= bufsize) {
                bufsize *= 2;
                buf = xrealloc(buf, bufsize);
            }
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        } else if (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT || c == ARROW_RIGHT) {
            /* allow callbacks to use arrows */
        }

        if (callback) callback(buf, c);
    }
}

/* ============================== Find (plain + regex) ====================== */
enum { FIND_PLAIN = 1, FIND_REGEX = 2 };

static int findLastSubstringBefore(const char *hay, int haylen, const char *needle, int needlen, int limit) {
    if (needlen <= 0) return -1;
    if (limit < 0) limit = 0;
    if (limit > haylen) limit = haylen;
    int last = -1;
    for (int i = 0; i + needlen <= haylen && i < limit; i++) {
        if (memcmp(hay + i, needle, (size_t)needlen) == 0) last = i;
    }
    return last;
}

static int rowRegexMatchForward(erow *row, regex_t *re, int start, int *mstart, int *mend, regmatch_t pm[10]) {
    if (start < 0) start = 0;
    if (start > row->size) return 0;
    const char *s = row->chars + start;

    if (regexec(re, s, 10, pm, 0) != 0) return 0;
    if (pm[0].rm_so < 0) return 0;

    *mstart = start + (int)pm[0].rm_so;
    *mend   = start + (int)pm[0].rm_eo;
    return 1;
}

static int rowRegexMatchBackwardBefore(erow *row, regex_t *re, int limit, int *mstart, int *mend, regmatch_t outpm[10]) {
    if (limit < 0) limit = 0;
    if (limit > row->size) limit = row->size;

    int pos = 0;
    int found = 0;
    regmatch_t pm[10];

    while (pos <= limit) {
        if (regexec(re, row->chars + pos, 10, pm, 0) != 0) break;
        if (pm[0].rm_so < 0) break;

        int so = pos + (int)pm[0].rm_so;
        int eo = pos + (int)pm[0].rm_eo;
        if (so >= limit) break;

        *mstart = so;
        *mend = eo;
        memcpy(outpm, pm, sizeof(pm));
        found = 1;

        if (eo == so) pos = so + 1; else pos = eo;
    }
    return found;
}

/* Find callback state */
static void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    static int saved_hl_line = -1;
    static unsigned char *saved_hl = NULL;
    static int last_mstart = 0;
    static int mode = FIND_PLAIN;

    editorBuffer *B = curBuf();
    if (!B) return;

    if (key == CTRL_E) mode = FIND_REGEX;
    if (key == CTRL_F) mode = FIND_PLAIN;

    if (saved_hl) {
        if (saved_hl_line >= 0 && saved_hl_line < B->numrows) {
            erow *r = &B->row[saved_hl_line];
            if (r->hl && r->rsize) memcpy(r->hl, saved_hl, (size_t)r->rsize);
        }
        free(saved_hl);
        saved_hl = NULL;
        saved_hl_line = -1;
    }

    if (key == ENTER || key == ESC) {
        last_match = -1;
        direction = 1;
        last_mstart = 0;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else if (key == CTRL_E) {
        last_match = -1; direction = 1; last_mstart = 0;
    } else if (key == CTRL_F) {
        last_match = -1; direction = 1; last_mstart = 0;
    } else {
        last_match = -1;
        direction = 1;
        last_mstart = 0;
    }

    if (!query || !*query) return;

    int current = last_match;

    regex_t re;
    int re_ok = 0;
    int re_compiled = 0;

    if (mode == FIND_REGEX) {
        int rc = regcomp(&re, query, REG_EXTENDED);
        re_compiled = 1;
        if (rc != 0) {
            char errbuf[128];
            regerror(rc, &re, errbuf, sizeof(errbuf));
            editorSetStatusMessage("Regex error: %s", errbuf);
            return;
        }
        re_ok = 1;
    }

    for (int i = 0; i < B->numrows; i++) {
        if (current == -1) current = (direction == 1) ? 0 : (B->numrows - 1);
        else current += direction;

        if (current < 0) current = B->numrows - 1;
        if (current >= B->numrows) current = 0;

        erow *row = &B->row[current];

        int mstart = 0, mend = 0;
        int ok = 0;

        if (mode == FIND_PLAIN) {
            int qlen = (int)strlen(query);
            if (qlen <= 0) continue;

            if (direction == 1) {
                int start = 0;
                if (current == last_match) start = last_mstart + 1;
                if (start < 0) start = 0;
                if (start > row->size) start = row->size;
                char *m = strstr(row->chars + start, query);
                if (m) {
                    mstart = (int)(m - row->chars);
                    mend = mstart + qlen;
                    ok = 1;
                }
            } else {
                int limit = row->size + 1;
                if (current == last_match) limit = last_mstart;
                int pos = findLastSubstringBefore(row->chars, row->size, query, qlen, limit);
                if (pos >= 0) {
                    mstart = pos;
                    mend = pos + qlen;
                    ok = 1;
                }
            }
        } else {
            if (!re_ok) continue;
            regmatch_t pm[10];
            if (direction == 1) {
                int start = 0;
                if (current == last_match) start = last_mstart + 1;
                ok = rowRegexMatchForward(row, &re, start, &mstart, &mend, pm);
            } else {
                int limit = row->size + 1;
                if (current == last_match) limit = last_mstart;
                ok = rowRegexMatchBackwardBefore(row, &re, limit, &mstart, &mend, pm);
            }
        }

        if (!ok) continue;

        int rx1 = editorRowCxToRx(row, mstart);
        int rx2 = editorRowCxToRx(row, mend);
        int rlen = rx2 - rx1;
        if (rlen < 1) rlen = 1;

        saved_hl_line = current;
        saved_hl = xmalloc((size_t)row->rsize);
        memcpy(saved_hl, row->hl, (size_t)row->rsize);
        if (rx1 >= 0 && rx1 < row->rsize) {
            int maxlen = row->rsize - rx1;
            if (rlen > maxlen) rlen = maxlen;
            memset(row->hl + rx1, HL_MATCH, (size_t)rlen);
        }

        last_match = current;
        last_mstart = mstart;

        B->cy = current;
        B->cx = mstart;
        editorScroll();
        break;
    }

    if (re_compiled) regfree(&re);
}

static void editorFindPlain(void) {
    (void)editorPrompt("Find: %s  (Arrows next/prev, Ctrl-E regex mode, ESC cancels)", editorFindCallback);
}

static void editorFindRegex(void) {
    (void)editorPrompt("Regex find: %s  (Arrows next/prev, Ctrl-F plain mode, ESC cancels)", editorFindCallback);
}

/* =========================== Regex Replace (All) + Undo ==================== */
static void replAppend(char **dst, size_t *cap, size_t *len, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap *= 2;
        *dst = xrealloc(*dst, *cap);
    }
    memcpy(*dst + *len, s, n);
    *len += n;
    (*dst)[*len] = '\0';
}

static char *expandReplacement(const char *repl, const char *base, regmatch_t pm[10], size_t *outlen) {
    size_t cap = 64, len = 0;
    char *out = xmalloc(cap);
    out[0] = '\0';

    for (size_t i = 0; repl[i]; i++) {
        if (repl[i] == '\\') {
            char n = repl[i + 1];
            if (n == '\\') {
                replAppend(&out, &cap, &len, "\\", 1);
                i++;
            } else if (n == '0' || (n >= '1' && n <= '9')) {
                int g = n - '0';
                i++;
                if (pm[g].rm_so >= 0 && pm[g].rm_eo >= pm[g].rm_so) {
                    replAppend(&out, &cap, &len, base + pm[g].rm_so, (size_t)(pm[g].rm_eo - pm[g].rm_so));
                }
            } else {
                if (n) { replAppend(&out, &cap, &len, &repl[i + 1], 1); i++; }
                else { replAppend(&out, &cap, &len, "\\", 1); }
            }
        } else {
            replAppend(&out, &cap, &len, &repl[i], 1);
        }
    }

    if (outlen) *outlen = len;
    return out;
}

static int rowReplaceAllRegex(editorBuffer *B, erow *row, regex_t *re, const char *repl, int *replcount) {
    int replaced_any = 0;

    size_t cap = (size_t)row->size + 64;
    size_t outlen = 0;
    char *out = xmalloc(cap);
    out[0] = '\0';

    int pos = 0;
    while (pos <= row->size) {
        regmatch_t pm[10];
        if (regexec(re, row->chars + pos, 10, pm, 0) != 0) break;
        if (pm[0].rm_so < 0) break;

        int so = pos + (int)pm[0].rm_so;
        int eo = pos + (int)pm[0].rm_eo;

        replAppend(&out, &cap, &outlen, row->chars + pos, (size_t)(so - pos));

        size_t explen = 0;
        char *exp = expandReplacement(repl, row->chars + pos, pm, &explen);
        replAppend(&out, &cap, &outlen, exp, explen);
        free(exp);

        replaced_any = 1;
        if (replcount) (*replcount)++;

        if (eo == so) {
            if (eo < row->size) {
                replAppend(&out, &cap, &outlen, row->chars + eo, 1);
            }
            pos = eo + 1;
        } else {
            pos = eo;
        }
    }

    if (pos < row->size) replAppend(&out, &cap, &outlen, row->chars + pos, (size_t)(row->size - pos));

    if (replaced_any) {
        free(row->chars);
        row->chars = out;
        row->size = (int)outlen;
        editorUpdateRow(B, row);
        return 1;
    }

    free(out);
    return 0;
}

static void editorReplaceRegexAll(void) {
    editorBuffer *B = curBuf();
    if (!B) return;

    char *pattern = editorPrompt("Replace (regex): %s  (ESC cancels)", NULL);
    if (!pattern) return;

    char *repl = editorPrompt("With: %s  (ESC cancels)", NULL);
    if (!repl) { free(pattern); return; }

    editorSetStatusMessage("Replace all? (y/n)");
    editorRefreshScreen();
    int yn = editorReadKey(STDIN_FILENO);
    if (yn != 'y' && yn != 'Y') {
        editorSetStatusMessage("Replace cancelled");
        free(pattern);
        free(repl);
        return;
    }

    /* Snapshot for undo/redo */
    int before_len = 0;
    char *before = bufferRowsToString(B, &before_len);
    int before_cy = B->cy, before_cx = B->cx, before_rowoff = B->rowoff, before_coloff = B->coloff;

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char errbuf[128];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        editorSetStatusMessage("Regex error: %s", errbuf);
        free(pattern);
        free(repl);
        free(before);
        return;
    }

    int count = 0;
    for (int i = 0; i < B->numrows; i++) {
        rowReplaceAllRegex(B, &B->row[i], &re, repl, &count);
    }
    regfree(&re);

    int after_len = 0;
    char *after = bufferRowsToString(B, &after_len);

    historyEntry he;
    memset(&he, 0, sizeof(he));
    he.type = HIST_SNAPSHOT;
    he.ts_ms = now_ms();
    he.before_cy = before_cy; he.before_cx = before_cx;
    he.before_rowoff = before_rowoff; he.before_coloff = before_coloff;
    he.after_cy = B->cy; he.after_cx = B->cx;
    he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;
    he.snap_before = before; he.snap_before_len = before_len;
    he.snap_after = after; he.snap_after_len = after_len;
    historyPush(B, &he);

    editorSetStatusMessage("Replaced %d match(es)", count);
    free(pattern);
    free(repl);
}

/* ================================ Tabs ==================================== */
static void bufferInit(editorBuffer *B) {
    memset(B, 0, sizeof(*B));
    B->cx = B->cy = B->rx = 0;
    B->rowoff = B->coloff = 0;
    B->numrows = 0;
    B->row = NULL;

    B->dirty = 0;
    B->history_save = 0;
    B->hist = NULL;
    B->hist_len = B->hist_cap = B->hist_index = 0;

    B->filename = NULL;
    B->syntax = NULL;
}

static void bufferFree(editorBuffer *B) {
    bufferClearContent(B);
    historyClear(B);
    free(B->filename);
    B->filename = NULL;
}

static const char *bufferDisplayName(editorBuffer *B) {
    return (B && B->filename) ? B->filename : "[No Name]";
}

static int editorTabNew(const char *filename, int switch_to) {
    E.tabs = xrealloc(E.tabs, sizeof(editorBuffer) * (size_t)(E.numtabs + 1));
    editorBuffer *B = &E.tabs[E.numtabs];
    bufferInit(B);

    if (filename) {
        if (bufferOpenFile(B, filename) == -1) {
            editorSetStatusMessage("Open failed: %s", strerror(errno));
        }
    } else {
        editorSelectSyntaxHighlight(B, NULL);
        B->history_save = 0;
        bufferUpdateDirty(B);
    }

    E.numtabs++;
    if (switch_to) E.curtab = E.numtabs - 1;
    return E.numtabs - 1;
}

static void editorTabSwitch(int idx) {
    if (idx < 0 || idx >= E.numtabs) return;
    E.curtab = idx;
    editorSetStatusMessage("Tab %d/%d: %s", E.curtab + 1, E.numtabs, bufferDisplayName(curBuf()));
}

static void editorTabNext(void) {
    if (E.numtabs <= 1) return;
    int n = (E.curtab + 1) % E.numtabs;
    editorTabSwitch(n);
}

static void editorTabPrev(void) {
    if (E.numtabs <= 1) return;
    int p = (E.curtab - 1 + E.numtabs) % E.numtabs;
    editorTabSwitch(p);
}

static void editorTabClose(int idx, int force) {
    if (idx < 0 || idx >= E.numtabs) return;
    editorBuffer *B = &E.tabs[idx];

    if (!force && B->dirty) {
        editorSetStatusMessage("Unsaved tab. Press Ctrl-W again to close.");
        return;
    }

    bufferFree(B);

    memmove(&E.tabs[idx], &E.tabs[idx + 1], sizeof(editorBuffer) * (size_t)(E.numtabs - idx - 1));
    E.numtabs--;

    if (E.numtabs == 0) {
        E.tabs = NULL;
        E.curtab = 0;
        editorTabNew(NULL, 1);
    } else {
        if (E.curtab >= E.numtabs) E.curtab = E.numtabs - 1;
    }
    editorSetStatusMessage("Closed tab");
}

static int editorAnyDirty(void) {
    for (int i = 0; i < E.numtabs; i++) {
        if (E.tabs[i].dirty) return 1;
    }
    return 0;
}

/* ============================ Session persistence ========================= */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    size_t len;

    if (!path) return -1;
    len = strnlen(path, sizeof(tmp) - 1);
    if (len == 0 || len >= sizeof(tmp) - 1) return -1;

    memcpy(tmp, path, len);
    tmp[len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) == -1 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) == -1 && errno != EEXIST) return -1;
    return 0;
}

static char *editorGetStateDir(void) {
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char base[PATH_MAX];

    if (xdg && *xdg) {
        snprintf(base, sizeof(base), "%s", xdg);
    } else if (home && *home) {
        snprintf(base, sizeof(base), "%s/.local/state", home);
    } else {
        snprintf(base, sizeof(base), ".");
    }

    char *dir = xmalloc(PATH_MAX);
    snprintf(dir, PATH_MAX, "%s/ekilo", base);
    (void)mkdir_p(dir, 0700);
    return dir;
}

static void editorSaveSession(void) {
    if (!E.state_dir) return;

    char session_path[PATH_MAX];
    snprintf(session_path, sizeof(session_path), "%s/session.v1", E.state_dir);

    FILE *fp = fopen(session_path, "w");
    if (!fp) return;

    fprintf(fp, "EKILO_SESSION 1\n");
    fprintf(fp, "numtabs=%d\n", E.numtabs);
    fprintf(fp, "curtab=%d\n", E.curtab);

    for (int i = 0; i < E.numtabs; i++) {
        editorBuffer *B = &E.tabs[i];

        char snapname[64];
        snprintf(snapname, sizeof(snapname), "tab_%03d.txt", i);

        char snappath[PATH_MAX];
        snprintf(snappath, sizeof(snappath), "%s/%s", E.state_dir, snapname);

        int len = 0;
        char *buf = bufferRowsToString(B, &len);

        int fd = open(snappath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd != -1) {
            (void)write(fd, buf, (size_t)len);
            close(fd);
        }
        free(buf);

        fprintf(fp, "TAB\n");
        fprintf(fp, "filename=%s\n", B->filename ? B->filename : "");
        fprintf(fp, "dirty=%d\n", B->dirty ? 1 : 0);
        fprintf(fp, "cx=%d\ncy=%d\nrowoff=%d\ncoloff=%d\n", B->cx, B->cy, B->rowoff, B->coloff);
        fprintf(fp, "snapshot=%s\n", snapname);
        fprintf(fp, "ENDTAB\n");
    }

    fclose(fp);
}

static int editorLoadSession(void) {
    if (!E.state_dir) return 0;

    char session_path[PATH_MAX];
    snprintf(session_path, sizeof(session_path), "%s/session.v1", E.state_dir);

    FILE *fp = fopen(session_path, "r");
    if (!fp) return 0;

    char line[PATH_MAX + 64];
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    if (strncmp(line, "EKILO_SESSION 1", 15) != 0) { fclose(fp); return 0; }

    int numtabs = 0;
    int curtab = 0;

    for (int i = 0; i < E.numtabs; i++) bufferFree(&E.tabs[i]);
    free(E.tabs);
    E.tabs = NULL;
    E.numtabs = 0;
    E.curtab = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "numtabs=", 8) == 0) numtabs = atoi(line + 8);
        else if (strncmp(line, "curtab=", 7) == 0) curtab = atoi(line + 7);
        else if (strncmp(line, "TAB", 3) == 0) break;
    }

    for (int i = 0; i < numtabs; i++) {
        char filename[PATH_MAX] = {0};
        int dirty = 0, cx = 0, cy = 0, rowoff = 0, coloff = 0;
        char snapshot[128] = {0};

        if (i > 0) {
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "TAB", 3) == 0) break;
            }
        }

        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "ENDTAB", 6) == 0) break;
            if (strncmp(line, "filename=", 9) == 0) {
                snprintf(filename, sizeof(filename), "%s", line + 9);
                filename[strcspn(filename, "\r\n")] = '\0';
            } else if (strncmp(line, "dirty=", 6) == 0) dirty = atoi(line + 6);
            else if (strncmp(line, "cx=", 3) == 0) cx = atoi(line + 3);
            else if (strncmp(line, "cy=", 3) == 0) cy = atoi(line + 3);
            else if (strncmp(line, "rowoff=", 7) == 0) rowoff = atoi(line + 7);
            else if (strncmp(line, "coloff=", 7) == 0) coloff = atoi(line + 7);
            else if (strncmp(line, "snapshot=", 9) == 0) {
                snprintf(snapshot, sizeof(snapshot), "%s", line + 9);
                snapshot[strcspn(snapshot, "\r\n")] = '\0';
            }
        }

        int idx = editorTabNew((filename[0] ? filename : NULL), 0);
        editorBuffer *B = &E.tabs[idx];

        if (snapshot[0]) {
            char snappath[PATH_MAX];
            snprintf(snappath, sizeof(snappath), "%s/%s", E.state_dir, snapshot);

            FILE *sf = fopen(snappath, "r");
            if (sf) {
                bufferClearContent(B);
                char *l = NULL;
                size_t cap = 0;
                ssize_t linelen;
                while ((linelen = getline(&l, &cap, sf)) != -1) {
                    while (linelen > 0 && (l[linelen - 1] == '\n' || l[linelen - 1] == '\r'))
                        linelen--;
                    bufferInsertRow(B, B->numrows, l, (size_t)linelen);
                }
                free(l);
                fclose(sf);
            }
        }

        B->cx = cx; B->cy = cy; B->rowoff = rowoff; B->coloff = coloff;
        bufferClampCursor(B);

        /* history empty; saved-state unknown if marked dirty */
        B->history_save = dirty ? -1 : 0;
        B->hist_index = 0;
        B->hist_len = 0;
        bufferUpdateDirty(B);

        editorSelectSyntaxHighlight(B, B->filename);
    }

    if (E.numtabs == 0) editorTabNew(NULL, 1);
    E.curtab = (curtab >= 0 && curtab < E.numtabs) ? curtab : 0;

    fclose(fp);
    return 1;
}

/* ============================ Screen drawing ============================== */
static void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

static void editorDrawTabBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    abAppend(ab, " ", 1);

    int used = 1;
    for (int i = 0; i < E.numtabs; i++) {
        editorBuffer *B = &E.tabs[i];
        const char *name = bufferDisplayName(B);
        const char *base = strrchr(name, '/');
        if (base) name = base + 1;

        char title[128];
        snprintf(title, sizeof(title), "%s%s", name, B->dirty ? "*" : "");

        if (i == E.curtab) abAppend(ab, "\x1b[1m", 4);

        int tlen = (int)strlen(title);
        if (used + tlen + 2 >= E.screencols) break;
        abAppend(ab, title, tlen);
        abAppend(ab, " ", 1);
        used += tlen + 1;

        if (i == E.curtab) abAppend(ab, "\x1b[22m", 5);
    }

    while (used < E.screencols) { abAppend(ab, " ", 1); used++; }
    abAppend(ab, "\x1b[0m", 4);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawHelpScreen(struct abuf *ab, int startup) {
    editorBuffer *B = curBuf();
    const char *file = bufferDisplayName(B);

    /* Content: concise but rich; scrollable if needed */
    const char *lines[] = {
        "eKilo — Help",
        "",
        "Essentials",
        "  F1 / Ctrl-/        Help (toggle)",
        "  Ctrl-S             Save   (Save As when unnamed)",
        "  Ctrl-Q             Quit   (press 3x if any tab modified)",
        "",
        "Tabs / Files",
        "  Ctrl-O             Open file in new tab",
        "  Ctrl-T             New tab",
        "  Ctrl-W             Close tab (press twice if modified)",
        "  Ctrl-N / Ctrl-P    Next / Previous tab",
        "",
        "Navigation",
        "  Arrows             Move cursor",
        "  Home / End         Line start / end",
        "  PgUp / PgDn        Scroll",
        "  Ctrl-G             Go to line   (accepts: 42, +10, -5, 42:8)",
        "",
        "Editing",
        "  Enter              New line",
        "  Backspace          Delete left",
        "  Del                Delete right",
        "  Ctrl-H             Delete current line",
        "  Ctrl-Z / Ctrl-Y    Undo / Redo",
        "  Tab                Insert tab character",
        "",
        "Find / Replace",
        "  Ctrl-F             Find (plain text)",
        "  Ctrl-E             Find (regex)",
        "  In Find prompt      Arrows: next/prev match, ESC: cancel",
        "  Ctrl-R             Replace all (regex; supports \\\\0..\\\\9)",
        "",
        "View",
        "  Ctrl-\\             Toggle line numbers",
        "",
        "Notes",
        "  • Search highlights the current match in blue.",
        "  • Undo/redo is per-tab and merges normal typing into larger steps.",
        NULL
    };

    char header[256];
    snprintf(header, sizeof(header), "File: %s%s   |   %s",
             file,
             (B && B->dirty) ? " (modified)" : "",
             startup ? "Press any key to start editing" : "ESC/F1/Ctrl-/ to close");

    int total = 0;
    while (lines[total]) total++;

    int view_h = E.textrows;
    if (view_h < 1) view_h = 1;

    if (E.help_scroll < 0) E.help_scroll = 0;
    if (E.help_scroll > total - view_h) E.help_scroll = (total > view_h) ? (total - view_h) : 0;

    /* Paint background */
    for (int y = 0; y < E.textrows; y++) {
        abAppend(ab, "\x1b[2m~\x1b[0m\x1b[0K\r\n", 18);
    }

    /* Reposition to below tab bar */
    abAppend(ab, "\x1b[H", 3);
    abAppend(ab, "\x1b[2;1H", 6);

    /* Header line */
    abAppend(ab, "\x1b[7m", 4);
    int hlen = (int)strlen(header);
    if (hlen > E.screencols) hlen = E.screencols;
    abAppend(ab, header, hlen);
    while (hlen < E.screencols) { abAppend(ab, " ", 1); hlen++; }
    abAppend(ab, "\x1b[0m", 4);
    abAppend(ab, "\r\n", 2);

    /* Body */
    int body_rows = E.textrows - 1;
    if (body_rows < 0) body_rows = 0;
    for (int i = 0; i < body_rows; i++) {
        int idx = E.help_scroll + i;
        if (idx >= total) {
            abAppend(ab, "\x1b[0K\r\n", 6);
            continue;
        }
        const char *s = lines[idx];
        int len = (int)strlen(s);
        if (len > E.screencols) len = E.screencols;

        /* subtle section styling */
        if (s[0] && !strncmp(s, "Essentials", 10)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "Tabs / Files", 12)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "Navigation", 10)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "Editing", 7)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "Find / Replace", 14)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "View", 4)) abAppend(ab, "\x1b[1m", 4);
        else if (s[0] && !strncmp(s, "Notes", 5)) abAppend(ab, "\x1b[1m", 4);

        abAppend(ab, s, len);
        abAppend(ab, "\x1b[22m", 5);
        abAppend(ab, "\x1b[0K\r\n", 6);
    }
}

static void editorRefreshScreen(void) {
    editorBuffer *B = curBuf();
    if (!B) return;

    if (!E.help_active) editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawTabBar(&ab);

    if (E.help_active) {
        editorDrawHelpScreen(&ab, 0);
    } else {
        int lnw = editorLineNumberWidth(B);
        int textcols = editorTextCols(B);

        for (int y = 0; y < E.textrows; y++) {
            int filerow = B->rowoff + y;

            /* Line numbers gutter */
            if (E.show_line_numbers) {
                char ln[64];
                if (filerow < B->numrows) {
                    int n = filerow + 1;
                    int w = lnw - 2;
                    if (filerow == B->cy) abAppend(&ab, "\x1b[7m", 4);
                    else abAppend(&ab, "\x1b[90m", 5); /* dim */

                    snprintf(ln, sizeof(ln), "%*d ", w, n);
                    int l = (int)strlen(ln);
                    if (l > lnw) l = lnw;
                    abAppend(&ab, ln, l);

                    abAppend(&ab, "\x1b[0m", 4);
                } else {
                    abAppend(&ab, "\x1b[90m", 5);
                    snprintf(ln, sizeof(ln), "%*s ", lnw - 2, "~");
                    int l = (int)strlen(ln);
                    if (l > lnw) l = lnw;
                    abAppend(&ab, ln, l);
                    abAppend(&ab, "\x1b[0m", 4);
                }
            }

            if (filerow >= B->numrows) {
                if (B->numrows == 0 && y == E.textrows / 3) {
                    char welcome[128];
                    int welcomelen = snprintf(welcome, sizeof(welcome),
                        "eKilo editor -- version %s   (F1/Ctrl-/ help)", EKILO_VERSION);
                    if (welcomelen > textcols) welcomelen = textcols;
                    int padding = (textcols - welcomelen) / 2;
                    if (padding) { abAppend(&ab, "~", 1); padding--; }
                    while (padding--) abAppend(&ab, " ", 1);
                    abAppend(&ab, welcome, welcomelen);
                } else {
                    abAppend(&ab, "~", 1);
                }
                abAppend(&ab, "\x1b[0K\r\n", 6);
                continue;
            }

            erow *r = &B->row[filerow];
            int len = r->rsize - B->coloff;
            if (len < 0) len = 0;
            if (len > textcols) len = textcols;

            char *c = r->render + B->coloff;
            unsigned char *hl = r->hl + B->coloff;

            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (hl[j] == HL_NONPRINT) {
                    char sym = (c[j] <= 26) ? (char)('@' + c[j]) : '?';
                    abAppend(&ab, "\x1b[7m", 4);
                    abAppend(&ab, &sym, 1);
                    abAppend(&ab, "\x1b[0m", 4);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(&ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(&ab, c + j, 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(&ab, buf, clen);
                        current_color = color;
                    }
                    abAppend(&ab, c + j, 1);
                }
            }
            abAppend(&ab, "\x1b[39m", 5);
            abAppend(&ab, "\x1b[0K", 4);
            abAppend(&ab, "\r\n", 2);
        }
    }

    /* status bar */
    abAppend(&ab, "\x1b[7m", 4);

    char status[160], rstatus[80];
    const char *name = bufferDisplayName(B);

    int len = 0;
    if (E.help_active) {
        len = snprintf(status, sizeof(status), "HELP — PgUp/PgDn/Arrows scroll — ESC to close");
    } else {
        len = snprintf(status, sizeof(status), "%.40s - %d lines %s",
                       name, B->numrows, B->dirty ? "(modified)" : "");
    }

    int rlen = snprintf(rstatus, sizeof(rstatus), "tab %d/%d | %d/%d",
                        E.curtab + 1, E.numtabs,
                        (B->numrows ? (B->cy + 1) : 0), B->numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(&ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppend(&ab, " ", 1);
            len++;
        }
    }
    abAppend(&ab, "\x1b[0m\r\n", 6);

    /* message bar */
    abAppend(&ab, "\x1b[0K", 4);
    int msglen = (int)strlen(E.statusmsg);
    if (!E.help_active && msglen && time(NULL) - E.statusmsg_time < 6) {
        if (msglen > E.screencols) msglen = E.screencols;
        abAppend(&ab, E.statusmsg, msglen);
    }

    /* cursor */
    int cx = 1;
    int cy = 1;

    if (E.help_active) {
        cy = 1;
        cx = 1;
    } else {
        int lnw = editorLineNumberWidth(B);

        int screeny = (B->cy - B->rowoff);
        if (screeny < 0) screeny = 0;
        if (screeny >= E.textrows) screeny = E.textrows - 1;
        cy = 1 /* tabbar */ + 1 /* 1-based */ + screeny;

        int screenx = (B->rx - B->coloff);
        if (screenx < 0) screenx = 0;
        int textcols = editorTextCols(B);
        if (screenx >= textcols) screenx = textcols - 1;
        cx = 1 + lnw + screenx;
        if (cx < 1) cx = 1;
        if (cx > E.screencols) cx = E.screencols;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
    abAppend(&ab, buf, (int)strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

/* ============================= Window resizing ============================ */
static void updateWindowSize(void) {
    int rows, cols;
    int attempts = 0;
    while (attempts++ < 3) {
        if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &rows, &cols) == 0) {
            E.screencols = cols;
            int usable = rows - 3; /* tabbar + status + msg */
            if (usable < 1) usable = 1;
            E.textrows = usable;
            return;
        }
        usleep(10000);
    }
    editorSetStatusMessage("Warning: could not update window size");
}

static void handleSigWinCh(int unused __attribute__((unused))) {
    updateWindowSize();
    editorRefreshScreen();
}

/* =============================== Init / exit ============================== */
static void initEditor(void) {
    memset(&E, 0, sizeof(E));
    E.textrows = 20;
    E.screencols = 80;
    E.rawmode = 0;
    E.paste_mode = 0;
    E.last_char_time.tv_sec = 0;
    E.last_char_time.tv_usec = 0;

    E.show_line_numbers = 1;

    E.help_active = 0;
    E.help_scroll = 0;

    E.tabs = NULL;
    E.numtabs = 0;
    E.curtab = 0;

    E.state_dir = editorGetStateDir();

    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);

    atexit(editorAtExit);
}

static void editorAtExit(void) {
    editorSaveSession();
    disableRawMode(STDIN_FILENO);
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/* =============================== Commands ================================= */
static void editorOpenPromptNewTab(void) {
    char *fname = editorPrompt("Open file: %s  (ESC cancels)", NULL);
    if (!fname) return;
    editorTabNew(fname, 1);
    free(fname);
}

static void editorSaveCurrent(void) {
    editorBuffer *B = curBuf();
    if (!B) return;

    if (!B->filename) {
        char *fname = editorPrompt("Save as: %s  (ESC cancels)", NULL);
        if (!fname) { editorSetStatusMessage("Save aborted"); return; }
        if (bufferSaveToFilename(B, fname) != 0)
            editorSetStatusMessage("Can't save: %s", strerror(errno));
        free(fname);
    } else {
        if (bufferSaveToFilename(B, B->filename) != 0)
            editorSetStatusMessage("Can't save: %s", strerror(errno));
    }
}

static void editorGoToLine(void) {
    editorBuffer *B = curBuf();
    if (!B) return;

    char *s = editorPrompt("Go to line: %s  (examples: 42, +10, -5, 42:8)", NULL);
    if (!s) return;

    int line = -1, col = -1;
    char *colon = strchr(s, ':');
    if (colon) {
        *colon = '\0';
        col = atoi(colon + 1);
        if (col < 1) col = 1;
    }

    if (s[0] == '+' || s[0] == '-') {
        int delta = atoi(s);
        line = (B->cy + 1) + delta;
    } else {
        line = atoi(s);
    }

    free(s);

    if (line < 1) line = 1;
    if (line > B->numrows) line = B->numrows;
    B->cy = (B->numrows > 0) ? (line - 1) : 0;

    if (col >= 1 && B->cy < B->numrows) {
        int cx = col - 1;
        if (cx > B->row[B->cy].size) cx = B->row[B->cy].size;
        B->cx = cx;
    } else {
        B->cx = 0;
    }

    editorScroll();
    editorSetStatusMessage("Moved to line %d", line);
}

static void editorToggleLineNumbers(void) {
    E.show_line_numbers = !E.show_line_numbers;
    editorSetStatusMessage("Line numbers: %s", E.show_line_numbers ? "ON" : "OFF");
}

/* ========================== Edits with history ============================ */
static void editorInsertTextWithHistory(editorBuffer *B, const char *ins, int ins_len) {
    if (!B || !ins || ins_len <= 0) return;

    historyEntry he;
    memset(&he, 0, sizeof(he));
    he.type = HIST_EDIT;
    he.ts_ms = now_ms();

    he.before_cy = B->cy; he.before_cx = B->cx;
    he.before_rowoff = B->rowoff; he.before_coloff = B->coloff;

    he.pos_cy = B->cy; he.pos_cx = B->cx;

    he.ins = xmalloc((size_t)ins_len + 1);
    memcpy(he.ins, ins, (size_t)ins_len);
    he.ins[ins_len] = '\0';
    he.ins_len = ins_len;

    he.del = NULL; he.del_len = 0;

    bufferInsertStringRaw(B, ins, ins_len);

    he.after_cy = B->cy; he.after_cx = B->cx;
    he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;

    historyPush(B, &he);
}

static void editorInsertCharAutoPairWithHistory(editorBuffer *B, int c) {
    if (!B) return;
    char s[1] = { (char)c };
    editorInsertTextWithHistory(B, s, 1);
}

static void editorInsertNewlineWithHistory(editorBuffer *B) {
    char nl = '\n';
    editorInsertTextWithHistory(B, &nl, 1);
}

static void editorDeleteBackwardWithHistory(editorBuffer *B) {
    if (!B) return;
    if (B->cy == B->numrows) return;
    if (B->cx == 0 && B->cy == 0) return;

    historyEntry he;
    memset(&he, 0, sizeof(he));
    he.type = HIST_EDIT;
    he.ts_ms = now_ms();

    he.before_cy = B->cy; he.before_cx = B->cx;
    he.before_rowoff = B->rowoff; he.before_coloff = B->coloff;

    /* Determine what gets deleted and where, then perform forward delete from that position */
    if (B->cx > 0) {
        erow *row = &B->row[B->cy];
        char ch = row->chars[B->cx - 1];

        he.pos_cy = B->cy;
        he.pos_cx = B->cx - 1;

        he.del = xmalloc(2);
        he.del[0] = ch;
        he.del[1] = '\0';
        he.del_len = 1;

        he.ins = NULL; he.ins_len = 0;

        /* move cursor to deletion pos and delete forward */
        B->cx--;
        bufferDelCharForward(B);

        he.after_cy = B->cy; he.after_cx = B->cx;
        he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;

        historyPush(B, &he);
        return;
    }

    /* At start of line: backspace deletes the newline between previous and this line */
    if (B->cy > 0) {
        int prev = B->cy - 1;
        int prevlen = B->row[prev].size;

        he.pos_cy = prev;
        he.pos_cx = prevlen;

        he.del = xmalloc(2);
        he.del[0] = '\n';
        he.del[1] = '\0';
        he.del_len = 1;

        B->cy = prev;
        B->cx = prevlen;
        bufferDelCharForward(B); /* merge */

        he.after_cy = B->cy; he.after_cx = B->cx;
        he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;

        historyPush(B, &he);
        return;
    }
}

static void editorDeleteForwardWithHistory(editorBuffer *B) {
    if (!B) return;
    if (B->cy >= B->numrows) return;

    erow *row = &B->row[B->cy];
    if (B->cx == row->size && B->cy == B->numrows - 1) return; /* nothing */

    historyEntry he;
    memset(&he, 0, sizeof(he));
    he.type = HIST_EDIT;
    he.ts_ms = now_ms();

    he.before_cy = B->cy; he.before_cx = B->cx;
    he.before_rowoff = B->rowoff; he.before_coloff = B->coloff;

    he.pos_cy = B->cy;
    he.pos_cx = B->cx;

    if (B->cx < row->size) {
        char ch = row->chars[B->cx];
        he.del = xmalloc(2);
        he.del[0] = ch;
        he.del[1] = '\0';
        he.del_len = 1;
    } else {
        /* delete the newline (merge next line) */
        he.del = xmalloc(2);
        he.del[0] = '\n';
        he.del[1] = '\0';
        he.del_len = 1;
    }

    bufferDelCharForward(B);

    he.after_cy = B->cy; he.after_cx = B->cx;
    he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;

    historyPush(B, &he);
}

static void editorDeleteLineWithHistory(editorBuffer *B) {
    if (!B) return;
    if (B->cy >= B->numrows) return;

    historyEntry he;
    memset(&he, 0, sizeof(he));
    he.type = HIST_ROWDEL;
    he.ts_ms = now_ms();

    he.before_cy = B->cy; he.before_cx = B->cx;
    he.before_rowoff = B->rowoff; he.before_coloff = B->coloff;

    he.row_idx = B->cy;

    erow *row = &B->row[B->cy];
    he.row_len = row->size;
    he.row_text = xmalloc((size_t)row->size + 1);
    memcpy(he.row_text, row->chars, (size_t)row->size);
    he.row_text[row->size] = '\0';

    bufferDelRow(B, B->cy);
    if (B->cy >= B->numrows) B->cy = B->numrows - 1;
    if (B->cy < 0) { B->cy = 0; B->cx = 0; }
    else B->cx = 0;

    he.after_cy = B->cy; he.after_cx = B->cx;
    he.after_rowoff = B->rowoff; he.after_coloff = B->coloff;

    historyPush(B, &he);
}

/* =============================== Help modal =============================== */
static void editorHelpModal(int startup) {
    int prev_help = E.help_active;
    int prev_scroll = E.help_scroll;

    /* Save message bar state */
    char prevmsg[160];
    memcpy(prevmsg, E.statusmsg, sizeof(prevmsg));
    time_t prevmsg_time = E.statusmsg_time;

    E.help_active = 1;
    if (startup) E.help_scroll = 0;

    while (1) {
        struct abuf ab = ABUF_INIT;
        abAppend(&ab, "\x1b[?25l", 6);
        abAppend(&ab, "\x1b[H", 3);
        editorDrawTabBar(&ab);
        editorDrawHelpScreen(&ab, startup);

        /* status + msg bars are drawn by normal refresh; emulate minimal here */
        /* status bar */
        abAppend(&ab, "\x1b[7m", 4);
        const char *st = startup ? "HELP — press any key to start editing" : "HELP — ESC/F1/Ctrl-/ to close — Arrows/PgUp/PgDn scroll";
        int sl = (int)strlen(st);
        if (sl > E.screencols) sl = E.screencols;
        abAppend(&ab, st, sl);
        while (sl < E.screencols) { abAppend(&ab, " ", 1); sl++; }
        abAppend(&ab, "\x1b[0m\r\n", 6);

        /* message bar */
        abAppend(&ab, "\x1b[0K", 4);

        abAppend(&ab, "\x1b[H", 3);
        abAppend(&ab, "\x1b[?25l", 6);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        int c = editorReadKey(STDIN_FILENO);

        if (startup) {
            break;
        }

        if (c == ESC || editorIsHelpKey(c) || c == 'q' || c == 'Q') break;

        if (c == ARROW_UP) E.help_scroll--;
        else if (c == ARROW_DOWN) E.help_scroll++;
        else if (c == PAGE_UP) E.help_scroll -= E.textrows;
        else if (c == PAGE_DOWN) E.help_scroll += E.textrows;
        else if (c == HOME_KEY) E.help_scroll = 0;
        else if (c == END_KEY) E.help_scroll = 1000000;

        /* clamp happens in draw */
    }

    E.help_active = prev_help;
    if (!startup) E.help_scroll = prev_scroll;

    /* restore message bar state */
    memcpy(E.statusmsg, prevmsg, sizeof(prevmsg));
    E.statusmsg_time = prevmsg_time;

    editorRefreshScreen();
}

/* ============================ Keypress processing ========================= */
#define EKILO_QUIT_TIMES 3

static void editorProcessKeypress(int fd) {
    static int quit_times = EKILO_QUIT_TIMES;
    static int close_armed_tab = -1;

    int c = editorReadKey(fd);
    E.last_key = c;

    if (editorIsHelpKey(c)) {
        editorHelpModal(0);
        return;
    }

    /* paste mode detection */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec == E.last_char_time.tv_sec) {
        long elapsed = (tv.tv_usec - E.last_char_time.tv_usec);
        if (elapsed < 30000) E.paste_mode = 1;
    } else if (tv.tv_sec - E.last_char_time.tv_sec > 0) {
        E.paste_mode = 0;
    }
    E.last_char_time = tv;

    editorBuffer *B = curBuf();
    if (!B) return;

    switch (c) {
        case ENTER:
            editorInsertNewlineWithHistory(B);
            break;

        case CTRL_C:
            break;

        case CTRL_Q:
            if (editorAnyDirty() && quit_times) {
                editorSetStatusMessage("WARNING: unsaved changes in tabs. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            exit(0);
            break;

        case CTRL_S:
            editorSaveCurrent();
            break;

        case CTRL_O:
            editorOpenPromptNewTab();
            break;

        case CTRL_T:
            editorTabNew(NULL, 1);
            editorSetStatusMessage("New tab");
            break;

        case CTRL_N:
            editorTabNext();
            break;

        case CTRL_P:
            editorTabPrev();
            break;

        case CTRL_W:
            if (close_armed_tab == E.curtab && !E.tabs[E.curtab].dirty) {
                close_armed_tab = -1;
            }
            if (E.tabs[E.curtab].dirty) {
                if (close_armed_tab == E.curtab) {
                    int idx = E.curtab;
                    close_armed_tab = -1;
                    editorTabClose(idx, 1);
                } else {
                    close_armed_tab = E.curtab;
                    editorTabClose(E.curtab, 0);
                }
            } else {
                editorTabClose(E.curtab, 1);
            }
            break;

        case CTRL_F:
            editorFindPlain();
            break;

        case CTRL_E:
            editorFindRegex();
            break;

        case CTRL_R:
            editorReplaceRegexAll();
            break;

        case CTRL_G:
            editorGoToLine();
            break;

        case CTRL_BACKSLASH:
            editorToggleLineNumbers();
            break;

        case CTRL_Z:
            editorUndo(B);
            break;

        case CTRL_Y:
            editorRedo(B);
            break;

        case BACKSPACE:
            editorDeleteBackwardWithHistory(B);
            break;

        case CTRL_H:
            /* keep the classic "delete line" behavior */
            editorDeleteLineWithHistory(B);
            break;

        case DEL_KEY:
            editorDeleteForwardWithHistory(B);
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                B->cy = B->rowoff;
            } else {
                B->cy = B->rowoff + E.textrows - 1;
                if (B->cy > B->numrows) B->cy = B->numrows;
            }
            int times = E.textrows;
            while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        } break;

        case HOME_KEY:
            B->cx = 0;
            break;
        case END_KEY:
            if (B->cy < B->numrows) B->cx = B->row[B->cy].size;
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_L:
        case ESC:
            break;

        default:
            if (c == '\t') {
                editorInsertCharAutoPairWithHistory(B, '\t');
            } else if (!iscntrl(c) && c < 128) {
                editorInsertCharAutoPairWithHistory(B, c);
            }
            break;
    }

    quit_times = EKILO_QUIT_TIMES;
}

/* ================================== main ================================== */
int main(int argc, char **argv) {
    initEditor();

    if (enableRawMode(STDIN_FILENO) == -1) die("enableRawMode");

    if (argc > 1) {
        for (int i = 1; i < argc; i++) editorTabNew(argv[i], (i == 1));
        editorSetStatusMessage("F1/Ctrl-/ help | Ctrl-S save | Ctrl-F find | Ctrl-E regex find | Ctrl-G goto | Ctrl-Z/Y undo/redo");
    } else {
        E.standalone_start = 1;
        int restored = editorLoadSession();
        if (!restored) editorTabNew(NULL, 1);

        /* Show help once on standalone start, smoothly */
        editorHelpModal(1);
        editorSetStatusMessage("F1/Ctrl-/ help | Ctrl-O open | Ctrl-S save | Ctrl-F find | Ctrl-G goto | Ctrl-Z/Y undo/redo | Ctrl-Q quit");
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
