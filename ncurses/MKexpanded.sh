#!/bin/sh
################################################################################
# Copyright 1997 by Thomas E. Dickey <dickey@clark.net>                        #
# All Rights Reserved.                                                         #
#                                                                              #
# Permission to use, copy, modify, and distribute this software and its        #
# documentation for any purpose and without fee is hereby granted, provided    #
# that the above copyright notice appear in all copies and that both that      #
# copyright notice and this permission notice appear in supporting             #
# documentation, and that the name of the above listed copyright holder(s) not #
# be used in advertising or publicity pertaining to distribution of the        #
# software without specific, written prior permission. THE ABOVE LISTED        #
# COPYRIGHT HOLDER(S) DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,    #
# INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT #
# SHALL THE ABOVE LISTED COPYRIGHT HOLDER(S) BE LIABLE FOR ANY SPECIAL,        #
# INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM   #
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE   #
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR    #
# PERFORMANCE OF THIS SOFTWARE.                                                #
################################################################################
# $Id: MKexpanded.sh,v 1.2 1997/03/15 22:43:48 tom Exp $
#
# Script to generate 'expanded.c', a dummy source that contains functions
# corresponding to complex macros used in this library.  By making functions,
# we simplify analysis and debugging.

if test $# != 0; then
preprocessor="$1"
else
preprocessor="cc -E"
fi
preprocessor="$preprocessor -DHAVE_CONFIG_H -I. -I../include"

TMP=gen$$.c
trap "rm -f $TMP" 0 1 2 5 15

cat >expanded.c <<EOF
/* generated by MKexpanded.sh */
#include <curses.priv.h>
#include <term.h>
#ifdef NCURSES_EXPANDED
EOF

cat >$TMP <<EOF
#include <config.h>
#undef NCURSES_EXPANDED /* this probably is set in config.h */
#include <curses.priv.h>
/* these are names we'd like to see */
#undef ALL_BUT_COLOR
#undef PAIR_NUMBER
#undef TRUE
#undef FALSE
/* this is a marker */
IGNORE
chtype _nc_ch_or_attr(chtype ch, attr_t at)
{
	return ch_or_attr(ch,at);
}
void _nc_toggle_attr_on(attr_t *S, attr_t at)
{
	toggle_attr_on(*S,at);
}
void _nc_toggle_attr_off(attr_t *S, attr_t at) 
{
	toggle_attr_off(*S,at);
}
int _nc_can_clear_with(chtype ch)
{
	return can_clear_with(ch);
}
int _nc_DelCharCost(int count)
{
	return DelCharCost(count);
}
int _nc_InsCharCost(int count)
{
	return InsCharCost(count);
}
void _nc_UpdateAttrs(chtype c)
{
	UpdateAttrs(c);
}
EOF

$preprocessor $TMP 2>/dev/null | sed -e '1,/^IGNORE$/d' >>expanded.c

cat >>expanded.c <<EOF
#else /* ! NCURSES_EXPANDED */
void _nc_expanded(void) { }
#endif /* NCURSES_EXPANDED */
EOF
