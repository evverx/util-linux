/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Michael Rendell of the Memorial University of Newfoundland.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Wed Jun 22 22:15:41 1994, faith@cs.unc.edu: Added internationalization
 *                           patches from Andries.Brouwer@cwi.nl
 * Wed Sep 14 22:31:17 1994: patches from Carl Christofferson
 *                           (cchris@connected.com)
 * 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
 * 	added Native Language Support
 * 1999-09-19 Bruno Haible <haible@clisp.cons.org>
 * 	modified to work correctly in multi-byte locales
 *
 */

/*
 * This command is deprecated.  The utility is in maintenance mode,
 * meaning we keep them in source tree for backward compatibility
 * only.  Do not waste time making this command better, unless the
 * fix is about security or other very critical issue.
 *
 * See Documentation/deprecated.txt for more information.
 */

#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>

#include "nls.h"
#include "xalloc.h"
#include "widechar.h"
#include "strutils.h"
#include "closestream.h"
#include "optutils.h"

#define	BS	'\b'		/* backspace */
#define	TAB	'\t'		/* tab */
#define	SPACE	' '		/* space */
#define	NL	'\n'		/* newline */
#define	CR	'\r'		/* carriage return */
#define	ESC	'\033'		/* escape */
#define	SI	'\017'		/* shift in to normal character set */
#define	SO	'\016'		/* shift out to alternate character set */
#define	VT	'\013'		/* vertical tab (aka reverse line feed) */
#define	RLF	'\007'		/* ESC-07 reverse line feed */
#define	RHLF	'\010'		/* ESC-010 reverse half-line feed */
#define	FHLF	'\011'		/* ESC-011 forward half-line feed */

/* build up at least this many lines before flushing them out */
#define	BUFFER_MARGIN		32

/* number of lines to allocate */
#define	NALLOC			64

typedef enum {
	CS_NORMAL,
	CS_ALTERNATE
} CSET;

typedef struct char_str {
	size_t		c_column;	/* column character is in */
	wchar_t		c_char;		/* character in question */
	int		c_width;	/* character width */
	CSET		c_set;		/* character set (currently only 2) */
} CHAR;

typedef struct line_str LINE;
struct line_str {
	CHAR	*l_line;		/* characters on the line */
	LINE	*l_prev;		/* previous line */
	LINE	*l_next;		/* next line */
	size_t	l_lsize;		/* allocated sizeof l_line */
	size_t	l_line_len;		/* strlen(l_line) */
	size_t	l_max_col;		/* max column in the line */
	unsigned int
		l_needs_sort:1;		/* set if chars went in out of order */
};

struct col_ctl {
	CSET last_set;			/* char_set of last char printed */
	LINE *lines;
	LINE *l;			/* current line */
	size_t max_bufd_lines;		/* max # lines to keep in memory */
	LINE *line_freelist;
	size_t nblank_lines;		/* # blanks after last flushed line */
	unsigned int
		compress_spaces:1,	/* if doing space -> tab conversion */
		fine:1,			/* if `fine' resolution (half lines) */
		no_backspaces:1,	/* if not to output any backspaces */
		pass_unknown_seqs:1;	/* whether to pass unknown control sequences */
};

struct col_lines {
	CHAR *c;
	CSET cur_set;
	wint_t ch;
	size_t adjust;
	size_t cur_col;
	ssize_t cur_line;
	size_t extra_lines;
	size_t max_line;
	size_t nflushd_lines;
	size_t this_line;
	unsigned int warned:1;
};

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;
	fprintf(out, _(
		"\nUsage:\n"
		" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Filter out reverse line feeds.\n"), out);

	fprintf(out, _(
		"\nOptions:\n"
		" -b, --no-backspaces    do not output backspaces\n"
		" -f, --fine             permit forward half line feeds\n"
		" -p, --pass             pass unknown control sequences\n"
		" -h, --tabs             convert spaces to tabs\n"
		" -x, --spaces           convert tabs to spaces\n"
		" -l, --lines NUM        buffer at least NUM lines\n"
		));
	printf( " -H, --help             %s\n", USAGE_OPTSTR_HELP);
	printf( " -V, --version          %s\n", USAGE_OPTSTR_VERSION);

	fputs(USAGE_SEPARATOR, out);
	fprintf(out, _(
		"%s reads from standard input and writes to standard output\n\n"),
		program_invocation_short_name);

	printf(USAGE_MAN_TAIL("col(1)"));
	exit(EXIT_SUCCESS);
}

