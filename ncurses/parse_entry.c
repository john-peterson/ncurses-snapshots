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


/*
 *	parse_entry.c -- compile one terminfo or termcap entry
 *
 *	Get an exact in-core representation of an entry.  Don't
 *	try to resolve use or tc capabilities, that is someone
 *	else's job.  Depends on the lexical analyzer to get tokens
 *	from the input stream.
 */

#include <config.h>

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "tic.h"
#define __INTERNAL_CAPS_VISIBLE
#include "term.h"
#include "term_entry.h"

#ifdef LINT
static short const parametrized[] = { 0 };
#else
#include "parametrized.h"
#endif

struct token	_nc_curr_token;

static	void postprocess_termcap(TERMTYPE *, bool);
static	void postprocess_terminfo(TERMTYPE *);
static	struct name_table_entry	const * lookup_fullname(const char *name);

/*
 *	int
 *	_nc_parse_entry(entry, literal, silent)
 *
 *	Compile one entry.  Doesn't try to resolve use or tc capabilities.
 *
 *	found-forward-use = FALSE
 *	re-initialise internal arrays
 *	get_token();
 *	if the token was not a name in column 1, complain and die
 *	save names in entry's string table
 *	while (get_token() is not EOF and not NAMES)
 *	        check for existance and type-correctness
 *	        enter cap into structure
 *	        if STRING
 *	            save string in entry's string table
 *	push back token
 */

