 /*
  * UAE - The Un*x Amiga Emulator
  *
  * ncurses frontend for a text-based user interface.
  *
  * Copyright 1996 Bernd Schmidt
  * If you find the routines in this file useful, you may use them in your
  * programs without restrictions. Essentially, it's in the public domain.
  *
  */


#include "sysconfig.h"
#include "sysdeps.h"

#ifdef HAVE_NCURSES_H
#include <ncurses.h>
#else
#include <curses.h>
#endif
#include <ctype.h>

#include "options.h"
#include "uae.h"
#include "tui.h"

#ifdef DONT_HAVE_ATTR_T
typedef int attr_t;
#endif

static WINDOW *currwin;

static WINDOW *winstack[10]; /* more than enough */
static int winnr = 0;

/****i* januae-svgancui/tui_setup ****************************************
*
*   NAME
*       tui_setup() -- Initialize the curses user interface.
*
*   SYNOPSIS
*       void tui_setup(void);
*
*   FUNCTION
*       Initialize curses input and display modes, install the default
*       color pair when supported, and register stdscr as window zero.
*
*   NOTES
*       Resets the ten-slot window stack and selects stdscr. Call before
*       using other tui_* routines.
*
****************************************************************************
*
*/
void tui_setup(void)
{
    int i;

    for (i = 0; i < 10; i++)
	winstack[i] = NULL;
    /* From the ncurses manpage... */
    initscr (); start_color (); cbreak(); noecho(); nonl (); intrflush (stdscr, FALSE); keypad (stdscr, TRUE);
    currwin = stdscr;
    if (has_colors ()) {
	init_pair (1, COLOR_WHITE, COLOR_BLUE);
	init_pair (2, COLOR_BLACK, COLOR_WHITE);
	wattron (currwin, COLOR_PAIR (1) | A_BOLD);
	wbkgd (currwin, ' ' | COLOR_PAIR (1));
    }

    winstack[0] = stdscr;
    winnr = 1;
}

/****i* januae-svgancui/tui_lines ****************************************
*
*   NAME
*       tui_lines() -- Query terminal height.
*
*   SYNOPSIS
*       int tui_lines (void);
*
*   FUNCTION
*       Read the height reported by curses.
*
*   RESULT
*       Current value of LINES.
*
****************************************************************************
*
*/
int tui_lines (void)
{
    return LINES;
}

/****i* januae-svgancui/tui_cols *****************************************
*
*   NAME
*       tui_cols() -- Query terminal width.
*
*   SYNOPSIS
*       int tui_cols (void);
*
*   FUNCTION
*       Read the width reported by curses.
*
*   RESULT
*       Current value of COLS.
*
****************************************************************************
*
*/
int tui_cols (void)
{
    return COLS;
}

/****i* januae-svgancui/tui_shutdown *************************************
*
*   NAME
*       tui_shutdown() -- End the curses session.
*
*   SYNOPSIS
*       void tui_shutdown (void);
*
*   FUNCTION
*       Call endwin() to terminate curses display mode.
*
****************************************************************************
*
*/
void tui_shutdown (void)
{
    endwin ();
}

/****i* januae-svgancui/tui_refresh **************************************
*
*   NAME
*       tui_refresh() -- Refresh open text windows.
*
*   SYNOPSIS
*       void tui_refresh (void);
*
*   FUNCTION
*       Mark each active window as changed, stage its contents and update
*       the terminal.
*
*   NOTES
*       Iterates from window zero to the current stack limit.
*
****************************************************************************
*
*/
void tui_refresh (void)
{
    int w;
    for (w = 0; w < winnr; w++) {
	touchwin (winstack[w]);
	wnoutrefresh (winstack[w]);
    }
    doupdate ();
}

/****i* januae-svgancui/tui_puts *****************************************
*
*   NAME
*       tui_puts() -- Write text in the selected window.
*
*   SYNOPSIS
*       void tui_puts (const char *s);
*
*   FUNCTION
*       Pass a NUL-terminated string to waddstr() for the current curses
*       window.
*
*   INPUTS
*       s - Text to write.
*
*   NOTES
*       Curses write errors are not reported.
*
****************************************************************************
*
*/
void tui_puts (const char *s)
{
    waddstr (currwin, s);
}

