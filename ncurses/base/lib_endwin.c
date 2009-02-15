/****************************************************************************
 * Copyright (c) 1998-2008,2009 Free Software Foundation, Inc.              *
 *                                                                          *
 * Permission is hereby granted, free of charge, to any person obtaining a  *
 * copy of this software and associated documentation files (the            *
 * "Software"), to deal in the Software without restriction, including      *
 * without limitation the rights to use, copy, modify, merge, publish,      *
 * distribute, distribute with modifications, sublicense, and/or sell       *
 * copies of the Software, and to permit persons to whom the Software is    *
 * furnished to do so, subject to the following conditions:                 *
 *                                                                          *
 * The above copyright notice and this permission notice shall be included  *
 * in all copies or substantial portions of the Software.                   *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS  *
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF               *
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.   *
 * IN NO EVENT SHALL THE ABOVE COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,   *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR    *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR    *
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.                               *
 *                                                                          *
 * Except as contained in this notice, the name(s) of the above copyright   *
 * holders shall not be used in advertising or otherwise to promote the     *
 * sale, use or other dealings in this Software without prior written       *
 * authorization.                                                           *
 ****************************************************************************/

/****************************************************************************
 *  Author: Zeyd M. Ben-Halim <zmbenhal@netcom.com> 1992,1995               *
 *     and: Eric S. Raymond <esr@snark.thyrsus.com>                         *
 ****************************************************************************/

/*
**	lib_endwin.c
**
**	The routine endwin().
**
*/

#include <curses.priv.h>

MODULE_ID("$Id: lib_endwin.c,v 1.19.1.3 2009/02/14 20:51:33 tom Exp $")

NCURSES_EXPORT(int)
NCURSES_SP_NAME(endwin) (NCURSES_SP_DCL)
{
    T((T_CALLED("endwin(%p)"), SP_PARM));

    if (SP_PARM) {
	TERMINAL_CONTROL_BLOCK *TCB = TCBOf(SP_PARM);
	SP_PARM->_endwin = TRUE;
	if (TCB && TCB->drv && TCB->drv->scexit)
	    TCB->drv->scexit(SP_PARM);
	returnCode(NCURSES_SP_NAME(reset_shell_mode) (NCURSES_SP_ARG));
    }
    returnCode(ERR);
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(int)
endwin(void)
{
    return NC_SNAME(endwin) (CURRENT_SCREEN);
}
#endif
