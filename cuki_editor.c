/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** toggles ***/

int toggleLineNumberShow = 1;		// Start with line numbers showing on.

/*** defines ***/

#define CUKI_VERSION "0.0.1"
#define TAB_LENGTH 4

#define COOL_COLOUR "\x1b[38;5;250m"	//Light grey colour.
#define FAST_MOVE_STEPS 5
#define FAST_MOVE_STEPS_SHIFT 10
#define FAST_MOVE_SHIFT_ADVANCED 25

#define CTRL_KEY(K) ((K) & 0x1f)

enum editorKey 
{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	CTRL_ARROW_UP = 1100,
	CTRL_ARROW_DOWN,
	CTRL_ARROW_LEFT,
	CTRL_ARROW_RIGHT,
	SHIFT_ARROW_UP = 1200,
	SHIFT_ARROW_DOWN,
	SHIFT_ARROW_LEFT,
	SHIFT_ARROW_RIGHT,
	CTRL_SHIFT_ARROW_UP = 1300,
	CTRL_SHIFT_ARROW_DOWN,
	CTRL_SHIFT_ARROW_LEFT,
	CTRL_SHIFT_ARROW_RIGHT,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
	MOUSE_EVENT,
	NUM_5,
	F1
};

// ENUM for UNDO/REDO:
typedef enum
{
	BYTE_INSERT,	// Inserted 1 byte.
	BYTE_DELETE	// Deleted 1 byte.
} ByteActionType;

/*** data ***/

typedef struct erow
{
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

// STRUCT for UNDO/REDO:
typedef struct ByteAction
{
	ByteActionType type;

	int row;			// Buffer row index.
	int col;			// Charcter offset inside the row.
	char ch;			// The exact byte inserted / removed.

	struct ByteAction *prev;
	struct ByteAction *next;
} ByteAction;

struct editorConfig
{
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[128];
	time_t statusmsg_time;
	struct termios orig_termios;
	ByteAction *undo_head;
	ByteAction *undo_tail;
	ByteAction *undo_current;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback) (char *, int));
int editorGetGutterWidth(void);
void editorInsertNewline(void);
void editorDelChar(void);
void editorMoveCursor(int key);

/*** terminal ***/

void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode()
{
	write(STDOUT_FILENO, "\x1b[?1049l", 8);

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
	{
		die("tcsetattr");
	}
}

