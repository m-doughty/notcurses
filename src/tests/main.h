#ifndef NOTCURSES_TEST_MAIN
#define NOTCURSES_TEST_MAIN

#include <memory>
#include <unistd.h>
#include "version.h"
#include "builddef.h"
#include <signal.h>
#include <doctest/doctest.h>
#include <notcurses/notcurses.h>
#include <ncpp/NotCurses.hh>
#include <ncpp/_exceptions.hh>
#include "lib/internal.h"

struct free_deleter{
  template <typename T>
  void operator()(T *p) const {
    std::free(const_cast<std::remove_const_t<T>*>(p));
  }
};

auto is_test_tty() -> bool;
auto find_data(const char* datum) -> std::unique_ptr<char, free_deleter>;
auto testing_notcurses() -> struct notcurses*;
auto ncreel_validate(const ncreel* n) -> bool;
// (fork) this tester's own path, to run it again as a helper process
auto tester_path() -> const char*;
#ifndef __MINGW32__
// (fork) the tester run as "--piped-terminal-child <pty> <locked|open>
// <loglevel>" (input.cpp)
auto piped_terminal_child(const char* slave, bool locked) -> int;
#endif

#endif