/****i* januae-svgancui/tui_cursoff **************************************
*
*   NAME
*       tui_cursoff() -- Disable the text cursor.
*
*   SYNOPSIS
*       void tui_cursoff(void);
*
*   FUNCTION
*       This function is a no-op in the curses frontend.
*
*   NOTES
*       Cursor visibility is not changed.
*
****************************************************************************
*
*/
void tui_cursoff(void)
{
}

/****i* januae-svgancui/tui_curson ***************************************
*
*   NAME
*       tui_curson() -- Enable the text cursor.
*
*   SYNOPSIS
*       void tui_curson (void);
*
*   FUNCTION
*       This function is a no-op in the curses frontend.
*
*   NOTES
*       Cursor visibility is not changed.
*
****************************************************************************
*
*/
void tui_curson (void)
{
}

/****i* januae-svgancui/tui_putc *****************************************
*
*   NAME
*       tui_putc() -- Write a character in the selected window.
*
*   SYNOPSIS
*       void tui_putc(char c);
*
*   FUNCTION
*       Pass one character to waddch() for the active curses window.
*
*   INPUTS
*       c - Character to write.
*
****************************************************************************
*
*/
void tui_putc(char c)
{
    waddch (currwin, c);
}

/****i* januae-svgancui/tui_cr *******************************************
*
*   NAME
*       tui_cr() -- Write a carriage return.
*
*   SYNOPSIS
*       void tui_cr (void);
*
*   FUNCTION
*       Pass a carriage-return character to waddch() for the current
*       window.
*
****************************************************************************
*
*/
void tui_cr (void)
{
    waddch (currwin, '\r');
}

/****i* januae-svgancui/tui_getc *****************************************
*
*   NAME
*       tui_getc() -- Read a terminal key.
*
*   SYNOPSIS
*       char tui_getc(void);
*
*   FUNCTION
*       Fetch one key with curses getch().
*
*   RESULT
*       The getch() result converted to char.
*
*   NOTES
*       Special curses key codes and ERR may be truncated by the char
*       return type.
*
****************************************************************************
*
*/
char tui_getc(void)
{
    return getch ();
}

/****i* januae-svgancui/tui_gotoxy ***************************************
*
*   NAME
*       tui_gotoxy() -- Set the current window cursor position.
*
*   SYNOPSIS
*       void tui_gotoxy (int x, int y);
*
*   FUNCTION
*       Position the cursor using one-based coordinates exposed by this
*       interface.
*
*   INPUTS
*       x - One-based column.
*       y - One-based row.
*
*   NOTES
*       Subtracts one from each coordinate before calling wmove().
*
****************************************************************************
*
*/
void tui_gotoxy (int x, int y)
{
    x--; y--;
    wmove (currwin, y, x);
}

/****i* januae-svgancui/tui_selwin ***************************************
*
*   NAME
*       tui_selwin() -- Select a text window.
*
*   SYNOPSIS
*       void tui_selwin (int w);
*
*   FUNCTION
*       Set the current output window to the indicated window-stack entry.
*
*   INPUTS
*       w - Window-stack index.
*
*   NOTES
*       Does not validate the index or check whether the entry is still
*       allocated.
*
****************************************************************************
*
*/
void tui_selwin (int w)
{
    currwin = winstack[w];
}

/****i* januae-svgancui/tui_clrwin ***************************************
*
*   NAME
*       tui_clrwin() -- Erase a text window.
*
*   SYNOPSIS
*       void tui_clrwin (int w);
*
*   FUNCTION
*       Clear the contents of a window-stack entry with werase().
*
*   INPUTS
*       w - Window-stack index.
*
****************************************************************************
*
*/
void tui_clrwin (int w)
{
    werase (winstack[w]);
}

/****i* januae-svgancui/tui_drawbox **************************************
*
*   NAME
*       tui_drawbox() -- Draw a window border.
*
*   SYNOPSIS
*       void tui_drawbox(int w);
*
*   FUNCTION
*       Draw the default curses border around a window-stack entry.
*
*   INPUTS
*       w - Window-stack index.
*
****************************************************************************
*
*/
void tui_drawbox(int w)
{
    wborder (winstack[w], 0, 0, 0, 0, 0, 0, 0, 0);
}

