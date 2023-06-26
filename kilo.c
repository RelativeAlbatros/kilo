#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h> 
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "toml.h"

#define KILO_VERSION "0.1.3"
#define KILO_QUIT_TIMES 3
#define KILO_LOG_PATH "/tmp/kilo.log"

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

enum editorHighlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_FUNCTION,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)
#define HL_HIGHLIGHT_FUNCTIONS (1<<2)

struct editorSyntax {
	char *filetype;
	char **filematch;
	char **keywords;
	char *singleline_comment_start;
	char *multiline_comment_start;
	char *multiline_comment_end;
	int flags;
};

typedef struct erow {
	int idx;
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl;
	int hl_open_comment;
} erow;

struct editorConfig {
	unsigned short int number;
	unsigned short int numberlen;
	unsigned short int line_indent;
	unsigned short int message_timeout;
	unsigned short int tab_stop;
	char *separator;
	char statusmsg[80];
	time_t statusmsg_time;
	erow *row;
	struct editorSyntax *syntax;
	char *config_path;
	// 0: normal, 1: input, 2: command, 3: search
	unsigned short int mode;
	unsigned short int cx, cy;
	unsigned short int rx;
	unsigned short int rowoff;
	unsigned short int coloff;
	int screenrows;
	int screencols;
	unsigned short int numrows;
	unsigned short int dirty;
	char *filename;
	struct termios orig_termios;
};

struct editorConfig E;

static int last_input_char;

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return", "else", "default",
	"union", "case", "#include", "#ifndef", "#define", "#endif", "#pragma once", "namespace",
	"struct|", "typedef|", "enum|", "class|", "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", "static|", "using|", "std", NULL
};

struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_FUNCTIONS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

// terminal
static void logger(const int tag, const char *msg, ...);
static void quit();
static void die(const char *s);
static void disableRawMode();
static void enableRawMode();
static int editorReadKey();
static int getCursorPosition(int *rows, int *cols);
static int getWindowSize(int *rows, int *cols);
// syntax highlight
static void editorUpdateSyntax(erow *row);
static int editorSyntaxToColor(int hl);
static void editorSelectSyntaxHighlight();
// row operations
static int editorRowCxToRx(erow *row, int cx);
static int editorRowRxToCx(erow *row, int rx);
static void editorUpdateRow(erow *row);
static void editorInsertRow(int at, char *s, size_t len);
static void editorFreeRow(erow *row);
static void editorDelRow(int at);
static void editorRowInsertChar(erow *row, int at, int c);
static void editorRowAppendString(erow *row, char *s, size_t len);
static void editorRowDelChar(erow *row, int at);
// editor operations
static void editorInsertChar(int c);
static void editorInsertNewLine();
static void editorDelChar();
// file i/o
static char *editorRowsToString(int *buflen);
static void editorOpen(char *filename);
static int editorSave();
// find
static void editorFindCallback(char *query, int key);
static void editorFind();
// output
static void editorScroll();
static void editorRefreshScreen();
static void editorSetStatusMessage(const char* fmt, ...);
// input
static char *editorPrompt(char *prompt, void (*callback)(char *, int));
static void editorMoveCursor(int key);
static void editorProcessKeypress();
// init
static void initEditor();


void logger(const int tag, const char *msg, ...) {
	va_list args;
	va_start(args, msg);
	FILE *log = fopen(KILO_LOG_PATH, "a");
	if (log == NULL) die("log");
	char message[256];
	char tag_type[16];
	time_t now;
	time(&now);

	switch(tag) {
		case (0) : strcpy(tag_type, "INFO");       break;
		case (1) : strcpy(tag_type, "DEBUG");      break;
		case (2) : strcpy(tag_type, "ERROR!");     break;
		case (3) : strcpy(tag_type, "CRITICAL!!"); break;
	}
	char *date = ctime(&now);
	date[strlen(date) - 1] = '\0';
	snprintf(message, sizeof(message), "[%s] at (%s): %s",
			tag_type, date, msg);
	message[strlen(message)] = '\n';
	vfprintf(log, message, args);

	va_end(args);
	fclose(log);
}

void quit() {
	disableRawMode();
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	exit(0);
}

void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
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
	} else {
		if (c > 31 && c < 127)
			last_input_char = c;
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}


