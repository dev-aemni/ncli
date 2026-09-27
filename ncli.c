#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

#define NCLI_VERSION "1.1.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* ================= Terminal Raw Mode ================= */

static struct termios orig_termios;
static int raw_mode_on = 0;

void disableRawMode(void) {
    if (raw_mode_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_on = 0;
    }
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_on = 1;
}

int readKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN && errno != EINTR) return -1;
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
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
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return (unsigned char)c;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ================= Multiline Inline Writer Engine ================= */

typedef struct {
    char *chars;
    int size;
    int cap;
} ILine;

typedef struct {
    ILine *lines;
    int count;
    int cur_line;
    int cur_col;
} InlineEditor;

void ilineInit(ILine *l) {
    l->cap = 32;
    l->size = 0;
    l->chars = malloc(l->cap);
    l->chars[0] = '\0';
}

void ilineInsertChar(ILine *l, int at, char c) {
    if (at < 0 || at > l->size) at = l->size;
    if (l->size + 2 >= l->cap) {
        l->cap *= 2;
        l->chars = realloc(l->chars, l->cap);
    }
    memmove(&l->chars[at + 1], &l->chars[at], l->size - at + 1);
    l->chars[at] = c;
    l->size++;
}

void ilineDelChar(ILine *l, int at) {
    if (at < 0 || at >= l->size) return;
    memmove(&l->chars[at], &l->chars[at + 1], l->size - at);
    l->size--;
}

void inlineInsertNewline(InlineEditor *ed) {
    ILine *cur = &ed->lines[ed->cur_line];
    ILine next;
    ilineInit(&next);

    int move_len = cur->size - ed->cur_col;
    if (move_len > 0) {
        for (int i = 0; i < move_len; i++) {
            ilineInsertChar(&next, i, cur->chars[ed->cur_col + i]);
        }
        cur->chars[ed->cur_col] = '\0';
        cur->size = ed->cur_col;
    }

    ed->lines = realloc(ed->lines, sizeof(ILine) * (ed->count + 1));
    memmove(&ed->lines[ed->cur_line + 2], &ed->lines[ed->cur_line + 1],
            sizeof(ILine) * (ed->count - (ed->cur_line + 1)));
    ed->lines[ed->cur_line + 1] = next;
    ed->count++;

    printf("\r\x1b[K%s\r\n", ed->lines[ed->cur_line].chars);
    for (int i = ed->cur_line + 1; i < ed->count; i++) {
        printf("\r\x1b[K%s", ed->lines[i].chars);
        if (i < ed->count - 1) printf("\r\n");
    }

    ed->cur_line++;
    ed->cur_col = 0;
    int lines_up = (ed->count - 1) - ed->cur_line;
    if (lines_up > 0) printf("\x1b[%dA", lines_up);
    printf("\r\x1b[1G");
    fflush(stdout);
}

void inlineJoinLines(InlineEditor *ed) {
    if (ed->cur_line == 0) return;

    ILine *prev = &ed->lines[ed->cur_line - 1];
    ILine *cur = &ed->lines[ed->cur_line];
    int prev_len = prev->size;

    for (int i = 0; i < cur->size; i++) {
        ilineInsertChar(prev, prev->size, cur->chars[i]);
    }
    free(cur->chars);

    memmove(&ed->lines[ed->cur_line], &ed->lines[ed->cur_line + 1],
            sizeof(ILine) * (ed->count - ed->cur_line - 1));
    ed->count--;

    ed->cur_line--;
    ed->cur_col = prev_len;
    printf("\x1b[A\r");

    for (int i = ed->cur_line; i < ed->count; i++) {
        printf("\x1b[K%s\r\n", ed->lines[i].chars);
    }
    printf("\x1b[K");

    int lines_up = ed->count - ed->cur_line;
    if (lines_up > 0) printf("\x1b[%dA", lines_up);
    printf("\x1b[%dG", ed->cur_col + 1);
    fflush(stdout);
}

