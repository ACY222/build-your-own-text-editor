#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h> // unix standard

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 1

// ^a-^z: 1-26, 0x1f = 0b0001_1111
// In C, you generally specify bitmasks using hexadecimal, since C doesn't have
// binary literals
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    // `Backspace` doesn't have a human-readable backslash-escape representation
    //  in C, so we make it part of the `editorKey` enum and assign it its ASCII
    //  value of 127
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT, // 1001
    ARROW_UP,    // 1002
    ARROW_DOWN,  // 1003
    DEL_KEY,     // \x1b[3~
    // Home key could be sent as \x1b[1~, \x1b[7~, \x1b[H, \x1b0H
    // End key could be sent as \x1b[4~, \x1b[8~, \x1b[F, \x1b0F
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

// The characters we store in memory are not always the same as the characters
// we draw on the screen
typedef struct erow {
    int size;     // size of the raw string (file content)
    int rsize;    // size of the rendered string (screen content)
    char *chars;  // the actual raw characters from the file
    char *render; // the characters as they appear on screen, like tabs expanded
} erow;

struct termios orig_termios;

struct editorConfig {
    // keep track of the cursor's x and y position in the file
    int cx, cy;
    int rx;      // an index into the render field
    int row_off; // row offset, refers to which row at the top of the screen
    int col_off; // col offset, refers to which column at the left of the screen
    int screen_rows; // number of rows the screen can display
    int screen_cols; // number of cols the screen can display
    int num_rows;    // number of rows of the file
    erow *rows;
    int dirty; // indicates the number of changes
    char *file_name;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
    // clear the screen and reposition the cursor when the program dies
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // if we use `atexit()` to clear the screen, the error message will also be
    //  erased
    perror(s);
    exit(1);
}

void disableRawMode() {
    // set the terminal attricutes to its original value
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    // tc means `terminal control`
    // capture the original terminal attributes into orig_termios
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    // register a function to be called automatically at program exit
    atexit(disableRawMode);

    // terminal and I/O system
    struct termios raw = E.orig_termios;

    // input flags
    // BRKINT: map BREAK to SIGINTR
    // ICRNL: map CR to NL (`Ctrl-M` from 10 to 13)
    // INPCK: enable checking of parity errors
    // ISTRIP: strip 8th bit off chars
    // IXON: enable output flow control (`^S` and `^Q`)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // output flags
    // OPOST: enable following output processing
    raw.c_oflag &= ~(OPOST);

    // control flags
    // CS8: set the character size (CS) to 8 bit per byte
    raw.c_cflag |= (CS8);

    // local flags
    // ECHO: enable echoing
    // ICANON: canonicalize input lines
    // IEXTEN: enable DISCARD and LNEXT (`Ctrl-V`)
    // ISIG: enable signals INTR (^C), QUIT (^\\), SUSP (^Z)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // spacial control characters
    // VMIN: sets the minimum number of bytes of input needed before `read()`
    //  can return
    // VTIME: sets the maximum amount of time to wait before `read()` returns,
    //  which is in tenths of a second (100ms)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // SA means `set attributes`, TCSAFLUSH specifies when to apply the change
    // FLUSH:
    //  1. Wait for output: wait for all output written the fildes has been
    //  transmitted to the terminal
    //  2. Flush input: discards any input that has been received but not yet
    //  read by the `read()` function
    // This ensures the editor starts with a clean state
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// wait for one keypress, and return it
// later, we will expand it to handle escape sequences
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    // If we read an escape character, we immediately read two more bytes into
    // the seq buffer. If either of these reads time out, then we assume the
    // user just pressed the Escape key and return that. Otherwise we look to
    // see if the escape sequence is an arrow key escape sequence
    if (c == '\x1b') {
        char seq[3];

        // try to read two more bytes, if time out (no more bytes, return \x1b)
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        // Home key could be sent as \x1b[1~, \x1b[7~, \x1b[H, \x1b0H
        // End key could be sent as \x1b[4~, \x1b[8~, \x1b[F, \x1b0F
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP; // \x1b[5~
                    case '6':
                        return PAGE_DOWN; // \x1b[6~
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP; // \x1b[A
                case 'B':
                    return ARROW_DOWN; // \x1b[B
                case 'C':
                    return ARROW_RIGHT; // \x1b[C
                case 'D':
                    return ARROW_LEFT; // \x1b[C
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == '0') {
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    // disable hjkl
    // else if (c == 'h' || c == 'j' || c == 'k' || c == 'l') {
    //     switch (c) {
    //     case 'h':
    //         return ARROW_LEFT;
    //     case 'j':
    //         return ARROW_DOWN;
    //     case 'k':
    //         return ARROW_UP;
    //     case 'l':
    //         return ARROW_RIGHT;
    //     default: // Obviously, this path won't happen
    //         return ARROW_LEFT;
    //     }
    // }
    else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    // `n` command can be used to query the terminal for status information
    // argument 6 to ask for the cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    // sscanf(const char *restrict s, const char *restrict format, ...);
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // ioctl isn't guaranteed to be able to request the window size on all
    //  systems, so we have to provide a fallback method of getting the window
    //  size
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // There is no simple "move the cursor to the bottom-right corner"
        // command `C` command moves the cursor to the right, `B` ... down.
        //  And they are specifically documented to stop the cursor from going
        //  past the edge of the screen.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0, cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) {
            return cx;
        }
    }
    return cx; // in case rx is out of range
}

