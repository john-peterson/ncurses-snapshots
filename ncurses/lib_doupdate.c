
/***************************************************************************
*                            COPYRIGHT NOTICE                              *
****************************************************************************
*                ncurses is copyright (C) 1992-1995                        *
*                          Zeyd M. Ben-Halim                               *
*                          zmbenhal@netcom.com                             *
*                          Eric S. Raymond                                 *
*                          esr@snark.thyrsus.com                           *
*                                                                          *
*        Permission is hereby granted to reproduce and distribute ncurses  *
*        by any means and for any fee, whether alone or as part of a       *
*        larger distribution, in source or in binary form, PROVIDED        *
*        this notice is included with any such distribution, and is not    *
*        removed from any of its header files. Mention of ncurses in any   *
*        applications linked with it is highly appreciated.                *
*                                                                          *
*        ncurses comes AS IS with no warranty, implied or expressed.       *
*                                                                          *
***************************************************************************/


/*-----------------------------------------------------------------
 *
 *	lib_doupdate.c
 *
 *	The routine doupdate() and its dependents.  Also _nc_outstr(),
 *	so all physcal output is concentrated here.
 *
 *-----------------------------------------------------------------*/

#include <curses.priv.h>

#include <sys/types.h>
#if HAVE_SYS_TIME_H && ! SYSTEM_LOOKS_LIKE_SCO
#include <sys/time.h>
#endif
#if HAVE_SYS_SELECT_H
#include <sys/types.h>
#include <sys/select.h>
#endif
#include <string.h>
#include "term.h"

/*
 * This define controls the line-breakout optimization.  Every once in a
 * while during screen refresh, we want to check for input and abort the
 * update if there's some waiting.  CHECK_INTERVAL controls the number of
 * changed lines to be emitted between input checks.
 *
 * Note: Input-check-and-abort is no longer done if the screen is being
 * updated from scratch.  This is a feature, not a bug.
 */
#define CHECK_INTERVAL	6

/*
 * Enable checking to see if doupdate and friends are tracking the true
 * cursor position correctly.  NOTE: this is a debugging hack which will
 * work ONLY on ANSI-compatible terminals!
 */
/* #define POSITION_DEBUG */

static inline chtype ClrBlank ( WINDOW *win );
static inline chtype ClrSetup ( WINDOW *scr );
static int ClrBottom(int total);
static int InsStr( chtype *line, int count );
static void ClearScreen( void );
static void ClrUpdate( WINDOW *scr );
static void DelChar( int count );
static void IDcTransformLine( int const lineno );
static void NoIDcTransformLine( int const lineno );
static void TransformLine( int const lineno );

#define DelCharCost(count) \
		((parm_dch != 0) \
		? SP->_dch_cost \
		: ((delete_character != 0) \
			? (SP->_dch1_cost * count) \
			: INFINITY))

#define InsCharCost(count) \
		((parm_ich != 0) \
		? SP->_ich_cost \
		: ((insert_character != 0) \
			? (SP->_ich1_cost * count) \
			: INFINITY))

#define UpdateAttrs(c)	if (SP->_current_attr != AttrOf(c)) \
				vidattr(AttrOf(c));

#ifdef POSITION_DEBUG
/****************************************************************************
 *
 * Debugging code.  Only works on ANSI-standard terminals.
 *
 ****************************************************************************/

void position_check(int expected_y, int expected_x, char *legend)
/* check to see if the real cursor position matches the virtual */
{
    static char  buf[9];
    int y, x;

    if (_nc_tracing)
	return;

    memset(buf, '\0', sizeof(buf));
    (void) write(1, "\033[6n", 4);	/* only works on ANSI-compatibles */
    (void) read(0, (void *)buf, 8);
    _tracef("probe returned %s", _nc_visbuf(buf));

    /* try to interpret as a position report */
    if (sscanf(buf, "\033[%d;%dR", &y, &x) != 2)
	_tracef("position probe failed in %s", legend);
    else if (y - 1 != expected_y || x - 1 != expected_x)
	_tracef("position seen (%d, %d) doesn't match expected one (%d, %d) in %s",
		y-1, x-1, expected_y, expected_x, legend);
    else
	_tracef("position matches OK in %s", legend);
}
#endif /* POSITION_DEBUG */

