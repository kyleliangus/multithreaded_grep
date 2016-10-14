/* grep.c - main driver file for grep.
   Copyright (C) 1992, 1997-2002, 2004-2016 Free Software Foundation, Inc.
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Written July 1992 by Mike Haertel.  */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include "system.h"

#include "argmatch.h"
#include "c-ctype.h"
#include "closeout.h"
#include "colorize.h"
#include "error.h"
#include "exclude.h"
#include "exitfail.h"
#include "fcntl-safer.h"
#include "fts_.h"
#include "getopt.h"
#include "grep.h"
#include "intprops.h"
#include "progname.h"
#include "propername.h"
#include "quote.h"
#include "safe-read.h"
#include "search.h"
#include "version-etc.h"
#include "xalloc.h"
#include "xstrtol.h"

#define SEP_CHAR_SELECTED ':'
#define SEP_CHAR_REJECTED '-'
#define SEP_STR_GROUP    "--"

#define AUTHORS \
  proper_name ("Mike Haertel"), \
  _("others, see <http://git.sv.gnu.org/cgit/grep.git/tree/AUTHORS>")

/* When stdout is connected to a regular file, save its stat
   information here, so that we can automatically skip it, thus
   avoiding a potential (racy) infinite loop.  */
static struct stat out_stat;

/* if non-zero, display usage information and exit */
static int show_help;

/* Print the version on standard output and exit.  */
static bool show_version;

/* Suppress diagnostics for nonexistent or unreadable files.  */
static bool suppress_errors;

/* If nonzero, use color markers.  */
static int color_option;

/* Show only the part of a line matching the expression. */
static bool only_matching;

/* If nonzero, make sure first content char in a line is on a tab stop. */
static bool align_tabs;

struct grepctx
{
  /* Opaque value from compile(), passed to execute() */
  void *compiled_pattern;

  bool out_quiet;		/* Suppress all normal output. */

  /* Internal variables to keep track of byte count, context, etc. */
  uintmax_t totalcc;	/* Total character count before bufbeg. */
  char const *lastnl;	/* Pointer after last newline counted. */
  char *lastout;      /* Pointer after last character output;
                            NULL if no character has been output
                            or if it's conceptually before bufbeg. */
  intmax_t outleft;	/* Maximum number of lines to be output.  */
  intmax_t pending;	/* Pending lines of output.
                            Always kept 0 if out_quiet is true.  */
  bool done_on_match;	/* Stop scanning file on first match.  */

  /* True if output from the current input file has been suppressed
     because an output line had an encoding error.  */
  bool encoding_error_output;

  /* The input file name, or (if standard input) "-" or a --label argument.  */
  char const *filename;

  /* Hairy buffering mechanism for grep.  The intent is to keep
     all reads aligned on a page boundary and multiples of the
     page size, unless a read yields a partial page.  */
  char *buffer;		/* Base of buffer. */
  size_t bufalloc;		/* Allocated buffer size, counting slop. */
  int bufdesc;		/* File descriptor. */
  char *bufbeg;		/* Beginning of user-visible stuff. */
  char *buflim;		/* Limit of user-visible stuff. */
  off_t bufoffset;		/* Read offset; defined on regular files.  */
  off_t after_last_match;	/* Pointer after last matching line that
                              would have been output if we were
                              outputting characters. */
  bool skip_nuls;		/* Skip '\0' in data.  */
  bool seek_data_failed;	/* lseek with SEEK_DATA failed.  */
  uintmax_t totalnl;	/* Total newline count before lastnl. */

#if HAVE_ASAN
  /* Record the starting address and length of the sole poisoned region,
     so that we can unpoison it later, just before each following read.  */
  void const *poison_buf;
  size_t poison_len;
#endif
};

#if HAVE_ASAN
static void
clear_asan_poison (struct grepctx *ctx)
{
  if (ctx->poison_buf)
    __asan_unpoison_memory_region (ctx->poison_buf, ctx->poison_len);
}

static void
asan_poison (struct grepctx *ctx, void const *addr, size_t size)
{
  ctx->poison_buf = addr;
  ctx->poison_len = size;

  __asan_poison_memory_region (ctx->poison_buf, ctx->poison_len);
}
#else
static void clear_asan_poison (struct grepctx *ctx) { }
static void asan_poison (struct grepctx *ctx, void const volatile *addr,
                         size_t size) { }
#endif

/* The group separator used when context is requested. */
static const char *group_separator = SEP_STR_GROUP;

/* The context and logic for choosing default --color screen attributes
   (foreground and background colors, etc.) are the following.
      -- There are eight basic colors available, each with its own
         nominal luminosity to the human eye and foreground/background
         codes (black [0 %, 30/40], blue [11 %, 34/44], red [30 %, 31/41],
         magenta [41 %, 35/45], green [59 %, 32/42], cyan [70 %, 36/46],
         yellow [89 %, 33/43], and white [100 %, 37/47]).
      -- Sometimes, white as a background is actually implemented using
         a shade of light gray, so that a foreground white can be visible
         on top of it (but most often not).
      -- Sometimes, black as a foreground is actually implemented using
         a shade of dark gray, so that it can be visible on top of a
         background black (but most often not).
      -- Sometimes, more colors are available, as extensions.
      -- Other attributes can be selected/deselected (bold [1/22],
         underline [4/24], standout/inverse [7/27], blink [5/25], and
         invisible/hidden [8/28]).  They are sometimes implemented by
         using colors instead of what their names imply; e.g., bold is
         often achieved by using brighter colors.  In practice, only bold
         is really available to us, underline sometimes being mapped by
         the terminal to some strange color choice, and standout best
         being left for use by downstream programs such as less(1).
      -- We cannot assume that any of the extensions or special features
         are available for the purpose of choosing defaults for everyone.
      -- The most prevalent default terminal backgrounds are pure black
         and pure white, and are not necessarily the same shades of
         those as if they were selected explicitly with SGR sequences.
         Some terminals use dark or light pictures as default background,
         but those are covered over by an explicit selection of background
         color with an SGR sequence; their users will appreciate their
         background pictures not be covered like this, if possible.
      -- Some uses of colors attributes is to make some output items
         more understated (e.g., context lines); this cannot be achieved
         by changing the background color.
      -- For these reasons, the grep color defaults should strive not
         to change the background color from its default, unless it's
         for a short item that should be highlighted, not understated.
      -- The grep foreground color defaults (without an explicitly set
         background) should provide enough contrast to be readable on any
         terminal with either a black (dark) or white (light) background.
         This only leaves red, magenta, green, and cyan (and their bold
         counterparts) and possibly bold blue.  */
/* The color strings used for matched text.
   The user can overwrite them using the deprecated
   environment variable GREP_COLOR or the new GREP_COLORS.  */
static const char *selected_match_color = "01;31";	/* bold red */
static const char *context_match_color  = "01;31";	/* bold red */

/* Other colors.  Defaults look damn good.  */
static const char *filename_color = "35";	/* magenta */
static const char *line_num_color = "32";	/* green */
static const char *byte_num_color = "32";	/* green */
static const char *sep_color      = "36";	/* cyan */
static const char *selected_line_color = "";	/* default color pair */
static const char *context_line_color  = "";	/* default color pair */

/* Select Graphic Rendition (SGR, "\33[...m") strings.  */
/* Also Erase in Line (EL) to Right ("\33[K") by default.  */
/*    Why have EL to Right after SGR?
         -- The behavior of line-wrapping when at the bottom of the
            terminal screen and at the end of the current line is often
            such that a new line is introduced, entirely cleared with
            the current background color which may be different from the
            default one (see the boolean back_color_erase terminfo(5)
            capability), thus scrolling the display by one line.
            The end of this new line will stay in this background color
            even after reverting to the default background color with
            "\33[m', unless it is explicitly cleared again with "\33[K"
            (which is the behavior the user would instinctively expect
            from the whole thing).  There may be some unavoidable
            background-color flicker at the end of this new line because
            of this (when timing with the monitor's redraw is just right).
         -- The behavior of HT (tab, "\t") is usually the same as that of
            Cursor Forward Tabulation (CHT) with a default parameter
            of 1 ("\33[I"), i.e., it performs pure movement to the next
            tab stop, without any clearing of either content or screen
            attributes (including background color); try
               printf 'asdfqwerzxcv\rASDF\tZXCV\n'
            in a bash(1) shell to demonstrate this.  This is not what the
            user would instinctively expect of HT (but is ok for CHT).
            The instinctive behavior would include clearing the terminal
            cells that are skipped over by HT with blank cells in the
            current screen attributes, including background color;
            the boolean dest_tabs_magic_smso terminfo(5) capability
            indicates this saner behavior for HT, but only some rare
            terminals have it (although it also indicates a special
            glitch with standout mode in the Teleray terminal for which
            it was initially introduced).  The remedy is to add "\33K"
            after each SGR sequence, be it START (to fix the behavior
            of any HT after that before another SGR) or END (to fix the
            behavior of an HT in default background color that would
            follow a line-wrapping at the bottom of the screen in another
            background color, and to complement doing it after START).
            Piping grep's output through a pager such as less(1) avoids
            any HT problems since the pager performs tab expansion.
      Generic disadvantages of this remedy are:
         -- Some very rare terminals might support SGR but not EL (nobody
            will use "grep --color" on a terminal that does not support
            SGR in the first place).
         -- Having these extra control sequences might somewhat complicate
            the task of any program trying to parse "grep --color"
            output in order to extract structuring information from it.
      A specific disadvantage to doing it after SGR START is:
         -- Even more possible background color flicker (when timing
            with the monitor's redraw is just right), even when not at the
            bottom of the screen.
      There are no additional disadvantages specific to doing it after
      SGR END.
      It would be impractical for GNU grep to become a full-fledged
      terminal program linked against ncurses or the like, so it will
      not detect terminfo(5) capabilities.  */
static const char *sgr_start = "\33[%sm\33[K";
static const char *sgr_end   = "\33[m\33[K";

/* SGR utility functions.  */
static void
pr_sgr_start (char const *s)
{
  if (*s)
    print_start_colorize (sgr_start, s);
}
static void
pr_sgr_end (char const *s)
{
  if (*s)
    print_end_colorize (sgr_end);
}
static void
pr_sgr_start_if (char const *s)
{
  if (color_option)
    pr_sgr_start (s);
}
static void
pr_sgr_end_if (char const *s)
{
  if (color_option)
    pr_sgr_end (s);
}

struct color_cap
  {
    const char *name;
    const char **var;
    void (*fct) (void);
  };

static void
color_cap_mt_fct (void)
{
  /* Our caller just set selected_match_color.  */
  context_match_color = selected_match_color;
}

static void
color_cap_rv_fct (void)
{
  /* By this point, it was 1 (or already -1).  */
  color_option = -1;  /* That's still != 0.  */
}

static void
color_cap_ne_fct (void)
{
  sgr_start = "\33[%sm";
  sgr_end   = "\33[m";
}