static inline void col_putchar(wchar_t ch)
{
	if (putwchar(ch) == WEOF)
		errx(EXIT_FAILURE, _("write error"));
}

/*
 * Print a number of newline/half newlines.  If fine flag is set, nblank_lines
 * is the number of half line feeds, otherwise it is the number of whole line
 * feeds.
 */
static void flush_blanks(struct col_ctl *ctl)
{
	uint8_t half = 0;
	size_t i;

	if (ctl->nblank_lines & 1) {
		if (ctl->fine)
			half = 1;
		else
			ctl->nblank_lines++;
	}
	ctl->nblank_lines /= 2;
	for (i = ctl->nblank_lines; 0 < i; i--)
		col_putchar(NL);
	if (half) {
		col_putchar(ESC);
		col_putchar('9');
		if (!ctl->nblank_lines)
			col_putchar(CR);
	}
	ctl->nblank_lines = 0;
}

/*
 * Write a line to stdout taking care of space to tab conversion (-h flag)
 * and character set shifts.
 */
static void flush_line(struct col_ctl *ctl, LINE *l)
{
	CHAR *c, *endc;
	size_t nchars = l->l_line_len, last_col = 0, this_col;

	if (l->l_needs_sort) {
		static CHAR *sorted = NULL;
		static size_t count_size = 0, *count = NULL, sorted_size = 0;
		size_t i, tot;

		/*
		 * Do an O(n) sort on l->l_line by column being careful to
		 * preserve the order of characters in the same column.
		 */
		if (l->l_lsize > sorted_size) {
			sorted_size = l->l_lsize;
			sorted = xrealloc(sorted, sizeof(CHAR) * sorted_size);
		}
		if (l->l_max_col >= count_size) {
			count_size = l->l_max_col + 1;
			count = xrealloc((void *)count, sizeof(size_t) * count_size);
		}
		memset(count, 0, sizeof(size_t) * l->l_max_col + 1);
		for (i = nchars, c = l->l_line; c && 0 < i; i--, c++)
			count[c->c_column]++;

		/*
		 * calculate running total (shifted down by 1) to use as
		 * indices into new line.
		 */
		for (tot = 0, i = 0; i <= l->l_max_col; i++) {
			size_t save = count[i];
			count[i] = tot;
			tot += save;
		}

		for (i = nchars, c = l->l_line; 0 < i; i--, c++)
			sorted[count[c->c_column]++] = *c;
		c = sorted;
	} else
		c = l->l_line;
	while (nchars > 0) {
		this_col = c->c_column;
		endc = c;
		do {
			++endc;
		} while (--nchars > 0 && this_col == endc->c_column);

		/* if -b only print last character */
		if (ctl->no_backspaces) {
			c = endc - 1;
			if (nchars > 0 &&
			    this_col + c->c_width > endc->c_column)
				continue;
		}

		if (this_col > last_col) {
			ssize_t nspace = this_col - last_col;

			if (ctl->compress_spaces && nspace > 1) {
				ssize_t ntabs;

				ntabs = this_col / 8 - last_col / 8;
				if (ntabs > 0) {
					nspace = this_col & 7;
					while (--ntabs >= 0)
						col_putchar(TAB);
				}
			}
			while (--nspace >= 0)
				col_putchar(SPACE);
			last_col = this_col;
		}

		for (;;) {
			if (c->c_set != ctl->last_set) {
				switch (c->c_set) {
				case CS_NORMAL:
					col_putchar(SI);
					break;
				case CS_ALTERNATE:
					col_putchar(SO);
				}
				ctl->last_set = c->c_set;
			}
			col_putchar(c->c_char);
			if ((c + 1) < endc) {
				int i;
				for (i=0; i < c->c_width; i++)
					col_putchar(BS);
			}
			if (++c >= endc)
				break;
		}
		last_col += (c - 1)->c_width;
	}
}

static LINE *alloc_line(struct col_ctl *ctl)
{
	LINE *l;
	size_t i;

	if (!ctl->line_freelist) {
		l = xmalloc(sizeof(LINE) * NALLOC);
		ctl->line_freelist = l;
		for (i = 1; i < NALLOC; i++, l++)
			l->l_next = l + 1;
		l->l_next = NULL;
	}
	l = ctl->line_freelist;
	ctl->line_freelist = l->l_next;

	memset(l, 0, sizeof(LINE));
	return l;
}

static void free_line(struct col_ctl *ctl, LINE *l)
{
	l->l_next = ctl->line_freelist;
	ctl->line_freelist = l;
}