int _nc_parse_entry(struct entry *entryp, int literal, bool silent)
{
    int			token_type;
    struct name_table_entry	const *entry_ptr;
    char			*ptr, namecpy[MAX_NAME_SIZE+1];

    token_type = _nc_get_token();

    if (token_type == EOF)
	return(EOF);
    if (token_type != NAMES)
	_nc_err_abort("Entry does not start with terminal names in column one");
	
    _nc_init_entry(&entryp->tterm);

    entryp->cstart = _nc_comment_start;
    entryp->cend = _nc_comment_end;
    entryp->startline = _nc_start_line;
    DEBUG(2, ("Comment range is %ld to %ld", entryp->cstart, entryp->cend));

    /* junk the 2-character termcap name, if present */
    ptr = _nc_curr_token.tk_name;
    if (ptr[2] == '|')
    {
	ptr = _nc_curr_token.tk_name + 3;
	_nc_curr_token.tk_name[2] = '\0';
    }

    entryp->tterm.str_table = entryp->tterm.term_names = _nc_save_str(ptr);

    DEBUG(1, ("Starting '%s'", ptr));

    /*
     * We do this because the one-token lookahead in the parse loop
     * results in the terminal type getting prematurely set to correspond
     * to that of the next entry.
     */
    _nc_set_type(_nc_first_name(entryp->tterm.term_names));

    /* check for overly-long names and aliases */
    (void) strncpy(namecpy, entryp->tterm.term_names, MAX_NAME_SIZE);
    namecpy[MAX_NAME_SIZE] = '\0';
    if ((ptr = strrchr(namecpy, '|')) != (char *)NULL)
	*ptr = '\0';
    ptr = strtok(namecpy, "|");
    if (strlen(ptr) > MAX_ALIAS)
	_nc_warning("primary name may be too long");
    while ((ptr = strtok((char *)NULL, "|")) != (char *)NULL)
	if (strlen(ptr) > MAX_ALIAS)
	    _nc_warning("alias `%s' may be too long", ptr);

    entryp->nuses = 0;

    for (token_type = _nc_get_token();
	 token_type != EOF  &&  token_type != NAMES;
	 token_type = _nc_get_token())
    {
	if (strcmp(_nc_curr_token.tk_name, "use") == 0
	    || strcmp(_nc_curr_token.tk_name, "tc") == 0) {
	    entryp->uses[entryp->nuses].parent = (void *)_nc_save_str(_nc_curr_token.tk_valstring);
	    entryp->uses[entryp->nuses].line = _nc_curr_line;
	    entryp->nuses++;
	} else {
	    /* normal token lookup */
	    entry_ptr = _nc_find_entry(_nc_curr_token.tk_name,
				       _nc_syntax ? _nc_cap_hash_table : _nc_info_hash_table);

	    /*
	     * Our kluge to handle aliasing.  The reason it's done
	     * this ugly way, with a linear search, is so the hashing
	     * machinery doesn't have to be made really complicated
	     * (also we get better warnings this way).  No point in
	     * making this case fast, aliased caps aren't common now
	     * and will get rarer.
	     */
	    if (entry_ptr == NOTFOUND)
	    {
		const struct alias	*ap;

		if (_nc_syntax == SYN_TERMCAP)
		{
		    for (ap = _nc_capalias_table; ap->from; ap++)
			if (strcmp(ap->from, _nc_curr_token.tk_name) == 0)
			{
			    if (ap->to == (char *)NULL)
			    {
				_nc_warning("%s (%s termcap extension) ignored",
					    ap->from, ap->source);
				goto nexttok;
			    }

			    entry_ptr = _nc_find_entry(ap->to, _nc_cap_hash_table);
			    if (entry_ptr && !silent)
				_nc_warning("%s (%s termcap extension) aliased to %s", ap->from, ap->source, ap->to);
			    break;
			}
		}
		else /* if (_nc_syntax == SYN_TERMINFO) */
		{
		    for (ap = _nc_infoalias_table; ap->from; ap++)
			if (strcmp(ap->from, _nc_curr_token.tk_name) == 0)
			{
			    if (ap->to == (char *)NULL)
			    {
				_nc_warning("%s (%s terminfo extension) ignored",
					    ap->from, ap->source);
				goto nexttok;
			    }

			    entry_ptr = _nc_find_entry(ap->to, _nc_info_hash_table);
			    if (entry_ptr && !silent)
				_nc_warning("%s (%s terminfo extension) aliased to %s", ap->from, ap->source, ap->to);
			    break;
			}

		    /* last chance: a full-name */
		    if (entry_ptr == NOTFOUND) {
			entry_ptr = lookup_fullname(_nc_curr_token.tk_name);
		    }
		}
	    }

	    /* can't find this cap name, not even as an alias */
	    if (entry_ptr == NOTFOUND) {
		if (!silent)
		    _nc_warning("unknown capability '%s'",
				_nc_curr_token.tk_name);
		continue;
	    }

	    /* deal with bad type/value combinations. */
	    if (token_type != CANCEL &&  entry_ptr->nte_type != token_type)
	    {
		/*
		 * Nasty special cases here handle situations in which type
		 * information can resolve name clashes.  Normal lookup
		 * finds the last instance in the capability table of a
		 * given name, regardless of type.  find_type_entry looks
		 * for a first matching instance with given type.  So as 
		 * long as all ambiguous names occur in pairs of distinct
		 * type, this will do the job.
		 */

		/* tell max_attributes from arrow_key_map */
		if (token_type == NUMBER && !strcmp("ma", _nc_curr_token.tk_name))
		    entry_ptr = _nc_find_type_entry("ma", NUMBER,
					_nc_get_table(_nc_syntax != 0));

		/* map terminfo's string MT to MT */
		else if (token_type==STRING &&!strcmp("MT",_nc_curr_token.tk_name))
		    entry_ptr = _nc_find_type_entry("MT", STRING,
					_nc_get_table(_nc_syntax != 0));

		/* treat strings without following "=" as empty strings */
		else if (token_type==BOOLEAN && entry_ptr->nte_type==STRING)
		    token_type = STRING;
		/* we couldn't recover; skip this token */
		else
		{
		    if (!silent)
		    {
			char *type_name;
			switch (entry_ptr->nte_type)
			{
			case BOOLEAN:
				type_name = "boolean";
				break;
			case STRING:
				type_name = "string";
				break;
			case NUMBER:
				type_name = "numeric";
				break;
			default:
				type_name = "unknown";
				break;
			}
			_nc_warning("wrong type used for %s capability '%s'",
				type_name, _nc_curr_token.tk_name);
		    }
		    continue;
		}
	    }

	    /* now we know that the type/value combination is OK */
	    switch (token_type) {
	    case CANCEL:
		switch (entry_ptr->nte_type) {
		case BOOLEAN:
		    entryp->tterm.Booleans[entry_ptr->nte_index] = CANCELLED_BOOLEAN;
		    break;

		case NUMBER:
		    entryp->tterm.Numbers[entry_ptr->nte_index] = CANCELLED_NUMERIC;
		    break;

		case STRING:
		    entryp->tterm.Strings[entry_ptr->nte_index] = CANCELLED_STRING;
		    break;
		}
		break;
		
	    case BOOLEAN:
		entryp->tterm.Booleans[entry_ptr->nte_index] = TRUE;
		break;
		    
	    case NUMBER:
		entryp->tterm.Numbers[entry_ptr->nte_index] =
		    _nc_curr_token.tk_valnumber;
		break;

	    case STRING:
		ptr = _nc_curr_token.tk_valstring;
		if (_nc_syntax==SYN_TERMCAP)
		    ptr = _nc_captoinfo(_nc_curr_token.tk_name,
				    ptr,
				    parametrized[entry_ptr->nte_index]);
		entryp->tterm.Strings[entry_ptr->nte_index] = _nc_save_str(ptr);
		break;

	    default:
		if (!silent)
		    _nc_warning("unknown token type");
		_nc_panic_mode((_nc_syntax==SYN_TERMCAP) ? ':' : ',');
		continue;
	    }
	} /* end else cur_token.name != "use" */
    nexttok:
	continue;	/* cannot have a label w/o statement */
    } /* endwhile (not EOF and not NAMES) */

    _nc_push_token(token_type);
    _nc_set_type(_nc_first_name(entryp->tterm.term_names));

    /*
     * Try to deduce as much as possible from extension capabilities
     * (this includes obsolete BSD capabilities).  Sigh...it would be more
     * space-efficient to call this after use resolution, but it has
     * to be done before entry allocation is wrapped up.
     */
    if (!literal)
	if (_nc_syntax == SYN_TERMCAP)
	{
	    bool	has_base_entry = FALSE;
	    int		i;

	    /*
	     * Don't insert defaults if this is a `+' entry meant only
	     * for inclusion in other entries (not sure termcap ever
	     * had these, actually).
	     */
	    if (strchr(entryp->tterm.term_names, '+'))
		has_base_entry = TRUE;
	    else
		/*
		 * Otherwise, look for a base entry that will already
		 * have picked up defaults via translation.
		 */
		for (i = 0; i < entryp->nuses; i++)
		    if (!strchr(entryp->uses[i].parent, '+'))
			has_base_entry = TRUE;

	    postprocess_termcap(&entryp->tterm, has_base_entry);
        }
	else 
	    postprocess_terminfo(&entryp->tterm);

    _nc_wrap_entry(entryp);

    return(OK);
}