// expand tab to spaces
void editorUpdateRow(erow *row) {
    // count the number of tabs to know how much memory to allocate
    int num_tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            num_tabs++;
        }
    }

    free(row->render); // free previous row's render
    row->render = malloc(row->size + num_tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    // expand tabs into spaces
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            // PAY ATTENTION! tab : spaces != 1 : KILO_TAB_STOP
            while (idx % KILO_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.num_rows)
        return;

    // each time we append a row to E.rows
    E.rows = realloc(E.rows, sizeof(erow) * (E.num_rows + 1));
    memmove(&E.rows[at + 1], &E.rows[at], sizeof(erow) * (E.num_rows - at));

    E.rows[at].size = len;
    E.rows[at].chars = malloc(len + 1);
    // E.rows[at].chars and s don't overlap, so use memcpy instead of memmove
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';

    E.rows[at].rsize = 0;
    E.rows[at].render = NULL;
    editorUpdateRow(&E.rows[at]);

    E.num_rows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.num_rows)
        return;
    editorFreeRow(&E.rows[at]); // free current row
    memmove(&E.rows[at], &E.rows[at + 1], sizeof(erow) * (E.num_rows - at - 1));
    E.num_rows--;
    E.dirty++;
}

// insert a character into a row
void editorRowInsertChar(erow *row, int at, int c) {
    // at is allowed to go one character past the end of the string
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    // we add 2 because we also have to make room for the null byte
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

// append a string s with length len to a erow row
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    // if the cursor is on the tilde line after the end of the file
    if (E.cy == E.num_rows) {
        editorInsertRow(E.num_rows, "", 0);
    }
    editorRowInsertChar(&E.rows[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    // insert a new blank row before the line we are on
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        // insert a new line and truncate current line
        erow *row = &E.rows[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.rows[E.cy];
        row->size = E.cx;
        row->chars = realloc(row->chars, row->size + 1);
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    // reposition the cursor
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.num_rows)
        return;
    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.rows[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    // if E.cx == 0,
    // append current row to previous row, and then delete current row
    else {
        E.cx = E.rows[E.cy - 1].size;
        editorRowAppendString(&E.rows[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file I/O ***/

char *editorRowsToString(int *buf_len) {
    int tot_len = 0;

    // add up the lengths of each row of text, adding 1 to each one for the
    // newline character we'll add to the end of each line
    for (int i = 0; i < E.num_rows; i++) {
        tot_len += E.rows[i].size + 1; // add one for '\n'
    }
    *buf_len = tot_len;

    char *buf = malloc(tot_len);
    char *p = buf;
    for (int i = 0; i < E.num_rows; ++i) {
        memcpy(p, E.rows[i].chars, E.rows[i].size);
        p += E.rows[i].size;
        *p = '\n';
        p++;
    }

    return buf; // expecting the caller to free() the memory
}

// it will open and read a file from the disk
void editorOpen(char *file_name) {
    free(E.file_name); // We may open more than one file at the same time
    E.file_name = strdup(file_name); // strdup: save a copy of a string
    // mode: "r": Open for reading, "w": Open for writing, "a": Open for
    // appending
    FILE *fp = fopen(file_name, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    // We pass getline a null line pointer and a linecap of 0. That makes it
    // allocate new memory for the next line it reads, and set line to point to
    // the memory, and set linecap to let you know how much memory it allocated.
    // It return value is the length of the line it reads, or -1 if it's at the
    // end of the file and there are no more lines to read
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        // truncate '\r\n' or '\n' at the end
        while (line_len > 0 &&
               (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line_len--;
        }
        editorInsertRow(E.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

// More advanced text editors will write to a new, temporary file, and then
// rename that file to the actual file the user wants to overwrite
void editorSave() {
    if (E.file_name == NULL) {
        E.file_name = editorPrompt("Save as: %s", NULL);
        if (E.file_name == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    // open for reading and writing. create if not exists
    int fd = open(E.file_name, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // Sets the file's size to the specified length. If the file is larger
        //  than that, it will cut off any data at the end of the file to make
        //  it that length. If the file is shorter, it will add `0` bytes at
        //  the end to make it that length.
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key) {
    if (key == '\r' || key == '\x1b') {
        return;
    }

    for (int i = 0; i < E.num_rows; i++) {
        erow *row = &E.rows[i];
        // strstr: locate a substring in a string
        // return a pointer if succeed, NULL otherwise
        char *match = strstr(row->render, query);
        if (match) {
            E.cy = i;
            E.cx = match - row->render;
            E.cx = editorRowRxToCx(row, match - row->render);
            // so that we are scrolled to the very bottom of the file, which
            // will cause editorScroll() to scroll upwards at the next screen
            // refresh so that the matching line will be at the very top of the
            // screen
            E.row_off = E.num_rows;
            break;
        }
    }
}

void editorFind() {
    char *query =
        editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);

    if (query) {
        free(query);
    }
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // realloc() will copy the contents of the old block to the new one, and
    // then free() the old one automatically
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/*** input ***/

// the if statements allow the caller to pass NULL for the callback, in case
// they don't want to use a callback (when we prompt the user for a filename)
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t buf_size = 128;
    char *buf = malloc(buf_size);

    size_t buf_len = 0;
    buf[0] = '\0';

    while (1) {
        // the `prompt` is expected to be a format string containing a %s, which
        // is where the user's input will be displayed
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buf_len != 0) {
                buf[--buf_len] = '\0';
            }
        }
        // press Escape to cancel the input prompt
        else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                editorSetStatusMessage("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buf_len == buf_size - 1) {
                buf_size *= 2;
                buf = realloc(buf, buf_size);
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }

        if (callback) {
            callback(buf, c);
        }
    }
}

void editorMoveCursor(int key) {
    // E.cy is allowed to be oone past the last line of the file
    erow *row = (E.cy >= E.num_rows) ? NULL : &E.rows[E.cy];

    switch (key) {
    // prevent moving the cursor off screen
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) { // move left at the start of a line
            E.cy--;
            E.cx = E.rows[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (E.cy < E.num_rows) { // move right at the end of a line
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.num_rows) {
            E.cy++;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
    }

    // if we move the cursor to the end of a long line, then move it down, the
    // E.cx won't change, and the cursor will be off to the right end of the
    // line it's now on, so we need to snap the cursor to end of line
    row = (E.cy > E.num_rows) ? NULL : &E.rows[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len) {
        E.cx = row_len;
    }
}
// wait for a keypress, and then handles it
// later, it will map various `Ctrl` key combinations and other special keys to
//  different editor functions, and insert any alphanumeric and other printable
//  key's characters into the text that is being edited
void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();
    switch (c) {
    case '\r': // Enter
        editorInsertNewline();
        break;

    case CTRL_KEY('q'): // C-q to quit
        // If the file is dirty, we will display a warning, and require the
        // user to press C-q KILO_QUIT_TIMES more times in order to quit without
        // saving
        if (E.dirty && quit_times > 0) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        // clear the screen and reposition the cursor when the program exits
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case CTRL_KEY('w'): // C-w to save
        editorSave();
        break;

    case HOME_KEY: // move the cursor to the beginning of the column
        E.cx = 0;
        break;
    case END_KEY: // move the cursor to the end of the column
        if (E.cy < E.num_rows) {
            E.cx = E.rows[E.cy].size;
        }
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cy = E.row_off;
        } else if (c == PAGE_DOWN) {
            E.cy = E.row_off + E.screen_rows - 1;
            if (E.cy > E.num_rows) {
                E.cy = E.num_rows;
            }
        }

        // create a code block so that we are allowed to declare times variable
        //  (we cannot declare variables directly inside a switch statement)
        //  simulate the user pressing Up and Down keys enough times to move to
        //  the top or bottom of the screen
        int times = E.screen_rows;
        while (times--) {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    }

    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default:
        editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** output ***/

void editorScroll() {
    E.rx = E.cx;
    if (E.cy < E.num_rows) {
        E.rx = editorRowCxToRx(&E.rows[E.cy], E.cx);
    }

    // check if the cursor is above the visible window, (we just move upward)
    // if so, scrolls up ...
    if (E.cy < E.row_off) {
        E.row_off = E.cy;
    }
    // check if the cursor is below the visible window, (we just move downward)
    // if so, scrolls down ...
    else if (E.cy >= E.row_off + E.screen_rows) {
        E.row_off = E.cy - E.screen_rows + 1;
    }

    if (E.rx < E.col_off) {
        E.col_off = E.rx;
    } else if (E.rx >= E.col_off + E.screen_cols) {
        E.col_off = E.rx - E.screen_cols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    // draw tildes at the beginning of each lines
    //  which means that row is not part of the file and can't contain any text
    for (int screen_row = 0; screen_row < E.screen_rows; screen_row++) {
        int file_row = screen_row + E.row_off;
        if (file_row >= E.num_rows) { // draw rows without texts
            // display when starting the program with on arguments, and not when
            // opening a file
            if (E.num_rows == 0 && screen_row == E.screen_rows / 3) {
                char welcome[80];
                // snprintf(char *restrict str, size_t size, const char
                // *restrict
                //  format, ...); write at most size-1 of the characters printed
                //  into the output string
                int welcome_len =
                    snprintf(welcome, sizeof(welcome),
                             "Kilo editor --version %s", KILO_VERSION);
                if (welcome_len > E.screen_cols) {
                    welcome_len = E.screen_cols;
                }
                // center the welcome
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abAppend(ab, " ", 1);
                }
                abAppend(ab, welcome, welcome_len);
            } else {
                // \x1b[K:  clear each line as we draw them, argument 0 erases
                // the part of the line to the right of the cursor
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.rows[file_row].rsize - E.col_off;
            if (len < 0) {
                len = 0;
            } else if (len > E.screen_cols) {
                len = E.screen_cols;
            }
            abAppend(ab, &E.rows[file_row].render[E.col_off], len);
        }
        // clean up the rest of the line
        abAppend(ab, "\x1b[K", 3);

        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // switch to inverted colors (black text on a white background)
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    // file name
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.file_name ? E.file_name : "[No Name]", E.num_rows,
                       E.dirty ? "(modified)" : "");
    // current position
    int progress = (E.cy + 1) * 100 / E.num_rows;
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d | %d%%", E.cy + 1,
                        E.num_rows, progress);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    abAppend(ab, status, len);

    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    // switch back to normal formatting
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    // clear the message bar
    abAppend(ab, "\x1b[K", 3);
    int msg_len = strlen(E.statusmsg);
    if (msg_len > E.screen_cols) {
        msg_len = E.screen_cols;
    }
    // display 5 seconds
    if (msg_len && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msg_len);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    // \x1b[?25l: make the cursor invisible
    abAppend(&ab, "\x1b[?25l", 6);
    // \x1b[H: reposition the cursor at the top-left corner
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // move the cursor to the position stored in E.cx and E.cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1,
             (E.rx - E.col_off) + 1);
    abAppend(&ab, buf, strlen(buf));

    // show the cursor immediately after the refresh finishes
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// ... makes it a variadic function
// It accepts a *format string* followed by a variable number of arguments,
//  which mimics the behavior of standard functions like `printf`
//  - `fmt`: The fixed starting argument, like "saved %d lines"
//  - `...`: The variable arguments that fill the placeholders in `fmt`
void editorSetStatusMessage(const char *fmt, ...) {
    // declare a variable (argument pointer) to traverse the list of extra
    //  arguments
    va_list ap;
    // initialize `ap` to point to the first argument after `fmt`, this is why
    //  you must always have at least one named parameter
    va_start(ap, fmt);
    // int vsnprintf(char * restrict str, size_t size, const char * restrict
    //  format, va_list ap);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    // cleans up the memory associated with the argument list traversal
    va_end(ap);
    E.statusmsg_time = time(NULL); // get the current time
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.row_off = 0;
    E.col_off = 0;
    E.num_rows = 0;
    E.rows = NULL;
    E.dirty = 0;
    E.file_name = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }
    // make room for a two-line status (file name, cursor position, etc.) and
    //  message bar at the bottom of the screen
    E.screen_rows -= 2;
}

// argc: The total number of arguments passed, including the name of the
//  program itseflf
// argv: The actual text of the arguments.
//  indexing:
//      argv[0]: the path of the executable
//      argv[1]: The first user-provided argument
//      argv[2]: The second argument, and so on
int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        // read file and get all lines in the file
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-W = save | Ctrl-Q = quit");

    // read 1 byte from standard input into `c`
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
