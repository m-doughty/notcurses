#include <string>
#include <vector>
#include "main.h"
#include "lib/win32input.h"

// Pure byte-level tests of the win32-input-mode transcoder. No notcurses
// instance and no terminal: the header is compiled on every platform so this
// runs on the Linux and macOS lanes too.

namespace {

// Feed |in| to the transcoder the way read_windows_console() does: the whole
// buffer is scanned, the last |fresh| bytes are the new read (all of them by
// default), and the buffer has room for the record held back last time.
std::string transcode(const std::string& in, w32im_state* st = nullptr, int fresh = -1){
  w32im_state local = { 0, 0, { 0, }, };
  if(!st){
    st = &local;
  }
  if(fresh < 0){
    fresh = static_cast<int>(in.size());
  }
  std::vector<unsigned char> buf(in.begin(), in.end());
  buf.resize(in.size() + W32IM_HOLD_MAX + 1, 0xee); // slack, poisoned
  int len = static_cast<int>(in.size());
  w32im_transcode(buf.data(), &len, fresh, st);
  return std::string(buf.begin(), buf.begin() + len);
}

// a key-down record as conhost writes it (always all six parameters)
std::string rec(unsigned vk, unsigned sc, unsigned uc, unsigned kd, unsigned cs, unsigned rc = 1){
  return "\x1b[" + std::to_string(vk) + ";" + std::to_string(sc) + ";"
         + std::to_string(uc) + ";" + std::to_string(kd) + ";"
         + std::to_string(cs) + ";" + std::to_string(rc) + "_";
}

constexpr unsigned CTRL = W32IM_LEFT_CTRL;
constexpr unsigned ALT = W32IM_LEFT_ALT;
constexpr unsigned SHIFT = W32IM_SHIFT;

} // namespace

TEST_CASE("Win32InputEnter") {
  CHECK("\x1b[13;1u" == transcode(rec(0x0d, 28, 13, 1, 0)));
  // Ctrl+Enter carries a line feed in Uc; the virtual key decides
  CHECK("\x1b[13;5u" == transcode(rec(0x0d, 28, 10, 1, CTRL)));
  CHECK("\x1b[13;2u" == transcode(rec(0x0d, 28, 13, 1, SHIFT)));
  CHECK("\x1b[13;3u" == transcode(rec(0x0d, 28, 13, 1, ALT)));
  CHECK("\x1b[13;6u" == transcode(rec(0x0d, 28, 13, 1, CTRL | SHIFT)));
  // keypad Enter is ENHANCED; NumLock is a lock bit and is not carried
  CHECK("\x1b[13;1u" == transcode(rec(0x0d, 28, 13, 1, W32IM_ENHANCED | W32IM_NUMLOCK)));
}

TEST_CASE("Win32InputText") {
  CHECK("a" == transcode(rec(0x41, 30, 'a', 1, 0)));
  CHECK("A" == transcode(rec(0x41, 30, 'A', 1, SHIFT)));
  CHECK("a" == transcode(rec(0x41, 30, 'a', 1, W32IM_NUMLOCK | W32IM_CAPSLOCK)));
  CHECK("!" == transcode(rec(0x31, 2, '!', 1, SHIFT)));
  CHECK(" " == transcode(rec(0x20, 57, ' ', 1, 0)));
  CHECK("\t" == transcode(rec(0x09, 15, '\t', 1, 0)));
  CHECK("\x1b" == transcode(rec(0x1b, 1, 27, 1, 0)));
  CHECK("\x7f" == transcode(rec(0x08, 14, 8, 1, 0)));
  CHECK("\xc3\xa9" == transcode(rec(0xba, 39, 0xe9, 1, 0))); // é, however the layout got there
  // keypad digits with NumLock on are plain digits
  CHECK("7" == transcode(rec(0x67, 71, '7', 1, W32IM_NUMLOCK)));
  // synthesized text (no virtual key) and VK_PACKET are the character as-is
  CHECK("x" == transcode(rec(0, 0, 'x', 1, 0)));
  CHECK("\xe4\xb8\xad" == transcode(rec(0xe7, 0, 0x4e2d, 1, 0)));
  // an old conhost's DA1 reply, encoded as synthesized characters, reassembles
  std::string reply;
  for(unsigned char c : std::string("\x1b[?61;4c")){
    reply += rec(0, 0, c, 1, 0);
  }
  CHECK("\x1b[?61;4c" == transcode(reply));
}