/* For GREP_COLORS.  */
static const struct color_cap color_dict[] =
  {
    { "mt", &selected_match_color, color_cap_mt_fct }, /* both ms/mc */
    { "ms", &selected_match_color, NULL }, /* selected matched text */
    { "mc", &context_match_color,  NULL }, /* context matched text */
    { "fn", &filename_color,       NULL }, /* filename */
    { "ln", &line_num_color,       NULL }, /* line number */
    { "bn", &byte_num_color,       NULL }, /* byte (sic) offset */
    { "se", &sep_color,            NULL }, /* separator */
    { "sl", &selected_line_color,  NULL }, /* selected lines */
    { "cx", &context_line_color,   NULL }, /* context lines */
    { "rv", NULL,                  color_cap_rv_fct }, /* -v reverses sl/cx */
    { "ne", NULL,                  color_cap_ne_fct }, /* no EL on SGR_* */
    { NULL, NULL,                  NULL }
  };

/* Saved errno value from failed output functions on stdout.  */
static int stdout_errno;

static void
putchar_errno (int c)
{
  if (putchar (c) < 0)
    stdout_errno = errno;
}

static void
fputs_errno (char const *s)
{
  if (fputs (s, stdout) < 0)
    stdout_errno = errno;
}

static void _GL_ATTRIBUTE_FORMAT_PRINTF (1, 2)
printf_errno (char const *format, ...)
{
  va_list ap;
  va_start (ap, format);
  if (vfprintf (stdout, format, ap) < 0)
    stdout_errno = errno;
  va_end (ap);
}

static void
fwrite_errno (void const *ptr, size_t size, size_t nmemb)
{
  if (fwrite (ptr, size, nmemb, stdout) != nmemb)
    stdout_errno = errno;
}

static void
fflush_errno (void)
{
  if (fflush (stdout) != 0)
    stdout_errno = errno;
}

static struct exclude *excluded_patterns[2];
static struct exclude *excluded_directory_patterns[2];
/* Short options.  */
static char const short_options[] =
"0123456789A:B:C:D:EFGHIM::PTUVX:abcd:e:f:hiLlm:noqRrsuvwxyZz";

/* Non-boolean long options that have no corresponding short equivalents.  */
enum
{
  BINARY_FILES_OPTION = CHAR_MAX + 1,
  COLOR_OPTION,
  EXCLUDE_DIRECTORY_OPTION,
  EXCLUDE_OPTION,
  EXCLUDE_FROM_OPTION,
  GROUP_SEPARATOR_OPTION,
  INCLUDE_OPTION,
  LINE_BUFFERED_OPTION,
  LABEL_OPTION
};

/* Long options equivalences. */
static struct option const long_options[] =
{
  {"basic-regexp",    no_argument, NULL, 'G'},
  {"extended-regexp", no_argument, NULL, 'E'},
  {"fixed-regexp",    no_argument, NULL, 'F'},
  {"fixed-strings",   no_argument, NULL, 'F'},
  {"perl-regexp",     no_argument, NULL, 'P'},
  {"after-context", required_argument, NULL, 'A'},
  {"before-context", required_argument, NULL, 'B'},
  {"binary-files", required_argument, NULL, BINARY_FILES_OPTION},
  {"byte-offset", no_argument, NULL, 'b'},
  {"context", required_argument, NULL, 'C'},
  {"color", optional_argument, NULL, COLOR_OPTION},
  {"colour", optional_argument, NULL, COLOR_OPTION},
  {"count", no_argument, NULL, 'c'},
  {"devices", required_argument, NULL, 'D'},
  {"directories", required_argument, NULL, 'd'},
  {"exclude", required_argument, NULL, EXCLUDE_OPTION},
  {"exclude-from", required_argument, NULL, EXCLUDE_FROM_OPTION},
  {"exclude-dir", required_argument, NULL, EXCLUDE_DIRECTORY_OPTION},
  {"file", required_argument, NULL, 'f'},
  {"files-with-matches", no_argument, NULL, 'l'},
  {"files-without-match", no_argument, NULL, 'L'},
  {"group-separator", required_argument, NULL, GROUP_SEPARATOR_OPTION},
  {"help", no_argument, &show_help, 1},
  {"include", required_argument, NULL, INCLUDE_OPTION},
  {"ignore-case", no_argument, NULL, 'i'},
  {"initial-tab", no_argument, NULL, 'T'},
  {"label", required_argument, NULL, LABEL_OPTION},
  {"line-buffered", no_argument, NULL, LINE_BUFFERED_OPTION},
  {"line-number", no_argument, NULL, 'n'},
  {"line-regexp", no_argument, NULL, 'x'},
  {"max-count", required_argument, NULL, 'm'},
  {"parallel", optional_argument, NULL, 'M'},

  {"no-filename", no_argument, NULL, 'h'},
  {"no-group-separator", no_argument, NULL, GROUP_SEPARATOR_OPTION},
  {"no-messages", no_argument, NULL, 's'},
  {"null", no_argument, NULL, 'Z'},
  {"null-data", no_argument, NULL, 'z'},
  {"only-matching", no_argument, NULL, 'o'},
  {"quiet", no_argument, NULL, 'q'},
  {"recursive", no_argument, NULL, 'r'},
  {"dereference-recursive", no_argument, NULL, 'R'},
  {"regexp", required_argument, NULL, 'e'},
  {"invert-match", no_argument, NULL, 'v'},
  {"silent", no_argument, NULL, 'q'},
  {"text", no_argument, NULL, 'a'},
  {"binary", no_argument, NULL, 'U'},
  {"unix-byte-offsets", no_argument, NULL, 'u'},
  {"version", no_argument, NULL, 'V'},
  {"with-filename", no_argument, NULL, 'H'},
  {"word-regexp", no_argument, NULL, 'w'},
  {0, 0, 0, 0}
};

/* Define flags declared in grep.h. */
bool match_icase;
bool match_words;
bool match_lines;
char eolbyte;

static char const *matcher;

static int out_file;		/* Print filenames. */
static int out_quiet;
static int done_on_match;

/* For error messages. */
/* Omit leading "./" from file names in diagnostics.  */
static bool omit_dot_slash;
static bool errseen;

enum directories_type
  {
    READ_DIRECTORIES = 2,
    RECURSE_DIRECTORIES,
    SKIP_DIRECTORIES
  };

/* How to handle directories.  */
static char const *const directories_args[] =
{
  "read", "recurse", "skip", NULL
};
static enum directories_type const directories_types[] =
{
  READ_DIRECTORIES, RECURSE_DIRECTORIES, SKIP_DIRECTORIES
};
ARGMATCH_VERIFY (directories_args, directories_types);

static enum directories_type directories = READ_DIRECTORIES;

enum { basic_fts_options = FTS_CWDFD | FTS_NOSTAT | FTS_TIGHT_CYCLE_CHECK };
static int fts_options = basic_fts_options | FTS_COMFOLLOW | FTS_PHYSICAL;

/* How to handle devices. */
static enum
  {
    READ_COMMAND_LINE_DEVICES,
    READ_DEVICES,
    SKIP_DEVICES
  } devices = READ_COMMAND_LINE_DEVICES;

static void search_file (int, char const *, char const *, bool, bool);

static void dos_binary (void);
static void dos_unix_byte_offsets (void);
static size_t undossify_input (struct grepctx *, char *, size_t);

static bool
is_device_mode (mode_t m)
{
  return S_ISCHR (m) || S_ISBLK (m) || S_ISSOCK (m) || S_ISFIFO (m);
}

static bool
skip_devices (bool command_line)
{
  return (devices == SKIP_DEVICES
          || (devices == READ_COMMAND_LINE_DEVICES && !command_line));
}

/* Return if ST->st_size is defined.  Assume the file is not a
   symbolic link.  */
static bool
usable_st_size (struct stat const *st)
{
  return S_ISREG (st->st_mode) || S_TYPEISSHM (st) || S_TYPEISTMO (st);
}

/* Lame substitutes for SEEK_DATA and SEEK_HOLE on platforms lacking them.
   Do not rely on these finding data or holes if they equal SEEK_SET.  */
#ifndef SEEK_DATA
enum { SEEK_DATA = SEEK_SET };
#endif
#ifndef SEEK_HOLE
enum { SEEK_HOLE = SEEK_SET };
#endif

/* Functions we'll use to search. */
typedef void *(*compile_fp_t) (char const *, size_t);
typedef size_t (*execute_fp_t) (void *, struct grepctx *, char *, size_t,
                                size_t *, char const *);
static compile_fp_t compile;
static execute_fp_t execute;

static pthread_mutex_t output_lock;

static void
lock_output (void)
{
  if (pthread_mutex_lock (&output_lock))
    abort ();
}

static void
unlock_output (void)
{
  if (pthread_mutex_unlock (&output_lock))
    abort ();
}

/* Thread-safe error() */
#define ts_error(s, e, f, ...) do { \
    lock_output ();                 \
    error (s, e, f, ##__VA_ARGS__); \
    unlock_output ();               \
  } while (0)

/* Like error, but suppress the diagnostic if requested.  */
static void
suppressible_error (char const *mesg, int errnum)
{
  if (! suppress_errors)
    ts_error (0, errnum, "%s", mesg);
  errseen = true;
}

/* If there has already been a write error, don't bother closing
   standard output, as that might elicit a duplicate diagnostic.  */
static void
clean_up_stdout (void)
{
  if (! stdout_errno)
    close_stdout ();
}

/* A cast to TYPE of VAL.  Use this when TYPE is a pointer type, VAL
   is properly aligned for TYPE, and 'gcc -Wcast-align' cannot infer
   the alignment and would otherwise complain about the cast.  */
#if 4 < __GNUC__ + (6 <= __GNUC_MINOR__)
# define CAST_ALIGNED(type, val)                           \
    ({ __typeof__ (val) val_ = val;                        \
       _Pragma ("GCC diagnostic push")                     \
       _Pragma ("GCC diagnostic ignored \"-Wcast-align\"") \
       (type) val_;                                        \
       _Pragma ("GCC diagnostic pop")                      \
    })
#else
# define CAST_ALIGNED(type, val) ((type) (val))
#endif

/* An unsigned type suitable for fast matching.  */
typedef uintmax_t uword;

/* A mask to test for unibyte characters, with the pattern repeated to
   fill a uword.  For a multibyte character encoding where
   all bytes are unibyte characters, this is 0.  For UTF-8, this is
   0x808080....  For encodings where unibyte characters have no discerned
   pattern, this is all 1s.  The unsigned char C is a unibyte
   character if C & UNIBYTE_MASK is zero.  If the uword W is the
   concatenation of bytes, the bytes are all unibyte characters
   if W & UNIBYTE_MASK is zero.  */
static uword unibyte_mask;

static void
initialize_unibyte_mask (void)
{
  /* For each encoding error I that MASK does not already match,
     accumulate I's most significant 1 bit by ORing it into MASK.
     Although any 1 bit of I could be used, in practice high-order
     bits work better.  */
  unsigned char mask = 0;
  int ms1b = 1;
  for (int i = 1; i <= UCHAR_MAX; i++)
    if (mbclen_cache[i] != 1 && ! (mask & i))
      {
        while (ms1b * 2 <= i)
          ms1b *= 2;
        mask |= ms1b;
      }

  /* Now MASK will detect any encoding-error byte, although it may
     cry wolf and it may not be optimal.  Build a uword-length mask by
     repeating MASK.  */
  uword uword_max = -1;
  unibyte_mask = uword_max / UCHAR_MAX * mask;
}

/* Skip the easy bytes in a buffer that is guaranteed to have a sentinel
   that is not easy, and return a pointer to the first non-easy byte.
   The easy bytes all have UNIBYTE_MASK off.  */
