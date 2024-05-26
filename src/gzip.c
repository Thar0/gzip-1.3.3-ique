/* gzip (GNU zip) -- compress files with zip algorithm and 'compress' interface
 * Copyright (C) 1999, 2001, 2002 Free Software Foundation, Inc.
 * Copyright (C) 1992-1993 Jean-loup Gailly
 * The unzip code was written and put in the public domain by Mark Adler.
 * Portions of the lzw code are derived from the public domain 'compress'
 * written by Spencer Thomas, Joe Orost, James Woods, Jim McKie, Steve Davies,
 * Ken Turkowski, Dave Mack and Peter Jannesen.
 *
 * See the license_msg below and the file COPYING for the software license.
 * See the file algorithm.doc for the compression algorithms and file formats.
 */

static char  *license_msg[] = {
"Copyright 2002 Free Software Foundation",
"Copyright 1992-1993 Jean-loup Gailly",
"This program comes with ABSOLUTELY NO WARRANTY.",
"You may redistribute copies of this program",
"under the terms of the GNU General Public License.",
"For more information about these matters, see the file named COPYING.",
0};

/* Compress files with zip algorithm and 'compress' interface.
 * See usage() and help() functions below for all options.
 * Outputs:
 *        file.gz:   compressed file with same mode, owner, and utimes
 *     or stdout with -c option or if stdin used as input.
 * If the output file name had to be truncated, the original name is kept
 * in the compressed file.
 * On MSDOS, file.tmp -> file.tmz. On VMS, file.tmp -> file.tmp-gz.
 *
 * Using gz on MSDOS would create too many file name conflicts. For
 * example, foo.txt -> foo.tgz (.tgz must be reserved as shorthand for
 * tar.gz). Similarly, foo.dir and foo.doc would both be mapped to foo.dgz.
 * I also considered 12345678.txt -> 12345txt.gz but this truncates the name
 * too heavily. There is no ideal solution given the MSDOS 8+3 limitation. 
 *
 * For the meaning of all compilation flags, see comments in Makefile.in.
 */

#ifdef RCSID
static char rcsid[] = "$Id: gzip.c,v 0.24 1993/06/24 10:52:07 jloup Exp $";
#endif

#include "config.h"
#include <ctype.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>

#include "tailor.h"
#include "gzip.h"
#include "deflate.h"
#include "revision.h"

		/* configuration */

#ifdef HAVE_TIME_H
#  include <time.h>
#else
#  include <sys/time.h>
#endif

#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif

#ifdef HAVE_LIMITS_H
#  include <limits.h>
#endif

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

#if defined STDC_HEADERS || defined HAVE_STDLIB_H
#  include <stdlib.h>
#else
   extern int errno;
#endif

#ifdef HAVE_DIRENT_H
#  include <dirent.h>
#  define NAMLEN(direct) strlen((direct)->d_name)
#  define DIR_OPT "DIRENT"
#else
#  define dirent direct
#  define NAMLEN(direct) ((direct)->d_namlen)
#  ifdef HAVE_SYS_NDIR_H
#    include <sys/ndir.h>
#    define DIR_OPT "SYS_NDIR"
#  endif
#  ifdef HAVE_SYS_DIR_H
#    include <sys/dir.h>
#    define DIR_OPT "SYS_DIR"
#  endif
#  ifdef HAVE_NDIR_H
#    include <ndir.h>
#    define DIR_OPT "NDIR"
#  endif
#  ifndef DIR_OPT
#    define DIR_OPT "NO_DIR"
#  endif
#endif

#ifdef CLOSEDIR_VOID
# define CLOSEDIR(d) (closedir(d), 0)
#else
# define CLOSEDIR(d) closedir(d)
#endif

#ifdef HAVE_UTIME
#  ifdef HAVE_UTIME_H
#    include <utime.h>
#    define TIME_OPT "UTIME"
#  else
#    ifdef HAVE_SYS_UTIME_H
#      include <sys/utime.h>
#      define TIME_OPT "SYS_UTIME"
#    else
       struct utimbuf {
         time_t actime;
         time_t modtime;
       };