int is_separator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
	row->hl = realloc(row->hl, row->rsize);
	memset(row->hl, HL_NORMAL, row->rsize);

	if (E.syntax == NULL) return;

	char **keywords = E.syntax->keywords;

	char *scs = E.syntax->singleline_comment_start;
	char *mcs = E.syntax->multiline_comment_start;
	char *mce = E.syntax->multiline_comment_end;

	int scs_len = scs ? strlen(scs) : 0;
	int mcs_len = mcs ? strlen(mcs) : 0;
	int mce_len = mce ? strlen(mce) : 0;

	int prev_sep = 1;
	int in_string = 0;
	int in_comment = (row->idx > 0 && E.row[row->idx-1].hl_open_comment);

	int i = 0;
	while (i < row->rsize) {
		char c = row->render[i];
		char prev_c = row->render[i-1];
		unsigned char prev_hl = (i>0) ? row->hl[i-1] : HL_NORMAL;

		if (E.syntax->flags && HL_HIGHLIGHT_FUNCTIONS) {
			if (!in_string && isalpha(row->render[i]) && row->render[i+1] == '(') {
				for (int j = i; row->render[j] != ' '; j--) {
					row->hl[j] = HL_FUNCTION;
				}
			}
		}
		if (scs_len && !in_string && !in_comment) {
			if (!strncmp(&row->render[i], scs, scs_len)) {
				memset(&row->hl[i], HL_COMMENT, row->rsize - i);
			}
		}
		if (mcs_len && mce_len && !in_string) {
			if (in_comment) {
				row->hl[i] = HL_COMMENT;
				if (!strncmp(&row->render[i], mce, mce_len)) {
					memset(&row->hl[i], HL_MLCOMMENT, mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				} else {
					i++;
					continue;
				}
			} else if (!strncmp(&row->render[i], mcs, mcs_len)) {
				memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}
		if (E.syntax->flags && HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				if (c == '\\' && i + 1 < row->rsize) {
					row->hl[i+1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0;
				i++;
				prev_sep = 1;
				continue;
			} else {
				if (c == '\"' || c == '\'') {
					in_string = c;
					row->hl[i] = HL_STRING;
					i++;
					continue;
				}
			}
		}
		if (E.syntax->flags && HL_HIGHLIGHT_NUMBERS) {
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
				((c == '.' && prev_hl == HL_NUMBER)) ||
				((c == 'x' || c == 'b') && prev_c == '0') ) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}
		if (prev_sep) {
			int j;
			for (j=0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) klen--;

				if (!strncmp(&row->render[i], keywords[j], klen) &&
						is_separator(row->render[i+klen])) {
					memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue;
			}
		}
		
		prev_sep = is_separator(c);
		i++;
	}

	int changed = (row->hl_open_comment != in_comment);
	row->hl_open_comment = in_comment;
	if (changed && row->idx + 1 < E.numrows)
		editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
	switch (hl) {
		case HL_FUNCTION: return 32;
		case HL_COMMENT:
		case HL_MLCOMMENT: return 90;
		case HL_KEYWORD1: return 31;
		case HL_KEYWORD2: return 36;
		case HL_STRING: return 33;
		case HL_NUMBER: return 35;
		case HL_MATCH: return 42;
		default: return 37;
	}
}

void editorSelectSyntaxHighlight() {
	E.syntax = NULL;
	if (E.filename == NULL) return;

	char *ext = strchr(E.filename, '.');

	for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;
		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');
			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
					(!is_ext && strstr(E.filename, s->filematch[i]))) {
				E.syntax = s;

				int filerow;
				for (filerow = 0; filerow < E.numrows; filerow++) {
					editorUpdateSyntax(&E.row[filerow]);
				}

				return;
			}
			i++;
		}
	}
}


int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') 
			rx += (E.tab_stop - 1) - (rx % E.tab_stop);
		rx++;
	}
	return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
	if (row->chars[cx] == '\t')
	  cur_rx += (E.tab_stop - 1) - (cur_rx % E.tab_stop);
	cur_rx++;
	if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs*(E.tab_stop - 1) + 1);
	
	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % E.tab_stop != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;

	editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
	if (at < 0 || at > E.numrows) return;

	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
	for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

	E.row[at].idx = at;

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	E.row[at].hl = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row) {
	free(row->render);
	free(row->chars);
	free(row->hl);
}

void editorDelRow(int at) {
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at - 1));
	for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
	E.numrows--;
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
	if (at < 0 || at > row->rsize) at = row->rsize;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at+1], &row->chars[at], row->size-at+1);
	row->size++;
	row->chars[at]=c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
} 