static char const * _GL_ATTRIBUTE_PURE
skip_easy_bytes (char const *buf)
{
  /* Search a byte at a time until the pointer is aligned, then a
     uword at a time until a match is found, then a byte at a time to
     identify the exact byte.  The uword search may go slightly past
     the buffer end, but that's benign.  */
  char const *p;
  uword const *s;
  for (p = buf; (uintptr_t) p % sizeof (uword) != 0; p++)
    if (to_uchar (*p) & unibyte_mask)
      return p;
  for (s = CAST_ALIGNED (uword const *, p); ! (*s & unibyte_mask); s++)
    continue;
  for (p = (char const *) s; ! (to_uchar (*p) & unibyte_mask); p++)
    continue;
  return p;
}

/* Return true if BUF, of size SIZE, has an encoding error.
   BUF must be followed by at least sizeof (uword) bytes,
   the first of which may be modified.  */
bool
buf_has_encoding_errors (char *buf, size_t size)
{
  if (! unibyte_mask)
    return false;

  mbstate_t mbs = { 0 };
  size_t clen;

  buf[size] = -1;
  for (char const *p = buf; (p = skip_easy_bytes (p)) < buf + size; p += clen)
    {
      clen = mbrlen (p, buf + size - p, &mbs);
      if ((size_t) -2 <= clen)
        return true;
    }

  return false;
}


/* Return true if BUF, of size SIZE, has a null byte.
   BUF must be followed by at least one byte,
   which may be arbitrarily written to or read from.  */
static bool
buf_has_nulls (char *buf, size_t size)
{
  buf[size] = 0;
  return strlen (buf) != size;
}

/* Return true if a file is known to contain null bytes.
   SIZE bytes have already been read from the file
   with descriptor FD and status ST.  */
static bool
file_must_have_nulls (struct grepctx *ctx, size_t size, int fd,
                      struct stat const *st)
{
  if (usable_st_size (st))
    {
      if (st->st_size <= size)
        return false;

      /* If the file has holes, it must contain a null byte somewhere.  */
      if (SEEK_HOLE != SEEK_SET)
        {
          off_t cur = size;
          if (O_BINARY || fd == STDIN_FILENO)
            {
              cur = lseek (fd, 0, SEEK_CUR);
              if (cur < 0)
                return false;
            }

          /* Look for a hole after the current location.  */
          off_t hole_start = lseek (fd, cur, SEEK_HOLE);
          if (0 <= hole_start)
            {
              if (lseek (fd, cur, SEEK_SET) < 0)
                suppressible_error (ctx->filename, errno);
              if (hole_start < st->st_size)
                return true;
            }
        }
    }

  return false;
}

/* Convert STR to a nonnegative integer, storing the result in *OUT.
   STR must be a valid context length argument; report an error if it
   isn't.  Silently ceiling *OUT at the maximum value, as that is
   practically equivalent to infinity for grep's purposes.  */
static void
context_length_arg (char const *str, intmax_t *out)
{
  switch (xstrtoimax (str, 0, 10, out, ""))
    {
    case LONGINT_OK:
    case LONGINT_OVERFLOW:
      if (0 <= *out)
        break;
      /* Fall through.  */
    default:
      ts_error (EXIT_TROUBLE, 0, "%s: %s", str,
                _("invalid context length argument"));
    }
}

/* Return the add_exclude options suitable for excluding a file name.
   If COMMAND_LINE, it is a command-line file name.  */
static int
exclude_options (bool command_line)
{
  return EXCLUDE_WILDCARDS | (command_line ? 0 : EXCLUDE_ANCHORED);
}

/* Return true if the file with NAME should be skipped.
   If COMMAND_LINE, it is a command-line argument.
   If IS_DIR, it is a directory.  */
static bool
skipped_file (char const *name, bool command_line, bool is_dir)
{
  struct exclude **pats;
  if (! is_dir)
    pats = excluded_patterns;
  else if (directories == SKIP_DIRECTORIES)
    return true;
  else if (command_line && omit_dot_slash)
    return false;
  else
    pats = excluded_directory_patterns;
  return pats[command_line] && excluded_file_name (pats[command_line], name);
}

/* Hairy buffering mechanism for grep.  The intent is to keep
   all reads aligned on a page boundary and multiples of the
   page size, unless a read yields a partial page.  */

#define INITIAL_BUFSIZE 32768	/* Initial buffer size, not counting slop. */
static size_t pagesize;		/* alignment of memory pages */
static bool skip_empty_lines;	/* Skip empty lines in data.  */

/* Return VAL aligned to the next multiple of ALIGNMENT.  VAL can be
   an integer or a pointer.  Both args must be free of side effects.  */
#define ALIGN_TO(val, alignment) \
  ((size_t) (val) % (alignment) == 0 \
   ? (val) \
   : (val) + ((alignment) - (size_t) (val) % (alignment)))

/* Add two numbers that count input bytes or lines, and report an
   error if the addition overflows.  */
static uintmax_t
add_count (uintmax_t a, uintmax_t b)
{
  uintmax_t sum = a + b;
  if (sum < a)
    ts_error (EXIT_TROUBLE, 0, _("input is too large to count"));
  return sum;
}

/* Return true if BUF (of size SIZE) is all zeros.  */
static bool
all_zeros (char const *buf, size_t size)
{
  for (char const *p = buf; p < buf + size; p++)
    if (*p)
      return false;
  return true;
}

/* Reset the buffer for a new file, returning false if we should skip it.
   Initialize on the first time through. */
static bool
reset (struct grepctx *ctx, int fd, struct stat const *st)
{
  ctx->bufbeg = ctx->buflim = ALIGN_TO (ctx->buffer + 1, pagesize);
  ctx->bufbeg[-1] = eolbyte;
  ctx->bufdesc = fd;

  if (S_ISREG (st->st_mode))
    {
      if (fd != STDIN_FILENO)
        ctx->bufoffset = 0;
      else
        {
          ctx->bufoffset = lseek (fd, 0, SEEK_CUR);
          if (ctx->bufoffset < 0)
            {
              suppressible_error (_("lseek failed"), errno);
              return false;
            }
        }
    }
  return true;
}

/* Read new stuff into the buffer, saving the specified
   amount of old stuff.  When we're done, 'bufbeg' points
   to the beginning of the buffer contents, and 'buflim'
   points just after the end.  Return false if there's an error.  */
static bool
fillbuf (struct grepctx *ctx, size_t save, struct stat const *st)
{
  size_t fillsize;
  bool cc = true;
  char *readbuf;
  size_t readsize;

  /* Offset from start of buffer to start of old stuff
     that we want to save.  */
  size_t saved_offset = ctx->buflim - save - ctx->buffer;

  if (pagesize <= ctx->buffer + ctx->bufalloc - sizeof (uword) - ctx->buflim)
    {
      readbuf = ctx->buflim;
      ctx->bufbeg = ctx->buflim - save;
    }
  else
    {
      size_t minsize = save + pagesize;
      size_t newsize;
      size_t newalloc;
      char *newbuf;

      /* Grow newsize until it is at least as great as minsize.  */
      for (newsize = ctx->bufalloc - pagesize - sizeof (uword);
           newsize < minsize;
           newsize *= 2)
        if ((SIZE_MAX - pagesize - sizeof (uword)) / 2 < newsize)
          xalloc_die ();

      /* Try not to allocate more memory than the file size indicates,
         as that might cause unnecessary memory exhaustion if the file
         is large.  However, do not use the original file size as a
         heuristic if we've already read past the file end, as most
         likely the file is growing.  */
      if (usable_st_size (st))
        {
          off_t to_be_read = st->st_size - ctx->bufoffset;
          off_t maxsize_off = save + to_be_read;
          if (0 <= to_be_read && to_be_read <= maxsize_off
              && maxsize_off == (size_t) maxsize_off
              && minsize <= (size_t) maxsize_off
              && (size_t) maxsize_off < newsize)
            newsize = maxsize_off;
        }

      /* Add enough room so that the buffer is aligned and has room
         for byte sentinels fore and aft, and so that a uword can
         be read aft.  */
      newalloc = newsize + pagesize + sizeof (uword);

      newbuf = ctx->bufalloc < newalloc ?
        xmalloc (ctx->bufalloc = newalloc) : ctx->buffer;
      readbuf = ALIGN_TO (newbuf + 1 + save, pagesize);
      ctx->bufbeg = readbuf - save;
      memmove (ctx->bufbeg, ctx->buffer + saved_offset, save);
      ctx->bufbeg[-1] = eolbyte;
      if (newbuf != ctx->buffer)
        {
          free (ctx->buffer);
          ctx->buffer = newbuf;
        }
    }

  clear_asan_poison (ctx);

  readsize = ctx->buffer + ctx->bufalloc - sizeof (uword) - readbuf;
  readsize -= readsize % pagesize;

  while (true)
    {
      fillsize = safe_read (ctx->bufdesc, readbuf, readsize);
      if (fillsize == SAFE_READ_ERROR)
        {
          fillsize = 0;
          cc = false;
        }
      ctx->bufoffset += fillsize;

      if (fillsize == 0 || !ctx->skip_nuls || !all_zeros (readbuf, fillsize))
        break;
      ctx->totalnl = add_count (ctx->totalnl, fillsize);

      if (SEEK_DATA != SEEK_SET && !ctx->seek_data_failed)
        {
          /* Solaris SEEK_DATA fails with errno == ENXIO in a hole at EOF.  */
          off_t data_start = lseek (ctx->bufdesc, ctx->bufoffset, SEEK_DATA);
          if (data_start < 0 && errno == ENXIO
              && usable_st_size (st) && ctx->bufoffset < st->st_size)
            data_start = lseek (ctx->bufdesc, 0, SEEK_END);

          if (data_start < 0)
            ctx->seek_data_failed = true;
          else
            {
              ctx->totalnl = add_count (ctx->totalnl,
                                        data_start - ctx->bufoffset);
              ctx->bufoffset = data_start;
            }
        }
    }

  fillsize = undossify_input (ctx, readbuf, fillsize);
  ctx->buflim = readbuf + fillsize;

  /* Initialize the following word, because skip_easy_bytes and some
     matchers read (but do not use) those bytes.  This avoids false
     positive reports of these bytes being used uninitialized.  */
  memset (ctx->buflim, 0, sizeof (uword));

  /* Mark the part of the buffer not filled by the read or set by
     the above memset call as ASAN-poisoned.  */
  asan_poison (ctx, ctx->buflim + sizeof (uword),
               ctx->bufalloc - (ctx->buflim - ctx->buffer) - sizeof (uword));

  return cc;
}

/* Flags controlling the style of output. */
static enum
{
  BINARY_BINARY_FILES,
  TEXT_BINARY_FILES,
  WITHOUT_MATCH_BINARY_FILES
} binary_files;		/* How to handle binary files.  */

/* Options for output as a list of matching/non-matching files */
static enum
{
  LISTFILES_NONE,
  LISTFILES_MATCHING,
  LISTFILES_NONMATCHING,
} list_files;