static void flush_lines(struct col_ctl *ctl, ssize_t nflush)
{
	LINE *l;

	while (--nflush >= 0) {
		l = ctl->lines;
		ctl->lines = l->l_next;
		if (l->l_line) {
			flush_blanks(ctl);
			flush_line(ctl, l);
		}
		ctl->nblank_lines++;
		free((void *)l->l_line);
		free_line(ctl, l);
	}
	if (ctl->lines)
		ctl->lines->l_prev = NULL;
}

static int handle_not_graphic(struct col_ctl *ctl, struct col_lines *lns)
{
	switch (lns->ch) {
	case BS:		/* can't go back further */
		if (lns->cur_col == 0)
			return 1;
		if (lns->c)
			lns->cur_col -= lns->c->c_width;
		else
			lns->cur_col -= 1;
		return 1;
	case CR:
		lns->cur_col = 0;
		return 1;
	case ESC:		/* just ignore EOF */
		switch (getwchar()) {
		case RLF:
			lns->cur_line -= 2;
			break;
		case RHLF:
			lns->cur_line -= 1;
			break;
		case FHLF:
			lns->cur_line += 1;
			if (0 < lns->cur_line && lns->max_line < (size_t)lns->cur_line)
				lns->max_line = lns->cur_line;
		}
		return 1;
	case NL:
		lns->cur_line += 2;
		if (0 < lns->cur_line && lns->max_line < (size_t)lns->cur_line)
			lns->max_line = lns->cur_line;
		lns->cur_col = 0;
		return 1;
	case SPACE:
		lns->cur_col += 1;
		return 1;
	case SI:
		lns->cur_set = CS_NORMAL;
		return 1;
	case SO:
		lns->cur_set = CS_ALTERNATE;
		return 1;
	case TAB:		/* adjust column */
		lns->cur_col |= 7;
		lns->cur_col += 1;
		return 1;
	case VT:
		lns->cur_line -= 2;
		return 1;
	}
	if (iswspace(lns->ch)) {
		if (wcwidth(lns->ch) > 0)
			lns->cur_col += wcwidth(lns->ch);
		return 1;
	}

	if (!ctl->pass_unknown_seqs)
		return 1;
	return 0;
}

static void update_cur_line(struct col_ctl *ctl, struct col_lines *lns)
{
	LINE *lnew;
	ssize_t nmove;

	lns->adjust = 0;
	nmove = lns->cur_line - lns->this_line;
	if (!ctl->fine) {
		/* round up to next line */
		if (lns->cur_line & 1) {
			lns->adjust = 1;
			nmove++;
		}
	}
	if (nmove < 0) {
		for (; nmove < 0 && ctl->l->l_prev; nmove++)
			ctl->l = ctl->l->l_prev;
		if (nmove) {
			if (lns->nflushd_lines == 0) {
				/*
				 * Allow backup past first line if nothing
				 * has been flushed yet.
				 */
				for (; nmove < 0; nmove++) {
					lnew = alloc_line(ctl);
					ctl->l->l_prev = lnew;
					lnew->l_next = ctl->l;
					ctl->l = ctl->lines = lnew;
					lns->extra_lines += 1;
				}
			} else {
				if (!lns->warned) {
					warnx(_("warning: can't back up %s."),
						  lns->cur_line < 0 ?
						    _("past first line") :
					            _("-- line already flushed"));
					lns->warned = 1;
				}
				lns->cur_line -= nmove;
			}
		}
	} else {
		/* may need to allocate here */
		for (; nmove > 0 && ctl->l->l_next; nmove--)
			ctl->l = ctl->l->l_next;
		for (; nmove > 0; nmove--) {
			lnew = alloc_line(ctl);
			lnew->l_prev = ctl->l;
			ctl->l->l_next = lnew;
			ctl->l = lnew;
		}
	}
	lns->this_line = lns->cur_line + lns->adjust;
	nmove = lns->this_line - lns->nflushd_lines;
	if (nmove > 0 && (size_t)nmove >= ctl->max_bufd_lines + BUFFER_MARGIN) {
		lns->nflushd_lines += nmove - ctl->max_bufd_lines;
		flush_lines(ctl, nmove - ctl->max_bufd_lines);
	}
}