#      define TIME_OPT "STRUCT_UTIMBUF"
#    endif
#  endif
#else
#  define TIME_OPT "NO_UTIME"
#endif

#if !defined(S_ISDIR) && defined(S_IFDIR)
#  define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#if !defined(S_ISREG) && defined(S_IFREG)
#  define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

typedef RETSIGTYPE (*sig_type) OF((int));

#ifndef	O_BINARY
#  define  O_BINARY  0  /* creation mode for open() */
#endif

#ifndef O_CREAT
   /* Pure BSD system? */
#  include <sys/file.h>
#  ifndef O_CREAT
#    define O_CREAT FCREAT
#  endif
#  ifndef O_EXCL
#    define O_EXCL FEXCL
#  endif
#endif

#ifndef S_IRUSR
#  define S_IRUSR 0400
#endif
#ifndef S_IWUSR
#  define S_IWUSR 0200
#endif
#define RW_USER (S_IRUSR | S_IWUSR)  /* creation mode for open() */

#ifndef MAX_PATH_LEN
#  define MAX_PATH_LEN   1024 /* max pathname length */
#endif

#ifndef SEEK_END
#  define SEEK_END 2
#endif

#ifndef CHAR_BIT
#  define CHAR_BIT 8
#endif

#ifndef OFF_T_MIN
#define OFF_T_MIN (~ (off_t) 0 << (sizeof (off_t) * CHAR_BIT - 1))
#endif

#ifndef OFF_T_MAX
#define OFF_T_MAX (~ (off_t) 0 - OFF_T_MIN)
#endif

/* Separator for file name parts (see shorten_name()) */
#define PART_SEP "."

		/* global buffers */

DECLARE(uch, inbuf,  INBUFSIZ +INBUF_EXTRA);
DECLARE(uch, outbuf, OUTBUFSIZ+OUTBUF_EXTRA);
DECLARE(ush, d_buf,  DIST_BUFSIZE);
/* XXX 2L*WSIZE -> 3L*WIZE and poking 0xFF makes the
   insufficient lookahead bug deterministic, matches
   OoT but may not match other use-cases */
DECLARE(uch, window, 3L*WSIZE) = { [2L*WSIZE+4] = 0xFF};
#ifndef MAXSEG_64K
DECLARE(ush, tab_prefix, 1L<<BITS);
#else
DECLARE(ush, tab_prefix0, 1L<<(BITS-1));
DECLARE(ush, tab_prefix1, 1L<<(BITS-1));
#endif

		/* local variables */

int ascii = 0;        /* convert end-of-lines to local OS conventions */
int to_stdout = 0;    /* output to stdout (-c) */
int verbose = 0;      /* be verbose (-v) */
int quiet = 0;        /* be very quiet (-q) */
int test = 0;         /* test .gz file integrity */
int foreground;       /* set if program run in foreground */
char *progname;       /* program name */
int method = DEFLATED;/* compression method */
int level = 6;        /* compression level */
int exit_code = OK;   /* program exit code */
int save_orig_name;   /* set if original name must be saved */
int last_member;      /* set for .zip and .Z files */
int part_nb;          /* number of parts in .gz file */
time_t time_stamp;      /* original time stamp (modification time) */
off_t ifile_size;      /* input file size, -1 for devices (debug only) */
char *z_suffix;       /* default suffix (can be set with --suffix) */
size_t z_len;         /* strlen(z_suffix) */

off_t bytes_in;             /* number of input bytes */
off_t bytes_out;            /* number of output bytes */
off_t total_in;		    /* input bytes for all files */
off_t total_out;	    /* output bytes for all files */
char ifname[MAX_PATH_LEN]; /* input file name */
char ofname[MAX_PATH_LEN]; /* output file name */
int  remove_ofname = 0;	   /* remove output file on error */
struct stat istat;         /* status for input file */
FILE *ifd;                 /* input file */
FILE *ofd;                 /* output file */
unsigned insize;           /* valid bytes in inbuf */
unsigned inptr;            /* index of next byte to be processed in inbuf */
unsigned outcnt;           /* bytes in output buffer */

