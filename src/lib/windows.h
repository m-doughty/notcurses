#ifndef NOTCURSES_WINDOWS
#define NOTCURSES_WINDOWS

#ifdef __cplusplus
extern "C" {
#endif

struct tinfo;

// ti has been memset to all zeroes. windows configuration is static.
int prepare_windows_terminal(struct tinfo* ti, size_t* tablelen,
                             size_t* tableused);

// Restore the exact console modes inherited by prepare_windows_terminal().
// The input-only form supports NCDIRECT_OPTION_INHIBIT_CBREAK after terminal
// interrogation; the complete form is used by shutdown and error paths.
int restore_windows_console_input(struct tinfo* ti);
int restore_windows_console(struct tinfo* ti);

// Ask conhost to encode keyboard input as win32-input-mode records (enable),
// or to stop (disable). Both are no-ops without a console (ttyfd < 0). See
// win32input.h for the decoding side and the mode's lifetime.
int win32_input_mode(struct tinfo* ti, bool enable);

#ifdef __cplusplus
}
#endif

#endif