void editorRowDelChar(erow *row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}


void editorInsertChar(int c) {
	if (E.cy == E.numrows) {
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewLine() {
	if (E.cx == 0) {
		editorInsertRow(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy+1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar() {
	if (E.cy == E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}


char *editorRowsToString(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; j++)
		totlen += E.row[j].size + 1;
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorConfigSource() {
	FILE *config;
	char *path = malloc(80);
	strcpy(path, getenv("HOME"));
	strcat(path, E.config_path);
	strcat(path, "/settings.toml");
	char errbuf[200];

	if ((config = fopen(path, "r")) == NULL) return;
	toml_table_t *conf = toml_parse_file(config, errbuf, sizeof(errbuf));
	fclose(config);
	if (!conf) return;

	toml_table_t *settings       = toml_table_in(conf,      "settings");
	toml_datum_t number          = toml_bool_in(settings,   "number");
	toml_datum_t numberlen       = toml_int_in(settings,    "numberlen");
	toml_datum_t message_timeout = toml_int_in(settings,    "message-timeout");
	toml_datum_t tab_stop        = toml_int_in(settings,    "tab-stop");
	toml_datum_t separator       = toml_string_in(settings, "separator");

	if (number.ok)
		E.number          = number.u.b;
	if (numberlen.ok)
		E.numberlen       = numberlen.u.i;
	if (message_timeout.ok)
		E.message_timeout = message_timeout.u.i;
	if (tab_stop.ok)
		E.tab_stop        = tab_stop.u.i;
	if (separator.ok)
		E.separator       = separator.u.s;

	free(separator.u.s);
}

void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);
	// ignore path starting with ./ and ../
	while (*(E.filename + 1) == '/') E.filename += 2;
	while (*(E.filename + 1) == '.') E.filename += 3;

	editorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
					line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

int editorSave() {
	if (E.filename == NULL) {
		if ((E.filename = editorPrompt("Save as: %s", NULL)) == NULL) {
			editorSetStatusMessage("Save aborted.");
			return -1;
		}
		editorSelectSyntaxHighlight();
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("\"%s\" %dL, %dB Written", E.filename, E.screencols, len);
				return 0;
			}
		}
		close(fd);
	}

	free(buf);
	editorSetStatusMessage("failed to save %s: %s", E.filename, strerror(errno));
	return -1;
}


void editorFindCallback(char *query, int key) {
	static int last_match = -1;
	static int direction = 1;

	static int saved_hl_line;
	static char *saved_hl = NULL;

	if (saved_hl) {
		memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}
	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1) direction = 1;
	int current = last_match;
	int i;
	for (i = 0; i < E.numrows; i++) {
		current += direction;
		if (current == -1) current = E.numrows - 1;
		else if (current == E.numrows) current = 0;

		erow *row = &E.row[current];
		char *match = strstr(row->render, query);
		if (match) {
			last_match = current;
			E.cy = current;
			E.cx = editorRowRxToCx(row, match - row->render);
			E.rowoff = E.numrows;
			
			saved_hl_line = current;
			saved_hl = malloc(row->rsize);
			memcpy(saved_hl, row->hl, row->rsize);
			memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editorFind() {
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff; 
	int saved_rowoff = E.rowoff;
	int saved_mode = E.mode;

	E.mode = 3;
	char *query = editorPrompt("/%s", editorFindCallback);
	if (query) {
		free(query);
	} else {
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
	E.mode = saved_mode;
}


struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

void editorScroll() {
	E.rx = E.line_indent;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx) + E.line_indent;
	}
	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}


void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
				"Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "\x1b[90m", 5);
					for (int pad=0; pad<E.line_indent-2; pad++) abAppend(ab, " ", 1);
					abAppend(ab, "~", 1);
					abAppend(ab, " ", 1);
					abAppend(ab, "\x1b[m", 3);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} else {
				abAppend(ab, "\x1b[90m", 5);
				for (int pad=0; pad<E.line_indent-2; pad++) abAppend(ab, " ", 1);
				abAppend(ab, "~", 1);
				abAppend(ab, " ", 1);
				abAppend(ab, "\x1b[m", 3);
			}
		} else {
			if (E.number) {
				char rcol[80];
				E.line_indent = snprintf(rcol, sizeof(rcol), " %*d ", 
						E.numberlen, filerow + 1);
				abAppend(ab, "\x1b[90m", 5);
				abAppend(ab, rcol, E.line_indent);
				abAppend(ab, "\x1b[m", 3);
			}
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			char *c = &E.row[filerow].render[E.coloff];
			unsigned char *hl = &E.row[filerow].hl[E.coloff];
			int current_color = -1;
			int j;
			for (j = 0; j < len; j++) {
				if (iscntrl(c[j])){
					char sym = (c[j] <= 26) ? '@' + c[j] : '?';
					abAppend(ab, "\x1b[7m", 4);
					abAppend(ab, &sym, 1);
					abAppend(ab, "\x1b[m", 3);
					if (current_color != -1) {
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
						abAppend(ab, buf, clen);
					}
				} else if (hl[j] == HL_COMMENT) {
					abAppend(ab, "\x1b[2m", 4);
					abAppend(ab, &c[j], 1);
					abAppend(ab, "\x1b[0m", 4);
				} else if (hl[j] == HL_NORMAL) {
					if (current_color != -1) {
						abAppend(ab, "\x1b[39m", 5);
						abAppend(ab, "\x1b[49m", 5);
						current_color = -1;
					}
					abAppend(ab, &c[j], 1);
				} else {
					int color = editorSyntaxToColor(hl[j]);
					if (color != current_color) {
						current_color = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						abAppend(ab, buf, clen);
					}
					abAppend(ab, &c[j], 1);
				}
			}
			abAppend(ab, "\x1b[39m", 5);
			abAppend(ab, "\x1b[49m", 5);
		}

		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}

}

void editorDrawStatusBar(struct abuf *ab) {
	abAppend(ab, "\x1b[40m", 5);

	char status[80], rstatus[80];
	char *e_mode;
	if (E.mode == 0)	  e_mode = "NORMAL";
	else if (E.mode == 1) e_mode = "INSERT";
	else if (E.mode == 2) e_mode = "COMMAND";
	else if (E.mode == 3) e_mode = "SEARCH";
	int len = snprintf(status, sizeof(status), " %s %s %.20s%s- %d",
			e_mode , E.separator,
			E.filename ? E.filename : "[No Name]", 
			E.dirty ? " [+] " : " ",
			E.numrows);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%c %s %s %d/%d ",
			last_input_char,
			E.syntax ? E.syntax->filetype : "no ft", E.separator,
			E.cy + 1, E.numrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < E.message_timeout)
		abAppend(ab, E.statusmsg, msglen);
}


