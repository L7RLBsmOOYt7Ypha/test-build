cmake_minimum_required(VERSION 3.20)
project(tosya-tools LANGUAGES CXX C)

add_compile_options(-Wall)

# The userspace half (sync-tool) and the timing instrument (uidbench).
#
# sync-tool: one process that watches HMA's config, evaluates it and pushes the
# resulting policy to the kernel over netlink.
# uidbench: the timing instrument for the module's hot path; it ships with
# nothing and is run by hand, on a device, before and after a change.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# The wire format both halves speak lives one directory up, beside the module
# sources that include the same file.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../include")

find_package(nlohmann_json REQUIRED)

add_executable(sync-tool
    abx.cpp
    text_xml.cpp
    packages_xml.cpp
    main.cpp
    sync.cpp
    watcher.cpp
    netlink.cpp
    status.cpp
    rules.cpp
    preset_rules.cpp
    packages.cpp
    paths.cpp
    paging.cpp)

target_link_libraries(sync-tool PRIVATE nlohmann_json::nlohmann_json dl z)
# Resolved at run time on purpose: __system_property_wait() and __system_property_serial() are
# platform-only (the NDK stubs carry no symbol for them), and a dynamic link is what lets dlsym()
# find them - with -static the NDK links its libdl.a stub, whose dlopen() always fails. libc++
# stays static, so nothing extra ships.


target_compile_options(sync-tool PRIVATE -O2 -Wall -Wextra)
target_link_options(sync-tool PRIVATE -s)

add_executable(uidbench uidbench.c)

# Cross compiling with the NDK: CMake filters the package's /usr/include out as an
# implicit directory, and the NDK's clang does not search the host /usr/include
# anyway. Adding /usr/include directly makes host C headers shadow libc++'s (which
# then #errors), so build a header root in the build tree that only contains nlohmann.
if(CMAKE_CROSSCOMPILING)
  get_target_property(_nlj_incs nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
  list(GET _nlj_incs 0 _nlj_root)
  set(_hdr_root "${CMAKE_BINARY_DIR}/include")
  file(MAKE_DIRECTORY "${_hdr_root}")
  file(REMOVE_RECURSE "${_hdr_root}/nlohmann")
  file(CREATE_LINK "${_nlj_root}/nlohmann" "${_hdr_root}/nlohmann" SYMBOLIC RESULT _lnk)
  target_include_directories(sync-tool PRIVATE "${_hdr_root}")
  message(STATUS "nlohmann/json: header root ${_hdr_root} -> ${_nlj_root}/nlohmann")
endif()
