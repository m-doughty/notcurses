#ifndef NOTCURSES_WIN32INPUT
#define NOTCURSES_WIN32INPUT

// win32-input-mode transcoder.
//
// WHY THIS EXISTS. On Windows, notcurses consumes the console as a plain VT
// byte stream (read_windows_console() in in.c) and learns key modifiers only
// through the kitty keyboard protocol. conhost, when it is acting as ConPTY,
// deliberately ignores the kitty push/pop/query sequences (its AdaptDispatch
// early-returns on IsConPTY()), so behind ConPTY the kitty protocol can never
// engage: Ctrl+Enter and Shift+Enter arrive as a bare Enter, Alt+Enter only
// arrives if the terminal does not steal it for fullscreen (WezTerm and
// Windows Terminal both do), and Ctrl+punctuation is lost outright.
//
// The channel conhost does offer VT clients is "win32-input-mode"
// (microsoft/terminal doc/specs/#4999). After the client writes CSI ?9001h,
// conhost encodes every keyboard event as
//
//     CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
//
// (virtual key, scan code, UTF-16 unit, key-down flag, dwControlKeyState,
// repeat count; trailing parameters may be omitted, defaulting to 0 except Rc
// which defaults to 1). The records arrive in the same byte stream we already
// read, so this file rewrites each complete record, in place and before the
// automaton sees the buffer, into what a kitty terminal would have sent for
// the same key: CSI-u for chords and functional keys, the raw character for
// plain text. Everything downstream -- the automaton table, kitty_kbd_txt(),
// load_ncinput()'s normalisations, and every consumer's expectations -- is
// untouched, and Windows produces the same ncinputs macOS does by
// construction. The rewrite never grows a record (see the length notes at
// w32im_emit()), so a write cursor trailing the read cursor is sufficient.
//
// WHAT IS DROPPED. Key-up records, presses of modifier and lock keys, dead
// keys (Uc of 0 with no Ctrl/Alt, the composed character follows in its own
// record), and lone surrogates. Lock bits (NumLock, CapsLock, ScrollLock) are
// not propagated: ncinput_nomod_p() does not mask them and NumLock is on for
// nearly every Windows user with a keypad.
//
// SAFETY OF THE STARTUP INTERROGATION. Current conhost translates records at
// write time and writes its own query responses (DA1, DSR, XTSMGRAPHICS,
// DECRPM...) as text records that bypass the key encoder, so replies stay raw
// bytes. Older in-box consoles pushed responses through the encoder; two
// guards cover them: the mode is enabled only after notcurses_core_init() has
// sent and consumed every query it ever sends, and a synthesized-text record
// (Vk of 0) is transcoded back into its raw character here, so an encoded
// reply reassembles anyway.
//
// MODE LIFETIME. Mode 9001 is a per-console TerminalInput flag (not screen
// buffer scoped): alternate screen switches need no re-assertion, and it
// survives process exit, so notcurses_stop() disables it. A crashed client
// leaves it on for the next VT client on that console; this transcoder makes
// that harmless for notcurses (the records are decoded whether or not we asked
// for them), and conhost handles the disable locally rather than bubbling it
// to the hosting terminal, so the terminal's own win32-input-mode leg is never
// disturbed.
//
// This header is compiled on every platform so the transcoder can be unit
// tested everywhere (src/tests/win32input.cpp); only the enable/disable
// emission is Windows-gated (windows.c, notcurses.c).

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#ifdef __MINGW32__
// the same pair, in the same order, as compat.h. winsock2.h pulls in
// windows.h itself, and has to come before any other include of it.
#include <lmcons.h>
#include <winsock2.h>
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define W32IM_ENABLE  "\x1b[?9001h"
#define W32IM_DISABLE "\x1b[?9001l"

// dwControlKeyState bits (wincontypes.h)
#define W32IM_RIGHT_ALT   0x0001u
#define W32IM_LEFT_ALT    0x0002u
#define W32IM_RIGHT_CTRL  0x0004u
#define W32IM_LEFT_CTRL   0x0008u
#define W32IM_SHIFT       0x0010u
#define W32IM_NUMLOCK     0x0020u
#define W32IM_SCROLLLOCK  0x0040u
#define W32IM_CAPSLOCK    0x0080u
#define W32IM_ENHANCED    0x0100u

// the longest thing w32im_emit() writes (CSI 57427;8u is ten bytes)
#define W32IM_MAX_EMIT 16