struct option longopts[] =
{
 /* { name  has_arg  *flag  val } */
    {"ascii",      0, 0, 'a'}, /* ascii text mode */
    {"to-stdout",  0, 0, 'c'}, /* write output on standard output */
    {"stdout",     0, 0, 'c'}, /* write output on standard output */
    {"help",       0, 0, 'h'}, /* give help */
    {"license",    0, 0, 'L'}, /* display software license */
    {"quiet",      0, 0, 'q'}, /* quiet mode */
    {"silent",     0, 0, 'q'}, /* quiet mode */
    {"suffix",     1, 0, 'S'}, /* use given suffix instead of .gz */
    {"verbose",    0, 0, 'v'}, /* verbose mode */
    {"version",    0, 0, 'V'}, /* display version number */
    {"fast",       0, 0, '1'}, /* compress faster */
    {"best",       0, 0, '9'}, /* compress better */
    {"lzw",        0, 0, 'Z'}, /* make output compatible with old compress */
    {"bits",       1, 0, 'b'}, /* max number of bits per code (implies -Z) */
    { 0, 0, 0, 0 }
};

/* local functions */

local void usage        OF((void));
local void help         OF((void));
local void license      OF((void));
local void version      OF((void));
local void treat_file   OF((char *iname));
local int create_outfile OF((void));
local int  do_stat      OF((char *name, struct stat *sbuf));
local char *get_suffix  OF((char *name));
local int  get_istat    OF((char *iname, struct stat *sbuf));
local int  make_ofname  OF((void));
local int  same_file    OF((struct stat *stat1, struct stat *stat2));
local void shorten_name  OF((char *name));
local int  check_ofname OF((void));
local void copy_stat    OF((struct stat *ifstat));
local void do_exit      OF((int exitcode));
      int main          OF((int argc, char **argv));
int (*work) OF((FILE *infile, FILE *outfile)) = zip; /* function to call */

#ifdef HAVE_UTIME
local void reset_times  OF((char *name, struct stat *statb));
#endif

#define strequ(s1, s2) (strcmp((s1),(s2)) == 0)

/* ======================================================================== */
local void usage()
{
    printf ("usage: %s [-%scdfhlLnN%stvV19] [-S suffix] [file ...]\n",
	    progname,
#if O_BINARY
	    "a",
#else
	    "",
#endif
#ifdef NO_DIR
	    ""
#else
	    "r"
#endif
	    );
}

/* ======================================================================== */
local void help()
{
    static char  *help_msg[] = {
#if O_BINARY
 " -a --ascii       ascii text; convert end-of-lines using local conventions",
#endif
 " -c --stdout      write on standard output, keep original files unchanged",
 " -h --help        give this help",
 " -L --license     display software license",
 " -q --quiet       suppress all warnings",
#ifndef NO_DIR
 " -r --recursive   operate recursively on directories",
#endif
 " -S .suf  --suffix .suf     use suffix .suf on compressed files",
 " -t --test        test compressed file integrity",
 " -v --verbose     verbose mode",
 " -V --version     display version number",
 " -1 --fast        compress faster",
 " -9 --best        compress better",
 " file...          files to (de)compress. If none given, use standard input.",
 "Report bugs to <bug-gzip@gnu.org>.",
  0};
    char **p = help_msg;

    printf ("%s %s\n(%s)\n", progname, VERSION, REVDATE);
    usage();
    while (*p) printf ("%s\n", *p++);
}

/* ======================================================================== */
local void license()
{
    char **p = license_msg;

    printf ("%s %s\n(%s)\n", progname, VERSION, REVDATE);
    while (*p) printf ("%s\n", *p++);
}