int _nc_capcmp(const char *s, const char *t)
/* compare two string capabilities, stripping out padding */
{
    if (!s && !t)
	return(0);
    else if (!s || !t)
	return(1);

    for (;;)
    {
	if (s[0] == '$' && s[1] == '<')
	{
	    for (s += 2; ; s++)
		if (!(isdigit(*s) || *s=='.' || *s=='*' || *s=='/' || *s=='>'))
		    break;
	}

	if (t[0] == '$' && t[1] == '<')
	{
	    for (t += 2; ; t++)
		if (!(isdigit(*t) || *t=='.' || *t=='*' || *t=='/' || *t=='>'))
		    break;
	}

	/* we've now pushed s and t past any padding they were pointing at */

	if (*s == '\0' && *t == '\0')
		return(0);

	if (*s != *t)
	    return(*t - *s);

	/* else *s == *t but one is not NUL, so continue */
	s++, t++;
    }
}

/*
 * The ko capability, if present, consists of a comma-separated capability
 * list.  For each capability, we may assume there is a keycap that sends the
 * string which is the value of that capability.  
 */
typedef struct {char *from; char *to;} assoc;
static assoc const ko_xlate[] =
{
    {"al",	"kil1"},	/* insert line key  -> KEY_IL    */
    {"bt",	"kcbt"},	/* back tab         -> KEY_BTAB  */
    {"cd",	"ked"},		/* clear-to-eos key -> KEY_EOL   */
    {"ce",	"kel"},		/* clear-to-eol key -> KEY_EOS   */
    {"cl",	"kclr"},	/* clear key        -> KEY_CLEAR */
    {"ct",	"tbc"},		/* clear all tabs   -> KEY_CATAB */
    {"dc",	"kdch1"},	/* delete char      -> KEY_DC    */
    {"dl",	"kdl1"},	/* delete line      -> KEY_DL    */
    {"do",	"kcud1"},	/* down key         -> KEY_DOWN  */
    {"ei",	"krmir"},	/* exit insert key  -> KEY_EIC   */
    {"ho",	"khome"},	/* home key         -> KEY_HOME  */
    {"ic",	"kich1"},	/* insert char key  -> KEY_IC    */
    {"im",	"kIC"},		/* insert-mode key  -> KEY_SIC   */
    {"le",	"kcub1"},	/* le key           -> KEY_LEFT  */
    {"nd",	"kcuf1"},	/* nd key           -> KEY_RIGHT */
    {"nl",	"kent"},	/* new line key     -> KEY_ENTER */
    {"st",	"khts"},	/* set-tab key      -> KEY_STAB  */
    {"ta",	CANCELLED_STRING},
    {"up",	"kcuu1"},	/* up-arrow key     -> KEY_UP    */
    {(char *)NULL, (char *)NULL},
};