/****i* januae-svgancui/tui_hline ****************************************
*
*   NAME
*       tui_hline() -- Draw a horizontal line.
*
*   SYNOPSIS
*       void tui_hline (int x1, int y1, int x2);
*
*   FUNCTION
*       Draw a line in the current window from x1 through x2 at row y1.
*
*   INPUTS
*       x1 - One-based start column.
*       y1 - One-based row.
*       x2 - One-based final column.
*
*   NOTES
*       The drawn length is x2 - x1 + 1; coordinates are converted to the
*       curses origin.
*
****************************************************************************
*
*/
void tui_hline (int x1, int y1, int x2)
{
    wmove (currwin, y1-1, x1-1);
    whline (currwin, 0, x2-x1+1);
}

/****i* januae-svgancui/tui_dlog *****************************************
*
*   NAME
*       tui_dlog() -- Create a dialog window.
*
*   SYNOPSIS
*       int tui_dlog(int x1, int y1, int x2, int y2);
*
*   FUNCTION
*       Allocate a curses window at the supplied one-based rectangle and
*       append it to the window stack.
*
*   INPUTS
*       x1 - Left column.
*       y1 - Top row.
*       x2 - Right edge.
*       y2 - Bottom edge.
*
*   RESULT
*       Index of the newly allocated window.
*
*   NOTES
*       The fixed window stack has ten entries and this function does not
*       check capacity or newwin() failure. Close the dialog with
*       tui_dlogdie().
*
****************************************************************************
*
*/
int tui_dlog(int x1, int y1, int x2, int y2)
{
    x1--; y1--;
    winstack[winnr] = newwin (y2 - y1, x2 - x1, y1, x1);
    return winnr++;
}

/****i* januae-svgancui/tui_dlogdie **************************************
*
*   NAME
*       tui_dlogdie() -- Close a dialog window.
*
*   SYNOPSIS
*       void tui_dlogdie (int w);
*
*   FUNCTION
*       Delete the indicated window, shrink unused trailing stack entries
*       and redraw surviving windows.
*
*   INPUTS
*       w - Index of the dialog to close.
*
*   NOTES
*       If the closed window is current, selection reverts to stdscr. Only
*       allocated dialog indices are valid.
*
****************************************************************************
*
*/
void tui_dlogdie (int w)
{
    if (currwin == winstack[w])
	currwin = stdscr;
    delwin (winstack[w]);
    winstack[w] = NULL;
    while (winstack[winnr-1] == NULL)
	winnr--;

    for (w = 0; w < winnr; w++)
	redrawwin (winstack[w]), wrefresh (winstack[w]);
}

/****i* januae-svgancui/tui_gets *****************************************
*
*   NAME
*       tui_gets() -- Read a line in a curses window.
*
*   SYNOPSIS
*       int tui_gets (char *buf, int x, int y, int n);
*
*   FUNCTION
*       Edit a NUL-terminated input buffer in the selected window until
*       Return or Escape.
*
*   INPUTS
*       buf - Writable buffer containing at least n bytes.
*       x - Zero-based input column.
*       y - Zero-based input row.
*       n - Available input buffer length.
*
*   RESULT
*       1 for carriage return; 0 for Escape.
*
*   NOTES
*       Keeps at most n - 1 input characters and NUL-terminates the result.
*       Requires n greater than zero; the editor does not handle ordinary
*       terminal errors.
*
****************************************************************************
*
*/
int tui_gets (char *buf, int x, int y, int n)
{
    int i = 0;
    for (;;) {
	int c, j;

	buf[i] = 0;
	wmove (currwin, y, x);
	for (j = 0; j < i; j++)
	    waddch (currwin, buf[j]);
	for (; j < n; j++)
	    waddch (currwin, ' ');

	wmove (currwin, y, x + i);
	wrefresh (currwin);

	c = getch ();

	wmove (currwin, y, x + i);
	if (c == 13)
	    return 1;
	else if (c == 27)
	    return 0;
	else if (i > 0 && c == KEY_BACKSPACE)
	    i--;
	else if (i + 1 < n && !iscntrl (c))
	    buf[i++] = c;
    }
}