void runInlineWriter(const char *filename) {
    enableRawMode();

    InlineEditor ed;
    ed.lines = malloc(sizeof(ILine) * 1);
    ed.count = 1;
    ilineInit(&ed.lines[0]);
    ed.cur_line = 0;
    ed.cur_col = 0;

    if (filename) {
        FILE *fp = fopen(filename, "r");
        if (fp) {
            int ch;
            while ((ch = fgetc(fp)) != EOF) {
                if (ch == '\r') continue;
                if (ch == '\n') {
                    ed.lines = realloc(ed.lines, sizeof(ILine) * (ed.count + 1));
                    ilineInit(&ed.lines[ed.count]);
                    ed.count++;
                } else {
                    ilineInsertChar(&ed.lines[ed.count - 1], ed.lines[ed.count - 1].size, (char)ch);
                }
            }
            fclose(fp);

            for (int i = 0; i < ed.count; i++) {
                printf("%s", ed.lines[i].chars);
                if (i < ed.count - 1) printf("\r\n");
            }
            ed.cur_line = ed.count - 1;
            ed.cur_col = ed.lines[ed.cur_line].size;
            printf("\x1b[%dG", ed.cur_col + 1);
            fflush(stdout);
        }
    }

    while (1) {
        int c = readKey();

        if (c == CTRL_KEY('x')) {
            int lines_to_bottom = ed.count - 1 - ed.cur_line;
            if (lines_to_bottom > 0) printf("\x1b[%dB", lines_to_bottom);
            printf("\r\n");
            fflush(stdout);
            break;
        }

        switch (c) {
            case '\r':
            case '\n':
                inlineInsertNewline(&ed);
                break;

            case BACKSPACE:
            case 8:
                if (ed.cur_col > 0) {
                    ilineDelChar(&ed.lines[ed.cur_line], ed.cur_col - 1);
                    ed.cur_col--;
                    printf("\b\x1b[K%s", &ed.lines[ed.cur_line].chars[ed.cur_col]);
                    printf("\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                } else if (ed.cur_col == 0 && ed.cur_line > 0) {
                    inlineJoinLines(&ed);
                }
                break;

            case DEL_KEY:
                if (ed.cur_col < ed.lines[ed.cur_line].size) {
                    ilineDelChar(&ed.lines[ed.cur_line], ed.cur_col);
                    printf("\x1b[K%s", &ed.lines[ed.cur_line].chars[ed.cur_col]);
                    printf("\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                }
                break;

            case CTRL_KEY('k'):
                ed.lines[ed.cur_line].chars[ed.cur_col] = '\0';
                ed.lines[ed.cur_line].size = ed.cur_col;
                printf("\x1b[K");
                fflush(stdout);
                break;

            case CTRL_KEY('u'): {
                ILine *l = &ed.lines[ed.cur_line];
                int rem = l->size - ed.cur_col;
                memmove(l->chars, &l->chars[ed.cur_col], rem + 1);
                l->size = rem;
                ed.cur_col = 0;
                printf("\r\x1b[K%s\r\x1b[1G", l->chars);
                fflush(stdout);
                break;
            }

            case CTRL_KEY('w'): {
                ILine *l = &ed.lines[ed.cur_line];
                if (ed.cur_col > 0) {
                    int start = ed.cur_col;
                    while (start > 0 && l->chars[start - 1] == ' ') start--;
                    while (start > 0 && l->chars[start - 1] != ' ') start--;
                    int del_count = ed.cur_col - start;
                    for (int i = 0; i < del_count; i++) ilineDelChar(l, start);
                    ed.cur_col = start;
                    printf("\r\x1b[K%s\x1b[%dG", l->chars, ed.cur_col + 1);
                    fflush(stdout);
                }
                break;
            }

            case ARROW_UP:
                if (ed.cur_line > 0) {
                    ed.cur_line--;
                    if (ed.cur_col > ed.lines[ed.cur_line].size) {
                        ed.cur_col = ed.lines[ed.cur_line].size;
                    }
                    printf("\x1b[A\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                }
                break;

            case ARROW_DOWN:
                if (ed.cur_line < ed.count - 1) {
                    ed.cur_line++;
                    if (ed.cur_col > ed.lines[ed.cur_line].size) {
                        ed.cur_col = ed.lines[ed.cur_line].size;
                    }
                    printf("\x1b[B\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                }
                break;

            case ARROW_LEFT:
                if (ed.cur_col > 0) {
                    ed.cur_col--;
                    printf("\x1b[D");
                    fflush(stdout);
                } else if (ed.cur_line > 0) {
                    ed.cur_line--;
                    ed.cur_col = ed.lines[ed.cur_line].size;
                    printf("\x1b[A\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                }
                break;

            case ARROW_RIGHT:
                if (ed.cur_col < ed.lines[ed.cur_line].size) {
                    ed.cur_col++;
                    printf("\x1b[C");
                    fflush(stdout);
                } else if (ed.cur_line < ed.count - 1) {
                    ed.cur_line++;
                    ed.cur_col = 0;
                    printf("\x1b[B\x1b[1G");
                    fflush(stdout);
                }
                break;

            case HOME_KEY:
                ed.cur_col = 0;
                printf("\x1b[1G");
                fflush(stdout);
                break;

            case END_KEY:
                ed.cur_col = ed.lines[ed.cur_line].size;
                printf("\x1b[%dG", ed.cur_col + 1);
                fflush(stdout);
                break;

            default:
                if (c >= 32 && c <= 126) {
                    ilineInsertChar(&ed.lines[ed.cur_line], ed.cur_col, (char)c);
                    ed.cur_col++;
                    printf("%c\x1b[K%s", (char)c, &ed.lines[ed.cur_line].chars[ed.cur_col]);
                    printf("\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                } else if (c == '\t') {
                    for (int i = 0; i < 4; i++) {
                        ilineInsertChar(&ed.lines[ed.cur_line], ed.cur_col, ' ');
                        ed.cur_col++;
                    }
                    printf("    \x1b[K%s", &ed.lines[ed.cur_line].chars[ed.cur_col]);
                    printf("\x1b[%dG", ed.cur_col + 1);
                    fflush(stdout);
                }
                break;
        }
    }

    disableRawMode();

    if (filename) {
        FILE *fp = fopen(filename, "w");
        if (fp) {
            for (int i = 0; i < ed.count; i++) {
                fputs(ed.lines[i].chars, fp);
                if (i < ed.count - 1) fputc('\n', fp);
            }
            fclose(fp);
            printf("[ncli] Saved to '%s'\n", filename);
        }
    }

    for (int i = 0; i < ed.count; i++) free(ed.lines[i].chars);
    free(ed.lines);
}

/* ================= Nano-like Window / TUI Editor ================= */

typedef struct {
    int size;
    char *chars;
} erow;

struct editorConfig {
    int cx, cy;
    int rowoff, coloff;
    int screenrows, screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
};

static struct editorConfig E;

struct abuf {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (!new) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
    E.dirty++;
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    free(E.row[at].chars);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    E.dirty++;
}

void editorInsertChar(int c) {
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline(void) {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar(void) {
    if (E.cy == E.numrows || (E.cx == 0 && E.cy == 0)) return;
    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        erow *prev = &E.row[E.cy - 1];
        prev->chars = realloc(prev->chars, prev->size + row->size + 1);
        memcpy(&prev->chars[prev->size], row->chars, row->size);
        prev->size += row->size;
        prev->chars[prev->size] = '\0';
        editorDelRow(E.cy);
        E.cy--;
    }
}

int editorSave(void) {
    if (!E.filename) return -1;
    FILE *fp = fopen(E.filename, "w+");
    if (!fp) return -1;
    for (int i = 0; i < E.numrows; i++) {
        fputs(E.row[i].chars, fp);
        fputc('\n', fp);
    }
    fclose(fp);
    E.dirty = 0;
    return 0;
}

void editorScroll(void) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff) E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screencols) E.coloff = E.cx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            abAppend(ab, "~\x1b[K\r\n", 5);
        } else {
            int len = E.row[filerow].size - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            if (len > 0) abAppend(ab, &E.row[filerow].chars[E.coloff], len);
            abAppend(ab, "\x1b[K\r\n", 5);
        }
    }
}

void editorRefreshScreen(void) {
    editorScroll();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l\x1b[H", 9);

    editorDrawRows(&ab);

    /* Status bar */
    abAppend(&ab, "\x1b[7m", 4);
    char status[120], rstatus[80];
    int len = snprintf(status, sizeof(status), " %-20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "Ln %d/%d, Col %d ", E.cy + 1, E.numrows, E.cx + 1);
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
    abAppend(&ab, "\x1b[m\r\n", 5);

    /* Command info */
    abAppend(&ab, "\x1b[K^X Save & Exit  |  ^S Save  |  ^Q Quit  |  Arrows to Move", 58);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void runTuiEditor(const char *filename) {
    enableRawMode();
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 2;
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (fp) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, fp)) != -1) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
            editorInsertRow(E.numrows, line, len);
        }
        free(line);
        fclose(fp);
    }
    if (E.numrows == 0) editorInsertRow(0, "", 0);
    E.dirty = 0;

    while (1) {
        editorRefreshScreen();
        int c = readKey();

        switch (c) {
            case '\r':
                editorInsertNewline();
                break;
            case CTRL_KEY('x'):
                editorSave();
                write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
                disableRawMode();
                return;
            case CTRL_KEY('s'):
                editorSave();
                break;
            case CTRL_KEY('q'):
                write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
                disableRawMode();
                return;
            case BACKSPACE:
            case 8:
                editorDelChar();
                break;
            case DEL_KEY:
                if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
                    E.cx++;
                    editorDelChar();
                }
                break;
            case ARROW_UP:
                if (E.cy > 0) {
                    E.cy--;
                    if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
                }
                break;
            case ARROW_DOWN:
                if (E.cy < E.numrows - 1) {
                    E.cy++;
                    if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
                }
                break;
            case ARROW_LEFT:
                if (E.cx > 0) E.cx--;
                else if (E.cy > 0) {
                    E.cy--;
                    E.cx = E.row[E.cy].size;
                }
                break;
            case ARROW_RIGHT:
                if (E.cy < E.numrows && E.cx < E.row[E.cy].size) E.cx++;
                else if (E.cy < E.numrows - 1) {
                    E.cy++;
                    E.cx = 0;
                }
                break;
            case HOME_KEY:
                E.cx = 0;
                break;
            case END_KEY:
                if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
                break;
            default:
                if (c >= 32 && c <= 126) {
                    editorInsertChar(c);
                } else if (c == '\t') {
                    for (int i = 0; i < 4; i++) editorInsertChar(' ');
                }
                break;
        }
    }
}

/* ================= Echo / Write Mode (-w) ================= */

char *unescapeString(const char *src) {
    size_t slen = strlen(src);
    char *out = malloc(slen + 1);
    size_t i = 0, j = 0;
    while (i < slen) {
        if (src[i] == '\\' && i + 1 < slen) {
            i++;
            switch (src[i]) {
                case 'n': out[j++] = '\n'; break;
                case 't': out[j++] = '\t'; break;
                case 'r': out[j++] = '\r'; break;
                case '\\': out[j++] = '\\'; break;
                case '\"': out[j++] = '\"'; break;
                default:
                    out[j++] = '\\';
                    out[j++] = src[i];
                    break;
            }
        } else {
            out[j++] = src[i];
        }
        i++;
    }
    out[j] = '\0';
    return out;
}

int writeToFile(const char *text, const char *filepath) {
    char *decoded = unescapeString(text);
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        fprintf(stderr, "ncli: cannot open '%s' for writing: %s\n", filepath, strerror(errno));
        free(decoded);
        return 1;
    }
    fputs(decoded, fp);
    fclose(fp);
    free(decoded);
    return 0;
}

/* ================= Replace / Sed Mode (-s) ================= */

int replaceInFile(const char *find, const char *rep, const char *filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "ncli: cannot open '%s': %s\n", filepath, strerror(errno));
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return 1; }
    if (sz > 0 && fread(buf, 1, sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        return 1;
    }
    buf[sz] = '\0';
    fclose(fp);

    size_t flen = strlen(find);
    size_t rlen = strlen(rep);
    if (flen == 0) {
        fprintf(stderr, "ncli: search pattern cannot be empty\n");
        free(buf);
        return 1;
    }

    int count = 0;
    const char *p = buf;
    while ((p = strstr(p, find)) != NULL) {
        count++;
        p += flen;
    }

    if (count == 0) {
        printf("[ncli] '%s' not found in '%s'.\n", find, filepath);
        free(buf);
        return 0;
    }

    size_t newsz = sz + count * (rlen - flen);
    char *newbuf = malloc(newsz + 1);
    if (!newbuf) { free(buf); return 1; }

    char *dst = newbuf;
    const char *src = buf;
    while (1) {
        const char *match = strstr(src, find);
        if (!match) {
            strcpy(dst, src);
            break;
        }
        size_t n = match - src;
        memcpy(dst, src, n);
        dst += n;
        memcpy(dst, rep, rlen);
        dst += rlen;
        src = match + flen;
    }

    fp = fopen(filepath, "wb");
    if (!fp) {
        fprintf(stderr, "ncli: write error on '%s': %s\n", filepath, strerror(errno));
        free(buf);
        free(newbuf);
        return 1;
    }
    fwrite(newbuf, 1, newsz, fp);
    fclose(fp);
    free(buf);
    free(newbuf);

    printf("[ncli] Replaced %d occurrence(s) in '%s'.\n", count, filepath);
    return 0;
}

/* ================= Reset File ================= */

int resetFile(const char *filepath) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        fprintf(stderr, "ncli: cannot reset '%s': %s\n", filepath, strerror(errno));
        return 1;
    }
    fclose(fp);
    return 0;
}

