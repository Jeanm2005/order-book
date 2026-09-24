cmake_minimum_required(VERSION 3.20)
project(order_book_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Debug builds are for correctness work only (property tests, sanitizers).
# Never benchmark a Debug build — see CLAUDE.md.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")

add_library(order_book_engine INTERFACE)
target_include_directories(order_book_engine INTERFACE include)

add_executable(order_book_main src/main.cpp)
target_link_libraries(order_book_main PRIVATE order_book_engine)

# Phase 2 (AF_XDP ingestion), default OFF: the default configure/CI never
# even probes for libbpf, so nothing about this option can affect the
# Phase 0/1 build. Needs libbpf-dev + clang (for the eBPF object) and, at
# runtime, CAP_NET_ADMIN/root and a real or loopback-capable NIC — see
# AGENTS.md "AF_XDP / eBPF work".
option(ENABLE_AF_XDP "Build the Phase 2 AF_XDP ingestion path" OFF)

if(ENABLE_AF_XDP)
    find_library(LIBBPF_LIBRARY NAMES bpf)
    find_path(LIBBPF_INCLUDE_DIR NAMES bpf/libbpf.h)
    if(NOT LIBBPF_LIBRARY OR NOT LIBBPF_INCLUDE_DIR)
        message(FATAL_ERROR "ENABLE_AF_XDP requires libbpf-dev (bpf/libbpf.h / libbpf.so not found)")
    endif()

    find_program(CLANG_BPF NAMES clang)
    if(NOT CLANG_BPF)
        message(FATAL_ERROR "ENABLE_AF_XDP requires clang to compile ebpf/xdp_redirect.c")
    endif()

    # Kernel-side object: compiled with clang's BPF backend, not the host
    # compiler. Same flags as this author's net-suite/ebpf-filter/Makefile.
    set(XDP_BPF_OBJ ${CMAKE_BINARY_DIR}/xdp_redirect.o)
    add_custom_command(
        OUTPUT ${XDP_BPF_OBJ}
        COMMAND ${CLANG_BPF} -O2 -g -target bpf
                -I/usr/include -I/usr/include/x86_64-linux-gnu
                -c ${CMAKE_SOURCE_DIR}/ebpf/xdp_redirect.c -o ${XDP_BPF_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/ebpf/xdp_redirect.c
        COMMENT "Compiling eBPF XDP redirect program -> ${XDP_BPF_OBJ}"
        VERBATIM
    )
    add_custom_target(xdp_bpf_obj ALL DEPENDS ${XDP_BPF_OBJ})

    target_sources(order_book_main PRIVATE src/xdp_socket.cpp src/xdp_listen_cmd.cpp)
    target_include_directories(order_book_main PRIVATE ${LIBBPF_INCLUDE_DIR})
    target_compile_definitions(order_book_main PRIVATE HAVE_AF_XDP XDP_BPF_OBJ_PATH="${XDP_BPF_OBJ}")
    target_link_libraries(order_book_main PRIVATE ${LIBBPF_LIBRARY} elf)
    add_dependencies(order_book_main xdp_bpf_obj)
endif()

enable_testing()

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE order_book_engine)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(feed_replay_tests tests/feed_replay_tests.cpp)
target_link_libraries(feed_replay_tests PRIVATE order_book_engine)
add_test(NAME feed_replay_tests COMMAND feed_replay_tests)

add_executable(signal_tests tests/signal_tests.cpp)
target_link_libraries(signal_tests PRIVATE order_book_engine)
add_test(NAME signal_tests COMMAND signal_tests)

add_executable(ladder_tests tests/ladder_tests.cpp)
target_link_libraries(ladder_tests PRIVATE order_book_engine)
add_test(NAME ladder_tests COMMAND ladder_tests)

add_executable(histogram_tests tests/histogram_tests.cpp)
target_link_libraries(histogram_tests PRIVATE order_book_engine)
add_test(NAME histogram_tests COMMAND histogram_tests)

# Phase 4 latency benchmark. Not a test: builds in every config (so CI keeps
# it compiling), but only a Release run on the dev box produces numbers —
# see AGENTS.md "Testing before any latency claim". Includes tests/ for the
# pre-swap MapOrderBook reference (--book map).
add_executable(order_book_bench bench/latency_bench.cpp)
target_link_libraries(order_book_bench PRIVATE order_book_engine)
target_include_directories(order_book_bench PRIVATE tests)

# A matching-loop bug tends to hang rather than fail (e.g. a stale best
# level that never empties) — bound every suite so CI fails instead of stalling.
set_tests_properties(order_book_tests feed_replay_tests signal_tests ladder_tests histogram_tests PROPERTIES TIMEOUT 120)