TEST_CASE("Win32InputChords") {
  // Ctrl+letter names the letter (lowercase; load_ncinput uppercases it)
  CHECK("\x1b[97;5u" == transcode(rec(0x41, 30, 1, 1, CTRL)));
  CHECK("\x1b[97;6u" == transcode(rec(0x41, 30, 1, 1, CTRL | SHIFT)));
  CHECK("\x1b[97;7u" == transcode(rec(0x41, 30, 0, 1, CTRL | ALT)));
  // Alt+letter: Windows hands over the character with the Alt bit
  CHECK("\x1b[120;3u" == transcode(rec(0x58, 45, 'x', 1, ALT)));
  // the control codes that legacy decoding would turn into other keys
  CHECK("\x1b[104;5u" == transcode(rec(0x48, 35, 8, 1, CTRL)));   // Ctrl+H, not Backspace
  CHECK("\x1b[105;5u" == transcode(rec(0x49, 23, 9, 1, CTRL)));   // Ctrl+I, not Tab
  CHECK("\x1b[109;5u" == transcode(rec(0x4d, 50, 13, 1, CTRL)));  // Ctrl+M, not Enter
  CHECK("\x1b[50;5u" == transcode(rec(0x32, 3, 0, 1, CTRL)));     // Ctrl+2
  // OEM keys name whatever the running layout puts on them (the US table
  // everywhere but Windows), so the expectation is computed, not spelled
  CHECK("\x1b[" + std::to_string(w32im_vk_base_char(0xdb)) + ";5u"
        == transcode(rec(0xdb, 26, 27, 1, CTRL)));                 // Ctrl+[ on US, not Escape
  CHECK("\x1b[" + std::to_string(w32im_vk_base_char(0xbc)) + ";5u"
        == transcode(rec(0xbc, 51, 0, 1, CTRL)));                  // Ctrl+, (VK_OEM_COMMA)
  CHECK('[' == w32im_vk_base_char_us(0xdb));
  CHECK(',' == w32im_vk_base_char_us(0xbc));
  CHECK("\x1b[127;5u" == transcode(rec(0x08, 14, 127, 1, CTRL))); // Ctrl+Backspace
  CHECK("\x1b[9;2u" == transcode(rec(0x09, 15, 9, 1, SHIFT)));    // Shift+Tab
  CHECK("\x1b[27;5u" == transcode(rec(0x1b, 1, 27, 1, CTRL)));    // Ctrl+Esc
  CHECK("\x1b[32;5u" == transcode(rec(0x20, 57, 0, 1, CTRL)));    // Ctrl+Space
  // AltGr: both Ctrl and Alt with a printable character is a composition
  CHECK("@" == transcode(rec(0x51, 16, '@', 1, CTRL | W32IM_RIGHT_ALT)));
  CHECK("@" == transcode(rec(0x51, 16, '@', 1, CTRL | W32IM_RIGHT_ALT | SHIFT)));
  // the base character comes from the US table when the layout offers nothing
  CHECK('\'' == w32im_vk_base_char_us(0xde));
  CHECK('`' == w32im_vk_base_char_us(0xc0));
  CHECK(0 == w32im_vk_base_char_us(0x10));
}

TEST_CASE("Win32InputFunctional") {
  CHECK("\x1b[A" == transcode(rec(0x26, 72, 0, 1, W32IM_ENHANCED)));
  CHECK("\x1b[1;5A" == transcode(rec(0x26, 72, 0, 1, W32IM_ENHANCED | CTRL)));
  CHECK("\x1b[1;2D" == transcode(rec(0x25, 75, 0, 1, W32IM_ENHANCED | SHIFT)));
  CHECK("\x1b[H" == transcode(rec(0x24, 71, 0, 1, W32IM_ENHANCED)));
  CHECK("\x1b[1;3F" == transcode(rec(0x23, 79, 0, 1, W32IM_ENHANCED | ALT)));
  CHECK("\x1b[E" == transcode(rec(0x0c, 76, 0, 1, 0)));
  CHECK("\x1b[P" == transcode(rec(0x70, 59, 0, 1, 0)));
  CHECK("\x1b[1;5P" == transcode(rec(0x70, 59, 0, 1, CTRL)));
  CHECK("\x1b[13~" == transcode(rec(0x72, 61, 0, 1, 0)));
  CHECK("\x1b[15~" == transcode(rec(0x74, 63, 0, 1, 0)));
  CHECK("\x1b[15;5~" == transcode(rec(0x74, 63, 0, 1, CTRL)));
  CHECK("\x1b[24;2~" == transcode(rec(0x7b, 88, 0, 1, SHIFT)));
  CHECK("\x1b[57376;1u" == transcode(rec(0x7c, 100, 0, 1, 0)));   // F13
  CHECK("\x1b[57387;5u" == transcode(rec(0x87, 111, 0, 1, CTRL))); // Ctrl+F24
  CHECK("\x1b[2~" == transcode(rec(0x2d, 82, 0, 1, W32IM_ENHANCED)));
  CHECK("\x1b[3;5~" == transcode(rec(0x2e, 83, 0, 1, W32IM_ENHANCED | CTRL)));
  CHECK("\x1b[5~" == transcode(rec(0x21, 73, 0, 1, W32IM_ENHANCED)));
  CHECK("\x1b[6~" == transcode(rec(0x22, 81, 0, 1, W32IM_ENHANCED)));
  CHECK("\x1b[57363;1u" == transcode(rec(0x5d, 93, 0, 1, 0)));   // APPS
  CHECK("\x1b[57440;1u" == transcode(rec(0xad, 32, 0, 1, 0)));   // VOLUME_MUTE
}