/*
 * This routine fills in string caps that either had defaults under
 * termcap or can be manufactured from obsolete termcap capabilities.
 * It was lifted from Ross Ridge's mytinfo package.
 */

static const char C_CR[] = "\r";
static const char C_LF[] = "\n";
static const char C_BS[] = "\b";
static const char C_HT[] = "\t";

/*
 * Note that WANTED and PRESENT are not simple inverses!  If a capability
 * has been explicitly cancelled, it's not considered WANTED.
 */
#define WANTED(s)	((s) == (char *)NULL)
#define PRESENT(s)	(((s) != (char *)NULL) && ((s) != CANCELLED_STRING))

/*
 * This bit of legerdemain turns all the terminfo variable names into
 * references to locations in the arrays Booleans, Numbers, and Strings ---
 * precisely what's needed.
 */

#undef CUR
#define CUR tp->

static
void postprocess_termcap(TERMTYPE *tp, bool has_base)
{
    char buf[MAX_LINE * 2 + 2];

    /*
     * TERMCAP DEFAULTS AND OBSOLETE-CAPABILITY TRANSLATIONS
     *
     * This first part of the code is the functional inverse of the
     * fragment in capdefaults.c.
     * ----------------------------------------------------------------------
     */

    /* if there was a tc entry, assume we picked up defaults via that */
    if (!has_base)
    {
	if (WANTED(init_3string) && termcap_init2)
	    init_3string = _nc_save_str(termcap_init2);

	if (WANTED(reset_1string) && termcap_reset)
	    reset_1string = _nc_save_str(termcap_reset);

	if (WANTED(carriage_return)) {
	    if (carriage_return_delay > 0) {
		sprintf(buf, "%s$<%d>", C_CR, carriage_return_delay);
		carriage_return = _nc_save_str(buf);
	    } else
		carriage_return = _nc_save_str(C_CR);
	}
	if (WANTED(cursor_left)) {
	    if (backspace_delay > 0) {
		sprintf(buf, "%s$<%d>", C_BS, backspace_delay);
		cursor_left = _nc_save_str(buf);
	    } else if (backspaces_with_bs == 1)
		cursor_left = _nc_save_str(C_BS);
	    else if (PRESENT(backspace_if_not_bs))
		cursor_left = backspace_if_not_bs;
	}
	/* vi doesn't use "do", but it does seems to use nl (or '\n') instead */
	if (WANTED(cursor_down)) {
	    if (PRESENT(linefeed_if_not_lf)) 
		cursor_down = linefeed_if_not_lf;
	    else if (linefeed_is_newline != 1) {
		if (new_line_delay > 0) {
		    sprintf(buf, "%s$<%d>", C_LF, new_line_delay);
		    cursor_down = _nc_save_str(buf);
		} else
		    cursor_down = _nc_save_str(C_LF);
	    }
	}
	if (WANTED(scroll_forward) && crt_no_scrolling != 1) {
	    if (PRESENT(linefeed_if_not_lf)) 
		cursor_down = linefeed_if_not_lf;
	    else if (linefeed_is_newline != 1) {
		if (new_line_delay > 0) {
		    sprintf(buf, "%s$<%d>", C_LF, new_line_delay);
		    scroll_forward = _nc_save_str(buf);
		} else
		    scroll_forward = _nc_save_str(C_LF);
	    }
	}
	if (WANTED(newline)) {
	    if (linefeed_is_newline == 1) {
		if (new_line_delay > 0) {
		    sprintf(buf, "%s$<%d>", C_LF, new_line_delay);
		    newline = _nc_save_str(buf);
		} else
		    newline = _nc_save_str(C_LF);
	    } else if (PRESENT(carriage_return) && PRESENT(scroll_forward)) {
		strncpy(buf, carriage_return, MAX_LINE-2);
		buf[MAX_LINE-1] = '\0';
		strncat(buf, scroll_forward, MAX_LINE-strlen(buf)-1);
		buf[MAX_LINE] = '\0';
		newline = _nc_save_str(buf);
	    } else if (PRESENT(carriage_return) && PRESENT(cursor_down)) {
		strncpy(buf, carriage_return, MAX_LINE-2);
		buf[MAX_LINE-1] = '\0';
		strncat(buf, cursor_down, MAX_LINE-strlen(buf)-1);
		buf[MAX_LINE] = '\0';
		newline = _nc_save_str(buf);
	    }
	}
    }

    /*
     * Inverse of capdefaults.c code ends here.
     * ----------------------------------------------------------------------
     *
     * TERMCAP-TO TERMINFO MAPPINGS FOR SOURCE TRANSLATION
     *
     * These translations will *not* be inverted by tgetent().
     */

    if (!has_base)
    {
	/*
	 * We wait until now to decide if we've got a working cr because even
	 * one that doesn't work can be used for newline. Unfortunately the
	 * space allocated for it is wasted.
	 */
	if (return_does_clr_eol == 1 || no_correctly_working_cr == 1)
	    carriage_return = NULL;

	/*
	 * Supposedly most termcap entries have ta now and '\t' is no longer a
	 * default, but it doesn't seem to be true...
	 */
	if (WANTED(tab)) {
	    if (horizontal_tab_delay > 0) {
		sprintf(buf, "%s$<%d>", C_HT, horizontal_tab_delay);
		tab = _nc_save_str(buf);
	    } else
		tab = _nc_save_str(C_HT);
	}
	if (init_tabs == -1 && has_hardware_tabs == TRUE)
	    init_tabs = 8;

	/*
	 * Assume we can beep with ^G unless we're given bl@.
	 */
	if (WANTED(bell))
	    bell = _nc_save_str("\007");
    }

    /*
     * Translate the old termcap :pt: capability to it#8 + ht=\t
     */
    if (has_hardware_tabs == TRUE)
	if (init_tabs != 8)
	    _nc_warning("hardware tabs with a width other than 8: %d", init_tabs);
        else
	{
	    if (tab && _nc_capcmp(tab, C_HT))
		_nc_warning("hardware tabs with a non-^I tab string `%s'",
			    _nc_visbuf(tab));
	    else
	    {
		if (WANTED(tab))
		    tab = _nc_save_str(C_HT);
		init_tabs = 8;
	    }
	}

    /*
     * Now translate the ko capability, if there is one.  This
     * isn't from mytinfo...
     */
    if (PRESENT(other_non_function_keys))
    {
	char	*dp, *cp = strtok(other_non_function_keys, ",");
	struct name_table_entry	const *from_ptr;
	struct name_table_entry	const *to_ptr;
	assoc	const *ap;
	char	buf2[MAX_TERMINFO_LENGTH];
	bool	foundim;

	/* we're going to use this for a special case later */
	dp = strchr(other_non_function_keys, 'i');
	foundim = dp && dp[1] == 'm';

	/* look at each comma-separated capability in the ko string... */
	do {
	    for (ap = ko_xlate; ap->from; ap++)
		if (strcmp(ap->from, cp) == 0)
		    break;
	    if (!ap->to)
	    {
		_nc_warning("unknown capability `%s' in ko string", cp);
		continue;
	    }
	    else if (ap->to == CANCELLED_STRING)	/* ignore it */
		continue;

	    /* now we know we found a match in ko_table, so... */

	    from_ptr = _nc_find_entry(ap->from, _nc_cap_hash_table);
	    to_ptr   = _nc_find_entry(ap->to,   _nc_info_hash_table);

	    if (!from_ptr || !to_ptr)	/* should never happen! */
		_nc_err_abort("ko translation table is invalid, I give up");

	    if (WANTED(tp->Strings[from_ptr->nte_index]))
	    {
		_nc_warning("no value for ko capability %s", ap->from);
		continue;
	    }

	    if (tp->Strings[to_ptr->nte_index])
	    {
		/* There's no point in warning about it if it's the same
		 * string; that's just an inefficiency.
		 */
		if (strcmp(
			tp->Strings[from_ptr->nte_index],
			tp->Strings[to_ptr->nte_index]) != 0)
		    _nc_warning("%s (%s) already has an explicit value (%s), ignoring ko",
			    ap->to, ap->from,
			    _nc_visbuf(tp->Strings[to_ptr->nte_index]) );
		continue;
	    }

	    /* 
	     * The magic moment -- copy the mapped key string over,
	     * stripping out padding.
	     */
	    dp = buf2;
	    for (cp = tp->Strings[from_ptr->nte_index]; *cp; cp++)
	    {
		if (cp[0] == '$' && cp[1] == '<')
		{
		    while (*cp && *cp != '>')
			if (!*cp)
			    break;
		        else
			    ++cp;
		}
		else
		    *dp++ = *cp;
	    }
	    *dp++ = '\0';
		    
	    tp->Strings[to_ptr->nte_index] = _nc_save_str(buf2);
	} while
	    ((cp = strtok((char *)NULL, ",")) != 0);

	/*
	 * Note: ko=im and ko=ic both want to grab the `Insert'
	 * keycap.  There's a kich1 but no ksmir, so the ic capability
	 * got mapped to kich1 and im to kIC to avoid a collision.
	 * If the description has im but not ic, hack kIC back to kich1.
	 */
	if (foundim && WANTED(key_ic) && key_sic)
	{
	    key_ic = key_sic;
	    key_sic = ABSENT_STRING;
	}
    }

    if (!hard_copy)
    {
	if (WANTED(key_backspace))
	    key_backspace = _nc_save_str(C_BS);
	if (WANTED(key_left))
	    key_left = _nc_save_str(C_BS);
	if (WANTED(key_down))
	    key_down = _nc_save_str(C_LF);
    }

    /*
     * Translate XENIX forms characters.
     */ 
    if (PRESENT(acs_ulcorner) ||
	PRESENT(acs_llcorner) ||
	PRESENT(acs_urcorner) ||
	PRESENT(acs_lrcorner) ||
	PRESENT(acs_ltee) ||
	PRESENT(acs_rtee) ||
	PRESENT(acs_btee) ||
	PRESENT(acs_ttee) ||
	PRESENT(acs_hline) ||
	PRESENT(acs_vline) ||
	PRESENT(acs_plus))
    {
	char	buf2[MAX_TERMCAP_LENGTH], *bp = buf2;

	if (acs_ulcorner && acs_ulcorner[1] == '\0')
	{
	    *bp++ = 'l';
	    *bp++ = *acs_ulcorner;
	}
	if (acs_llcorner && acs_llcorner[1] == '\0')
	{
	    *bp++ = 'm';
	    *bp++ = *acs_llcorner;
	}
	if (acs_urcorner && acs_urcorner[1] == '\0')
	{
	    *bp++ = 'k';
	    *bp++ = *acs_urcorner;
	}
	if (acs_lrcorner && acs_lrcorner[1] == '\0')
	{
	    *bp++ = 'j';
	    *bp++ = *acs_lrcorner;
	}
	if (acs_ltee && acs_ltee[1] == '\0')
   	{
	    *bp++ = 't';
	    *bp++ = *acs_ltee;
	}
	if (acs_rtee && acs_rtee[1] == '\0')
   	{
	    *bp++ = 'u';
	    *bp++ = *acs_rtee;
	}
	if (acs_btee && acs_btee[1] == '\0')
   	{
	    *bp++ = 'v';
	    *bp++ = *acs_btee;
	}
	if (acs_ttee && acs_ttee[1] == '\0')
   	{
	    *bp++ = 'w';
	    *bp++ = *acs_ttee;
	}
	if (acs_hline && acs_hline[1] == '\0')
  	{
	    *bp++ = 'q';
	    *bp++ = *acs_hline;
	}
	if (acs_vline && acs_vline[1] == '\0')
  	{
	    *bp++ = 'x';
	    *bp++ = *acs_vline;
	}
	if (acs_plus)
   	{
	    *bp++ = 'n';
	    strcpy(bp, acs_plus);
	    bp = buf2 + strlen(buf2);
	}

	if (bp != buf2)
	{
	    *bp++ = '\0';
	    acs_chars = _nc_save_str(buf2);
	    _nc_warning("acsc string synthesized from XENIX capabilities");
	}
    }
}

