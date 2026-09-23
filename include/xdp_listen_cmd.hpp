#pragma once

// Declared only when built with -DENABLE_AF_XDP (see CMakeLists.txt): the
// default build/CI never links libbpf or sees this symbol, so main.cpp can
// include this header unconditionally and gate the call site on the same
// HAVE_AF_XDP macro.
#ifdef HAVE_AF_XDP
int run_xdp_listen(int argc, char** argv);
#endif