// the longest incomplete record w32im_transcode() will hold back: CSI plus
// six parameters of at most six digits each, semicolons included, is 44.
#define W32IM_HOLD_MAX 48

typedef struct w32im_state {
  uint32_t hisurrogate; // pending UTF-16 high surrogate, 0 for none
  int holdlen;          // bytes of an incomplete record kept back in hold
  unsigned char hold[W32IM_HOLD_MAX];
} w32im_state;

typedef struct w32im_record {
  unsigned vk, sc, uc, kd, cs, rc;
} w32im_record;

// kitty modifier parameter: 1 + (shift 1 | alt 2 | ctrl 4). Lock bits are
// deliberately not carried (see the header comment).
static inline unsigned
w32im_mods(unsigned cs){
  unsigned m = 1;
  if(cs & W32IM_SHIFT){
    m += 1;
  }
  if(cs & (W32IM_LEFT_ALT | W32IM_RIGHT_ALT)){
    m += 2;
  }
  if(cs & (W32IM_LEFT_CTRL | W32IM_RIGHT_CTRL)){
    m += 4;
  }
  return m;
}

// virtual keys that produce no keypress of their own: modifiers, locks, the
// IME keys, and launcher/browser buttons
static inline bool
w32im_vk_silent(unsigned vk){
  return (vk >= 0x10 && vk <= 0x12)    // SHIFT CONTROL MENU
      || vk == 0x14                    // CAPITAL
      || (vk >= 0x15 && vk <= 0x1a)    // KANA .. KANJI
      || (vk >= 0x1c && vk <= 0x1f)    // CONVERT .. MODECHANGE
      || vk == 0x5b || vk == 0x5c      // LWIN RWIN
      || vk == 0x5f                    // SLEEP
      || vk == 0x90 || vk == 0x91      // NUMLOCK SCROLL
      || (vk >= 0xa0 && vk <= 0xac)    // L/R SHIFT CONTROL MENU, BROWSER_*
      || (vk >= 0xb4 && vk <= 0xb7)    // LAUNCH_*
      || vk == 0xe5                    // PROCESSKEY (IME)
      || (vk >= 0xf6 && vk <= 0xfe);   // ATTN .. OEM_CLEAR
}

// the unshifted character of a virtual key on the US layout, or 0. A virtual
// key names the letter or digit produced, not the physical key, so letters
// and digits are layout-independent; only the OEM punctuation keys vary.
static inline uint32_t
w32im_vk_base_char_us(unsigned vk){
  if(vk >= 0x30 && vk <= 0x39){
    return vk;
  }
  if(vk >= 0x41 && vk <= 0x5a){
    return vk + 0x20;
  }
  if(vk >= 0x60 && vk <= 0x69){
    return '0' + (vk - 0x60);
  }
  switch(vk){
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6c: return ',';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return ';';
    case 0xbb: return '=';
    case 0xbc: return ',';
    case 0xbd: return '-';
    case 0xbe: return '.';
    case 0xbf: return '/';
    case 0xc0: return '`';
    case 0xdb: return '[';
    case 0xdc: return '\\';
    case 0xdd: return ']';
    case 0xde: return '\'';
    case 0xe2: return '\\';
    default: return 0;
  }
}

// the unshifted character of a virtual key for the running layout. OEM keys
// are asked of the layout on Windows (the input thread's layout, which is the
// console's in every ordinary case); the US table is the fallback, and the
// only answer elsewhere.
static inline uint32_t
w32im_vk_base_char(unsigned vk){
#ifdef __MINGW32__
  if(vk >= 0xba && vk <= 0xe2){
    UINT c = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    if(c && !(c & 0x80000000u)){ // high bit flags a dead key
      c &= 0xffffu;
      if(c >= 'A' && c <= 'Z'){
        c += 0x20;
      }
      if(c >= 0x20 && c != 0x7f){
        return c;
      }
    }
  }
#endif
  return w32im_vk_base_char_us(vk);
}