/****i* januae-svgancui/tui_wgets ****************************************
*
*   NAME
*       tui_wgets() -- Prompt for a line of input.
*
*   SYNOPSIS
*       int tui_wgets (char *buf, const char *title, int n);
*
*   FUNCTION
*       Create a centered dialog containing a title and use tui_gets() to
*       edit the supplied buffer.
*
*   INPUTS
*       buf - Writable buffer holding at least n bytes.
*       title - NUL-terminated dialog title.
*       n - Input buffer length.
*
*   RESULT
*       1 for Return; 0 for Escape, as returned by tui_gets().
*
*   NOTES
*       The dialog is closed before returning. Callers must check the
*       result if cancellation matters.
*
****************************************************************************
*
*/
int tui_wgets (char *buf, const char *title, int n)
{
    int l = strlen (title);
    int ww = (l > n ? l : n) + 2;
    int w = tui_dlog((tui_cols ()-ww)/2, tui_lines ()/2-1, (tui_cols ()+ww)/2, tui_lines ()/2+1);
    int result;

    tui_selwin (w); tui_drawbox(w);
    wmove (currwin, 0, (ww-l)/2);
    waddstr (currwin, title);
    result = tui_gets (buf, 1, 1, n);
    tui_dlogdie (w);
    return result;
}

/****i* januae-svgancui/tui_menubrowse ***********************************
*
*   NAME
*       tui_menubrowse() -- Browse a selectable text menu.
*
*   SYNOPSIS
*       int tui_menubrowse (struct bstring *menu, int xoff, int yoff, int selected, int height);
*
*   FUNCTION
*       Display entries described by bstring markers, handle navigation and
*       shortcut keys, and close the dialog after selection.
*
*   INPUTS
*       menu - Array terminated by a bstring entry with val -3.
*       xoff - Horizontal placement; nonpositive values are relative to the
*       right edge.
*       yoff - Vertical placement; nonpositive values are relative to the
*       bottom edge.
*       selected - Initial selectable item index.
*       height - Maximum visible entry count.
*
*   RESULT
*       Selected menu item index, or -1 when Escape is pressed.
*
*   NOTES
*       Menu val markers define titles, disabled entries and shortcuts as
*       specified in tui.h. Invalid menu indices and zero selectable
*       entries are not guarded.
*
****************************************************************************
*
*/
int tui_menubrowse (struct bstring *menu, int xoff, int yoff, int selected, int height)
{
    int count = 0, maxsel = 0, maxw = 0;
    int i, j, w, s, yp, oldyp;
    chtype moresave[6][2];
    int xpos, ypos;

    const char *mtitle = NULL;

    for (i = 0; menu[i].val != -3; i++) {
	int tmp;
	if (menu[i].val == -4) {
	    if (maxsel < selected)
		selected--;
	    continue;
	}
	if (menu[i].val != 0) {
	    count++;
	    if (menu[i].val != -2)
		maxsel++;
	} else
	    mtitle = menu[i].data;
	if ((tmp = strlen (menu[i].data)) > maxw)
	    maxw = tmp;
    }
    if (height > count)
	height = count;
    maxw += 3;
    if (strlen (mtitle ? mtitle : "") + 8 > maxw)
	maxw = strlen (mtitle ? mtitle : "") + 8;
    if (xoff > 0)
	xpos = xoff;
    else
	xpos = tui_cols () + xoff - maxw - 1;
    if (yoff > 0)
	ypos = yoff;
    else
	ypos = tui_lines () + yoff - height - 2;
    w = tui_dlog(xpos, ypos, xpos+maxw, ypos+height+1);
    tui_selwin (w);
    tui_drawbox(w);
    if (mtitle != NULL) {
	mvwaddstr (currwin, 0, 1, mtitle);
    }
    for (i = 0; i < 6; i++) {
	moresave[i][0] = mvwinch (currwin, 0, maxw-6+i);
	moresave[i][1] = mvwinch (currwin, height+1, maxw-6+i);
    }
    s = yp = 0; oldyp = -1;
    for (;;) {
	int c;
	int s2;

	while (selected < yp)
	    yp--;
	while (selected >= yp + height)
	    yp++;
	if (yp == 0)
	    for (i = 0; i < 6; i++)
		mvwaddch (currwin, 0, maxw-6+i, moresave[i][0]);
	else
	    mvwaddstr (currwin, 0, maxw-6, "(more)");
	if (yp + height == count)
	    for (i = 0; i < 6; i++)
		mvwaddch (currwin, height+1, maxw-6+i, moresave[i][1]);
	else
	    mvwaddstr (currwin, height+1, maxw-6, "(more)");
	for (i = s2 = j = 0; i < count; i++, j++) {
	    int k, x;
	    attr_t a = s2 == selected ? A_STANDOUT : 0;
	    while (menu[j].val == 0 || menu[j].val == -4)
		j++;
	    if (i >= yp && i < yp+height) {
		mvwaddch (currwin, 1+i-yp, 1, ' ' | a);
		for (k = x = 0; menu[j].data[k]; k++, x++) {
		    int a2 = 0;
		    c = menu[j].data[k];
		    if (c == '_')
			c = menu[j].data[++k], a2 = A_UNDERLINE;
		    mvwaddch (currwin, 1+i-yp, 2+x, c | a | a2);
		}
		for (; x < maxw-2; x++) {
		    mvwaddch (currwin, 1+i-yp, 2+x, ' ' | a);
		}
	    }
	    if (menu[j].val != -2)
		s2++;
	}

	tui_refresh ();
	c = getch ();
	if (c == 27) {
	    tui_dlogdie (w);
	    return -1;
	} else if (c == KEY_ENTER || c == 13 || c == ' ') {
	    tui_dlogdie (w);
	    for (i = s2 = j = 0; s2 <= selected; j++) {
		if (menu[j].val == -4) {
		    i++; j++; continue;
		}
		while (menu[j].val == 0)
		    j++;
		if (s2 == selected)
		    return i;
		if (menu[j].val != -2)
		    s2++, i++;
	    }
	    abort();
	} else switch (c) {
	 case KEY_UP:
	    if (selected > 0)
		selected--;
	    break;
	 case KEY_DOWN:
	    if (selected + 1 < count)
		selected++;
	    break;
	 case KEY_PPAGE:
	    if (selected > height)
		selected -= height;
	    else
		selected = 0;
	    break;
	 case KEY_NPAGE:
	    if (selected + height < count)
		selected += height;
	    else
		selected = count-1;
	    break;
	 default:
	    for (j = i = 0; menu[i].val != -3; i++)
		if (menu[i].val == toupper (c)) {
		    tui_dlogdie (w);
		    return j;
		} else if (menu[i].val == -1 || menu[i].val == -4 || menu[i].val > 0) {
		    j++;
		}

	    break;
	}
    }
    return -1; /* Can't get here */
}