TEST_CASE("Win32InputDropped") {
  // key-ups
  CHECK("" == transcode(rec(0x41, 30, 'a', 0, 0)));
  CHECK("" == transcode("\x1b[17;29_"));
  // modifier and lock keys pressed alone
  CHECK("" == transcode(rec(0x10, 42, 0, 1, SHIFT)));
  CHECK("" == transcode(rec(0x11, 29, 0, 1, CTRL)));
  CHECK("" == transcode(rec(0xa0, 42, 0, 1, SHIFT)));
  CHECK("" == transcode(rec(0x5b, 91, 0, 1, 0)));
  CHECK("" == transcode(rec(0x14, 58, 0, 1, W32IM_CAPSLOCK)));
  CHECK("" == transcode(rec(0x90, 69, 0, 1, W32IM_NUMLOCK)));
  // a dead key press: nothing typed yet, the composed character follows
  CHECK("" == transcode(rec(0xdd, 27, 0, 1, 0)));
  // a lone low surrogate
  CHECK("" == transcode(rec(0, 0, 0xde00, 1, 0)));
  // an out-of-range virtual key
  CHECK("" == transcode(rec(0x1234, 0, 'a', 1, 0)));
}

TEST_CASE("Win32InputSurrogates") {
  // 😀 is U+1F600: high D83D, low DE00, as two records
  CHECK("\xf0\x9f\x98\x80" == transcode(rec(0, 0, 0xd83d, 1, 0) + rec(0, 0, 0xde00, 1, 0)));
  // the pair survives the key-up of the high half in between
  CHECK("\xf0\x9f\x98\x80" == transcode(rec(0, 0, 0xd83d, 1, 0) + rec(0, 0, 0xd83d, 0, 0)
                                        + rec(0, 0, 0xde00, 1, 0)));
  // the pair survives a read boundary between the two records
  w32im_state st = { 0, 0, { 0, }, };
  CHECK("" == transcode(rec(0, 0, 0xd83d, 1, 0), &st));
  CHECK("\xf0\x9f\x98\x80" == transcode(rec(0, 0, 0xde00, 1, 0), &st));
  // a high surrogate followed by an ordinary key is abandoned
  CHECK("a" == transcode(rec(0, 0, 0xd83d, 1, 0) + rec(0x41, 30, 'a', 1, 0)));
}

TEST_CASE("Win32InputRecordShapes") {
  // omitted trailing parameters, as the spec allows
  CHECK("a" == transcode("\x1b[65;30;97;1_"));
  CHECK("\x1b[13;5u" == transcode("\x1b[13;28;10;1;8_"));
  CHECK("\x1b[13;5u" == transcode("\x1b[13;;10;1;8;_"));
  // a repeat count is honoured within the footprint already consumed
  CHECK("aaa" == transcode(rec(0x41, 30, 'a', 1, 0, 3)));
  CHECK("aaa" == transcode(rec(0x41, 30, 'a', 1, 0, 0) + rec(0x41, 30, 'a', 1, 0) + rec(0x41, 30, 'a', 1, 0)));
  // a record too short for its own translation (nine bytes here, against
  // the ten of CSI 57363;1u) drops the key rather than overwrite the bytes
  // after it; the slack left by an earlier record (a key-up) makes room
  CHECK("\x1b[57376;1u" == transcode("\x1b[124;;;1_")); // exactly fits
  CHECK("" == transcode("\x1b[93;;;1_"));
  CHECK("z" == transcode("\x1b[93;;;1_" "z"));
  CHECK("\x1b[57363;1u" == transcode("\x1b[93;;;0_\x1b[93;;;1_"));
  CHECK("\x1b[57363;1u" "z" == transcode("\x1b[93;;;0_\x1b[93;;;1_" "z"));
  // records back to back, mixed with plain bytes and other sequences
  const std::string mouse = "\x1b[<0;10;5M";
  const std::string da1 = "\x1b[?61;4;6c";
  const std::string apc = "\x1b_Gi=1;OK\x1b\\";
  CHECK("x" + mouse + "\x1b[13;5u" + da1 + "y" + apc
        == transcode(rec(0x58, 45, 'x', 1, 0) + mouse + rec(0x0d, 28, 10, 1, CTRL)
                     + da1 + rec(0x59, 21, 'y', 1, 0) + apc));
  // plain text, a bare escape and a bare CSI pass through unchanged
  CHECK("hello\x1b" == transcode("hello\x1b"));
  CHECK("\x1b[" == transcode("\x1b["));
  CHECK("\x1b[x" == transcode("\x1b[x"));
  // an over-long or over-wide parameter list is not a record
  CHECK("\x1b[1;2;3;4;5;6;7_" == transcode("\x1b[1;2;3;4;5;6;7_"));
  CHECK("\x1b[12345678_" == transcode("\x1b[12345678_"));
}