static inline int
w32im_put_utf8(unsigned char* out, uint32_t cp){
  if(cp < 0x80){
    out[0] = (unsigned char)cp;
    return 1;
  }
  if(cp < 0x800){
    out[0] = (unsigned char)(0xc0 | (cp >> 6));
    out[1] = (unsigned char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if(cp >= 0xd800 && cp <= 0xdfff){
    return 0;
  }
  if(cp < 0x10000){
    out[0] = (unsigned char)(0xe0 | (cp >> 12));
    out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (unsigned char)(0x80 | (cp & 0x3f));
    return 3;
  }
  if(cp < 0x110000){
    out[0] = (unsigned char)(0xf0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
  }
  return 0;
}

static inline int
w32im_put_uint(unsigned char* out, unsigned v){
  unsigned char tmp[12];
  int n = 0;
  do{
    tmp[n++] = (unsigned char)('0' + v % 10);
    v /= 10;
  }while(v);
  for(int i = 0 ; i < n ; ++i){
    out[i] = tmp[n - 1 - i];
  }
  return n;
}

// CSI key ; mods u -- decoded by kitty_cb()
static inline int
w32im_put_csi_u(unsigned char* out, uint32_t key, unsigned mods){
  int n = 0;
  out[n++] = 0x1b;
  out[n++] = '[';
  n += w32im_put_uint(out + n, key);
  out[n++] = ';';
  n += w32im_put_uint(out + n, mods);
  out[n++] = 'u';
  return n;
}

// CSI 1 ; mods X (legacy_cb_*), or CSI X when unmodified
static inline int
w32im_put_csi_final(unsigned char* out, unsigned mods, unsigned char final){
  int n = 0;
  out[n++] = 0x1b;
  out[n++] = '[';
  if(mods > 1){
    out[n++] = '1';
    out[n++] = ';';
    n += w32im_put_uint(out + n, mods);
  }
  out[n++] = final;
  return n;
}

// CSI num ; mods ~ (wezterm_cb + legacy_functional), or CSI num ~ unmodified
static inline int
w32im_put_csi_tilde(unsigned char* out, unsigned num, unsigned mods){
  int n = 0;
  out[n++] = 0x1b;
  out[n++] = '[';
  n += w32im_put_uint(out + n, num);
  if(mods > 1){
    out[n++] = ';';
    n += w32im_put_uint(out + n, mods);
  }
  out[n++] = '~';
  return n;
}

// Translate one record into the bytes a kitty terminal would have sent for
// the same key. Writes at most W32IM_MAX_EMIT bytes to |out| and returns the
// count; 0 means the record produces nothing.
//
// Length invariant, relied on by w32im_transcode() rewriting in place: every
// emission is no longer than the shortest record that can produce it. The
// shortest real record is CSI Vk ; ; ; 1 _ (a key-down with everything else
// omitted); unmodified Backspace, Tab, Escape and Space are therefore emitted
// as their single legacy byte and unmodified Clear as CSI E, since their
// CSI-u forms would be one byte too long for that minimal shape.
static inline int
w32im_emit(const w32im_record* r, w32im_state* st, unsigned char* out){
  if(!r->kd){ // key-up: dropped before anything else, so a pending high
    return 0; // surrogate survives the release of its own key
  }
  if(r->vk > 0xff){
    return 0;
  }
  uint32_t uc = r->uc;
  if(uc >= 0xd800 && uc <= 0xdbff){
    st->hisurrogate = uc;
    return 0;
  }
  if(uc >= 0xdc00 && uc <= 0xdfff){
    if(!st->hisurrogate){
      return 0;
    }
    uc = 0x10000 + ((st->hisurrogate - 0xd800) << 10) + (uc - 0xdc00);
    st->hisurrogate = 0;
  }else{
    st->hisurrogate = 0;
  }
  // synthesized text: injected or pasted characters without a virtual key, and
  // VK_PACKET unicode injections. Also how an old conhost encodes its replies.
  if(r->vk == 0 || r->vk == 0xe7){
    return uc ? w32im_put_utf8(out, uc) : 0;
  }
  if(w32im_vk_silent(r->vk)){
    return 0;
  }
  const unsigned mods = w32im_mods(r->cs);
  const bool ctrl = (r->cs & (W32IM_LEFT_CTRL | W32IM_RIGHT_CTRL)) != 0;
  const bool alt = (r->cs & (W32IM_LEFT_ALT | W32IM_RIGHT_ALT)) != 0;
  const bool printable = uc >= 0x20 && uc != 0x7f;
  // functional keys are keyed on the virtual key alone: Uc lies for them
  // (Ctrl+Enter carries a line feed, Ctrl+Backspace carries 0x7f).
  switch(r->vk){
    case 0x0d: return w32im_put_csi_u(out, 13, mods); // RETURN, keypad too
    case 0x08: if(mods > 1){ return w32im_put_csi_u(out, 127, mods); }
               out[0] = 0x7f; return 1;
    case 0x09: if(mods > 1){ return w32im_put_csi_u(out, 9, mods); }
               out[0] = '\t'; return 1;
    case 0x1b: if(mods > 1){ return w32im_put_csi_u(out, 27, mods); }
               out[0] = 0x1b; return 1;
    case 0x20: if(mods > 1){ return w32im_put_csi_u(out, 32, mods); }
               out[0] = ' '; return 1;
    case 0x25: return w32im_put_csi_final(out, mods, 'D'); // LEFT
    case 0x26: return w32im_put_csi_final(out, mods, 'A'); // UP
    case 0x27: return w32im_put_csi_final(out, mods, 'C'); // RIGHT
    case 0x28: return w32im_put_csi_final(out, mods, 'B'); // DOWN
    case 0x24: return w32im_put_csi_final(out, mods, 'H'); // HOME
    case 0x23: return w32im_put_csi_final(out, mods, 'F'); // END
    case 0x0c: return w32im_put_csi_final(out, mods, 'E'); // CLEAR (keypad 5)
    case 0x70: return w32im_put_csi_final(out, mods, 'P'); // F1
    case 0x71: return w32im_put_csi_final(out, mods, 'Q'); // F2
    case 0x72: return w32im_put_csi_tilde(out, 13, mods);  // F3
    case 0x73: return w32im_put_csi_final(out, mods, 'S'); // F4
    case 0x74: return w32im_put_csi_tilde(out, 15, mods);  // F5
    case 0x75: return w32im_put_csi_tilde(out, 17, mods);  // F6
    case 0x76: return w32im_put_csi_tilde(out, 18, mods);  // F7
    case 0x77: return w32im_put_csi_tilde(out, 19, mods);  // F8
    case 0x78: return w32im_put_csi_tilde(out, 20, mods);  // F9
    case 0x79: return w32im_put_csi_tilde(out, 21, mods);  // F10
    case 0x7a: return w32im_put_csi_tilde(out, 23, mods);  // F11
    case 0x7b: return w32im_put_csi_tilde(out, 24, mods);  // F12
    case 0x2d: return w32im_put_csi_tilde(out, 2, mods);   // INSERT
    case 0x2e: return w32im_put_csi_tilde(out, 3, mods);   // DELETE
    case 0x21: return w32im_put_csi_tilde(out, 5, mods);   // PRIOR (PgUp)
    case 0x22: return w32im_put_csi_tilde(out, 6, mods);   // NEXT (PgDn)
    case 0x2c: return w32im_put_csi_u(out, 57361, mods);   // SNAPSHOT
    case 0x13: return w32im_put_csi_u(out, 57362, mods);   // PAUSE
    case 0x5d: return w32im_put_csi_u(out, 57363, mods);   // APPS (menu)
    case 0xb3: return w32im_put_csi_u(out, 57430, mods);   // MEDIA_PLAY_PAUSE
    case 0xb2: return w32im_put_csi_u(out, 57432, mods);   // MEDIA_STOP
    case 0xb0: return w32im_put_csi_u(out, 57435, mods);   // MEDIA_NEXT_TRACK
    case 0xb1: return w32im_put_csi_u(out, 57436, mods);   // MEDIA_PREV_TRACK
    case 0xae: return w32im_put_csi_u(out, 57438, mods);   // VOLUME_DOWN
    case 0xaf: return w32im_put_csi_u(out, 57439, mods);   // VOLUME_UP
    case 0xad: return w32im_put_csi_u(out, 57440, mods);   // VOLUME_MUTE
    default: break;
  }
  if(r->vk >= 0x7c && r->vk <= 0x87){ // F13 .. F24
    return w32im_put_csi_u(out, 57376 + (r->vk - 0x7c), mods);
  }
  // text as typed: plain, shifted, locked -- and AltGr compositions, where the
  // layout has already folded Ctrl+Alt into the character
  if(printable && (!(ctrl || alt) || (ctrl && alt))){
    return w32im_put_utf8(out, uc);
  }
  if(ctrl || alt){ // a chord: name the key, not the control code Uc carries
    uint32_t base = w32im_vk_base_char(r->vk);
    if(!base && printable){
      base = uc;
    }
    if(!base){
      return 0;
    }
    return w32im_put_csi_u(out, base, mods);
  }
  if(uc && uc < 0x20){ // an injected control character: pass it through raw
    out[0] = (unsigned char)uc;
    return 1;
  }
  return 0; // dead key, IME intermediate, or nothing typed
}

// Rewrite every complete win32-input-mode record in buf[0..*len) in place,
// copying every other byte through unchanged, and update *len. The last
// |fresh| bytes of the region were just read; whatever precedes them was
// already seen by an earlier call (and by the automaton, which leaves an
// unfinished escape sequence at the front of the buffer for the next read).
//
// A record cut off by the end of a read is not shown to the automaton at all,
// whose walk would fall off the trie on such a prefix and hand it up as a
// keypress of Escape followed by text: it is kept back in st->hold and put in
// front of the fresh bytes on the next call. The caller therefore leaves
// st->holdlen bytes free beyond *len when it reads. A tail of only ESC or
// ESC [ is copied through instead -- the automaton keeps those as a pending
// escape whatever the trie holds, and outside win32-input-mode (mintty, a
// redirected stdin) they can be a real Escape or Alt+[ that must not wait for
// the next key -- and is picked up by the rescan when the rest arrives.
//
// Returns the number of records rewritten (including ones that produced
// nothing), so the caller knows whether the automaton must restart its walk.
static inline int
w32im_transcode(unsigned char* buf, int* len, int fresh, w32im_state* st){
  if(st->holdlen){
    unsigned char* f = buf + *len - fresh;
    memmove(f + st->holdlen, f, fresh);
    memcpy(f, st->hold, st->holdlen);
    *len += st->holdlen;
    st->holdlen = 0;
  }
  const int n = *len;
  int r = 0, w = 0, records = 0;
  bool partial = false;
  while(r < n){
    if(buf[r] != 0x1b){
      buf[w++] = buf[r++];
      continue;
    }
    // ESC: is a record starting here? parse without committing.
    int p = r + 1;
    if(p >= n){ // a lone ESC at the end: copied through below
      break;
    }
    if(buf[p] != '['){
      buf[w++] = buf[r++];
      continue;
    }
    ++p;
    unsigned params[6] = { 0, 0, 0, 0, 0, 1, };
    int nparams = 0;
    bool record = false;
    while(p < n){
      if(nparams == 6){ // a seventh parameter: not a record
        break;
      }
      unsigned v = 0;
      int digits = 0;
      while(p < n && buf[p] >= '0' && buf[p] <= '9'){
        if(digits++ == 6){ // absurd for any field
          break;
        }
        v = v * 10 + (buf[p] - '0');
        ++p;
      }
      if(digits > 6){
        break;
      }
      if(p >= n){
        partial = true;
        break;
      }
      if(digits){
        params[nparams] = v;
      }
      ++nparams;
      if(buf[p] == ';'){
        ++p;
        if(p >= n){
          partial = true;
        }
        continue;
      }
      if(buf[p] == '_'){
        ++p;
        record = true;
      }
      break;
    }
    if(partial){
      break;
    }
    if(!record){ // some other CSI: copy the ESC and rescan from the next byte
      buf[w++] = buf[r++];
      continue;
    }
    ++records;
    w32im_record rec = { params[0], params[1], params[2],
                         params[3], params[4], params[5], };
    unsigned char tmp[W32IM_MAX_EMIT];
    const int m = w32im_emit(&rec, st, tmp);
    // Only bytes already consumed may be overwritten: the write cursor trails
    // the read cursor, so everything up to the end of this record is fair
    // game. conhost always writes all six parameters (14 bytes at the least),
    // longer than anything emitted; a hand-made record with the parameters
    // omitted can be too short, and then the key is dropped rather than the
    // unread bytes after it being clobbered.
    const int room = p - w;
    if(m > 0 && m <= room){
      unsigned copies = rec.rc > 1 ? rec.rc : 1;
      if(copies > (unsigned)(room / m)){ // honour repeats that fit
        copies = (unsigned)(room / m);
      }
      while(copies--){
        memcpy(buf + w, tmp, m);
        w += m;
      }
    }
    r = p;
  }
  if(r < n){
    const int tail = n - r;
    if(partial && tail <= W32IM_HOLD_MAX){ // an unfinished record: hold it back
      memcpy(st->hold, buf + r, tail);
      st->holdlen = tail;
    }else{
      memmove(buf + w, buf + r, tail);
      w += tail;
    }
  }
  *len = w;
  return records;
}

#ifdef __cplusplus
}
#endif

#endif
