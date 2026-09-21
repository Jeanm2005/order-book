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

enable_testing()

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE order_book_engine)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(feed_replay_tests tests/feed_replay_tests.cpp)
target_link_libraries(feed_replay_tests PRIVATE order_book_engine)
add_test(NAME feed_replay_tests COMMAND feed_replay_tests)