/* ======================================================================== */
local void version()
{
    license ();
    printf ("Compilation options:\n%s %s ", DIR_OPT, TIME_OPT);
#ifdef STDC_HEADERS
    printf ("STDC_HEADERS ");
#endif
#ifdef HAVE_UNISTD_H
    printf ("HAVE_UNISTD_H ");
#endif
#ifdef HAVE_MEMORY_H
    printf ("HAVE_MEMORY_H ");
#endif
#ifdef HAVE_STRING_H
    printf ("HAVE_STRING_H ");
#endif
#ifdef HAVE_LSTAT
    printf ("HAVE_LSTAT ");
#endif
#ifdef DEBUG
    printf ("DEBUG ");
#endif
#ifdef DYN_ALLOC
    printf ("DYN_ALLOC ");
#endif
#ifdef MAXSEG_64K
    printf ("MAXSEG_64K");
#endif
    printf ("\n");
    printf ("Written by Jean-loup Gailly.\n");
}

local void progerror (string)
    char *string;
{
    int e = errno;
    fprintf(stderr, "%s: ", progname);
    errno = e;
    perror(string);
    exit_code = ERROR;
}

/* ======================================================================== */
int main (argc, argv)
    int argc;
    char **argv;
{
    int file_count;     /* number of files to precess */
    int proglen;        /* length of progname */
    int optc;           /* current option */

    EXPAND(argc, argv); /* wild card expansion if necessary */

    progname = base_name (argv[0]);
    proglen = strlen(progname);

    /* Suppress .exe for MSDOS, OS/2 and VMS: */
    if (proglen > 4 && strequ(progname+proglen-4, ".exe")) {
        progname[proglen-4] = '\0';
    }

    foreground = signal(SIGINT, SIG_IGN) != SIG_IGN;
    if (foreground) {
	(void) signal (SIGINT, (sig_type)abort_gzip);
    }
#ifdef SIGTERM
    if (signal(SIGTERM, SIG_IGN) != SIG_IGN) {
	(void) signal(SIGTERM, (sig_type)abort_gzip);
    }
#endif
#ifdef SIGHUP
    if (signal(SIGHUP, SIG_IGN) != SIG_IGN) {
	(void) signal(SIGHUP,  (sig_type)abort_gzip);
    }
#endif

    z_suffix = Z_SUFFIX;
    z_len = strlen(z_suffix);

    while ((optc = getopt_long (argc, argv, "ab:cdfhH?lLmMnNqrS:tvVZ123456789",
				longopts, (int *)0)) != -1) {
	switch (optc) {
        case 'a':
            ascii = 1; break;
	case 'c':
	    to_stdout = 1; break;
	case 'h': case 'H': case '?':
	    help(); do_exit(OK); break;
	case 'L':
	    license(); do_exit(OK); break;
	case 'q':
	    quiet = 1; verbose = 0; break;
	case 'S':
            z_len = strlen(optarg);
	    z_suffix = optarg;
            break;
	case 'n':
	    /* Used to be --no-name, now ignored and always TRUE */
	    break;
	case 'v':
	    verbose++; quiet = 0; break;
	case 'V':
	    version(); do_exit(OK); break;
	case 'Z':
	    fprintf(stderr, "%s: -Z not supported in this version\n",
		    progname);
	    usage();
	    do_exit(ERROR); break;
	case '1':  case '2':  case '3':  case '4':
	case '5':  case '6':  case '7':  case '8':  case '9':
	    level = optc - '0';
	    break;
	default:
	    /* Error message already emitted by getopt_long. */
	    usage();
	    do_exit(ERROR);
	}
    } /* loop on all arguments */

#ifdef SIGPIPE
    /* Ignore "Broken Pipe" message with --quiet */
    if (quiet && signal (SIGPIPE, SIG_IGN) != SIG_IGN)
      signal (SIGPIPE, (sig_type) abort_gzip);
#endif

    file_count = argc - optind;

#if O_BINARY
#else
    if (ascii && !quiet) {
	fprintf(stderr, "%s: option --ascii ignored on this system\n",
		progname);
    }
#endif
    if ((z_len == 0) || z_len > MAX_SUFFIX) {
        fprintf(stderr, "%s: incorrect suffix '%s'\n",
                progname, optarg);
        do_exit(ERROR);
    }

    /* Allocate all global buffers (for DYN_ALLOC option) */
    ALLOC(uch, inbuf,  INBUFSIZ +INBUF_EXTRA);
    ALLOC(uch, outbuf, OUTBUFSIZ+OUTBUF_EXTRA);
    ALLOC(ush, d_buf,  DIST_BUFSIZE);
    ALLOC(uch, window, 2L*WSIZE);
#ifndef MAXSEG_64K
    ALLOC(ush, tab_prefix, 1L<<BITS);
#else
    ALLOC(ush, tab_prefix0, 1L<<(BITS-1));
    ALLOC(ush, tab_prefix1, 1L<<(BITS-1));
#endif

    /* And get to work */
    if (file_count != 0) {
	if (to_stdout && !test) {
	    SET_BINARY_MODE(fileno(stdout));
	}
        while (optind < argc) {
	    treat_file(argv[optind++]);
	}
    }
    do_exit(exit_code);
    return exit_code; /* just to avoid lint warning */
}

