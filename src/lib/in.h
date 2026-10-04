#ifndef NOTCURSES_IN
#define NOTCURSES_IN

#ifdef __cplusplus
extern "C" {
#endif

// internal header, not installed

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

struct tinfo;
struct inputctx;
struct ncsharedstats;

int init_inputlayer(struct tinfo* ti, FILE* infp, int lmargin, int tmargin,
                    int rmargin, int bmargin, struct ncsharedstats* stats,
                    unsigned drain, int linesigs_enabled)
  __attribute__ ((nonnull (1, 2, 7)));

int stop_inputlayer(struct tinfo* ti);

// (fork) How long the input layer waits for the rest of a terminal-generated
// control sequence that reached it in pieces before replaying the piece it
// has as keypresses. Only pieces of three or more bytes wait: an Escape, or
// an Alt-modified key whose byte begins a sequence, is two bytes at most and
// is delivered at once, as ever. See hold_partial_escape() in in.c.
#define NCINPUT_ESCAPE_HOLD_MS 100

// (fork) The decision behind NCINPUT_ESCAPE_HOLD_MS, apart from the clock so
// that it can be tested. An unfinished escape of |used| bytes ends the input
// read so far; |*deadline| is when the current hold expires (0 if there is
// none) and |now| the time, both CLOCK_MONOTONIC nanoseconds (a |now| of 0
// means the clock failed). True to keep waiting for the remainder, starting
// a hold if there is none; false to replay the bytes as input now.
static inline bool
ncinput_hold_escape(int used, uint64_t* deadline, uint64_t now){
  if(used < 3 || now == 0){
    return false;
  }
  if(*deadline == 0){
    *deadline = now + UINT64_C(1000000) * NCINPUT_ESCAPE_HOLD_MS;
    return true;
  }
  return now < *deadline;
}

// Owner-thread geometry polling; never waits for input or paints. Output
// retains the caller's last valid cell size until a report has arrived.
int inputlayer_poll_cell_geometry(struct inputctx* ictx, int fd,
                                  unsigned* y, unsigned* x);
void inputlayer_set_geometry(struct inputctx* ictx, unsigned rows, unsigned cols,
                             unsigned y, unsigned x);

int inputready_fd(const struct inputctx* ictx)
  __attribute__ ((nonnull (1)));

// allow another source provide raw input for distribution to client code.
// drops input if there is no room in appropriate output queue.
int ncinput_shovel(struct inputctx* ictx, const void* buf, int len)
  __attribute__ ((nonnull (1, 2)));

typedef enum {
    TERMINAL_UNKNOWN,       // no useful information from queries; use termname
    // the very limited linux VGA/serial console, or possibly the (deprecated,
    // pixel-drawable, RGBA8888) linux framebuffer console. *not* fbterm.
    TERMINAL_LINUX,         // ioctl()s
    // the linux KMS/DRM console, *not* kmscon, but DRM direct dumb buffers
    TERMINAL_LINUXDRM,      // ioctl()s
    TERMINAL_XTERM,         // XTVERSION == 'XTerm(ver)'
    TERMINAL_VTE,           // TDA: "~VTE"
    TERMINAL_KITTY,         // XTGETTCAP['TN'] == 'xterm-kitty'
    TERMINAL_FOOT,          // TDA: "\EP!|464f4f54\E\\"
    TERMINAL_MLTERM,        // XTGETTCAP['TN'] == 'mlterm'
    TERMINAL_TMUX,          // XTVERSION == "tmux ver"
    TERMINAL_GNUSCREEN,     // SDA: "83;ver;0c"
    TERMINAL_WEZTERM,       // XTVERSION == 'WezTerm *'
    TERMINAL_ALACRITTY,     // can't be detected; match TERM+SDA
    TERMINAL_CONTOUR,       // XTVERSION == 'contour ver'
    TERMINAL_ITERM,         // XTVERSION == 'iTerm2 [ver]'
    TERMINAL_TERMINOLOGY,   // TDA: "~~TY"
    TERMINAL_APPLE,         // Terminal.App, determined by TERM_PROGRAM + macOS
    TERMINAL_RXVT,          // rxvt/urxvt, determined by TERM + UNIX
    TERMINAL_MSTERMINAL,    // Microsoft Windows Terminal
    TERMINAL_MINTTY,        // XTVERSION == 'mintty ver' MinTTY (Cygwin, MSYS2)
    TERMINAL_KONSOLE,       // TDA: "~KDE" (7e4b4445)
    TERMINAL_GHOSTTY,       // XTVERSION == 'ghostty '
} queried_terminals_e;

// after spawning the input layer, send initial queries to the terminal. its
// responses will be built up herein. it's dangerous to go alone! take this!
struct initial_responses {
  int cursory;                 // cursor location, -1 for none
  int cursorx;                 // cursor location, -1 for none
  unsigned appsync_supported;  // is application-synchronized mode supported?
  queried_terminals_e qterm;   // determined terminal
  unsigned kitty_graphics;     // kitty graphics supported
  uint32_t bg;                 // default background
  uint32_t fg;                 // default foreground
  bool got_bg;                 // have we read default background?
  bool got_fg;                 // have we read default foreground?
  bool rgb;                    // was RGB DirectColor advertised?
  bool rectangular_edits;      // were rectangular edits advertised?
  int pixx;                    // screen geometry in pixels
  int pixy;                    // screen geometry in pixels
  int dimx;                    // screen geometry in cells
  int dimy;                    // screen geometry in cells
  // these next three might be set even if there is no actual Sixel support
  // (see e.g. XTerm prior to 370). we determine whether there is Sixel
  // support by checking the DA1 attributes, and scrub them if necessary.
  int color_registers;         // sixel color registers
  int sixely;                  // maximum sixel height
  int sixelx;                  // maximum sixel width
  char* version;               // version string, heap-allocated
  unsigned kbdlevel;           // enabled kitty keyboard functions
  ncpalette palette;           // palette entries
  int maxpaletteread;          // maximum palette index read
  bool pixelmice;              // have we pixel-based mice events?
  char* hpa;                   // control sequence for hpa via XTGETTCAP
};

// Blocking call. Waits until the input thread has processed all responses to
// our initial queries, and returns them.
struct initial_responses* inputlayer_get_responses(struct inputctx* ictx)
  __attribute__ ((nonnull (1)));

int get_cursor_location(struct inputctx* ictx, const char* u7, unsigned* y, unsigned* x)
  __attribute__ ((nonnull (1, 2)));

#ifdef __cplusplus
}
#endif

#endif