static int filename_mask;	/* If zero, output nulls after filenames.  */
static bool out_invert;		/* Print nonmatching stuff. */
static bool out_line;		/* Print line numbers. */
static bool out_byte;		/* Print byte offsets. */
static intmax_t out_before;	/* Lines of leading context. */
static intmax_t out_after;	/* Lines of trailing context. */
static bool count_matches;	/* Count matching lines.  */
static bool no_filenames;	/* Suppress file names.  */
static intmax_t max_count;	/* Stop after outputting this many
                                   lines from an input file.  */
static bool line_buffered;	/* Use line buffering.  */
static char *label = NULL;      /* Fake filename for stdin */

static bool exit_on_match;	/* Exit on first match.  */

#include "dosbuf.c"

static void
nlscan (struct grepctx *ctx, char const *lim)
{
  size_t newlines = 0;
  char const *beg;
  for (beg = ctx->lastnl; beg < lim; beg++)
    {
      beg = memchr (beg, eolbyte, lim - beg);
      if (!beg)
        break;
      newlines++;
    }
  ctx->totalnl = add_count (ctx->totalnl, newlines);
  ctx->lastnl = lim;
}

/* Print the current filename.  */
static void
print_filename (struct grepctx *ctx)
{
  pr_sgr_start_if (filename_color);
  fputs_errno (ctx->filename);
  pr_sgr_end_if (filename_color);
}

/* Print a character separator.  */
static void
print_sep (char sep)
{
  pr_sgr_start_if (sep_color);
  putchar_errno (sep);
  pr_sgr_end_if (sep_color);
}

/* Print a line number or a byte offset.  */
static void
print_offset (uintmax_t pos, int min_width, const char *color)
{
  /* Do not rely on printf to print pos, since uintmax_t may be longer
     than long, and long long is not portable.  */

  char buf[sizeof pos * CHAR_BIT];
  char *p = buf + sizeof buf;

  do
    {
      *--p = '0' + pos % 10;
      --min_width;
    }
  while ((pos /= 10) != 0);

  /* Do this to maximize the probability of alignment across lines.  */
  if (align_tabs)
    while (--min_width >= 0)
      *--p = ' ';

  pr_sgr_start_if (color);
  fwrite_errno (p, 1, buf + sizeof buf - p);
  pr_sgr_end_if (color);
}

/* Print a whole line head (filename, line, byte).  The output data
   starts at BEG and contains LEN bytes; it is followed by at least
   sizeof (uword) bytes, the first of which may be temporarily modified.
   The output data comes from what is perhaps a larger input line that
   goes until LIM, where LIM[-1] is an end-of-line byte.  Use SEP as
   the separator on output.
   Return true unless the line was suppressed due to an encoding error.  */

static bool
print_line_head (struct grepctx *ctx, char *beg, size_t len, char const *lim,
                 char sep)
{
  bool encoding_errors = false;
  if (binary_files != TEXT_BINARY_FILES)
    {
      char ch = beg[len];
      encoding_errors = buf_has_encoding_errors (beg, len);
      beg[len] = ch;
    }
  if (encoding_errors)
    {
      ctx->encoding_error_output = ctx->done_on_match = ctx->out_quiet = true;
      return false;
    }

  bool pending_sep = false;

  if (out_file)
    {
      print_filename (ctx);
      if (filename_mask)
        pending_sep = true;
      else
        putchar_errno (0);
    }

  if (out_line)
    {
      if (ctx->lastnl < lim)
        {
          nlscan (ctx, beg);
          ctx->totalnl = add_count (ctx->totalnl, 1);
          ctx->lastnl = lim;
        }
      if (pending_sep)
        print_sep (sep);
      print_offset (ctx->totalnl, 4, line_num_color);
      pending_sep = true;
    }

  if (out_byte)
    {
      uintmax_t pos = add_count (ctx->totalcc, beg - ctx->bufbeg);
      pos = dossified_pos (pos);
      if (pending_sep)
        print_sep (sep);
      print_offset (pos, 6, byte_num_color);
      pending_sep = true;
    }

  if (pending_sep)
    {
      /* This assumes sep is one column wide.
         Try doing this any other way with Unicode
         (and its combining and wide characters)
         filenames and you're wasting your efforts.  */
      if (align_tabs)
        fputs_errno ("\t\b");

      print_sep (sep);
    }

  return true;
}

static char *
print_line_middle (struct grepctx *ctx, char *beg, char *lim,
                   const char *line_color, const char *match_color)
{
  size_t match_size;
  size_t match_offset;
  char *cur;
  char *mid = NULL;
  char *b;

  for (cur = beg;
       (cur < lim
        && ((match_offset = execute (ctx->compiled_pattern, ctx, beg, lim - beg,
                                     &match_size, cur)) != (size_t) -1));
       cur = b + match_size)
    {
      b = beg + match_offset;

      /* Avoid matching the empty line at the end of the buffer. */
      if (b == lim)
        break;

      /* Avoid hanging on grep --color "" foo */
      if (match_size == 0)
        {
          /* Make minimal progress; there may be further non-empty matches.  */
          /* XXX - Could really advance by one whole multi-octet character.  */
          match_size = 1;
          if (!mid)
            mid = cur;
        }
      else
        {
          /* This function is called on a matching line only,
             but is it selected or rejected/context?  */
          if (only_matching)
            {
              char sep = out_invert ? SEP_CHAR_REJECTED : SEP_CHAR_SELECTED;
              if (! print_line_head (ctx, b, match_size, lim, sep))
                return NULL;
            }
          else
            {
              pr_sgr_start (line_color);
              if (mid)
                {
                  cur = mid;
                  mid = NULL;
                }
              fwrite_errno (cur, 1, b - cur);
            }

          pr_sgr_start_if (match_color);
          fwrite_errno (b, 1, match_size);
          pr_sgr_end_if (match_color);
          if (only_matching)
            putchar_errno (eolbyte);
        }
    }

  if (only_matching)
    cur = lim;
  else if (mid)
    cur = mid;

  return cur;
}

static char *
print_line_tail (char *beg, const char *lim, const char *line_color)
{
  size_t eol_size;
  size_t tail_size;

  eol_size   = (lim > beg && lim[-1] == eolbyte);
  eol_size  += (lim - eol_size > beg && lim[-(1 + eol_size)] == '\r');
  tail_size  =  lim - eol_size - beg;

  if (tail_size > 0)
    {
      pr_sgr_start (line_color);
      fwrite_errno (beg, 1, tail_size);
      beg += tail_size;
      pr_sgr_end (line_color);
    }

  return beg;
}

static void
prline (struct grepctx *ctx, char *beg, char *lim, char sep)
{
  bool matching;
  const char *line_color;
  const char *match_color;

  if (!only_matching)
    if (! print_line_head (ctx, beg, lim - beg - 1, lim, sep))
      return;

  matching = (sep == SEP_CHAR_SELECTED) ^ out_invert;

  if (color_option)
    {
      line_color = (((sep == SEP_CHAR_SELECTED)
                     ^ (out_invert && (color_option < 0)))
                    ? selected_line_color  : context_line_color);
      match_color = (sep == SEP_CHAR_SELECTED
                     ? selected_match_color : context_match_color);
    }
  else
    line_color = match_color = NULL; /* Shouldn't be used.  */

  if ((only_matching && matching)
      || (color_option && (*line_color || *match_color)))
    {
      /* We already know that non-matching lines have no match (to colorize). */
      if (matching && (only_matching || *match_color))
        {
          beg = print_line_middle (ctx, beg, lim, line_color, match_color);
          if (! beg)
            return;
        }

      if (!only_matching && *line_color)
        {
          /* This code is exercised at least when grep is invoked like this:
             echo k| GREP_COLORS='sl=01;32' src/grep k --color=always  */
          beg = print_line_tail (beg, lim, line_color);
        }
    }

  if (!only_matching && lim > beg)
    fwrite_errno (beg, 1, lim - beg);

  if (line_buffered)
    fflush_errno ();

  if (stdout_errno)
    ts_error (EXIT_TROUBLE, stdout_errno, _("write error"));

  ctx->lastout = lim;
}

/* Print pending lines of trailing context prior to LIM. Trailing context ends
   at the next matching line when OUTLEFT is 0.  */
static void
prpending (struct grepctx *ctx, char const *lim)
{
  if (!ctx->lastout)
    ctx->lastout = ctx->bufbeg;
  lock_output ();
  while (ctx->pending > 0 && ctx->lastout < lim)
    {
      char *nl = memchr (ctx->lastout, eolbyte, lim - ctx->lastout);
      size_t match_size;
      --ctx->pending;
      if (ctx->outleft
          || ((execute (ctx->compiled_pattern, ctx, ctx->lastout,
                        nl + 1 - ctx->lastout, &match_size, NULL)
               == (size_t) -1)
              == !out_invert))
        prline (ctx, ctx->lastout, nl + 1, SEP_CHAR_REJECTED);
      else
        ctx->pending = 0;
    }
  unlock_output ();
}

/* Output the lines between BEG and LIM.  Deal with context.  */
static void
prtext (struct grepctx *ctx, char *beg, char *lim)
{
  /* Avoid printing SEP_STR_GROUP before any output.  Static is OK here since
     it's only accessed under output_lock. */
  static bool used;
  char eol = eolbyte;

  if (!ctx->out_quiet && ctx->pending > 0)
    prpending (ctx, beg);

  char *p = beg;

  lock_output ();

  if (!ctx->out_quiet)
    {
      /* Deal with leading context.  */
      char const *bp = ctx->lastout ? ctx->lastout : ctx->bufbeg;
      intmax_t i;
      for (i = 0; i < out_before; ++i)
        if (p > bp)
          do
            --p;
          while (p[-1] != eol);

      /* Print the group separator unless the output is adjacent to
         the previous output in the file.  */
      if ((0 <= out_before || 0 <= out_after) && used
          && p != ctx->lastout && group_separator)
        {
          pr_sgr_start_if (sep_color);
          fputs_errno (group_separator);
          pr_sgr_end_if (sep_color);
          putchar_errno ('\n');
        }

      while (p < beg)
        {
          char *nl = memchr (p, eol, beg - p);
          nl++;
          prline (ctx, p, nl, SEP_CHAR_REJECTED);
          p = nl;
        }
    }

  intmax_t n;
  if (out_invert)
    {
      /* One or more lines are output.  */
      for (n = 0; p < lim && n < ctx->outleft; n++)
        {
          char *nl = memchr (p, eol, lim - p);
          nl++;
          if (!ctx->out_quiet)
            prline (ctx, p, nl, SEP_CHAR_SELECTED);
          p = nl;
        }
    }
  else
    {
      /* Just one line is output.  */
      if (!ctx->out_quiet)
        prline (ctx, beg, lim, SEP_CHAR_SELECTED);
      n = 1;
      p = lim;
    }

  ctx->after_last_match = ctx->bufoffset - (ctx->buflim - p);
  ctx->pending = ctx->out_quiet ? 0 : MAX (0, out_after);
  used = true;
  ctx->outleft -= n;

  unlock_output ();
}

/* Replace all NUL bytes in buffer P (which ends at LIM) with EOL.
   This avoids running out of memory when binary input contains a long
   sequence of zeros, which would otherwise be considered to be part
   of a long line.  P[LIM] should be EOL.  */
static void
zap_nuls (char *p, char *lim, char eol)
{
  if (eol)
    while (true)
      {
        *lim = '\0';
        p += strlen (p);
        *lim = eol;
        if (p == lim)
          break;
        do
          *p++ = eol;
        while (!*p);
      }
}