void enableRawMode()
{
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die ("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;

	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");

	write(STDOUT_FILENO, "\x1b[?1049h", 8);
}

static inline int has_byte(void)
{
	struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
	return poll(&pfd, 1, 0) > 0;
}

int editorReadKey()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b')
	{
		char seq[6];
		if (!has_byte() || read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (!has_byte() || read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[')
		{
			if (seq[1] == 'P') return DEL_KEY;
			if (seq[1] == 'E') return NUM_5;

			if (seq[1] == '1')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; /* ';' */
				if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b'; /* '5' */
				if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b'; /* Direction */

				if (seq[2] == ';' && seq[3] == '5')
				{
					switch (seq[4])
					{
					case 'A': return CTRL_ARROW_UP;
					case 'B': return CTRL_ARROW_DOWN;
					case 'C': return CTRL_ARROW_RIGHT;
					case 'D': return CTRL_ARROW_LEFT;
					}
				}

				else if (seq[2] == ';' && seq[3] == '2')
				{
					switch (seq[4])
					{
					case 'A': return SHIFT_ARROW_UP;
					case 'B': return SHIFT_ARROW_DOWN;
					case 'C': return SHIFT_ARROW_RIGHT;
					case 'D': return SHIFT_ARROW_LEFT;
					}
				}

				else if (seq[2] == ';' && seq[3] == '6')
				{
					switch (seq[4])
					{
					case 'A': return CTRL_SHIFT_ARROW_UP;
					case 'B': return CTRL_SHIFT_ARROW_DOWN;
					case 'C': return CTRL_SHIFT_ARROW_RIGHT;
					case 'D': return CTRL_SHIFT_ARROW_LEFT;
					}
				}
			}

			if (seq[1] >= '0' && seq[1] <= '9')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~')
				{
					switch (seq[1])
					{
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else
			{
				switch (seq[1])
				{
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O' && seq[1] == 'P')
		{
			return F1;
		}

		else if (seq[0] == 'O' || seq[0] == 'P')
		{
			switch (seq[1])
			{
				case 'P': return DEL_KEY;
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		if (seq[0] == '[') {
		if (seq[1] >= '0' && seq[1] <= '9') {
			char seq2;
			if (read(STDIN_FILENO, &seq2, 1) != 1) return '\x1b';
			if (seq2 == '~' && seq[1] == '3') return DEL_KEY;
		}
	}

		return '\x1b';
	}
	else
	{
		return c;
	}
}

int getCursorPosition(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1)
	{
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return 1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return 1;

	return 0;
}

int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	}
	else
	{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (TAB_LENGTH - 1) - (rx % TAB_LENGTH);
		rx++;
	}
	return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
	int cur_rx = 0;
	int cx;
	for (cx = 0; cx < row->size; cx++)
	{
		if (row->chars[cx] == '\t')
			cur_rx += (TAB_LENGTH - 1) - (cur_rx % TAB_LENGTH);
		cur_rx++;

		if (cur_rx > rx) return cx;
	}
	return cx;
}

void editorUpdateRow(erow *row)
{
	int tabs = 0;
	int j;

	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row -> render);
	row->render = malloc(row->size + tabs* (TAB_LENGTH - 1) + 1);

	int idx = 0;
	for (j = 0; j < row -> size; j++)
	{
		if (row -> chars[j] == '\t')
		{
			row -> render[idx++] = ' ';
			while (idx % TAB_LENGTH != 0) row -> render[idx++] = ' ';
		}

		else
		{
			row -> render[idx++] = row -> chars[j];
		}
	}

	row -> render[idx] = '\0';
	row -> rsize = idx;
}

void editorInsertRow(int at,char *s, size_t len)
{
	if (at < 0 || at > E.numrows) return;
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row)
{
	free(row->render);
	free(row->chars);
}

void editorDelRow(int at)
{
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

/*** undo/redo ***/

void editorUndo(void)
{
	if (!E.undo_current) return;

	ByteAction *act = E.undo_current;

	/* Ensure row exists before modifying */
	if (act->row < E.numrows)
	{
		erow *row = &E.row[act->row];

		if (act->type == BYTE_INSERT)
		{
			/* Undo an insertion -> Delete the character directly from the row */
			editorRowDelChar(row, act->col);
			E.dirty++;
		} 
		else if (act->type == BYTE_DELETE)
		{
			/* Undo a deletion -> Insert the character directly back into the row */
			editorRowInsertChar(row, act->col, act->ch);
			E.dirty++;
		}

		/* Move cursor to action position */
		E.cy = act->row;
		E.cx = act->col;
	}

	E.undo_current = act->prev;
}

void editorRedo(void)
{
	ByteAction *act = E.undo_current ? E.undo_current->next : E.undo_head;
	if (!act) return;

	if (act->row < E.numrows)
	{
		erow *row = &E.row[act->row];

		if (act->type == BYTE_INSERT)
		{
			/* Redo insertion -> Re-insert the character into the row */
			editorRowInsertChar(row, act->col, act->ch);
			E.dirty++;
		}
		else if (act->type == BYTE_DELETE)
		{
			/* Redo deletion -> Delete the character from the row */
			editorRowDelChar(row, act->col);
			E.dirty++;
		}

		/* Move cursor to action position */
		E.cy = act->row;
		E.cx = (act->type == BYTE_INSERT) ? act->col + 1 : act->col;
	}

	E.undo_current = act;
}


void push_byte_action(ByteActionType type, int row, int col, char ch)
{
	/* Truncate forward redo history if we are in the middle of the stack */
	ByteAction *curr = E.undo_current ? E.undo_current->next : E.undo_head;
	while (curr)
	{
		ByteAction *next = curr->next;
		free(curr);
		curr = next;
	}

	if (E.undo_current)
	{
		E.undo_current->next = NULL;
		E.undo_tail = E.undo_current;
	}
	else if (E.undo_head)
	{
		/* If undo_current is NULL, we undid everything; clear history */
		E.undo_head = NULL;
		E.undo_tail = NULL;
	}

	/* Allocate new action */
	ByteAction *new_action = malloc(sizeof(ByteAction));
	new_action->type = type;
	new_action->row = row;
	new_action->col = col;
	new_action->ch = ch;
	new_action->prev = E.undo_tail;
	new_action->next = NULL;

	if (E.undo_tail)
	{
		E.undo_tail->next = new_action;
	}
	else
	{
		E.undo_head = new_action;
	}

	E.undo_tail = new_action;
	E.undo_current = new_action;
}

/*** editor operations ***/

void editorInsertChar(int c)
{
	if (E.cy == E.numrows)
	{
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	/* Right before or after inserting 'c' into E.row[cy] */
	push_byte_action(BYTE_INSERT, E.cy, E.cx, (char)c);
	E.cx++;
}

void editorInsertString(const char *s)
{
	if (!s) return;
	
	while (*s)
	{
		if (*s == '\n')	// New Line
		{
			editorInsertNewline();
			s++;
		}
		else if (*s == '\r')	// Whatever it is
		{
			editorInsertChar('\r');
			s++;
		}
/*
		else if (*s == '\t')	// Tab
		{
			editorInsertChar('\t');
		}
*/
		else if (*s == '\b')	// Backspace
		{
			editorDelChar();
			s++;
		}
		else if (strncmp(s, "\x1b[A", 3) == 0)	// Cursor Up
		{
			editorMoveCursor(ARROW_UP);
			s += 3;
		}
		else if (strncmp(s, "\x1b[B", 3) == 0)	// Cursor Down
		{
			editorMoveCursor(ARROW_DOWN);
			s += 3;
		}
		else if (strncmp(s, "\x1b[C", 3) == 0)	// Cursor Right
		{
			editorMoveCursor(ARROW_RIGHT);
			s += 3;
		}
		else if (strncmp(s, "\x1b[D", 3) == 0)	// Cursor Left
		{
			editorMoveCursor(ARROW_LEFT);
			s += 3;
		}
/*
		else if (strncmp(s, "\x1b[F", 3) == 0)	// Cursor End
		{
			if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
			s += 3;
		}
*/
		else if (strncmp(s, "\x1b[F", 3) == 0) {
			if (E.numrows > 0 && E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;
			}
			else
			{
				E.cx = 0;
			}
			s += 3;
		}

		else if (strncmp(s, "\x1b[H", 3) == 0)
		{
			E.cx = 0;
			s += 3;
		}
		else
		{
			editorInsertChar(*s);
			s++;
		}
	}
}

void editorInsertNewline(void)
{
	if (E.cx == 0)
	{
		editorInsertRow(E.cy, "", 0);
	}

	else
	{
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorAppendedInsertNewline(void)
{
	if (E.cx == 0)
	{
		editorInsertRow(E.cy, "", 0);
	}

	else
	{
		erow *row = &E.row[E.cy];

		// 1. Calculate leading whitespace on the current line
		int indent_len = 0;
		while (indent_len < row->size && (row->chars[indent_len] == ' ' || row->chars[indent_len] == '\t'))
		{
			indent_len++;
		}

		// 2. Split line at current cursor position
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);

		// 3. Prepend leading whitespace to the newly created line
		if (indent_len > 0) {
			erow *new_row = &E.row[E.cy + 1];
			new_row->chars = realloc(new_row->chars, new_row->size + indent_len + 1);
			
			// Shift existing contents right
			memmove(&new_row->chars[indent_len], new_row->chars, new_row->size + 1);
			
			//Copy the indentation chars
			memcpy(new_row->chars, E.row[E.cy].chars, indent_len);
			new_row->size += indent_len;
			editorUpdateRow(new_row);
		}

		// 4. Position cursor on the new line past the indent
		E.cy++;
		E.cx = indent_len;
		E.dirty++;
		return;
	}

	E.cy++;
	E.cx = 0;
}


void editorDelChar()
{
	if (E.cy == E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		/* Capture character BEFORE row deletion modifies the string */
		char deleted_ch = row->chars[E.cx - 1];
		push_byte_action(BYTE_DELETE, E.cy, E.cx - 1, deleted_ch);

		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	}
	else
	{
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

void editorDelCharForward(void)
{
	if (E.cy == E.numrows) return;

	erow *row = &E.row[E.cy];

	if (E.cx < row->size)
	{
		/* Delete character directly under/at the cursor position */
		char deleted_ch = row->chars[E.cx];
		push_byte_action(BYTE_DELETE, E.cy, E.cx, deleted_ch);

		editorRowDelChar(row, E.cx);
		/* Note: E.cx does not decrement because the text shifts left */
	}
	else if (E.cy < E.numrows - 1)
	{
		/* At the end of line: append next row to current row */
		erow *next_row = &E.row[E.cy + 1];

		push_byte_action(BYTE_DELETE, E.cy, E.cx, '\n');

		editorRowAppendString(row, next_row->chars, next_row->size);
		editorDelRow(E.cy + 1);
	}
}

/*** file i/o ***/

char *editorRowsToString(int *buflen)
{
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

void editorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp)
	{
		if (errno == ENOENT) return;
		die ("fopen");
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1)
	{
		while (linelen > 0 && (	line[linelen - 1] == '\n' ||
					line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editorSave()
{
	if (E.filename == NULL)
	{
		E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
		if (E.filename == NULL)
		{
			editorSetStatusMessage("Save aborted");
			return;
		}
	}
	int len;
	char *buf = editorRowsToString(&len);
	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1)
	{
		if (ftruncate(fd, len) != -1)
		{
			if (write(fd, buf, len) == len)
			{
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close (fd);
	}

	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key)
{
	static int last_match = -1;
	static int direction = 1;

	if (key == '\r' || key == '\x1b')
	{
		last_match = -1;
		direction = 1;
		return;
	}
	else if (key == ARROW_RIGHT || key == ARROW_DOWN)
	{
		direction = 1;
	}
	else if (key == ARROW_LEFT || key == ARROW_UP)
	{
		direction = -1;
	}
	else
	{
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1) direction = 1;
	int current = last_match;
	int i;
	for (i = 0; i < E.numrows; i++)
	{
		current += direction;
		if (current == -1) current = E.numrows - 1;
		else if (current == E.numrows) current = 0;

		erow *row = &E.row[current];
		char *match = strstr(row->render, query);
		if (match)
		{
			last_match = current;
			E.cy = current;
			E.cx = editorRowRxToCx(row, match - row->render);
			E.rowoff = E.numrows;
			break;
		}
	}
}

void editorFind()
{
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff;
	int saved_rowoff = E.rowoff;

	char *query = editorPrompt(	"Search: %s (Use ESC/Arrows/Enter)",
					editorFindCallback);

	if (query)
	{
		free(query);
	}
	else
	{
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
}

/*** append buffer ***/

struct abuf
{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab -> b, ab -> len + len);

	if (new == NULL) return;
	memcpy(&new[ab -> len], s, len);
	ab -> b = new;
	ab -> len += len;
}

void abFree(struct abuf *ab)
{
	free(ab -> b);
}

/*** output ***/

void editorScroll()
{
	E.rx = 0;

	if (E.cy < E.numrows)
	{
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff)
	{
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows)
	{
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff)
	{
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols)
	{
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab)	// Remove "~" part of this later
{					// Reason: Looks ugly.
	int y = 0;
	int gutter_width = editorGetGutterWidth();
	int usable_cols = E.screencols - gutter_width;
	int filerow = E.rowoff;

	while (y < E.screenrows)
	{
		if (filerow < E.numrows)
		{
			erow *row = &E.row[filerow];
			int row_len = row->rsize;
			int offset = 0;

			/* Handle empty lines */
			if (row_len == 0)
			{
				if (toggleLineNumberShow == 1) {
					char buf[16];
					int len = snprintf(buf, sizeof(buf), "%*d ", gutter_width - 1, filerow + 1);
					abAppend(ab, "\x1b[90m", 5);
					abAppend(ab, buf, len);
					abAppend(ab, "\x1b[0m", 4);
				} else {
					for (int i = 0; i < gutter_width; i++) abAppend(ab, " ", 1);
				}

				abAppend(ab, "\x1b[K\r\n", 5);
				y++;
			}
			/* Soft-wrap long lines across screen rows */
			else
			{
				while (offset < row_len && y < E.screenrows)
				{
					if (toggleLineNumberShow == 1) {
						if (offset == 0) {
							/* First line segment gets line number */
							char buf[16];
							int len = snprintf(buf, sizeof(buf), "%*d ", gutter_width - 1, filerow + 1);
							abAppend(ab, "\x1b[90m", 5);
							abAppend(ab, buf, len);
							abAppend(ab, "\x1b[0m", 4);
						} else {
							/* Wrapped continuation lines get empty gutter padding */
							for (int i = 0; i < gutter_width; i++) abAppend(ab, " ", 1);
						}
					} else {
						for (int i = 0; i < gutter_width; i++) abAppend(ab, " ", 1);
					}

					int chunk_len = row_len - offset;
					if (chunk_len > usable_cols) chunk_len = usable_cols;

					abAppend(ab, &row->render[offset], chunk_len);



					abAppend(ab, "\x1b[K\r\n", 5);

					offset += chunk_len;
					y++;
				}
			}

			filerow++; /* Move to next file row after processing all wrapped slices */
		}
		else
		{
			/* Screen rows past end of file (filerow >= E.numrows) */
			if (E.numrows == 0 && y == E.screenrows / 3)
			{
				char welcome[80];
				int welcomelen = snprintf(welcome,
							sizeof(welcome),
							"Cuki editor -- version %s",
							CUKI_VERSION);
				if (welcomelen > E.screencols)
					welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen)/2;
				if (padding)
				{
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			}
			else
			{
				abAppend(ab, "~", 1);
			}

			abAppend(ab, "\x1b[K\r\n", 5);
			y++;
		}
	}
}

void editorDrawStatusBar(struct abuf *ab)
{
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
	E.filename ? E.filename : "[No Name]", E.numrows,
	E.dirty ? "(modified)" : "");
	int rlen = snprintf(	rstatus, sizeof(rstatus), "%d/%d",
				E.cy + 1, E.numrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols)
	{
		if (E.screencols - len == rlen)
		{
			abAppend(ab, rstatus, rlen);
			break;
		}

		else
		{
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen)
		abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	int gutter_width = editorGetGutterWidth();
	int usable_cols = E.screencols - gutter_width - 1;

	/* Calculate visual Y offset considering soft-wrapped lines above E.cy */
	int visual_y = 0;
	for (int i = E.rowoff; i < E.cy && i < E.numrows; i++) {
		int rlen = E.row[i].rsize;
		if (rlen == 0) {
			visual_y++;
		} else {
			visual_y += (rlen + usable_cols - 1) / usable_cols;
		}
	}

	/* Account for wraps inside the current row (E.cy) */
	if (E.cy < E.numrows) {
		visual_y += (E.rx / usable_cols);
	}

	int screen_y = visual_y + 1;
	int screen_x = (E.rx % usable_cols) + gutter_width + 1;

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", screen_y, screen_x);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // show cursor

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
}

int editorGetGutterWidth(void)
{
	if (toggleLineNumberShow)
	{
		int digits = 1;
		int max_rows = E.numrows;
		while (max_rows >= 10) {
			digits++;
			max_rows /= 10;
		}
		/* Ensure a minimum width of 3 digits + 1 space separator */
		if (digits < 3) digits = 3;
		return digits + 1; 
	}
	else
	{
		return 0;
	}
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);
	size_t buflen = 0;
	buf[0] = '\0';
	while (1)
	{
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
			if (buflen != 0) buf[--buflen] = '\0';
		}

		else if (c == '\x1b')
		{
			editorSetStatusMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		}

		else if (c == '\r')
		{
			if (buflen != 0)
			{
				editorSetStatusMessage("");
				if (callback) callback(buf, c);
				return buf;
			}
		}

		else if (!iscntrl(c) && c < 128)
		{
			if (buflen == bufsize - 1)
			{
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}

		if (callback) callback(buf, c);
	}
}

void editorGetScreenCursorPos(int *screen_x, int *screen_y) {
		int gutter_width = editorGetGutterWidth();
		int usable_cols = E.screencols - gutter_width;

		/* Calculate visual row offset from top of screen (E.rowoff) */
		int visual_y = 0;
		for (int i = E.rowoff; i < E.cy; i++) {
				if (i < E.numrows) {
						int rlen = E.row[i].rsize;
						int num_wrapped_lines = (rlen == 0) ? 1 : (rlen + usable_cols - 1) / usable_cols;
						visual_y += num_wrapped_lines;
				} else {
						visual_y++;
				}
		}

		/* Add wrapped lines within current row (E.cy) */
		int current_row_rx = editorRowCxToRx(&E.row[E.cy], E.cx); // Handles tab expansions if used
		visual_y += (current_row_rx / usable_cols);

		*screen_x = (current_row_rx % usable_cols) + gutter_width + 1; // +1 for 1-based ANSI indexing
		*screen_y = visual_y + 1; // +1 for 1-based ANSI indexing
}

void editorMoveCursor(int key)
{
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key)
	{
		case ARROW_LEFT:
			if (E.cx != 0)
			{
				E.cx--;
			}
			else if (E.cy > 0)
			{
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row -> size)
			{
				E.cx++;
			}
			else if (row && E.cx == row -> size)
			{
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0)
			{
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows)
			{
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row -> size : 0;
	if (E.cx > rowlen)
	{
		E.cx = rowlen;
	}
}

void editorProcessKeypress()
{
	int c = editorReadKey();

	switch (c)
	{
		case '\r':
			editorInsertNewline();
			break;

		case NUM_5:
			editorInsertString("#include <stdio.h>\n\nint main()\n{\n\n\n\treturn 0;\n\b}\x1b[A\x1b[A\x1b[A\t");
			break;

		case F1:
			editorInsertString("\x1b[F\n{\n\t\n\b}\x1b[A");
			break;

		case CTRL_KEY('z'):
			editorUndo();
			editorSetStatusMessage("Undo");
			break;

		case CTRL_KEY('y'):
			editorRedo();
			editorSetStatusMessage("Redo");
			break;

		case CTRL_KEY('a'):
			toggleLineNumberShow = 1;
			break;

		case CTRL_KEY('s'):
			toggleLineNumberShow = 0;
			break;

		case CTRL_KEY('g'):
			editorSetStatusMessage("Help: Ctrl: O = Save, X = Exit, W = Find, Z/Y = Undo/Redo, A/S = Line show on/off.");
			break;

		case CTRL_KEY('x'):
			if (E.dirty)
			{
				editorSetStatusMessage("Save modified buffer? [y/n]: ");
				editorRefreshScreen();
				int c = editorReadKey();
				if (c == 'y' || c == 'Y')
				{
					editorSave();
					if (E.dirty)
					{
						editorSetStatusMessage("Save aborted.");
						return;
					}
				}
					else if (c == 'n' || c == 'N')
				{
				}
				else
				{
					editorSetStatusMessage("Aborted.");
					return;
				}
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('o'):
			editorSave();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case CTRL_KEY('w'):
			editorFind();
			break;

		case DEL_KEY:
			editorDelCharForward();
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
			editorDelChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
		{
			if (c == PAGE_UP)
			{
				E.cy = E.rowoff;
			}
			else if (c == PAGE_DOWN)
			{
				E.cy = E.rowoff + E.screenrows - 1;
				if (E.cy > E.numrows) E.cy = E.numrows;
			}
			int times = E.screenrows;
			while (times--)
			{
				editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		}

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;

		case CTRL_ARROW_UP:
			for (int i = 0; i < FAST_MOVE_STEPS; i++) editorMoveCursor(ARROW_UP);
			break;

		case CTRL_ARROW_DOWN:
			for (int i = 0; i < FAST_MOVE_STEPS; i++) editorMoveCursor(ARROW_DOWN);
			break;

		case CTRL_ARROW_LEFT:
			for (int i = 0; i < FAST_MOVE_STEPS; i++) editorMoveCursor(ARROW_LEFT);
			break;

		case CTRL_ARROW_RIGHT:
			for (int i = 0; i < FAST_MOVE_STEPS; i++) editorMoveCursor(ARROW_RIGHT);
			break;


		case SHIFT_ARROW_UP:
			for (int i = 0; i < FAST_MOVE_STEPS_SHIFT; i++) editorMoveCursor(ARROW_UP);
			break;

		case SHIFT_ARROW_DOWN:
			for (int i = 0; i < FAST_MOVE_STEPS_SHIFT; i++) editorMoveCursor(ARROW_DOWN);
			break;

		case SHIFT_ARROW_LEFT:
			for (int i = 0; i < FAST_MOVE_STEPS_SHIFT; i++) editorMoveCursor(ARROW_LEFT);
			break;

		case SHIFT_ARROW_RIGHT:
			for (int i = 0; i < FAST_MOVE_STEPS_SHIFT; i++) editorMoveCursor(ARROW_RIGHT);
			break;


		case CTRL_SHIFT_ARROW_UP:
			for (int i = 0; i < FAST_MOVE_SHIFT_ADVANCED; i++) editorMoveCursor(ARROW_UP);
			break;

		case CTRL_SHIFT_ARROW_DOWN:
			for (int i = 0; i < FAST_MOVE_SHIFT_ADVANCED; i++) editorMoveCursor(ARROW_DOWN);
			break;

		case CTRL_SHIFT_ARROW_LEFT:
			for (int i = 0; i < FAST_MOVE_SHIFT_ADVANCED; i++) editorMoveCursor(ARROW_LEFT);
			break;

		case CTRL_SHIFT_ARROW_RIGHT:
			for (int i = 0; i < FAST_MOVE_SHIFT_ADVANCED; i++) editorMoveCursor(ARROW_RIGHT);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		case '\t':
			editorInsertChar('\t');
			break;

		default:
			if (isprint(c))
				editorInsertChar(c);
			break;
	}

}

/*** init ***/

void initEditor()
{
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
//	E.showWhiteSpace = 0;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2;
	E.undo_head = NULL;
	E.undo_tail = NULL;
	E.undo_current = NULL;
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if (argc >= 2)
	{
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("Help: Ctrl: O = Save, X = Exit, W = Find, Z/Y = Undo/Redo, A/S = Line show on/off.");

	while (1)
	{
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
