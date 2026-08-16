#include "termdesc.h"
#include "internal.h"
#include "windows.h"
#ifdef __MINGW32__
int restore_windows_console_input(tinfo* ti){
  if(!ti->input_mode_preserved_valid){
    return 0;
  }
  if(!SetConsoleMode(ti->inhandle, ti->input_mode_preserved)){
    logerror("couldn't restore input console mode");
    return -1;
  }
  return 0;
}

int restore_windows_console(tinfo* ti){
  int ret = restore_windows_console_input(ti);
  if(ti->output_mode_preserved_valid &&
     !SetConsoleMode(ti->outhandle, ti->output_mode_preserved)){
    logerror("couldn't restore output console mode");
    ret = -1;
  }
  return ret;
}

// ti has been memset to all zeroes. windows configuration is static.
int prepare_windows_terminal(tinfo* ti, size_t* tablelen, size_t* tableused){
  const struct wtermdesc {
    escape_e esc;
    const char* tinfo;
  } wterms[] = {
    { ESCAPE_CUP,   "\x1b[%i%p1%d;%p2%dH", },
    { ESCAPE_RMKX,  "\x1b[?1h", },
    { ESCAPE_SMKX,  "\x1b[?1l", },
    { ESCAPE_VPA,   "\x1b[%i%p1%dd", },
    { ESCAPE_HPA,   "\x1b[%i%p1%dG", },
    { ESCAPE_SC,    "\x1b[s", },
    { ESCAPE_RC,    "\x1b[u", },
    { ESCAPE_INITC, "\x1b]4;%p1%d;rgb:%p2%{255}%*%{1000}%/%2.2X/%p3%{255}%*%{1000}%/%2.2X/%p4%{255}%*%{1000}%/%2.2X\E\\", },
    { ESCAPE_CLEAR, "\x1b[2J", },
    { ESCAPE_SMCUP, "\x1b[?1049h", },
    { ESCAPE_RMCUP, "\x1b[?1049l", },
    { ESCAPE_SETAF, "\x1b[38;5;%i%p1%dm", },
    { ESCAPE_SETAB, "\x1b[48;5;%i%p1%dm", },
    { ESCAPE_OP,    "\x1b[39;49m", },
    { ESCAPE_CIVIS, "\x1b[?25l", },
    { ESCAPE_CNORM, "\x1b[?25h", },
    { ESCAPE_U7,    "\x1b[6n", },
    { ESCAPE_CUU,   "\x1b[A", },
    { ESCAPE_CUB,   "\x1b[D", },
    { ESCAPE_CUD,   "\x1b[B", },
    { ESCAPE_CUF,   "\x1b[C", },
    { ESCAPE_BOLD,  "\x1b[1m", },
    { ESCAPE_SITM,  "\x1b[3m", },
    { ESCAPE_RITM,  "\x1b[23m", },
    { ESCAPE_SMUL,  "\x1b[4m", },
    { ESCAPE_RMUL,  "\x1b[24m", },
    { ESCAPE_SGR0,  "\x1b[0m", },
    { ESCAPE_MAX, NULL, }
  }, *w;
  for(w = wterms ; w->tinfo; ++w){
    if(grow_esc_table(ti, w->tinfo, w->esc, tablelen, tableused)){
      return -1;
    }
  }
  ti->caps.rgb = true;
  ti->caps.colors = 256;
  ti->inhandle = GetStdHandle(STD_INPUT_HANDLE);
  ti->outhandle = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD inmode;
  DWORD outmode;
  const bool console_input = ti->inhandle &&
                             ti->inhandle != INVALID_HANDLE_VALUE &&
                             GetConsoleMode(ti->inhandle, &inmode);
  const bool console_output = ti->outhandle &&
                              ti->outhandle != INVALID_HANDLE_VALUE &&
                              GetConsoleMode(ti->outhandle, &outmode);
  if(!console_input || !console_output){
    // A test harness or redirected child can have pipe/file standard handles
    // rather than a ConPTY. Keep the static Windows escape table, but skip
    // console interrogation and input-mode changes. ttyfd gates the later
    // cbreak/query paths and default geometry supplies the off-screen size.
    ti->ttyfd = -1;
    ti->qterm = TERMINAL_MSTERMINAL;
    loginfo("using redirected Windows terminal mode");
    return 0;
  }
  if(!SetConsoleOutputCP(CP_UTF8)){
    logerror("couldn't set output page to utf8");
    return -1;
  }
  if(!SetConsoleCP(CP_UTF8)){
    logerror("couldn't set input page to utf8");
    return -1;
  }
  ti->input_mode_preserved = inmode;
  ti->input_mode_preserved_valid = true;
  ti->output_mode_preserved = outmode;
  ti->output_mode_preserved_valid = true;
  // This input layer consumes a VT byte stream through read(), not Win32
  // INPUT_RECORD structures.  Mixing ENABLE_VIRTUAL_TERMINAL_INPUT with
  // ENABLE_MOUSE_INPUT / ENABLE_WINDOW_INPUT can signal the console handle
  // for a record that read() cannot consume, leaving the input thread stuck
  // inside read() and making notcurses_stop() hang forever at pthread_join().
  // Mouse events are requested below using DECSET and arrive as SGR VT bytes.
  // Quick Edit must also stay disabled so selecting cannot suspend input.
  // ECHO_INPUT and LINE_INPUT are cleared later by cbreak_mode().
  inmode &= ~(ENABLE_INSERT_MODE | ENABLE_MOUSE_INPUT |
              ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
  inmode |= ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS |
            ENABLE_VIRTUAL_TERMINAL_INPUT;
  if(!SetConsoleMode(ti->inhandle, inmode)){
    logerror("couldn't set input console mode");
    return -1;
  }
  // if we're a true Windows Terminal, SetConsoleMode() ought succeed.
  // otherwise, we're something else; go ahead and try.
  // FIXME handle redirection to a file, where this fails
  outmode |= ENABLE_PROCESSED_OUTPUT
             | ENABLE_WRAP_AT_EOL_OUTPUT
             | ENABLE_VIRTUAL_TERMINAL_PROCESSING
             | DISABLE_NEWLINE_AUTO_RETURN
             | ENABLE_LVB_GRID_WORLDWIDE;
  if(!SetConsoleMode(ti->outhandle, outmode)){
    logerror("couldn't set output console mode");
    (void)restore_windows_console(ti);
    return -1;
  }
  loginfo("verified Windows ConPTY");
  // ConPTY intercepts most control sequences. It does pass through XTVERSION
  // (for now), but since it responds to the DA1 itself, we usually get that
  // prior to any XTVERSION response. We instead key off of mintty's pretty
  // reliable use of TERM_PROGRAM and TERM_PROGRAM_VERSION.
  const char* tp = getenv("TERM_PROGRAM");
  if(tp){
    if(strcmp(tp, "mintty") == 0){
      const char* ver = getenv("TERM_PROGRAM_VERSION");
      if(ver){
        ti->termversion = strdup(ver);
      }
      loginfo("detected mintty %s", ti->termversion ? ti->termversion : "");
      ti->qterm = TERMINAL_MINTTY;
      return 0;
    }
  }
  ti->qterm = TERMINAL_MSTERMINAL;
  return 0;
}
#endif