/* Scan the specified portion of the buffer, matching lines (or
   between matching lines if OUT_INVERT is true).  Return a count of
   lines printed.  Replace all NUL bytes with NUL_ZAPPER as we go.  */
static intmax_t
grepbuf (struct grepctx *ctx, char *beg, char const *lim)
{
  intmax_t outleft0 = ctx->outleft;
  char *endp;

  for (char *p = beg; p < lim; p = endp)
    {
      size_t match_size;
      size_t match_offset = execute (ctx->compiled_pattern, ctx, p, lim - p,
                                     &match_size, NULL);
      if (match_offset == (size_t) -1)
        {
          if (!out_invert)
            break;
          match_offset = lim - p;
          match_size = 0;
        }
      char *b = p + match_offset;
      endp = b + match_size;
      /* Avoid matching the empty line at the end of the buffer. */
      if (!out_invert && b == lim)
        break;
      if (!out_invert || p < b)
        {
          char *prbeg = out_invert ? p : b;
          char *prend = out_invert ? b : endp;
          prtext (ctx, prbeg, prend);
          if (!ctx->outleft || ctx->done_on_match)
            {
              if (exit_on_match)
                exit (errseen ? exit_failure : EXIT_SUCCESS);
              break;
            }
        }
    }

  return outleft0 - ctx->outleft;
}

/* Search a given (non-directory) file.  Return a count of lines printed. */
static intmax_t
grep (struct grepctx *ctx, int fd, struct stat const *st)
{
  intmax_t nlines, i;
  size_t residue, save;
  char oldc;
  char *beg;
  char *lim;
  char eol = eolbyte;
  char nul_zapper = '\0';
  bool done_on_match_0 = ctx->done_on_match;
  bool out_quiet_0 = ctx->out_quiet;

  /* The value of NLINES when nulls were first deduced in the input;
     this is not necessarily the same as the number of matching lines
     before the first null.  -1 if no input nulls have been deduced.  */
  intmax_t nlines_first_null = -1;

  if (! reset (ctx, fd, st))
    return 0;

  ctx->totalcc = 0;
  ctx->lastout = 0;
  ctx->totalnl = 0;
  ctx->outleft = max_count;
  ctx->after_last_match = 0;
  ctx->pending = 0;
  ctx->skip_nuls = skip_empty_lines && !eol;
  ctx->encoding_error_output = false;
  ctx->seek_data_failed = false;

  nlines = 0;
  residue = 0;
  save = 0;

  if (! fillbuf (ctx, save, st))
    {
      suppressible_error (ctx->filename, errno);
      return 0;
    }

  for (bool firsttime = true; ; firsttime = false)
    {
      if (nlines_first_null < 0 && eol && binary_files != TEXT_BINARY_FILES
          && (buf_has_nulls (ctx->bufbeg, ctx->buflim - ctx->bufbeg)
              || (firsttime
                  && file_must_have_nulls (ctx, ctx->buflim - ctx->bufbeg, fd,
                                           st))))
        {
          if (binary_files == WITHOUT_MATCH_BINARY_FILES)
            return 0;
          if (!count_matches)
            ctx->done_on_match = ctx->out_quiet = true;
          nlines_first_null = nlines;
          nul_zapper = eol;
          ctx->skip_nuls = skip_empty_lines;
        }

      ctx->lastnl = ctx->bufbeg;
      if (ctx->lastout)
        ctx->lastout = ctx->bufbeg;

      beg = ctx->bufbeg + save;

      /* no more data to scan (eof) except for maybe a residue -> break */
      if (beg == ctx->buflim)
        break;

      zap_nuls (beg, ctx->buflim, nul_zapper);

      /* Determine new residue (the length of an incomplete line at the end of
         the buffer, 0 means there is no incomplete last line).  */
      oldc = beg[-1];
      beg[-1] = eol;
      /* FIXME: use rawmemrchr if/when it exists, since we have ensured
         that this use of memrchr is guaranteed never to return NULL.  */
      lim = memrchr (beg - 1, eol, ctx->buflim - beg + 1);
      ++lim;
      beg[-1] = oldc;
      if (lim == beg)
        lim = beg - residue;
      beg -= residue;
      residue = ctx->buflim - lim;

      if (beg < lim)
        {
          if (ctx->outleft)
            nlines += grepbuf (ctx, beg, lim);
          if (ctx->pending)
            prpending (ctx, lim);
          if ((!ctx->outleft && !ctx->pending)
              || (ctx->done_on_match && MAX (0, nlines_first_null) < nlines))
            goto finish_grep;
        }

      /* The last OUT_BEFORE lines at the end of the buffer will be needed as
         leading context if there is a matching line at the begin of the
         next data. Make beg point to their begin.  */
      i = 0;
      beg = lim;
      while (i < out_before && beg > ctx->bufbeg && beg != ctx->lastout)
        {
          ++i;
          do
            --beg;
          while (beg[-1] != eol);
        }

      /* Detect whether leading context is adjacent to previous output.  */
      if (beg != ctx->lastout)
        ctx->lastout = 0;

      /* Handle some details and read more data to scan.  */
      save = residue + lim - beg;
      if (out_byte)
        ctx->totalcc = add_count (ctx->totalcc,
                                  ctx->buflim - ctx->bufbeg - save);
      if (out_line)
        nlscan (ctx, beg);
      if (! fillbuf (ctx, save, st))
        {
          suppressible_error (ctx->filename, errno);
          goto finish_grep;
        }
    }
  if (residue)
    {
      *ctx->buflim++ = eol;
      if (ctx->outleft)
        nlines += grepbuf (ctx, ctx->bufbeg + save - residue, ctx->buflim);
      if (ctx->pending)
        prpending (ctx, ctx->buflim);
    }

 finish_grep:
  ctx->done_on_match = done_on_match_0;
  ctx->out_quiet = out_quiet_0;
  if (!ctx->out_quiet
      && (ctx->encoding_error_output
          || (0 <= nlines_first_null && nlines_first_null < nlines)))
    {
      lock_output ();
      printf_errno (_("Binary file %s matches\n"), ctx->filename);
      if (line_buffered)
        fflush_errno ();
      unlock_output ();
    }
  return nlines;
}

struct workfile
{
  int fd;
  char *path;
  struct stat st;
  struct workfile *next;
};

static struct
{
  struct workfile *head;
  struct workfile *tail;
  int num_files;
  int producer_done;
  pthread_mutex_t lock;
  pthread_cond_t consumer_cond;
  pthread_cond_t producer_cond;
} workqueue;

static intmax_t max_queued_files;

/* Retrieve a workfile from the work queue, returning NULL if there's
   nothing left to process. */
static struct workfile *
dequeue_workfile (void)
{
  struct workfile *wf;

  pthread_mutex_lock (&workqueue.lock);
  while (!workqueue.num_files && !workqueue.producer_done)
    pthread_cond_wait (&workqueue.consumer_cond, &workqueue.lock);
  if (!workqueue.num_files && workqueue.producer_done)
    wf = NULL;
  else
    {
      wf = workqueue.head;
      workqueue.head = workqueue.head->next;
      if (!workqueue.head)
        workqueue.tail = NULL;
      workqueue.num_files--;
      pthread_cond_signal (&workqueue.producer_cond);
    }
  pthread_mutex_unlock (&workqueue.lock);

  return wf;
}

static void
enqueue_workfile (int fd, char const *path, struct stat *st)
{
  struct workfile *wf;

  wf = xmalloc (sizeof (*wf));
  wf->fd = fd;
  wf->path = xstrdup (path);
  wf->st = *st;
  wf->next = NULL;

  pthread_mutex_lock (&workqueue.lock);
  while (workqueue.num_files >= max_queued_files)
    pthread_cond_wait (&workqueue.producer_cond, &workqueue.lock);
  if (!workqueue.head)
    workqueue.head = workqueue.tail = wf;
  else
    {
      workqueue.tail->next = wf;
      workqueue.tail = wf;
    }
  workqueue.num_files++;
  pthread_cond_signal (&workqueue.consumer_cond);
  pthread_mutex_unlock (&workqueue.lock);
}

static void
finish_workqueue (void)
{
  pthread_mutex_lock (&workqueue.lock);
  workqueue.producer_done = 1;
  pthread_cond_broadcast (&workqueue.consumer_cond);
  pthread_mutex_unlock (&workqueue.lock);
}

static void *
worker_thread_func (void *arg)
{
  struct workfile* wf;
  struct grepctx ctx;
  intmax_t count;
  bool status = true;

  memset (&ctx, 0, sizeof (ctx));
  if (pagesize == 0 || 2 * pagesize + 1 <= pagesize)
    abort ();
  ctx.bufalloc = (ALIGN_TO (INITIAL_BUFSIZE, pagesize)
                  + pagesize + sizeof (uword));
  ctx.buffer = xmalloc (ctx.bufalloc);

  ctx.out_quiet = out_quiet;
  ctx.done_on_match = done_on_match;
  ctx.compiled_pattern = arg;

  while ((wf = dequeue_workfile ()))
    {
      ctx.filename = wf->path;

#if defined SET_BINARY
      /* Set input to binary mode.  Pipes are simulated with files
         on DOS, so this includes the case of "foo | grep bar".  */
      if (!isatty (wf->fd))
        SET_BINARY (wf->fd);
#endif

      count = grep (&ctx, wf->fd, &wf->st);
      status = !count && status;
      if (count_matches)
        {
          lock_output ();
          if (out_file)
            {
              print_filename (&ctx);
              if (filename_mask)
                print_sep (SEP_CHAR_SELECTED);
              else
                putchar_errno (0);
            }
          printf_errno ("%" PRIdMAX "\n", count);
          if (line_buffered)
            fflush_errno ();
          unlock_output ();
        }

      if ((list_files == LISTFILES_MATCHING && count > 0)
          || (list_files == LISTFILES_NONMATCHING && count == 0))
        {
          lock_output ();
          print_filename (&ctx);
          putchar_errno ('\n' & filename_mask);
          if (line_buffered)
            fflush_errno ();
          unlock_output ();
        }

      if (wf->fd == STDIN_FILENO)
        {
          off_t required_offset =
            ctx.outleft ? ctx.bufoffset : ctx.after_last_match;
          if (required_offset != ctx.bufoffset
              && lseek (wf->fd, required_offset, SEEK_SET) < 0
              && S_ISREG (wf->st.st_mode))
            suppressible_error (wf->path, errno);
        }

      if (wf->fd != STDIN_FILENO && close (wf->fd) != 0)
        suppressible_error (wf->path, errno);
      free (wf->path);
      free (wf);
    }

  return (void *) status;
}