/****************************************************************************
 *
 * Optimized update code
 *
 ****************************************************************************/

static inline void GoTo(int const row, int const col)
{
	chtype	oldattr = SP->_current_attr;

	TR(TRACE_MOVE, ("GoTo(%d, %d) from (%d, %d)",
			row, col, SP->_cursrow, SP->_curscol));

#ifdef POSITION_DEBUG
	position_check(SP->_cursrow, SP->_curscol, "GoTo");
#endif /* POSITION_DEBUG */

	/*
	 * Force restore even if msgr is on when we're in an alternate
	 * character set -- these have a strong tendency to screw up the
	 * CR & LF used for local character motions!
	 */
	if ((oldattr & A_ALTCHARSET)
	    || (oldattr && !move_standout_mode))
	{
       		TR(TRACE_CHARPUT, ("turning off (%lx) %s before move",
		   oldattr, _traceattr(oldattr)));
		vidattr(A_NORMAL);
	}

	mvcur(SP->_cursrow, SP->_curscol, row, col);
	SP->_cursrow = row;
	SP->_curscol = col;
}

static inline void PutAttrChar(chtype ch)
{
	if (tilde_glitch && (TextOf(ch) == '~'))
		ch = ('`' | AttrOf(ch));

	TR(TRACE_CHARPUT, ("PutAttrChar(%s) at (%d, %d)",
			  _tracechtype(ch),
			   SP->_cursrow, SP->_curscol));
	UpdateAttrs(ch);
	putc((int)TextOf(ch), SP->_ofp);
	SP->_curscol++;
	if (char_padding) {
		TPUTS_TRACE("char_padding");
		putp(char_padding);
	}
}

