#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "gzip.h"
#include "deflate.h"

static void
usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [-c] [-n] [-V] [-[0-9]] <ifilename> <ofilename>\n", progname);
    exit(EXIT_FAILURE);
}

static void
myerror(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\x1b[91m"
                    "Error: "
                    "\x1b[97m");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\x1b[0m"
                    "\n");
    va_end(ap);

    exit(EXIT_FAILURE);
}

int level;
FILE *ifd;
FILE *ofd;
off_t ifile_size;       /* input file size, -1 for devices (debug only) */
int method = DEFLATED;  /* compression method */
time_t time_stamp;      /* original time stamp (modification time) */
int verbose;            /* be verbose (-v) */
int save_orig_name;     /* set if original name must be saved */
unsigned insize;        /* valid bytes in inbuf */
unsigned inptr;         /* index of next byte to be processed in inbuf */
unsigned outcnt;        /* bytes in output buffer */
off_t bytes_in;         /* number of input bytes */
off_t bytes_out;        /* number of output bytes */
int  remove_ofname;	    /* remove output file on error */
char *ifname;           /* input file name */
char *ofname;           /* output file name */
char *progname;

DECLARE(uch, inbuf,  INBUFSIZ +INBUF_EXTRA);
DECLARE(uch, outbuf, OUTBUFSIZ+OUTBUF_EXTRA);
DECLARE(ush, d_buf,  DIST_BUFSIZE);
/* Add an extra WIZE to the window to make the insufficient lookahead bug deterministic,
   poking an 0xFF into position 4 matches OoT but may not match other use-cases */
DECLARE(uch, window, 2L*WSIZE + WSIZE) = { [2L*WSIZE+4] = 0xFF};
#ifndef MAXSEG_64K
DECLARE(ush, tab_prefix, 1L<<BITS);
#else
DECLARE(ush, tab_prefix0, 1L<<(BITS-1));
DECLARE(ush, tab_prefix1, 1L<<(BITS-1));
#endif

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

RETSIGTYPE abort_gzip()
{
   if (remove_ofname) {
       fclose(ofd);
       xunlink (ofname);
   }
   do_exit(ERROR);
}

int
main(int argc, char **argv)
{
    const char *ifilename = NULL;
    const char *ofilename = NULL;

    // parse args

#define arg_error(fmt, ...)                       \
    do {                                          \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        usage(argv[0]);                           \
    } while (0)

    int argn = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // Optional args
            if ('0' <= argv[i][1] && argv[i][1] <= '9') {
                level = argv[i][1] - '0';
                continue;
            }

            if (argv[i][1] == 'c') {
                continue;
            }

            if (argv[i][1] == 'n') {
                continue;
            }

            if (argv[i][1] == 'V') {
                verbose = 1;
                continue;
            }

            arg_error("Unknown option \"%s\"", argv[i]);
        } else {
            // Required args

            switch (argn) {
                case 0:
                    ifilename = argv[i];
                    break;
                case 1:
                    ofilename = argv[i];
                    break;
                default:
                    arg_error("Unknown positional argument \"%s\"", argv[i]);
                    break;
            }
            argn++;
        }
    }
    if (argn != 2)
        arg_error("Not enough positional arguments");

#undef arg_error

    FILE *ifile = fopen(ifilename, "rb");
    if (ifile == NULL)
        myerror("Could not open input file \"%s\" for reading", ifilename);

    FILE *ofile = fopen(ofilename, "wb");
    if (ofile == NULL)
        myerror("Could not open output file \"%s\" for writing", ofilename);

    // Construct gzip state
    fseek(ifile, 0, SEEK_END);
    ifile_size = ftell(ifile);
    fseek(ifile, 0, SEEK_SET);

    progname = argv[0];
    remove_ofname = 1;
    ifname = (char *)ifilename;
    ofname = (char *)ofilename;
    time_stamp = 0;
    clear_bufs();
    save_orig_name = 0;

    // Go
    if (zip(ifile, ofile) != OK) {
        myerror("gzip fail");
    }

    if(verbose) {
        display_ratio(bytes_in-(bytes_out-header_bytes), bytes_in, stderr);
        fprintf(stderr, "\n");
    }

    // Done
    fclose(ifile);
    fclose(ofile);

    return EXIT_SUCCESS;
}