static void
search_dirent (FTS *fts, FTSENT *ent, bool command_line)
{
  char *name;
  bool follow;
  command_line &= ent->fts_level == FTS_ROOTLEVEL;

  if (ent->fts_info == FTS_DP)
    return;

  if (!command_line
      && skipped_file (ent->fts_name, false,
                       (ent->fts_info == FTS_D || ent->fts_info == FTS_DC
                        || ent->fts_info == FTS_DNR)))
    {
      fts_set (fts, ent, FTS_SKIP);
      return;
    }

  name = ent->fts_path;
  if (omit_dot_slash && strlen (name) >= 2)
    name += 2;
  follow = (fts->fts_options & FTS_LOGICAL
            || (fts->fts_options & FTS_COMFOLLOW && command_line));

  switch (ent->fts_info)
    {
    case FTS_D:
      if (directories == RECURSE_DIRECTORIES)
        return;
      fts_set (fts, ent, FTS_SKIP);
      break;

    case FTS_DC:
      if (!suppress_errors)
        ts_error (0, 0, _("warning: %s: %s"), name,
                    _("recursive directory loop"));
      return;

    case FTS_DNR:
    case FTS_ERR:
    case FTS_NS:
      suppressible_error (name, ent->fts_errno);
      return;

    case FTS_DEFAULT:
    case FTS_NSOK:
      if (skip_devices (command_line))
        {
          struct stat *st = ent->fts_statp;
          struct stat st1;
          if (! st->st_mode)
            {
              /* The file type is not already known.  Get the file status
                 before opening, since opening might have side effects
                 on a device.  */
              int flag = follow ? 0 : AT_SYMLINK_NOFOLLOW;
              if (fstatat (fts->fts_cwd_fd, ent->fts_accpath, &st1, flag) != 0)
                {
                  suppressible_error (name, errno);
                  return;
                }
              st = &st1;
            }
          if (is_device_mode (st->st_mode))
            return;
        }
      break;

    case FTS_F:
    case FTS_SLNONE:
      break;

    case FTS_SL:
    case FTS_W:
      return;

    default:
      abort ();
    }

  search_file (fts->fts_cwd_fd, ent->fts_accpath, name, follow, command_line);
}

static void
search_desc (int desc, char const *path, bool command_line)
{
  struct stat st;

  if (fstat (desc, &st) != 0)
    {
      suppressible_error (path, errno);
      goto closeout;
    }

  if (desc != STDIN_FILENO && skip_devices (command_line)
      && is_device_mode (st.st_mode))
    goto closeout;

  if (desc != STDIN_FILENO && command_line
      && skipped_file (path, true, S_ISDIR (st.st_mode)))
    goto closeout;

  if (desc != STDIN_FILENO
      && directories == RECURSE_DIRECTORIES && S_ISDIR (st.st_mode))
    {
      /* Traverse the directory starting with its full name, because
         unfortunately fts provides no way to traverse the directory
         starting from its file descriptor.  */

      FTS *fts;
      FTSENT *ent;
      int opts = fts_options & ~(command_line ? 0 : FTS_COMFOLLOW);
      char *fts_arg[2] = { (char *) path, NULL, };

      /* Close DESC now, to conserve file descriptors if the race
         condition occurs many times in a deep recursion.  */
      if (close (desc) != 0)
        suppressible_error (path, errno);

      fts = fts_open (fts_arg, opts, NULL);

      if (!fts)
        xalloc_die ();
      while ((ent = fts_read (fts)))
        search_dirent (fts, ent, command_line);
      if (errno)
        suppressible_error (path, errno);
      if (fts_close (fts))
        suppressible_error (path, errno);
      return;
    }
  if (desc != STDIN_FILENO
      && ((directories == SKIP_DIRECTORIES && S_ISDIR (st.st_mode))
          || ((devices == SKIP_DEVICES
               || (devices == READ_COMMAND_LINE_DEVICES && !command_line))
              && is_device_mode (st.st_mode))))
    goto closeout;

  /* If there is a regular file on stdout and the current file refers
     to the same i-node, we have to report the problem and skip it.
     Otherwise when matching lines from some other input reach the
     disk before we open this file, we can end up reading and matching
     those lines and appending them to the file from which we're reading.
     Then we'd have what appears to be an infinite loop that'd terminate
     only upon filling the output file system or reaching a quota.
     However, there is no risk of an infinite loop if grep is generating
     no output, i.e., with --silent, --quiet, -q.
     Similarly, with any of these:
       --max-count=N (-m) (for N >= 2)
       --files-with-matches (-l)
       --files-without-match (-L)
     there is no risk of trouble.
     For --max-count=1, grep stops after printing the first match,
     so there is no risk of malfunction.  But even --max-count=2, with
     input==output, while there is no risk of infloop, there is a race
     condition that could result in "alternate" output.  */
  if (!out_quiet && list_files == LISTFILES_NONE && 1 < max_count
      && SAME_INODE (st, out_stat))
    {
      if (! suppress_errors)
        ts_error (0, 0, _("input file %s is also the output"), quote (path));
      errseen = true;
      goto closeout;
    }

  /* Request readahead and enqueue a piece of work to worker threads */
  posix_fadvise (desc, 0, 0, POSIX_FADV_WILLNEED);
  enqueue_workfile (desc, path, &st);

  return;

 closeout:
  if (desc != STDIN_FILENO && close (desc) != 0)
    suppressible_error (path, errno);
}

/* True if errno is ERR after 'open ("symlink", ... O_NOFOLLOW ...)'.
   POSIX specifies ELOOP, but it's EMLINK on FreeBSD and EFTYPE on NetBSD.  */
static bool
open_symlink_nofollow_error (int err)
{
  if (err == ELOOP || err == EMLINK)
    return true;
#ifdef EFTYPE
  if (err == EFTYPE)
    return true;
#endif
  return false;
}

static void
search_file (int dirdesc, char const *name, char const *path, bool follow,
             bool command_line)
{
  int oflag = (O_RDONLY | O_NOCTTY
               | (follow ? 0 : O_NOFOLLOW)
               | (skip_devices (command_line) ? O_NONBLOCK : 0));
  int desc = openat_safer (dirdesc, name, oflag);
  if (desc < 0)
    {
      if (follow || ! open_symlink_nofollow_error (errno))
        suppressible_error (name, errno);
      return;
    }
  search_desc (desc, path, command_line);
}

static void
search_command_line_arg (char const *arg)
{
  if (STREQ (arg, "-"))
    search_desc (STDIN_FILENO, label ? label : _("(standard input)"), true);
  else
    search_file (AT_FDCWD, arg, arg, true, true);
}

_Noreturn void usage (int);
void
usage (int status)
{
  if (status != 0)
    {
      fprintf (stderr, _("Usage: %s [OPTION]... PATTERN [FILE]...\n"),
               program_name);
      fprintf (stderr, _("Try '%s --help' for more information.\n"),
               program_name);
    }
  else
    {
      printf (_("Usage: %s [OPTION]... PATTERN [FILE]...\n"), program_name);
      printf (_("Search for PATTERN in each FILE or standard input.\n"));
      printf (_("PATTERN is, by default, a basic regular expression (BRE).\n"));
      printf (_("\
Example: %s -i 'hello world' menu.h main.c\n\
\n\
Regexp selection and interpretation:\n"), program_name);
      printf (_("\
  -E, --extended-regexp     PATTERN is an extended regular expression (ERE)\n\
  -F, --fixed-strings       PATTERN is a set of newline-separated strings\n\
  -G, --basic-regexp        PATTERN is a basic regular expression (BRE)\n\
  -P, --perl-regexp         PATTERN is a Perl regular expression\n"));
  /* -X is deliberately undocumented.  */
      printf (_("\
  -e, --regexp=PATTERN      use PATTERN for matching\n\
  -f, --file=FILE           obtain PATTERN from FILE\n\
  -i, --ignore-case         ignore case distinctions\n\
  -w, --word-regexp         force PATTERN to match only whole words\n\
  -x, --line-regexp         force PATTERN to match only whole lines\n\
  -z, --null-data           a data line ends in 0 byte, not newline\n"));
      printf (_("\
\n\
Miscellaneous:\n\
  -s, --no-messages         suppress error messages\n\
  -v, --invert-match        select non-matching lines\n\
  -M, --parallel=NUM        use NUM search threads\n\
  -V, --version             display version information and exit\n\
      --help                display this help text and exit\n"));
      printf (_("\
\n\
Output control:\n\
  -m, --max-count=NUM       stop after NUM matches\n\
  -b, --byte-offset         print the byte offset with output lines\n\
  -n, --line-number         print line number with output lines\n\
      --line-buffered       flush output on every line\n\
  -H, --with-filename       print the file name for each match\n\
  -h, --no-filename         suppress the file name prefix on output\n\
      --label=LABEL         use LABEL as the standard input file name prefix\n\
"));
      printf (_("\
  -o, --only-matching       show only the part of a line matching PATTERN\n\
  -q, --quiet, --silent     suppress all normal output\n\
      --binary-files=TYPE   assume that binary files are TYPE;\n\
                            TYPE is 'binary', 'text', or 'without-match'\n\
  -a, --text                equivalent to --binary-files=text\n\
"));
      printf (_("\
  -I                        equivalent to --binary-files=without-match\n\
  -d, --directories=ACTION  how to handle directories;\n\
                            ACTION is 'read', 'recurse', or 'skip'\n\
  -D, --devices=ACTION      how to handle devices, FIFOs and sockets;\n\
                            ACTION is 'read' or 'skip'\n\
  -r, --recursive           like --directories=recurse\n\
  -R, --dereference-recursive  likewise, but follow all symlinks\n\
"));
      printf (_("\
      --include=FILE_PATTERN  search only files that match FILE_PATTERN\n\
      --exclude=FILE_PATTERN  skip files and directories matching\
 FILE_PATTERN\n\
      --exclude-from=FILE   skip files matching any file pattern from FILE\n\
      --exclude-dir=PATTERN  directories that match PATTERN will be skipped.\n\
"));
      printf (_("\
  -L, --files-without-match  print only names of FILEs containing no match\n\
  -l, --files-with-matches  print only names of FILEs containing matches\n\
  -c, --count               print only a count of matching lines per FILE\n\
  -T, --initial-tab         make tabs line up (if needed)\n\
  -Z, --null                print 0 byte after FILE name\n"));
      printf (_("\
\n\
Context control:\n\
  -B, --before-context=NUM  print NUM lines of leading context\n\
  -A, --after-context=NUM   print NUM lines of trailing context\n\
  -C, --context=NUM         print NUM lines of output context\n\
"));
      printf (_("\
  -NUM                      same as --context=NUM\n\
      --color[=WHEN],\n\
      --colour[=WHEN]       use markers to highlight the matching strings;\n\
                            WHEN is 'always', 'never', or 'auto'\n\
  -U, --binary              do not strip CR characters at EOL (MSDOS/Windows)\n\
  -u, --unix-byte-offsets   report offsets as if CRs were not there\n\
                            (MSDOS/Windows)\n\
\n"));
      printf (_("\
'egrep' means 'grep -E'.  'fgrep' means 'grep -F'.\n\
Direct invocation as either 'egrep' or 'fgrep' is deprecated.\n"));
      printf (_("\
When FILE is -, read standard input.  With no FILE, read . if a command-line\n\
-r is given, - otherwise.  If fewer than two FILEs are given, assume -h.\n\
Exit status is 0 if any line is selected, 1 otherwise;\n\
if any error occurs and -q is not given, the exit status is 2.\n"));
      emit_bug_reporting_address ();
    }
  exit (status);
}

/* Pattern compilers and matchers.  */

static void *
Gcompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_GREP);
}

static void *
Ecompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_EGREP);
}

static void *
Acompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_AWK);
}

static void *
GAcompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_GNU_AWK);
}

static void *
PAcompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_POSIX_AWK);
}