/****i* januae-svgancui/tui_errorbox *************************************
*
*   NAME
*       tui_errorbox() -- Show a blocking error dialog.
*
*   SYNOPSIS
*       void tui_errorbox(const char *err);
*
*   FUNCTION
*       Display the error message and wait for a carriage-return key before
*       closing the dialog.
*
*   INPUTS
*       err - NUL-terminated error text.
*
*   NOTES
*       Despite the displayed prompt, keys other than carriage return do
*       not dismiss the dialog.
*
****************************************************************************
*
*/
void tui_errorbox(const char *err)
{
    const char *hak = "Hit any key";
    int n = strlen (hak);
    int l = strlen (err);
    int ww = (l > n ? l : n) + 2;
    int w = tui_dlog((tui_cols ()-ww)/2, tui_lines ()/2-1, (tui_cols ()+ww)/2, tui_lines ()/2+1);
    tui_selwin (w); tui_drawbox(w);

    wmove (currwin, 0, (ww-6)/2);
    waddstr (currwin, "Error!");
    wmove (currwin, 1, (ww-l)/2);
    waddstr (currwin, err);
    wmove (currwin, 2, (ww-n)/2);
    waddstr (currwin, hak);

    wrefresh (currwin);
    for (;;) {
	int c = getch ();
	if (c == 13)
	    break;
    }
    tui_dlogdie (w);
}

static char *pattern;
static int maxlen;

/****i* januae-svgancui/put_filename *************************************
*
*   NAME
*       put_filename() -- Draw one entry in the file selector.
*
*   SYNOPSIS
*       static void put_filename (char *s, int x, int y, attr_t a);
*
*   FUNCTION
*       Render the selected filename, or "(none)" for the dot entry, padded
*       to the current selector width.
*
*   INPUTS
*       s - NUL-terminated filename.
*       x - One-based column.
*       y - One-based row.
*       a - Curses attribute mask.
*
*   NOTES
*       Uses shared maxlen and a fixed 256-byte local buffer; overlong
*       names are not checked before copying.
*
****************************************************************************
*
*/
static void put_filename (char *s, int x, int y, attr_t a)
{
    char buf[256];
    int i;

    tui_gotoxy (x,y);
    if (strcmp (s, ".") == 0)
	strcpy (buf, "(none)");
    else
	strcpy (buf, s);
    buf[maxlen] = 0;
    for (i = 0; i < strlen (buf); i++)
	waddch (currwin, buf[i] | a);
    for (; i < maxlen; i++)
	waddch (currwin, ' ' | a);
}