/* ========================================================================
 * Compress or decompress the given file
 */
local void treat_file(iname)
    char *iname;
{
    /* Check if the input file is present, set ifname and istat: */
    if (get_istat(iname, &istat) != OK) return;

    /* If the input name is that of a directory, recurse or ignore: */
    if (S_ISDIR(istat.st_mode)) {
	WARN((stderr,"%s: %s is a directory -- ignored\n", progname, ifname));
	return;
    }
    if (!S_ISREG(istat.st_mode)) {
	WARN((stderr,
	      "%s: %s is not a directory or a regular file - ignored\n",
	      progname, ifname));
	return;
    }
    if (istat.st_nlink > 1 && !to_stdout) {
	WARN((stderr, "%s: %s has %lu other link%c -- unchanged\n",
	      progname, ifname, (unsigned long) istat.st_nlink - 1,
	      istat.st_nlink > 2 ? 's' : ' '));
	return;
    }

    ifile_size = istat.st_size;
    time_stamp = 0;

    /* Generate output file name. For -r and (-t or -l), skip files
     * without a valid gzip suffix (check done in make_ofname).
     */
    if (to_stdout && !test) {
	strcpy(ofname, "stdout");

    } else if (make_ofname() != OK) {
	return;
    }

    /* Open the input file and determine compression method. The mode
     * parameter is ignored but required by some systems (VMS) and forbidden
     * on other systems (MacOS).
     */
    ifd = fopen(ifname, ascii ? "r" : "rb");
    if (ifd == NULL) {
	progerror(ifname);
	return;
    }
    clear_bufs(); /* clear input and output buffers */
    part_nb = 0;

    /* If compressing to a file, check if ofname is not ambiguous
     * because the operating system truncates names. Otherwise, generate
     * a new ofname and save the original name in the compressed file.
     */
    if (to_stdout) {
	ofd = stdout;
	/* keep remove_ofname as zero */
    } else {
	if (create_outfile() != OK) return;

	if (save_orig_name && !verbose && !quiet) {
	    fprintf(stderr, "%s: %s compressed to %s\n",
		    progname, ifname, ofname);
	}
    }
    /* Keep the name even if not truncated except with --no-name: */
    if (!save_orig_name) save_orig_name = 0;

    if (verbose) {
	fprintf(stderr, "%s:\t", ifname);
    }

    /* Actually do the compression/decompression. Loop over zipped members.
     */
    if (work(ifd, ofd) != OK) {
	method = -1; /* force cleanup */
    }

    fclose(ifd);
    if (!to_stdout && fclose(ofd)) {
	write_error();
    }
    if (method == -1) {
	if (!to_stdout) xunlink (ofname);
	return;
    }
    /* Display statistics */
    if(verbose) {
	if (test) {
	    fprintf(stderr, " OK");
	} else {
	    display_ratio(bytes_in-(bytes_out-header_bytes), bytes_in, stderr);
	}
	if (!test && !to_stdout) {
	    fprintf(stderr, " -- replaced with %s", ofname);
	}
	fprintf(stderr, "\n");
    }
    /* Copy modes, times, ownership, and remove the input file */
    if (!to_stdout) {
	copy_stat(&istat);
    }
}