struct matcher
{
  char const name[16];
  compile_fp_t compile;
  execute_fp_t execute;
};
static struct matcher const matchers[] = {
  { "grep",      Gcompile, EGexecute },
  { "egrep",     Ecompile, EGexecute },
  { "fgrep",     Fcompile,  Fexecute },
  { "awk",       Acompile, EGexecute },
  { "gawk",     GAcompile, EGexecute },
  { "posixawk", PAcompile, EGexecute },
  { "perl",      Pcompile,  Pexecute },
  { "", NULL, NULL },
};

/* Set the matcher to M if available.  Exit in case of conflicts or if
   M is not available.  */
static void
setmatcher (char const *m)
{
  struct matcher const *p;

  if (matcher && !STREQ (matcher, m))
    ts_error (EXIT_TROUBLE, 0, _("conflicting matchers specified"));

  for (p = matchers; p->compile; p++)
    if (STREQ (m, p->name))
      {
        matcher = p->name;
        compile = p->compile;
        execute = p->execute;
        return;
      }

  ts_error (EXIT_TROUBLE, 0, _("invalid matcher %s"), m);
}

/* Find the white-space-separated options specified by OPTIONS, and
   using BUF to store copies of these options, set ARGV[0], ARGV[1],
   etc. to the option copies.  Return the number N of options found.
   Do not set ARGV[N] to NULL.  If ARGV is NULL, do not store ARGV[0]
   etc.  Backslash can be used to escape whitespace (and backslashes).  */
static size_t
prepend_args (char const *options, char *buf, char **argv)
{
  char const *o = options;
  char *b = buf;
  size_t n = 0;

  for (;;)
    {
      while (c_isspace (to_uchar (*o)))
        o++;
      if (!*o)
        return n;
      if (argv)
        argv[n] = b;
      n++;

      do
        if ((*b++ = *o++) == '\\' && *o)
          b[-1] = *o++;
      while (*o && ! c_isspace (to_uchar (*o)));

      *b++ = '\0';
    }
}

/* Prepend the whitespace-separated options in OPTIONS to the argument
   vector of a main program with argument count *PARGC and argument
   vector *PARGV.  Return the number of options prepended.  */
static int
prepend_default_options (char const *options, int *pargc, char ***pargv)
{
  if (options && *options)
    {
      char *buf = xmalloc (strlen (options) + 1);
      size_t prepended = prepend_args (options, buf, NULL);
      int argc = *pargc;
      char *const *argv = *pargv;
      char **pp;
      enum { MAX_ARGS = MIN (INT_MAX, SIZE_MAX / sizeof *pp - 1) };
      if (MAX_ARGS - argc < prepended)
        xalloc_die ();
      pp = xmalloc ((prepended + argc + 1) * sizeof *pp);
      *pargc = prepended + argc;
      *pargv = pp;
      *pp++ = *argv++;
      pp += prepend_args (options, buf, pp);
      while ((*pp++ = *argv++))
        continue;
      return prepended;
    }

  return 0;
}

/* Get the next non-digit option from ARGC and ARGV.
   Return -1 if there are no more options.
   Process any digit options that were encountered on the way,
   and store the resulting integer into *DEFAULT_CONTEXT.  */
static int
get_nondigit_option (int argc, char *const *argv, intmax_t *default_context)
{
  /* Static is OK here since this is only called from the main thread. */
  static int prev_digit_optind = -1;
  int this_digit_optind;
  bool was_digit;
  char buf[INT_BUFSIZE_BOUND (intmax_t) + 4];
  char *p = buf;
  int opt;

  was_digit = false;
  this_digit_optind = optind;
  while (true)
    {
      opt = getopt_long (argc, (char **) argv, short_options,
                         long_options, NULL);
      if ( ! ('0' <= opt && opt <= '9'))
        break;

      if (prev_digit_optind != this_digit_optind || !was_digit)
        {
          /* Reset to start another context length argument.  */
          p = buf;
        }
      else
        {
          /* Suppress trivial leading zeros, to avoid incorrect
             diagnostic on strings like 00000000000.  */
          p -= buf[0] == '0';
        }

      if (p == buf + sizeof buf - 4)
        {
          /* Too many digits.  Append "..." to make context_length_arg
             complain about "X...", where X contains the digits seen
             so far.  */
          strcpy (p, "...");
          p += 3;
          break;
        }
      *p++ = opt;

      was_digit = true;
      prev_digit_optind = this_digit_optind;
      this_digit_optind = optind;
    }
  if (p != buf)
    {
      *p = '\0';
      context_length_arg (buf, default_context);
    }

  return opt;
}

/* Parse GREP_COLORS.  The default would look like:
     GREP_COLORS='ms=01;31:mc=01;31:sl=:cx=:fn=35:ln=32:bn=32:se=36'
   with boolean capabilities (ne and rv) unset (i.e., omitted).
   No character escaping is needed or supported.  */
static void
parse_grep_colors (void)
{
  const char *p;
  char *q;
  char *name;
  char *val;

  p = getenv ("GREP_COLORS"); /* Plural! */
  if (p == NULL || *p == '\0')
    return;

  /* Work off a writable copy.  */
  q = xstrdup (p);

  name = q;
  val = NULL;
  /* From now on, be well-formed or you're gone.  */
  for (;;)
    if (*q == ':' || *q == '\0')
      {
        char c = *q;
        struct color_cap const *cap;

        *q++ = '\0'; /* Terminate name or val.  */
        /* Empty name without val (empty cap)
         * won't match and will be ignored.  */
        for (cap = color_dict; cap->name; cap++)
          if (STREQ (cap->name, name))
            break;
        /* If name unknown, go on for forward compatibility.  */
        if (cap->var && val)
          *(cap->var) = val;
        if (cap->fct)
          cap->fct ();
        if (c == '\0')
          return;
        name = q;
        val = NULL;
      }
    else if (*q == '=')
      {
        if (q == name || val)
          return;
        *q++ = '\0'; /* Terminate name.  */
        val = q; /* Can be the empty string.  */
      }
    else if (val == NULL)
      q++; /* Accumulate name.  */
    else if (*q == ';' || (*q >= '0' && *q <= '9'))
      q++; /* Accumulate val.  Protect the terminal from being sent crap.  */
    else
      return;
}

/* Return true if PAT (of length PATLEN) contains an encoding error.  */
static bool
contains_encoding_error (char const *pat, size_t patlen)
{
  mbstate_t mbs = { 0 };
  size_t i, charlen;

  for (i = 0; i < patlen; i += charlen)
    {
      charlen = mb_clen (pat + i, patlen - i, &mbs);
      if ((size_t) -2 <= charlen)
        return true;
    }
  return false;
}

/* Change a pattern for fgrep into grep.  */
static void
fgrep_to_grep_pattern (size_t len, char const *keys,
                       size_t *new_len, char **new_keys)
{
  char *p = *new_keys = xnmalloc (len + 1, 2);
  mbstate_t mb_state = { 0 };
  size_t n;

  for (; len; keys += n, len -= n)
    {
      n = mb_clen (keys, len, &mb_state);
      switch (n)
        {
        case (size_t) -2:
          n = len;
          /* Fall through.  */
        default:
          p = mempcpy (p, keys, n);
          break;

        case (size_t) -1:
          memset (&mb_state, 0, sizeof mb_state);
          /* Fall through.  */
        case 1:
          *p = '\\';
          p += strchr ("$*.[\\^", *keys) != NULL;
          /* Fall through.  */
        case 0:
          *p++ = *keys;
          n = 1;
          break;
        }
    }

  *new_len = p - *new_keys;
}