static char fsbuf[256];

/****i* januae-svgancui/selectfn *****************************************
*
*   NAME
*       selectfn() -- Filter directory entries for the file selector.
*
*   SYNOPSIS
*       static int selectfn (const struct dirent *de);
*
*   FUNCTION
*       Exclude hidden names other than dot and dot-dot, and entries too
*       long for the terminal; track the longest accepted name.
*
*   INPUTS
*       de - Directory entry supplied by scandir().
*
*   RESULT
*       1 to include the entry; 0 to reject it.
*
*   NOTES
*       The stored wildcard pattern is not evaluated.
*
****************************************************************************
*
*/
static int selectfn (const struct dirent *de)
{
    int l1, l2;

/*    l1 = strlen (pattern + 1);*/
    l2 = strlen (de->d_name);

    if (l2 >= tui_cols ()-10) /* Restrict length of filenames so we won't mess up the display */
	return 0;

    /* No pattern matching for now. But we don't show hidden files. */
    if (strcmp (de->d_name, ".") != 0 && strcmp (de->d_name, "..") != 0
	&& de->d_name[0] == '.')
	return 0;
    if (l2 > maxlen)
	maxlen = l2;
    return 1;
}

/****i* januae-svgancui/my_alphasort *************************************
*
*   NAME
*       my_alphasort() -- Compare directory entry names.
*
*   SYNOPSIS
*       static int my_alphasort (const void *a, const void *b);
*
*   FUNCTION
*       Compare scandir() entry names using strcmp().
*
*   INPUTS
*       a - Pointer to the first struct dirent * entry.
*       b - Pointer to the second struct dirent * entry.
*
*   RESULT
*       strcmp() ordering value.
*
****************************************************************************
*
*/
static int my_alphasort (const void *a, const void *b)
{
    return strcmp ((*(struct dirent **) a)->d_name,
		   (*(struct dirent **) b)->d_name);
}