/* ================= Help ================= */

void printHelp(void) {
    printf("ncli - Note CLI (v%s)\n\n", NCLI_VERSION);
    printf("Usage:\n");
    printf("  ncli <file>                      Open full TUI editor (nano-like)\n");
    printf("  ncli -                           Interactive scratchpad (Enter: newline, Ctrl+x: finish, no output)\n");
    printf("  ncli - <file>                    Inline writer/editor for file (Ctrl+x to save & exit)\n");
    printf("  ncli -r <file>                   Reset / empty file (truncate to 0 bytes)\n");
    printf("  ncli -r - <file>                 Reset file and open inline writer\n");
    printf("  ncli -w \"text\" <file>            Write text to file (supports \\n, \\t, etc.)\n");
    printf("  ncli -s \"find\" \"rep\" <file>      Replace string in file (sed-like)\n");
    printf("  ncli -h, --help                  Show this help menu\n\n");
    printf("Controls in Inline Mode:\n");
    printf("  Arrow Up / Down                  Switch to upper / bottom line\n");
    printf("  Arrow Left / Right               Move within line\n");
    printf("  Backspace                        Delete char / join lines\n");
    printf("  Ctrl + K                         Cut to end of line\n");
    printf("  Ctrl + U                         Cut to beginning of line\n");
    printf("  Ctrl + W                         Cut previous word\n");
    printf("  Ctrl + X                         Finish / Save and exit\n");
}

/* ================= Main Entry Point ================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printHelp();
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printHelp();
        return 0;
    }

    if (strcmp(argv[1], "-r") == 0) {
        if (argc == 3) {
            if (resetFile(argv[2]) == 0) {
                printf("[ncli] File '%s' has been emptied.\n", argv[2]);
            }
            return 0;
        } else if (argc >= 4 && strcmp(argv[2], "-") == 0) {
            resetFile(argv[3]);
            runInlineWriter(argv[3]);
            return 0;
        } else {
            fprintf(stderr, "ncli: missing target file for -r\n");
            return 1;
        }
    }

    if (strcmp(argv[1], "-w") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: ncli -w \"text\" <output_file>\n");
            return 1;
        }
        return writeToFile(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "-s") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: ncli -s \"find\" \"replace\" <file>\n");
            return 1;
        }
        return replaceInFile(argv[2], argv[3], argv[4]);
    }

    if (strcmp(argv[1], "-") == 0) {
        if (argc >= 3) {
            runInlineWriter(argv[2]);
        } else {
            runInlineWriter(NULL);
        }
        return 0;
    }

    runTuiEditor(argv[1]);
    return 0;
}
