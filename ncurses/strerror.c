
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if !HAVE_STRERROR
#include <stdio.h>

#include <errno.h>
#if !HAVE_EXTERN_ERRNO
extern int errno;
#endif

#if !HAVE_EXTERN_SYS_ERRLIST
extern char *sys_errlist[];
extern int sys_nerr;
#endif

char *strerror(int err)
{
	if (err >= sys_nerr)
		return NULL;
	return sys_errlist[err];
}
#else
void _nc_strerror(void) { }	/* nonempty for strict ANSI compilers */
#endif