/****i* januae-svgancui/tui_filereq **************************************
*
*   NAME
*       tui_filereq() -- Select a path using the text file requester.
*
*   SYNOPSIS
*       char *tui_filereq(char *s, char *oldfile, const char *title);
*
*   FUNCTION
*       Browse directory entries with curses, descend into directories and
*       return an absolute path on file selection.
*
*   INPUTS
*       s - Wildcard text; accepted for logging but not used to filter
*       files.
*       oldfile - Initial path, copied into a shared 256-byte buffer.
*       title - Optional dialog title, or NULL.
*
*   RESULT
*       Pointer to the shared static fsbuf on selection, or NULL on
*       cancellation or certain failures.
*
*   NOTES
*       The selected dot entry yields an empty string. Most exits restore
*       the original working directory, but scandir() returning no entries
*       exits without restoring it. Returned storage is overwritten by
*       later requests; input length is not checked before copying.
*
****************************************************************************
*
*/
char *tui_filereq(char *s, char *oldfile, const char *title)
{
    char cwd[256];
    char *retval = fsbuf;
    char *tmp;
    int fin = 0;
    chtype moresave[6][2];

    /* Save wd */
    if (getcwd (cwd, 256) == NULL)
	return NULL;

    /* Change into directory of old file */
    strcpy (fsbuf, oldfile);
    tmp = strrchr (fsbuf, '/');
    if (tmp != NULL) {
	*tmp = 0;
	if (strlen (fsbuf) > 0)
	    chdir (fsbuf);
    }

    pattern = s;
    if (s[0] != '*')
	write_log ("Can't handle wildcard %s\n", s);
    if (s[1] != 0 && strchr (s+1, '*') != NULL)
	write_log ("Can't handle wildcard %s\n", s);
    for (;!fin;) {
	struct dirent **names;
	int i, w, n, l, yp, oldyp, s;

	maxlen = 0;
	n = scandir (".", &names, selectfn, my_alphasort);

	if (n <= 0)
	    return NULL;
	if (title != NULL && strlen (title) + 6 > maxlen)
	    maxlen = strlen (title) + 6;
	l = n;
	if (l > 15)
	    l = 15;
	yp = s = 0; oldyp = -1;
	w = tui_dlog (tui_cols () - maxlen - 8, 5, tui_cols () - 5, 5 + l + 1);
	tui_selwin (w); tui_drawbox (w);
	if (title)
	    mvwaddstr (currwin, 0, 2, title);
	for (i = 0; i < 6; i++) {
	    moresave[i][0] = mvwinch (currwin, 0, maxlen-3+i);
	    moresave[i][1] = mvwinch (currwin, l+1, maxlen-3+i);
	}
	for (;;) {
	    int c;
	    char tmp[256];
	    while (s < yp)
		yp--;
	    while (s >= yp + l)
		yp++;
	    if (oldyp != yp) {
		oldyp = yp;
		for (i = 0; i < l; i++) {
		    put_filename (names[i + yp]->d_name, 3, 2 + i, 0);
		}
	    }
	    put_filename (names[s]->d_name, 3, 2 + s - yp, A_STANDOUT);

	    if (yp == 0)
		for (i = 0; i < 6; i++)
		    mvwaddch (currwin, 0, maxlen-3+i, moresave[i][0]);
	    else
		mvwaddstr (currwin, 0, maxlen-3, "(more)");
	    if (yp + l == n)
		for (i = 0; i < 6; i++)
		    mvwaddch (currwin, l+1, maxlen-3+i, moresave[i][1]);
	    else
		mvwaddstr (currwin, l+1, maxlen-3, "(more)");

	    tui_refresh ();
	    c = getch ();
	    put_filename (names[s]->d_name, 3, 2 + s - yp, 0);
	    if (c == 27) {
		retval = NULL; fin = 1;
		break;
	    } else if (c == KEY_ENTER || c == 13 || c == ' ') {
		int err;

		if (strcmp (names[s]->d_name, ".") == 0) {
		    fin = 1;
		    strcpy (fsbuf, "");
		    break;
		}
		err = chdir (names[s]->d_name);

		if (err == 0)
		    break;
		else if (errno == ENOTDIR) {
		    fin = 1;
		    if (getcwd (fsbuf, 256) == NULL)
			retval = NULL;
		    if (strlen (fsbuf) + strlen (names[s]->d_name) + 2 >= 256)
			retval = NULL;
		    else {
			strcat(fsbuf, "/");
			strcat(fsbuf, names[s]->d_name);
		    }
		    break;
		} /* else what? */
	    }
	    switch (c) {
	     case KEY_UP:
		if (s > 0)
		    s--;
		break;
	     case KEY_DOWN:
		if (s + 1 < n)
		    s++;
		break;
	     case KEY_PPAGE:
		if (s > l)
		    s -= l;
		else
		    s = 0;
		break;
	     case KEY_NPAGE:
		if (s + l < n)
		    s += l;
		else
		    s = n - 1;
		break;
	     default:
		i = 0;
		if (names[s]->d_name[0] == c)
		    i = s+1;
		for (; i < n*2; i++) {
		    int j = i;
		    if (i >= n)
			j -= n;
		    if (names[j]->d_name[0] == c) {
			s = j;
			break;
		    }
		}
	    }
	}
#if 0
	/* @@@ is this right? */
	for (i = 0; i < n; i++)
	    free (names[i]);
	free (names);
#endif
	tui_dlogdie (w);
    }
    chdir (cwd);
    return retval;
}

/****i* januae-svgancui/tui_backup_optionsfile ***************************
*
*   NAME
*       tui_backup_optionsfile() -- Back up the options file.
*
*   SYNOPSIS
*       int tui_backup_optionsfile (void);
*
*   FUNCTION
*       Rename the file named by optionsfile to the same name with a
*       trailing tilde.
*
*   RESULT
*       Return value of rename(): zero on success, -1 on failure.
*
*   NOTES
*       Uses a fixed 257-byte buffer and does not check the source path
*       length before appending the suffix.
*
****************************************************************************
*
*/
int tui_backup_optionsfile (void)
{
    char tmp[257];
    strcpy (tmp, optionsfile);
    strcat (tmp, "~");
    return rename (optionsfile, tmp);
}