void editorRefreshScreen() {
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	/* return cursor to init position */
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
			(E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}


char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if(buflen != 0) buf[--buflen] = '\0';
		}else if (c == '\x1b'){
			editorSetStatusMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editorSetStatusMessage("");
				if (callback) callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
		if (callback) callback(buf, c);
	}
}

void editorMoveCursor(int key) {
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch(key) {
		case 'h':
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case 'l':
		case ARROW_RIGHT:
			if (row && E.cx < row->size - 1)
				E.cx++;
			break;
		case 'k':
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case 'j':
		case ARROW_DOWN:
			if (E.mode == 1 && E.cy < E.numrows) {
				E.cy++;
			} else if (E.mode == 0 && E.cy < E.numrows - 1) {
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
}

void editorProcessKeypress() {
	static int quit_times = KILO_QUIT_TIMES;

	int c = editorReadKey();

	if (E.mode == 0) {
		switch (c) {
			case CTRL_KEY('q'):
				if (E.dirty && quit_times > 0) {
					editorSetStatusMessage("Error: no write since last change, press Ctrl-Q %d more times to quit.", quit_times);
					quit_times--;
				return;
				}
				quit();
				break;

			case CTRL_KEY('s'):
				editorSave();
				break;

			case '/':
				editorFind();
				break;

			case 'i':
				E.mode = 1;
				break;

			case 'I':
				E.cx = 0;
				E.mode = 1;
				break;

			case 'a':
				if (E.cx != 0) {
					E.cx += 1;
				}
				E.mode = 1;
				break;

			case 'A':
				E.cx = E.row[E.cy].size;
				E.mode = 1;
				break;

			case 'o':
				editorInsertRow(E.cy + 1, "", 0);
				editorMoveCursor(ARROW_DOWN);
				E.cx = 0;
				E.mode = 1;
				break;

			case 'O':
				editorInsertRow(E.cy, "", 0);
				E.cx = 0;
				E.mode = 1;
				break;

			case 'x':
				editorRowDelChar(&E.row[E.cy], E.cx);
				break;

			case 'r': {
				int c = editorReadKey();
				editorRowDelChar(&E.row[E.cy], E.cx);
				editorInsertChar(c);
				editorMoveCursor(ARROW_LEFT);
				break;
			}

			case 'd': {
				int c = editorReadKey();
				if (c == 'd') {
					editorDelRow(E.cy);
				} else if (c == 'k') {
					editorDelRow(E.cy);
					editorDelRow(E.cy - 1);
					editorMoveCursor(ARROW_UP);
				} else if (c == 'j') {
					editorDelRow(E.cy + 1);
					editorDelRow(E.cy);
				} else if (c == 'l') {
					editorRowDelChar(&E.row[E.cy], E.cx+1);
					editorMoveCursor(ARROW_LEFT);
				} else if (c == 'h') {
					editorRowDelChar(&E.row[E.cy], E.cx-1);
				} else if (c == 'w') {
					while (E.row[E.cy].chars[E.cx] != ' ' && E.cx != E.row[E.cy].size) 
					editorRowDelChar(&E.row[E.cy], E.cx);
				}
				break;
			}

			case BACKSPACE:
				editorMoveCursor(ARROW_RIGHT);
				break;

			case CTRL_KEY('h'):
			case DEL_KEY:
				editorDelChar();
				break;

			case '\r':
				editorMoveCursor(ARROW_DOWN);
				break;

			case 'h':
			case 'j':
			case 'k':
			case 'l':
			case ARROW_LEFT:
			case ARROW_DOWN:
			case ARROW_UP:
			case ARROW_RIGHT:
				editorMoveCursor(c);
				break;

			case 'g': E.cy = 0; break;
			case 'G': E.cy = E.numrows; break;

			case '>':
				editorRowInsertChar(&E.row[E.cy], 0, '\t');
				editorMoveCursor(ARROW_RIGHT);
				break;

			case '<':
				if (E.row[E.cy].chars[0] == '\t') {
					editorRowDelChar(&E.row[E.cy], 0);
					editorMoveCursor(ARROW_LEFT);
				}
				break;

			case HOME_KEY: E.cx = 0; break;
			case END_KEY: 
				if (E.cy < E.numrows)
					E.cx = E.row[E.cy].size;
				break;

			case PAGE_UP:
			case PAGE_DOWN: {
				if (c == PAGE_UP) {
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				} 
				
				int times = E.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
				break;
			}

			case CTRL_KEY('l'):
			case '\x1b':
				break;

			default:
				break;
		}
	} else if (E.mode == 1) {
		switch (c) {
			case CTRL_KEY('l'):
			case '\x1b':
				if (E.cx != 0) editorMoveCursor(ARROW_LEFT);
				E.mode = 0;
				break;
			
			case CTRL_KEY('q'):
				if (E.dirty && quit_times > 0) {
					editorSetStatusMessage("Error: no write since last change, press Ctrl-Q %d more times to quit.", quit_times);
					quit_times--;
					return;
				}
				quit();
				break;

			case DEL_KEY:
			case BACKSPACE:
				if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
				editorDelChar();
				break;

			case '\r':
				editorInsertNewLine();
				break;

			case ARROW_LEFT:
			case ARROW_DOWN:
			case ARROW_UP:
			case ARROW_RIGHT:
				editorMoveCursor(c);
				break;

			default:
				if ((c > 31 && c < 127) || c == 9)
					editorInsertChar(c);
				break;
		}
	}

	quit_times = KILO_QUIT_TIMES;
}


void initEditor() {
	E.number          = 1;
	E.numberlen       = 4;
	E.line_indent     = E.numberlen + 2;
	E.message_timeout = 5;
	E.tab_stop        = 4;
	E.separator       = "|";
	E.statusmsg[0]    = '\0';
	E.statusmsg_time  = 0;
	E.row             = NULL;
	E.syntax          = NULL;

	E.config_path     = "/.config/kilo";
	E.mode            = 0;
	E.cx              = 0;
	E.cy              = 0;
	E.rx              = E.line_indent;
	E.rowoff          = 0;
	E.coloff          = 0;
	E.numrows         = 0;
	E.dirty           = 0;
	E.filename        = NULL;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2;
}


int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	editorConfigSource();

	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-Q: Quit | Ctrl-S: Save");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	quit();
}