static bool check_pending(void)
/* check for pending input */
{
	if (SP->_checkfd >= 0) {
	fd_set fdset;
	struct timeval ktimeout;

		ktimeout.tv_sec =
		ktimeout.tv_usec = 0;

		FD_ZERO(&fdset);
		FD_SET(SP->_checkfd, &fdset);
		if (select(SP->_checkfd+1, &fdset, NULL, NULL, &ktimeout) != 0)
		{
			fflush(SP->_ofp);
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * No one supports recursive inline functions.  However, gcc is quieter if we
 * instantiate the recursive part separately.
 */
#if CC_HAS_INLINE_FUNCS
static void callPutChar(chtype const);
#else
#define callPutChar(ch) PutChar(ch)
#endif

static inline void PutChar(chtype const ch)
/* insert character, handling automargin stuff */
{
    if (!(SP->_cursrow == screen_lines-1 && SP->_curscol == screen_columns-1
		&& auto_right_margin && !eat_newline_glitch))
    {
	PutAttrChar(ch);	/* normal case */
    }
    else if (!auto_right_margin 	/* maybe we can suppress automargin */
	     || (enter_am_mode && exit_am_mode))
    {
	bool old_am = auto_right_margin;

	if (old_am)
	{
	    TPUTS_TRACE("exit_am_mode");
	    putp(exit_am_mode);
	}
	PutAttrChar(ch);
	if (old_am)
	{
	    TPUTS_TRACE("enter_am_mode");
	    putp(enter_am_mode);
	}
    }
    else
    {
	GoTo(screen_lines-1,screen_columns-2);
	callPutChar(ch);
	GoTo(screen_lines-1,screen_columns-2);
	if (InsStr(newscr->_line[screen_lines-1].text+screen_columns-2,1)==ERR)
	    return;
    }

    if (SP->_curscol >= screen_columns)
    {
	if (eat_newline_glitch)
	{
	    /*
	     * xenl can manifest two different ways.  The vt100
	     * way is that, when you'd expect the cursor to wrap,
	     * it stays hung at the right margin (on top of the
	     * character just emitted) and doesn't wrap until the
	     * *next* graphic char is emitted.  The c100 way is
	     * to ignore LF received just after an am wrap.
	     *
	     * An aggressive way to handle this would be to
	     * emit CR/LF after the char and then assume the wrap
	     * is done, you're on the first position of the next
	     * line, and the terminal out of its weird state.
	     * Here it's safe to just tell the code that the
	     * cursor is in hyperspace and let the next mvcur()
	     * call straighten things out.
	     */
	    SP->_curscol = -1;
	    SP->_cursrow = -1;
	}
	else if (auto_right_margin)
	{
	    SP->_curscol = 0;
	    SP->_cursrow++;
	}
	else
	{
	    SP->_curscol--;
	}
    }
#ifdef POSITION_DEBUG
    position_check(SP->_cursrow, SP->_curscol, "PutChar");
#endif /* POSITION_DEBUG */
}

/*
 * Issue a given span of characters from an array.  Must be
 * functionally equivalent to:
 *	for (i = 0; i < num; i++)
 *	    PutChar(ntext[i]);
 * Soon we'll optimize using ech and rep.
 */
static inline void EmitRange(const chtype *ntext, int num)
{
    int	i;

#ifdef __UNFINISHED__
    if (erase_chars || repeat_chars)
    {
	bool wrap_possible = (SP->_curscol + num >= screen_columns);
	chtype lastchar;

	if (wrap_possible)
	    lastchar = ntext[num--];

	while (num)
	{
	    int	runcount = 1;

	    PutChar(ntext[0]);

	    while (runcount < num && ntext[runcount] = ntext[0])
		runcount++;

	    /* more */

	}

	if (wrap_possible)
	    PutChar(lastchar);

	return;
    }
#endif

    /* code actually used */
    for (i = 0; i < num; i++)
	PutChar(ntext[i]);
}

/*
 * Output the line in the given range [first .. last]
 *
 * If there's a run of identical characters that's long enough to justify
 * cursor movement, use that also.
 */
static void PutRange(
	const chtype *otext,
	const chtype *ntext,
	int row,
	int first, int last)
{
	int j, run;
	int cost = min(SP->_cup_cost, SP->_hpa_cost);

	TR(TRACE_CHARPUT, ("PutRange(%p, %p, %d, %d, %d)",
			 otext, ntext, row, first, last));

	if (otext != ntext
	 && (last-first+1) > cost) {
		for (j = first, run = 0; j <= last; j++) {
			if (otext[j] == ntext[j]) {
				run++;
			} else {
				if (run > cost) {
					int before_run = (j - run);
					EmitRange(ntext+first, before_run-first);
					GoTo(row, first = j);
				}
				run = 0;
			}
		}
	}
	EmitRange(ntext + first, last-first+1);
}

#if CC_HAS_INLINE_FUNCS
static void callPutChar(chtype const ch)
{
	PutChar(ch);
}
#endif

#define MARK_NOCHANGE(win,row) \
	{ \
		win->_line[row].firstchar = _NOCHANGE; \
		win->_line[row].lastchar = _NOCHANGE; \
		win->_line[row].oldindex = row; \
	}

int doupdate(void)
{
int	i;

	T(("doupdate() called"));

#ifdef TRACE
	if (_nc_tracing & TRACE_UPDATE)
	{
	    if (curscr->_clear)
		_tracef("curscr is clear");
	    else
		_tracedump("curscr", curscr);
	    _tracedump("newscr", newscr);
	}
#endif /* TRACE */

	_nc_signal_handler(FALSE);

	if (SP->_endwin == TRUE) {
		T(("coming back from shell mode"));
		reset_prog_mode();
		if (enter_ca_mode)
		{
			TPUTS_TRACE("enter_ca_mode");
			putp(enter_ca_mode);
		}
		/*
		 * Undo the effects of terminal init strings that assume
		 * they know the screen size.  Useful when you're running
		 * a vt100 emulation through xterm.  Note: this may change
		 * the physical cursor location.
		 */
		if (change_scroll_region)
		{
			TPUTS_TRACE("change_scroll_region");
			putp(tparm(change_scroll_region, 0, screen_lines - 1));
		}
		_nc_mouse_resume(SP);
		newscr->_clear = TRUE;
		SP->_endwin = FALSE;
	}

	/*
	 * FIXME: Full support for magic-cookie terminals could go in here.
	 * The theory: we scan the virtual screen looking for attribute
	 * changes.  Where we find one, check to make sure it's realizable
	 * by seeing if the required number of un-attributed blanks are
	 * present before or after the change.  If not, nuke the attributes
	 * out of the following or preceding cells on the virtual screen,
	 * forward to the next change or backwards to the previous one.  If
	 * so, displace the change by the required number of characters.
	 */

	if (curscr->_clear) {		/* force refresh ? */
		T(("clearing and updating curscr"));
		ClrUpdate(curscr);		/* yes, clear all & update */
		curscr->_clear = FALSE;	/* reset flag */
	} else {
		if (newscr->_clear) {
			T(("clearing and updating newscr"));
			ClrUpdate(newscr);
			newscr->_clear = FALSE;
		} else {
			int changedlines;
			int total = min(screen_lines, newscr->_maxy+1);

#if 0			/* 960615 - still slower */
			_nc_hash_map();
#endif
		        _nc_scroll_optimize();

			if (clr_eos)
				total = ClrBottom(total);

			T(("Transforming lines"));
			for (i = changedlines = 0; i < total; i++) {
				/*
				 * newscr->line[i].firstchar is normally set
				 * by wnoutrefresh.  curscr->line[i].firstchar
				 * is normally set by _nc_scroll_window in the
				 * vertical-movement optimization code,
				 */
				if (newscr->_line[i].firstchar != _NOCHANGE
				    || curscr->_line[i].firstchar != _NOCHANGE)
				{
					TransformLine(i);
					changedlines++;
				}

				/* mark line changed successfully */
				if (i <= newscr->_maxy)
					MARK_NOCHANGE(newscr,i)
				if (i <= curscr->_maxy)
					MARK_NOCHANGE(curscr,i)

				/*
				 * Here is our line-breakout optimization.
				 */
				if ((changedlines % CHECK_INTERVAL) == changedlines-1
				 && check_pending())
					goto cleanup;
			}
		}
	}

	/* this code won't be executed often */
	for (i = screen_lines; i <= newscr->_maxy; i++)
		MARK_NOCHANGE(newscr,i)
	for (i = screen_lines; i <= curscr->_maxy; i++)
		MARK_NOCHANGE(curscr,i)

	curscr->_curx = newscr->_curx;
	curscr->_cury = newscr->_cury;

	GoTo(curscr->_cury, curscr->_curx);

    cleanup:
	/*
	 * Keep the physical screen in normal mode in case we get other
	 * processes writing to the screen.
	 */
	UpdateAttrs(A_NORMAL);

	fflush(SP->_ofp);
	curscr->_attrs = newscr->_attrs;
/*	curscr->_bkgd  = newscr->_bkgd; */

	_nc_signal_handler(TRUE);

	return OK;
}

/*
 *	ClrBlank(win)
 *
 *	Returns the attributed character that corresponds to the "cleared"
 *	screen.  If the terminal has the back-color-erase feature, this will be
 *	colored according to the wbkgd() call.  (Other attributes are
 *	unspecified, hence assumed to be reset in accordance with
 *	'ClrSetup()').
 *
 *	We treat 'curscr' specially because it isn't supposed to be set directly
 *	in the wbkgd() call.  Assume 'stdscr' for this case.
 */
#define BCE_ATTRS (A_NORMAL|A_COLOR)
#define BCE_BKGD(win) (((win) == curscr ? stdscr : (win))->_bkgd)

static inline chtype ClrBlank (WINDOW *win)
{
chtype	blank = BLANK;
	if (back_color_erase)
		blank |= (BCE_BKGD(win) & BCE_ATTRS);
	return blank;
}

/*
 *	ClrSetup(win)
 *
 *	Ensures that if the terminal recognizes back-color-erase, that we
 *	set the video attributes to match the window's background color
 *	before an erase operation.
 */
static inline chtype ClrSetup (WINDOW *win)
{
	if (back_color_erase)
		vidattr(BCE_BKGD(win) & BCE_ATTRS);
	return ClrBlank(win);
}

/*
**	ClrUpdate(scr)
**
**	Update by clearing and redrawing the entire screen.
**
*/

static void ClrUpdate(WINDOW *scr)
{
int	i = 0, j = 0;
int	lastNonBlank;
chtype	blank = ClrSetup(scr);

	T(("ClrUpdate(%p) called", scr));
	ClearScreen();

	if (scr != curscr) {
		for (i = 0; i < screen_lines ; i++)
			for (j = 0; j < screen_columns; j++)
				curscr->_line[i].text[j] = blank;
	}

	T(("updating screen from scratch"));
	for (i = 0; i < min(screen_lines, scr->_maxy + 1); i++) {
		lastNonBlank = scr->_maxx;

		while (lastNonBlank >= 0
		  &&   scr->_line[i].text[lastNonBlank] == blank)
			lastNonBlank--;

		if (lastNonBlank >= 0) {
			if (lastNonBlank > screen_columns)
				lastNonBlank = screen_columns;
			GoTo(i, 0);
			PutRange(curscr->_line[i].text,
				    scr->_line[i].text, i, 0, lastNonBlank);
		}
	}

	if (scr != curscr) {
		for (i = 0; i < screen_lines ; i++)
			memcpy(curscr->_line[i].text,
			          scr->_line[i].text,
				  screen_columns * sizeof(chtype));
	}
}

/*
**	ClrToEOL()
**
**	Clear to end of current line, starting at the cursor position
*/

static void ClrToEOL(void)
{
int	j;
chtype	blank = ClrSetup(curscr);
bool	needclear = FALSE;

	for (j = SP->_curscol; j < screen_columns; j++)
	{
	    chtype *cp = &(curscr->_line[SP->_cursrow].text[j]);

	    if (*cp != blank)
	    {
		*cp = blank;
		needclear = TRUE;
	    }
	}

	if (needclear)
	{
	    TPUTS_TRACE("clr_eol");
	    if (SP->_el_cost > (screen_columns - SP->_curscol))
	    {
		int count = (screen_columns - SP->_curscol);
		while (count-- > 0)
			putc(' ', SP->_ofp);
	    }
	    else
		putp(clr_eol);
	}
}

/*
**	ClrToBOL()
**
**	Clear to beginning of current line, counting the cursor position
*/

static void ClrToBOL(void)
{
int	j;
chtype	blank = ClrSetup(curscr);
bool	needclear = FALSE;

	for (j = 0; j <= SP->_curscol; j++)
	{
	    chtype *cp = &(curscr->_line[SP->_cursrow].text[j]);

	    if (*cp != blank)
	    {
		*cp = blank;
		needclear = TRUE;
	    }
	}

	if (needclear)
	{
	    TPUTS_TRACE("clr_bol");
	    putp(clr_bol);
	}
}

/*
 *	ClrBottom(total)
 *
 *	Test if clearing the end of the screen would satisfy part of the
 *	screen-update.  Do this by scanning backwards through the lines in the
 *	screen, checking if each is blank, and one or more are changed.
 */
static int ClrBottom(int total)
{
static	chtype	*tstLine;
static	size_t	lenLine;

int	row, col;
int	top    = total;
chtype	blank  = ClrBlank(curscr);
int	last   = min(screen_columns, newscr->_maxx+1);
size_t	length = sizeof(chtype) * last;

	if (tstLine == 0)
		tstLine = (chtype *)malloc(length);
	else if (length > lenLine)
		tstLine = (chtype *)realloc(tstLine, length);

	if (tstLine != 0) {
		lenLine = length;
		for (col = 0; col < last; col++)
			tstLine[col] = blank;

		for (row = total-1; row >= 0; row--) {
			if (memcmp(tstLine, newscr->_line[row].text, length))
				break;
			if (newscr->_line[row].firstchar != _NOCHANGE)
				top = row;
		}

		if (top < total) {
			(void) ClrSetup (curscr);
			GoTo(top,0);
			TPUTS_TRACE("clr_eos");
			putp(clr_eos);
			while (total-- > top) {
				for (col = 0; col <= curscr->_maxx; col++)
					curscr->_line[total].text[col] = blank;
				if (total <= curscr->_maxy)
					MARK_NOCHANGE(curscr,total)
				if (total <= newscr->_maxy)
					MARK_NOCHANGE(newscr,total)
			}
			total++;
		}
	}
	return total;
}

/*
**	TransformLine(lineno)
**
**	Call either IDcTransformLine or NoIDcTransformLine to do the
**	update, depending upon availability of insert/delete character.
*/

static void TransformLine(int const lineno)
{

	T(("TransformLine(%d) called",lineno));

	if ( (insert_character  ||  (enter_insert_mode  &&  exit_insert_mode))
		 &&  delete_character)
		IDcTransformLine(lineno);
	else
		NoIDcTransformLine(lineno);
}



/*
**	NoIDcTransformLine(lineno)
**
**	Transform the given line in curscr to the one in newscr, without
**	using Insert/Delete Character.
**
**		firstChar = position of first different character in line
**		lastChar = position of last different character in line
**
**		overwrite all characters between firstChar and lastChar.
**
*/

static void NoIDcTransformLine(int const lineno)
{
int	firstChar, lastChar;
chtype	*newLine = newscr->_line[lineno].text;
chtype	*oldLine = curscr->_line[lineno].text;
bool	attrchanged = FALSE;

	T(("NoIDcTransformLine(%d) called", lineno));

	firstChar = 0;
	while (firstChar < screen_columns - 1 &&  newLine[firstChar] == oldLine[firstChar]) {
		if(ceol_standout_glitch) {
			if(AttrOf(newLine[firstChar]) != AttrOf(oldLine[firstChar]))
			attrchanged = TRUE;
		}
		firstChar++;
	}

	T(("first char at %d is %lx", firstChar, newLine[firstChar]));
	if (firstChar > screen_columns)
		return;

	if(ceol_standout_glitch && attrchanged) {
		firstChar = 0;
		lastChar = screen_columns - 1;
		GoTo(lineno, firstChar);
		if(clr_eol)
			ClrToEOL();
	} else {
		lastChar = screen_columns - 1;
		while (lastChar > firstChar  &&  newLine[lastChar] == oldLine[lastChar])
			lastChar--;
		GoTo(lineno, firstChar);
	}

	T(("updating chars %d to %d", firstChar, lastChar));

	if (lastChar >= firstChar) {
		PutRange(oldLine, newLine, lineno, firstChar, lastChar);
		memcpy(oldLine + firstChar,
		       newLine + firstChar,
		       (lastChar - firstChar + 1) * sizeof(chtype));
	}
}

/*
**	IDcTransformLine(lineno)
**
**	Transform the given line in curscr to the one in newscr, using
**	Insert/Delete Character.
**
**		firstChar = position of first different character in line
**		oLastChar = position of last different character in old line
**		nLastChar = position of last different character in new line
**
**		move to firstChar
**		overwrite chars up to min(oLastChar, nLastChar)
**		if oLastChar < nLastChar
**			insert newLine[oLastChar+1..nLastChar]
**		else
**			delete oLastChar - nLastChar spaces
*/

static void IDcTransformLine(int const lineno)
{
int	firstChar, oLastChar, nLastChar;
chtype	*newLine = newscr->_line[lineno].text;
chtype	*oldLine = curscr->_line[lineno].text;
int	n;
bool	attrchanged = FALSE;

	T(("IDcTransformLine(%d) called", lineno));

	if(ceol_standout_glitch && clr_eol) {
		firstChar = 0;
		while(firstChar < screen_columns) {
			if(AttrOf(newLine[firstChar]) != AttrOf(oldLine[firstChar]))
				attrchanged = TRUE;
			firstChar++;
		}
	}

	firstChar = 0;

	if (attrchanged) {	/* we may have to disregard the whole line */
		GoTo(lineno, firstChar);
		ClrToEOL();
		PutRange(oldLine, newLine, lineno, 0, (screen_columns-1));
	} else {
		chtype blank = ClrBlank(curscr);

		/* find the first differing character */
		while (firstChar < screen_columns  &&
				newLine[firstChar] == oldLine[firstChar])
			firstChar++;

		/* if there wasn't one, we're done */
		if (firstChar >= screen_columns)
			return;

		/* it may be cheap to clear leading whitespace with clr_bol */
		if (clr_bol)
		{
			int oFirstChar, nFirstChar;

			for (oFirstChar = 0; oFirstChar < screen_columns; oFirstChar++)
				if (oldLine[oFirstChar] != blank)
					break;
			for (nFirstChar = 0; nFirstChar < screen_columns; nFirstChar++)
				if (newLine[nFirstChar] != blank)
					break;

			if (nFirstChar > oFirstChar + SP->_el1_cost)
			{
			    GoTo(lineno, nFirstChar - 1);
			    ClrToBOL();

			    if(nFirstChar == screen_columns)
				return;

 			    if (nFirstChar > firstChar)
				firstChar = nFirstChar;
			}
		}

		/* find last non-blank character on old line */
		oLastChar = screen_columns - 1;
		while (oLastChar > firstChar  &&  oldLine[oLastChar] == blank)
			oLastChar--;

		/* find last non-blank character on new line */
		nLastChar = screen_columns - 1;
		while (nLastChar > firstChar  &&  newLine[nLastChar] == blank)
			nLastChar--;

		if((nLastChar == firstChar)
		 && (SP->_el_cost < (screen_columns - nLastChar))
		 && ((SP->_current_attr | BLANK) == blank)) {
			GoTo(lineno, firstChar);
			ClrToEOL();
			if(newLine[firstChar] != blank )
				PutChar(newLine[firstChar]);
		} else if( newLine[nLastChar] != oldLine[oLastChar] ) {
			GoTo(lineno, firstChar);
			if ((oLastChar - nLastChar) > SP->_el_cost) {
				PutRange(oldLine, newLine, lineno, firstChar, nLastChar);
				ClrToEOL();
			} else {
				n = max( nLastChar , oLastChar );
				PutRange(oldLine, newLine, lineno, firstChar, n);
			}
		} else {
			int nLastNonblank = nLastChar;
			int oLastNonblank = oLastChar;

			/* find the last characters that really differ */
			while (newLine[nLastChar] == oldLine[oLastChar]) {
				if (nLastChar != 0
				 && oLastChar != 0) {
					nLastChar--;
					oLastChar--;
				 } else {
					break;
				 }
			}

			n = min(oLastChar, nLastChar);
			if (n >= firstChar) {
				GoTo(lineno, firstChar);
				PutRange(oldLine, newLine, lineno, firstChar, n);
			} else {
				GoTo(lineno, n+1);
			}

			if (oLastChar < nLastChar) {
				int m = max(nLastNonblank, oLastNonblank);
				if (InsCharCost(nLastChar - oLastChar)
				 > (m - n)) {
					PutRange(oldLine, newLine, lineno, n+1, m);
				} else {
					InsStr(&newLine[n+1], nLastChar - oLastChar);
				}
			} else if (oLastChar > nLastChar ) {
				if (DelCharCost(oLastChar - nLastChar)
				 > SP->_el_cost
				 + nLastNonblank - (n+1)) {
					PutRange(oldLine, newLine, lineno,
						n+1, nLastNonblank);
					ClrToEOL();
				} else {
					/*
					 * The delete-char sequence will
					 * effectively shift in blanks from the
					 * right margin of the screen.  Ensure
					 * that they are the right color by
					 * setting the video attributes from
					 * the last character on the row.
					 */
					UpdateAttrs(newLine[screen_columns-1]);
					DelChar(oLastChar - nLastChar);
				}
			}
		}
	}

	/* update the code's internal representation */
	if (screen_columns > firstChar)
		memcpy( oldLine + firstChar,
			newLine + firstChar,
			(screen_columns - firstChar) * sizeof(chtype));
}

/*
**	ClearScreen()
**
**	Clear the physical screen and put cursor at home
**
*/

static void ClearScreen(void)
{

	T(("ClearScreen() called"));

	if (clear_screen) {
		TPUTS_TRACE("clear_screen");
		putp(clear_screen);
		SP->_cursrow = SP->_curscol = 0;
#ifdef POSITION_DEBUG
		position_check(SP->_cursrow, SP->_curscol, "ClearScreen");
#endif /* POSITION_DEBUG */
	} else if (clr_eos) {
		SP->_cursrow = SP->_curscol = -1;
		GoTo(0,0);

		TPUTS_TRACE("clr_eos");
		putp(clr_eos);
	} else if (clr_eol) {
		SP->_cursrow = SP->_curscol = -1;

		while (SP->_cursrow < screen_lines) {
			GoTo(SP->_cursrow, 0);
			TPUTS_TRACE("clr_eol");
			putp(clr_eol);
		}
		GoTo(0,0);
	}
	T(("screen cleared"));
}


/*
**	InsStr(line, count)
**
**	Insert the count characters pointed to by line.
**
*/

static int InsStr(chtype *line, int count)
{
	T(("InsStr(%p,%d) called", line, count));

	if (enter_insert_mode  &&  exit_insert_mode) {
		TPUTS_TRACE("enter_insert_mode");
		putp(enter_insert_mode);
		while (count) {
			PutAttrChar(*line);
			line++;
			count--;
		}
		TPUTS_TRACE("exit_insert_mode");
		putp(exit_insert_mode);
		return(OK);
	} else if (parm_ich) {
		TPUTS_TRACE("parm_ich");
		tputs(tparm(parm_ich, count), count, _nc_outch);
		while (count) {
			PutAttrChar(*line);
			line++;
			count--;
		}
		return(OK);
	} else {
		while (count) {
			TPUTS_TRACE("insert_character");
			putp(insert_character);
			PutAttrChar(*line);
			if (insert_padding)
			{
				TPUTS_TRACE("insert_padding");
				putp(insert_padding);
			}
			line++;
			count--;
		}
		return(OK);
	}
}

/*
**	DelChar(count)
**
**	Delete count characters at current position
**
*/

static void DelChar(int count)
{
	T(("DelChar(%d) called, position = (%d,%d)", count, newscr->_cury, newscr->_curx));

	if (parm_dch) {
		TPUTS_TRACE("parm_dch");
		tputs(tparm(parm_dch, count), count, _nc_outch);
	} else {
		while (count--)
		{
			TPUTS_TRACE("delete_character");
			putp(delete_character);
		}
	}
}

/*
**	_nc_outstr(char *str)
**
**	Emit a string without waiting for update.
*/

void _nc_outstr(char *str)
{
    FILE *ofp = SP ? SP->_ofp : stdout;

    (void) fputs(str, ofp);
    (void) fflush(ofp);
}