int
main (int argc, char **argv)
{
  char *keys;
  size_t keycc, oldcc, keyalloc;
  bool with_filenames;
  size_t cc;
  int i, status;
  int opt, prepended;
  int prev_optind, last_recursive;
  int fread_errno;
  intmax_t default_context;
  intmax_t num_threads;
  FILE *fp;
  pthread_t *worker_threads;
  pthread_mutexattr_t output_lock_attr;
  void *worker_status;
  struct rlimit rlim;
  struct grepctx tmpctx;
  exit_failure = EXIT_TROUBLE;
  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  program_name = argv[0];

  pagesize = getpagesize ();

  keys = NULL;
  keycc = 0;
  with_filenames = false;
  eolbyte = '\n';
  filename_mask = ~0;

  max_count = INTMAX_MAX;
  num_threads = 1;

  /* The value -1 means to use DEFAULT_CONTEXT. */
  out_after = out_before = -1;
  /* Default before/after context: changed by -C/-NUM options */
  default_context = -1;
  /* Changed by -o option */
  only_matching = false;

  pthread_mutexattr_init (&output_lock_attr);

  /* Recursive locking (output_lock in this case) is always a little
     unfortunate, but ts_error() is called from functions that are sometimes
     within print functions that already hold it, and sometimes not. */
  if (pthread_mutexattr_settype (&output_lock_attr, PTHREAD_MUTEX_RECURSIVE)
      || pthread_mutex_init (&output_lock, &output_lock_attr)
      || pthread_mutex_init (&workqueue.lock, NULL)
      || pthread_cond_init (&workqueue.producer_cond, NULL)
      || pthread_cond_init (&workqueue.consumer_cond, NULL))
    abort ();

  /* Internationalization. */
#if defined HAVE_SETLOCALE
  setlocale (LC_ALL, "");
#endif
#if defined ENABLE_NLS
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
#endif

  dfa_init ();

  atexit (clean_up_stdout);

  last_recursive = 0;

  prepended = prepend_default_options (getenv ("GREP_OPTIONS"), &argc, &argv);
  if (prepended)
    ts_error (0, 0, _("warning: GREP_OPTIONS is deprecated;"
                      " please use an alias or script"));

  compile = matchers[0].compile;
  execute = matchers[0].execute;

  while (prev_optind = optind,
         (opt = get_nondigit_option (argc, argv, &default_context)) != -1)
    switch (opt)
      {
      case 'A':
        context_length_arg (optarg, &out_after);
        break;

      case 'B':
        context_length_arg (optarg, &out_before);
        break;

      case 'C':
        /* Set output match context, but let any explicit leading or
           trailing amount specified with -A or -B stand. */
        context_length_arg (optarg, &default_context);
        break;

      case 'D':
        if (STREQ (optarg, "read"))
          devices = READ_DEVICES;
        else if (STREQ (optarg, "skip"))
          devices = SKIP_DEVICES;
        else
          ts_error (EXIT_TROUBLE, 0, _("unknown devices method"));
        break;

      case 'E':
        setmatcher ("egrep");
        break;

      case 'F':
        setmatcher ("fgrep");
        break;

      case 'P':
        setmatcher ("perl");
        break;

      case 'G':
        setmatcher ("grep");
        break;

      case 'X': /* undocumented on purpose */
        setmatcher (optarg);
        break;

      case 'H':
        with_filenames = true;
        no_filenames = false;
        break;

      case 'I':
        binary_files = WITHOUT_MATCH_BINARY_FILES;
        break;

      case 'T':
        align_tabs = true;
        break;

      case 'U':
        dos_binary ();
        break;

      case 'u':
        dos_unix_byte_offsets ();
        break;

      case 'V':
        show_version = true;
        break;

      case 'a':
        binary_files = TEXT_BINARY_FILES;
        break;

      case 'b':
        out_byte = true;
        break;

      case 'c':
        count_matches = true;
        break;

      case 'd':
        directories = XARGMATCH ("--directories", optarg,
                                 directories_args, directories_types);
        if (directories == RECURSE_DIRECTORIES)
          last_recursive = prev_optind;
        break;

      case 'e':
        cc = strlen (optarg);
        keys = xrealloc (keys, keycc + cc + 1);
        strcpy (&keys[keycc], optarg);
        keycc += cc;
        keys[keycc++] = '\n';
        break;

      case 'f':
        fp = STREQ (optarg, "-") ? stdin : fopen (optarg, O_TEXT ? "rt" : "r");
        if (!fp)
          ts_error (EXIT_TROUBLE, errno, "%s", optarg);
        for (keyalloc = 1; keyalloc <= keycc + 1; keyalloc *= 2)
          ;
        keys = xrealloc (keys, keyalloc);
        oldcc = keycc;
        while ((cc = fread (keys + keycc, 1, keyalloc - 1 - keycc, fp)) != 0)
          {
            keycc += cc;
            if (keycc == keyalloc - 1)
              keys = x2nrealloc (keys, &keyalloc, sizeof *keys);
          }
        fread_errno = errno;
        if (ferror (fp))
          ts_error (EXIT_TROUBLE, fread_errno, "%s", optarg);
        if (fp != stdin)
          fclose (fp);
        /* Append final newline if file ended in non-newline. */
        if (oldcc != keycc && keys[keycc - 1] != '\n')
          keys[keycc++] = '\n';
        break;

      case 'h':
        with_filenames = false;
        no_filenames = true;
        break;

      case 'i':
      case 'y':			/* For old-timers . . . */
        match_icase = true;
        break;

      case 'L':
        /* Like -l, except list files that don't contain matches.
           Inspired by the same option in Hume's gre. */
        list_files = LISTFILES_NONMATCHING;
        break;

      case 'l':
        list_files = LISTFILES_MATCHING;
        break;

      case 'M':
        if (optarg)
          {
            status = xstrtoimax (optarg, 0, 10, &num_threads, "");
            if ((status != LONGINT_OK && status != LONGINT_OVERFLOW)
                || num_threads < 1)
              ts_error (EXIT_TROUBLE, 0, _("invalid number of threads"));
          }
        else
          {
            num_threads = sysconf (_SC_NPROCESSORS_ONLN);
            if (num_threads < 1)
              num_threads = 1;
          }
        break;

      case 'm':
        switch (xstrtoimax (optarg, 0, 10, &max_count, ""))
          {
          case LONGINT_OK:
          case LONGINT_OVERFLOW:
            break;

          default:
            ts_error (EXIT_TROUBLE, 0, _("invalid max count"));
          }
        break;

      case 'n':
        out_line = true;
        break;

      case 'o':
        only_matching = true;
        break;

      case 'q':
        exit_on_match = true;
        exit_failure = 0;
        break;

      case 'R':
        fts_options = basic_fts_options | FTS_LOGICAL;
        /* Fall through.  */
      case 'r':
        directories = RECURSE_DIRECTORIES;
        last_recursive = prev_optind;
        break;

      case 's':
        suppress_errors = true;
        break;

      case 'v':
        out_invert = true;
        break;

      case 'w':
        match_words = true;
        break;

      case 'x':
        match_lines = true;
        break;

      case 'Z':
        filename_mask = 0;
        break;

      case 'z':
        eolbyte = '\0';
        break;

      case BINARY_FILES_OPTION:
        if (STREQ (optarg, "binary"))
          binary_files = BINARY_BINARY_FILES;
        else if (STREQ (optarg, "text"))
          binary_files = TEXT_BINARY_FILES;
        else if (STREQ (optarg, "without-match"))
          binary_files = WITHOUT_MATCH_BINARY_FILES;
        else
          ts_error (EXIT_TROUBLE, 0, _("unknown binary-files type"));
        break;

      case COLOR_OPTION:
        if (optarg)
          {
            if (!strcasecmp (optarg, "always") || !strcasecmp (optarg, "yes")
                || !strcasecmp (optarg, "force"))
              color_option = 1;
            else if (!strcasecmp (optarg, "never") || !strcasecmp (optarg, "no")
                     || !strcasecmp (optarg, "none"))
              color_option = 0;
            else if (!strcasecmp (optarg, "auto") || !strcasecmp (optarg, "tty")
                     || !strcasecmp (optarg, "if-tty"))
              color_option = 2;
            else
              show_help = 1;
          }
        else
          color_option = 2;
        break;

      case EXCLUDE_OPTION:
      case INCLUDE_OPTION:
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_patterns[cmd])
              excluded_patterns[cmd] = new_exclude ();
            add_exclude (excluded_patterns[cmd], optarg,
                         ((opt == INCLUDE_OPTION ? EXCLUDE_INCLUDE : 0)
                          | exclude_options (cmd)));
          }
        break;
      case EXCLUDE_FROM_OPTION:
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_patterns[cmd])
              excluded_patterns[cmd] = new_exclude ();
            if (add_exclude_file (add_exclude, excluded_patterns[cmd],
                                  optarg, exclude_options (cmd), '\n')
                != 0)
              ts_error (EXIT_TROUBLE, errno, "%s", optarg);
          }
        break;

      case EXCLUDE_DIRECTORY_OPTION:
        strip_trailing_slashes (optarg);
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_directory_patterns[cmd])
              excluded_directory_patterns[cmd] = new_exclude ();
            add_exclude (excluded_directory_patterns[cmd], optarg,
                         exclude_options (cmd));
          }
        break;

      case GROUP_SEPARATOR_OPTION:
        group_separator = optarg;
        break;

      case LINE_BUFFERED_OPTION:
        line_buffered = true;
        break;

      case LABEL_OPTION:
        label = optarg;
        break;

      case 0:
        /* long options */
        break;

      default:
        usage (EXIT_TROUBLE);
        break;

      }

  if (show_version)
    {
      version_etc (stdout, program_name, PACKAGE_NAME, VERSION, AUTHORS,
                   (char *) NULL);
      return EXIT_SUCCESS;
    }

  if (show_help)
    usage (EXIT_SUCCESS);

  bool possibly_tty = false;
  struct stat tmp_stat;
  if (! exit_on_match && fstat (STDOUT_FILENO, &tmp_stat) == 0)
    {
      if (S_ISREG (tmp_stat.st_mode))
        out_stat = tmp_stat;
      else if (S_ISCHR (tmp_stat.st_mode))
        {
          struct stat null_stat;
          if (stat ("/dev/null", &null_stat) == 0
              && SAME_INODE (tmp_stat, null_stat))
            exit_on_match = true;
          else
            possibly_tty = true;
        }
    }

  if (color_option == 2)
    color_option = possibly_tty && should_colorize () && isatty (STDOUT_FILENO);
  init_colorize ();

  if (color_option)
    {
      /* Legacy.  */
      char *userval = getenv ("GREP_COLOR");
      if (userval != NULL && *userval != '\0')
        selected_match_color = context_match_color = userval;

      /* New GREP_COLORS has priority.  */
      parse_grep_colors ();
    }

  /* POSIX says -c, -l and -q are mutually exclusive.  In this
     implementation, -q overrides -l and -L, which in turn override -c.  */
  if (exit_on_match)
    list_files = LISTFILES_NONE;
  if (exit_on_match || list_files != LISTFILES_NONE)
    {
      count_matches = false;
      done_on_match = true;
    }
  out_quiet = count_matches || done_on_match;

  if (out_after < 0)
    out_after = default_context;
  if (out_before < 0)
    out_before = default_context;

  if (keys)
    {
      if (keycc == 0)
        {
          /* No keys were specified (e.g. -f /dev/null).  Match nothing.  */
          out_invert ^= true;
          match_lines = match_words = false;
        }
      else
        /* Strip trailing newline. */
        --keycc;
    }
  else if (optind < argc)
    {
      /* A copy must be made in case of an xrealloc() or free() later.  */
      keycc = strlen (argv[optind]);
      keys = xmemdup (argv[optind++], keycc + 1);
    }
  else
    usage (EXIT_TROUBLE);

  build_mbclen_cache ();
  initialize_unibyte_mask ();

  /* In a unibyte locale, switch from fgrep to grep if
     the pattern matches words (where grep is typically faster).
     In a multibyte locale, switch from fgrep to grep if either
     (1) case is ignored (where grep is typically faster), or
     (2) the pattern has an encoding error (where fgrep might not work).  */
  if (compile == Fcompile
      && (MB_CUR_MAX <= 1
          ? match_words
          : match_icase || contains_encoding_error (keys, keycc)))
    {
      size_t new_keycc;
      char *new_keys;
      fgrep_to_grep_pattern (keycc, keys, &new_keycc, &new_keys);
      free (keys);
      keys = new_keys;
      keycc = new_keycc;
      matcher = "grep";
      compile = Gcompile;
      execute = EGexecute;
    }

  /* Mild hack -- temporary little on-stack grepctx */
  memset (&tmpctx, 0, sizeof (tmpctx));

  tmpctx.compiled_pattern = compile (keys, keycc);
  /* We need one byte prior and one after.  */
  char eolbytes[3] = { 0, eolbyte, 0 };
  size_t match_size;
  skip_empty_lines = ((execute (tmpctx.compiled_pattern, &tmpctx, eolbytes + 1,
                                1, &match_size, NULL) == 0)
                      == out_invert);

  if (((argc - optind > 1 || directories == RECURSE_DIRECTORIES)
       && !no_filenames)
      || with_filenames)
    out_file = 1;

#ifdef SET_BINARY
  /* Output is set to binary mode because we shouldn't convert
     NL to CR-LF pairs, especially when grepping binary files.  */
  if (!isatty (STDOUT_FILENO))
    SET_BINARY (STDOUT_FILENO);
#endif

  if (max_count == 0)
    return EXIT_FAILURE;

  if (fts_options & FTS_LOGICAL && devices == READ_COMMAND_LINE_DEVICES)
    devices = READ_DEVICES;

  /* Each entry in the work queue consumes an open file descriptor, so limit
     the queue to half the relevant rlimit. */
  if (getrlimit (RLIMIT_NOFILE, &rlim))
    abort ();
  max_queued_files = rlim.rlim_cur / 2;

  worker_threads = xmalloc (num_threads * sizeof (*worker_threads));
  for (i = 0; i < num_threads; i++)
    {
      if (pthread_create (&worker_threads[i], NULL, worker_thread_func,
                          compile (keys, keycc)))
        abort ();
    }

  free (keys);

  char *const *files;
  if (optind < argc)
    {
      files = argv + optind;
    }
  else if (directories == RECURSE_DIRECTORIES && prepended < last_recursive)
    {
      static char *const cwd_only[] = { (char *) ".", NULL };
      files = cwd_only;
      omit_dot_slash = true;
    }
  else
    {
      static char *const stdin_only[] = { (char *) "-", NULL };
      files = stdin_only;
    }

  do
    search_command_line_arg (*files++);
  while (*files != NULL);

  finish_workqueue ();

  status = 1;
  for (i = 0; i < num_threads; i++)
    {
      if (pthread_join (worker_threads[i], &worker_status))
        abort ();
      status = status && !!worker_status;
    }

  /* We register via atexit() to test stdout.  */
  return errseen ? EXIT_TROUBLE : status;
}