static
void postprocess_terminfo(TERMTYPE *tp)
{
    /*
     * TERMINFO-TO-TERMINFO MAPPINGS FOR SOURCE TRANSLATION 
     * ----------------------------------------------------------------------
     */

    /*
     * Translate AIX forms characters.
     */
    if (PRESENT(box_chars_1))
    {
	char	buf2[MAX_TERMCAP_LENGTH], *bp = buf2;

	if (box_chars_1[0])	/* ACS_ULCORNER */
	{
	    *bp++ = 'l';
	    *bp++ = box_chars_1[0];
	}
	if (box_chars_1[1])	/* ACS_HLINE */
  	{
	    *bp++ = 'q';
	    *bp++ = box_chars_1[1];
	}
	if (box_chars_1[2])	/* ACS_URCORNER */
	{
	    *bp++ = 'k';
	    *bp++ = box_chars_1[2];
	}
	if (box_chars_1[3])	/* ACS_VLINE */
  	{
	    *bp++ = 'x';
	    *bp++ = box_chars_1[3];
	}
	if (box_chars_1[4])	/* ACS_LRCORNER */
	{
	    *bp++ = 'j';
	    *bp++ = box_chars_1[4];
	}
	if (box_chars_1[5])	/* ACS_LLCORNER */
	{
	    *bp++ = 'm';
	    *bp++ = box_chars_1[5];
	}
	if (box_chars_1[6])	/* ACS_TTEE */
   	{
	    *bp++ = 'w';
	    *bp++ = box_chars_1[6];
	}
	if (box_chars_1[7])	/* ACS_RTEE */
   	{
	    *bp++ = 'u';
	    *bp++ = box_chars_1[7];
	}
	if (box_chars_1[8])	/* ACS_BTEE */
   	{
	    *bp++ = 'v';
	    *bp++ = box_chars_1[8];
	}
	if (box_chars_1[9])	/* ACS_LTEE */
   	{
	    *bp++ = 't';
	    *bp++ = box_chars_1[9];
	}
	if (box_chars_1[10])	/* ACS_PLUS */
   	{
	    *bp++ = 'n';
	    *bp++ = box_chars_1[10];
	}

	if (bp != buf2)
	{
	    *bp++ = '\0';
	    acs_chars = _nc_save_str(buf2);
	    _nc_warning("acsc string synthesized from AIX capabilities");
	    box_chars_1 = ABSENT_STRING;
	}
    }
    /*
     * ----------------------------------------------------------------------
     */
}

/*
 * Do a linear search through the terminfo tables to find a given full-name. 
 * We don't expect to do this often, so there's no hashing function.
 *
 * In effect, this scans through the 3 lists of full-names, and looks them
 * up in _nc_info_table, which is organized so that the nte_index fields are
 * sorted, but the nte_type fields are not necessarily grouped together.
 */
static
struct name_table_entry	const * lookup_fullname(const char *find)
{
    int state = -1;

    for (;;) {
	int count = 0;
	char *const *names;

	switch (++state) {
	case BOOLEAN:
	    names = boolfnames;
	    break;
	case STRING:
	    names = strfnames;
	    break;
	case NUMBER:
	    names = numfnames;
	    break;
	default:
	    return NOTFOUND;
	}

	for (count = 0; names[count] != 0; count++) {
	    if (!strcmp(names[count], find)) {
		struct name_table_entry	const *entry_ptr = _nc_get_table(FALSE);
		while (entry_ptr->nte_type  != state
 		    || entry_ptr->nte_index != count)
			entry_ptr++;
		return entry_ptr;
	    }
	}
    }
}

/* parse_entry.c ends here */