static void parse_options(struct col_ctl *ctl, int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "no-backspaces", no_argument,		NULL, 'b' },
		{ "fine",	   no_argument,		NULL, 'f' },
		{ "pass",	   no_argument,		NULL, 'p' },
		{ "tabs",	   no_argument,		NULL, 'h' },
		{ "spaces",	   no_argument,		NULL, 'x' },
		{ "lines",	   required_argument,	NULL, 'l' },
		{ "version",	   no_argument,		NULL, 'V' },
		{ "help",	   no_argument,		NULL, 'H' },
		{ NULL, 0, NULL, 0 }
	};
	static const ul_excl_t excl[] = {
		{ 'h', 'x' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;
	int opt;

	while ((opt = getopt_long(argc, argv, "bfhl:pxVH", longopts, NULL)) != -1) {
		err_exclusive_options(opt, longopts, excl, excl_st);

		switch (opt) {
		case 'b':		/* do not output backspaces */
			ctl->no_backspaces = 1;
			break;
		case 'f':		/* allow half forward line feeds */
			ctl->fine = 1;
			break;
		case 'h':		/* compress spaces into tabs */
			ctl->compress_spaces = 1;
			break;
		case 'l':
			/*
			 * Buffered line count, which is a value in half
			 * lines e.g. twice the amount specified.
			 */
			ctl->max_bufd_lines = strtou32_or_err(optarg, _("bad -l argument")) * 2;
			break;
		case 'p':
			ctl->pass_unknown_seqs = 1;
			break;
		case 'x':		/* do not compress spaces into tabs */
			ctl->compress_spaces = 0;
			break;

		case 'V':
			print_version(EXIT_SUCCESS);
		case 'H':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		warnx(_("bad usage"));
		errtryhelp(EXIT_FAILURE);
	}
}

int main(int argc, char **argv)
{
	struct col_ctl ctl = {
		.compress_spaces = 1,
		.last_set = CS_NORMAL,
		.max_bufd_lines = BUFFER_MARGIN * 2,
	};
	struct col_lines lns = {
		.cur_set = CS_NORMAL,
	};
	int ret = EXIT_SUCCESS;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	ctl.lines = ctl.l = alloc_line(&ctl);

	parse_options(&ctl, argc, argv);

	while (feof(stdin) == 0) {
		errno = 0;
		if ((lns.ch = getwchar()) == WEOF) {
			if (errno == EILSEQ) {
				warn(_("failed on line %lu"), lns.max_line + 1);
				ret = EXIT_FAILURE;
			}
			break;
		}
		if (!iswgraph(lns.ch) &&
		    handle_not_graphic(&ctl, &lns))
			continue;

		/* Must stuff ch in a line - are we at the right one? */
		if ((size_t)lns.cur_line != lns.this_line - lns.adjust)
			update_cur_line(&ctl, &lns);

		/* grow line's buffer? */
		if (ctl.l->l_line_len + 1 >= ctl.l->l_lsize) {
			size_t need;

			need = ctl.l->l_lsize ? ctl.l->l_lsize * 2 : NALLOC;
			ctl.l->l_line = xrealloc(ctl.l->l_line, need * sizeof(CHAR));
			ctl.l->l_lsize = need;
		}
		lns.c = &ctl.l->l_line[ctl.l->l_line_len++];
		lns.c->c_char = lns.ch;
		lns.c->c_set = lns.cur_set;
		if (0 < lns.cur_col)
			lns.c->c_column = lns.cur_col;
		else
			lns.c->c_column = 0;
		lns.c->c_width = wcwidth(lns.ch);
		/*
		 * If things are put in out of order, they will need sorting
		 * when it is flushed.
		 */
		if (lns.cur_col < ctl.l->l_max_col)
			ctl.l->l_needs_sort = 1;
		else
			ctl.l->l_max_col = lns.cur_col;
		if (lns.c->c_width > 0)
			lns.cur_col += lns.c->c_width;
	}
	/* goto the last line that had a character on it */
	for (; ctl.l->l_next; ctl.l = ctl.l->l_next)
		lns.this_line++;
	if (lns.max_line == 0 && lns.cur_col == 0)
		return EXIT_SUCCESS;	/* no lines, so just exit */
	flush_lines(&ctl, lns.this_line - lns.nflushd_lines + lns.extra_lines + 1);

	/* make sure we leave things in a sane state */
	if (ctl.last_set != CS_NORMAL)
		col_putchar(SI);

	/* flush out the last few blank lines */
	ctl.nblank_lines = lns.max_line - lns.this_line;
	if (lns.max_line & 1)
		ctl.nblank_lines++;
	else if (!ctl.nblank_lines)
		/* missing a \n on the last line? */
		ctl.nblank_lines = 2;
	flush_blanks(&ctl);
	return ret;
}