/* ========================================================================
 * Create the output file. Return OK or ERROR.
 * Try several times if necessary to avoid truncating the z_suffix. For
 * example, do not create a compressed file of name "1234567890123."
 * Sets save_orig_name to true if the file name has been truncated.
 * IN assertions: the input file has already been open (ifd is set) and
 *   ofname has already been updated if there was an original name.
 * OUT assertions: ifd and ofd are closed in case of error.
 */
local int create_outfile()
{
    struct stat	ostat; /* stat for ofname */
    const char *mode = "wb";

    /* Make sure that ofname is not an existing file */
    if (check_ofname() != OK) {
	fclose(ifd);
	return ERROR;
    }
    /* Create the output file */
    remove_ofname = 1;
    ofd = fopen(ofname, mode);
    if (ofd == NULL) {
	progerror(ofname);
	fclose(ifd);
	return ERROR;
    }

    /* Check for name truncation on new file (1234567890123.gz) */
#ifdef NO_FSTAT
    if (stat(ofname, &ostat) != 0) {
#else
    if (fstat(fileno(ofd), &ostat) != 0) {
#endif
	progerror(ofname);
	fclose(ifd); fclose(ofd);
	xunlink (ofname);
	return ERROR;
    }
    return OK;
}

/* ========================================================================
 * Use lstat if available, except for -c or -f. Use stat otherwise.
 * This allows links when not removing the original file.
 */
local int do_stat(name, sbuf)
    char *name;
    struct stat *sbuf;
{
    errno = 0;
#ifdef HAVE_LSTAT
    if (!to_stdout) {
	return lstat(name, sbuf);
    }
#endif
    return stat(name, sbuf);
}

/* ========================================================================
 * Return a pointer to the 'z' suffix of a file name, or NULL. For all
 * systems, ".gz", ".z", ".Z", ".taz", ".tgz", "-gz", "-z" and "_z" are
 * accepted suffixes, in addition to the value of the --suffix option.
 * ".tgz" is a useful convention for tar.z files on systems limited
 * to 3 characters extensions. On such systems, ".?z" and ".??z" are
 * also accepted suffixes. For Unix, we do not want to accept any
 * .??z suffix as indicating a compressed file; some people use .xyz
 * to denote volume data.
 *   On systems allowing multiple versions of the same file (such as VMS),
 * this function removes any version suffix in the given name.
 */
local char *get_suffix(name)
    char *name;
{
    int nlen, slen;
    char suffix[MAX_SUFFIX+3]; /* last chars of name, forced to lower case */
    static char *known_suffixes[] =
       {NULL, ".gz", ".z", ".taz", ".tgz", "-gz", "-z", "_z",
#ifdef MAX_EXT_CHARS
          "z",
#endif
          NULL};
    char **suf = known_suffixes;

    *suf = z_suffix;
    if (strequ(z_suffix, "z")) suf++; /* check long suffixes first */

#ifdef SUFFIX_SEP
    /* strip a version number from the file name */
    {
	char *v = strrchr(name, SUFFIX_SEP);
 	if (v != NULL) *v = '\0';
    }
#endif
    nlen = strlen(name);
    if (nlen <= MAX_SUFFIX+2) {
        strcpy(suffix, name);
    } else {
        strcpy(suffix, name+nlen-MAX_SUFFIX-2);
    }
    strlwr(suffix);
    slen = strlen(suffix);
    do {
       int s = strlen(*suf);
       if (slen > s && suffix[slen-s-1] != PATH_SEP
           && strequ(suffix + slen - s, *suf)) {
           return name+nlen-s;
       }
    } while (*++suf != NULL);

    return NULL;
}


/* ========================================================================
 * Set ifname to the input file name (with a suffix appended if necessary)
 * and istat to its stats. For decompression, if no file exists with the
 * original name, try adding successively z_suffix, .gz, .z, -z and .Z.
 * For MSDOS, we try only z_suffix and z.
 * Return OK or ERROR.
 */
local int get_istat(iname, sbuf)
    char *iname;
    struct stat *sbuf;
{
    static char *suffixes[] = {NULL, ".gz", ".z", "-z", ".Z", NULL};
    char **suf = suffixes;

    *suf = z_suffix;

    if (sizeof (ifname) - 1 <= strlen (iname))
	goto name_too_long;

    strcpy(ifname, iname);

    /* If input file exists, return OK. */
    if (do_stat(ifname, sbuf) == 0)
	return OK;

    progerror(ifname);
    return ERROR;

 name_too_long:
    fprintf (stderr, "%s: %s: file name too long\n", progname, iname);
    exit_code = ERROR;
    return ERROR;
}

/* ========================================================================
 * Generate ofname given ifname. Return OK, or WARNING if file must be skipped.
 * Sets save_orig_name to true if the file name has been truncated.
 */
local int make_ofname()
{
    char *suff;            /* ofname z suffix */

    strcpy(ofname, ifname);
    /* strip a version number if any and get the gzip suffix if present: */
    suff = get_suffix(ofname);

    if (suff != NULL) {
	/* Avoid annoying messages with -r (see treat_dir()) */
	if (verbose || !quiet) {
	    WARN((stderr, "%s: %s already has %s suffix -- unchanged\n",
		  progname, ifname, suff));
	}
	return WARNING;
    } else {
        save_orig_name = 0;

	if (sizeof (ofname) <= strlen (ofname) + z_len)
	    goto name_too_long;
	strcat(ofname, z_suffix);

    } /* decompress ? */
    return OK;

 name_too_long:
    WARN ((stderr, "%s: %s: file name too long\n", progname, ifname));
    return WARNING;
}

/* ========================================================================
 * Return true if the two stat structures correspond to the same file.
 */
local int same_file(stat1, stat2)
    struct stat *stat1;
    struct stat *stat2;
{
    return stat1->st_ino   == stat2->st_ino
	&& stat1->st_dev   == stat2->st_dev
#ifdef NO_ST_INO
        /* Can't rely on st_ino and st_dev, use other fields: */
	&& stat1->st_mode  == stat2->st_mode
	&& stat1->st_uid   == stat2->st_uid
	&& stat1->st_gid   == stat2->st_gid
	&& stat1->st_size  == stat2->st_size
	&& stat1->st_atime == stat2->st_atime
	&& stat1->st_mtime == stat2->st_mtime
	&& stat1->st_ctime == stat2->st_ctime
#endif
	    ;
}

/* ========================================================================
 * Shorten the given name by one character, or replace a .tar extension
 * with .tgz. Truncate the last part of the name which is longer than
 * MIN_PART characters: 1234.678.012.gz -> 123.678.012.gz. If the name
 * has only parts shorter than MIN_PART truncate the longest part.
 * For decompression, just remove the last character of the name.
 *
 * IN assertion: for compression, the suffix of the given name is z_suffix.
 */
local void shorten_name(name)
    char *name;
{
    int len;                 /* length of name without z_suffix */
    char *trunc = NULL;      /* character to be truncated */
    int plen;                /* current part length */
    int min_part = MIN_PART; /* current minimum part length */
    char *p;

    len = strlen(name);
    p = get_suffix(name);
    if (p == NULL) error("can't recover suffix\n");
    *p = '\0';
    save_orig_name = 1;

    /* compress 1234567890.tar to 1234567890.tgz */
    if (len > 4 && strequ(p-4, ".tar")) {
	strcpy(p-4, ".tgz");
	return;
    }
    /* Try keeping short extensions intact:
     * 1234.678.012.gz -> 123.678.012.gz
     */
    do {
	p = strrchr(name, PATH_SEP);
	p = p ? p+1 : name;
	while (*p) {
	    plen = strcspn(p, PART_SEP);
	    p += plen;
	    if (plen > min_part) trunc = p-1;
	    if (*p) p++;
	}
    } while (trunc == NULL && --min_part != 0);

    if (trunc != NULL) {
	do {
	    trunc[0] = trunc[1];
	} while (*trunc++);
	trunc--;
    } else {
	trunc = strrchr(name, PART_SEP[0]);
	if (trunc == NULL) error("internal error in shorten_name");
	if (trunc[1] == '\0') trunc--; /* force truncation */
    }
    strcpy(trunc, z_suffix);
}

/* ========================================================================
 * If compressing to a file, check if ofname is not ambiguous
 * because the operating system truncates names. Otherwise, generate
 * a new ofname and save the original name in the compressed file.
 * If the compressed file already exists, ask for confirmation.
 *    The check for name truncation is made dynamically, because different
 * file systems on the same OS might use different truncation rules (on SVR4
 * s5 truncates to 14 chars and ufs does not truncate).
 *    This function returns -1 if the file must be skipped, and
 * updates save_orig_name if necessary.
 * IN assertions: save_orig_name is already set if ofname has been
 * already truncated because of NO_MULTIPLE_DOTS. The input file has
 * already been open and istat is set.
 */
local int check_ofname()
{
    struct stat	ostat; /* stat for ofname */

#ifdef ENAMETOOLONG
    /* Check for strictly conforming Posix systems (which return ENAMETOOLONG
     * instead of silently truncating filenames).
     */
    errno = 0;
    while (stat(ofname, &ostat) != 0) {
        if (errno != ENAMETOOLONG) return 0; /* ofname does not exist */
	shorten_name(ofname);
    }
#else
    if (stat(ofname, &ostat) != 0) return 0;
#endif
    /* Check that the input and output files are different (could be
     * the same by name truncation or links).
     */
    if (same_file(&istat, &ostat)) {
	if (strequ(ifname, ofname)) {
	    fprintf(stderr, "%s: %s: cannot %scompress onto itself\n",
		    progname, ifname, "");
	} else {
	    fprintf(stderr, "%s: %s and %s are the same file\n",
		    progname, ifname, ofname);
	}
	exit_code = ERROR;
	return ERROR;
    }
    if (xunlink (ofname)) {
	progerror(ofname);
	return ERROR;
    }
    return OK;
}


#ifndef NO_UTIME
/* ========================================================================
 * Set the access and modification times from the given stat buffer.
 */
local void reset_times (name, statb)
    char *name;
    struct stat *statb;
{
    struct utimbuf	timep;

    /* Copy the time stamp */
    timep.actime  = statb->st_atime;
    timep.modtime = statb->st_mtime;

    /* Some systems (at least OS/2) do not support utime on directories */
    if (utime(name, &timep) && !S_ISDIR(statb->st_mode)) {
	int e = errno;
	WARN((stderr, "%s: ", progname));
	if (!quiet) {
	    errno = e;
	    perror(ofname);
	}
    }
}
#endif


/* ========================================================================
 * Copy modes, times, ownership from input file to output file.
 * IN assertion: to_stdout is false.
 */
local void copy_stat(ifstat)
    struct stat *ifstat;
{
#ifndef NO_UTIME
    reset_times(ofname, ifstat);
#endif
    /* Copy the protection modes */
    if (chmod(ofname, ifstat->st_mode & 07777)) {
	int e = errno;
	WARN((stderr, "%s: ", progname));
	if (!quiet) {
	    errno = e;
	    perror(ofname);
	}
    }
#ifndef NO_CHOWN
    chown(ofname, ifstat->st_uid, ifstat->st_gid);  /* Copy ownership */
#endif
    remove_ofname = 0;
    /* It's now safe to remove the input file: */
    if (xunlink (ifname)) {
	int e = errno;
	WARN((stderr, "%s: ", progname));
	if (!quiet) {
	    errno = e;
	    perror(ifname);
	}
    }
}

/* ========================================================================
 * Free all dynamically allocated variables and exit with the given code.
 */
local void do_exit(exitcode)
    int exitcode;
{
    static int in_exit = 0;

    if (in_exit) exit(exitcode);
    in_exit = 1;
    FREE(inbuf);
    FREE(outbuf);
    FREE(d_buf);
    FREE(window);
#ifndef MAXSEG_64K
    FREE(tab_prefix);
#else
    FREE(tab_prefix0);
    FREE(tab_prefix1);
#endif
    exit(exitcode);
}

/* ========================================================================
 * Signal and error handler.
 */
RETSIGTYPE abort_gzip()
{
   if (remove_ofname) {
       fclose(ofd);
       xunlink (ofname);
   }
   do_exit(ERROR);
}