TEST_CASE("Win32InputSplitRecords") {
  const std::string full = rec(0x0d, 28, 10, 1, CTRL);
  const std::string before = rec(0x58, 45, 'x', 1, 0);
  const std::string after = rec(0x59, 21, 'y', 1, 0);
  // a record cut at every point by the end of a read: a tail of ESC or ESC [
  // is passed through (the automaton keeps it pending and the rescan of the
  // next read sees it whole), anything longer is held back and spliced in
  // ahead of the next read
  for(size_t cut = 1 ; cut < full.size() ; ++cut){
    w32im_state st = { 0, 0, { 0, }, };
    const std::string head = full.substr(0, cut);
    const std::string tail = full.substr(cut);
    const std::string out1 = transcode(before + head, &st);
    std::string leftover;
    if(cut <= 2){
      CHECK("x" + head == out1);
      CHECK(0 == st.holdlen);
      leftover = head;
    }else{
      CHECK("x" == out1);
      CHECK(static_cast<int>(cut) == st.holdlen);
    }
    // the automaton has consumed "x" and keeps a pending escape, if any, at
    // the front; then the rest of the stream arrives
    const std::string out2 = transcode(leftover + tail + after, &st,
                                       static_cast<int>(tail.size() + after.size()));
    CHECK("\x1b[13;5u" "y" == out2);
    CHECK(0 == st.holdlen);
  }
  // a split that turns out not to be a record at all is passed through once
  // the deciding byte arrives (a cursor position report cut before its R)
  {
    w32im_state st = { 0, 0, { 0, }, };
    CHECK("" == transcode("\x1b[24;80", &st));
    CHECK(7 == st.holdlen);
    CHECK("\x1b[24;80R" == transcode("R", &st, 1));
    CHECK(0 == st.holdlen);
  }
  // a hold survives an empty read in between
  {
    w32im_state st = { 0, 0, { 0, }, };
    CHECK("" == transcode("\x1b[65;30;97", &st));
    CHECK("" == transcode("", &st, 0));
    CHECK(10 == st.holdlen);
    CHECK("a" == transcode(";1;0;1_", &st, 7));
  }
  // the longest prefix that can still be a record is held; one byte more and
  // it cannot be, so it is passed through
  {
    w32im_state st = { 0, 0, { 0, }, };
    const std::string widest = "\x1b[111111;222222;333333;444444;555555;666666;";
    CHECK("" == transcode(widest, &st));
    CHECK(static_cast<int>(widest.size()) == st.holdlen);
    CHECK(widest + "7" == transcode("7", &st, 1));
    CHECK(0 == st.holdlen);
  }
}

TEST_CASE("Win32InputNeverGrowsOrLeaksControls") {
  // over every virtual key, control-range character and common modifier
  // state: the rewrite never grows the buffer, and a chord never turns into a
  // bare control byte that legacy decoding would misread. The two text
  // records (no virtual key, VK_PACKET) are not chords: they carry whatever
  // character was injected, as is.
  const unsigned states[] = { 0, CTRL, ALT, SHIFT, CTRL | SHIFT, W32IM_NUMLOCK, };
  for(unsigned vk = 0 ; vk <= 0xff ; ++vk){
    for(unsigned uc = 0 ; uc < 32 ; ++uc){
      for(unsigned cs : states){
        const std::string in = rec(vk, 1, uc, 1, cs);
        const std::string out = transcode(in);
        CHECK(out.size() <= in.size());
        if((cs & (CTRL | ALT)) && vk != 0 && vk != 0xe7){
          for(unsigned char c : out){
            CHECK((c >= 32 || c == 0x1b));
          }
        }
      }
    }
  }
}
