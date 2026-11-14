# SPDX-License-Identifier: MIT
#
# CANsub RP1210 driver (CANSUB32.dll), its tools and tests.
#
#   cmake --preset x64            configure (or x86)
#   cmake --build --preset x64    build into build/x64/
#
# build.bat runs both architectures and assembles dist/.

cmake_minimum_required(VERSION 3.27)

# Static C runtime (/MT) for everything, the SDK included. The driver DLL is loaded into third-party
# processes on machines that may not have the Visual C++ runtime installed. Set before project() so
# the SDK's targets pick it up too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(rp1210_cansub VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The RP1210 driver is Windows only (RP1210 is a Windows API, and libcansub uses Winsock).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything runnable lands directly in build/<arch>/, without a Release/ subfolder, so the tests and
# the documentation can point at one fixed place. $<0:> stops multi-config generators appending one.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<0:>")

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/W3> $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

# ---------------------------------------------------------------------------------------------------
# The official C/C++ SDK for CANsub: TLS, HTTP, WebSocket and the frame codec.
# ---------------------------------------------------------------------------------------------------

set(CANSUB_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/c-cpp-sdk-cansub")

if(NOT EXISTS "${CANSUB_SDK_DIR}/include/cansub/cansub_ffi.h")
    message(FATAL_ERROR
        "The CANsub SDK is missing from third_party/c-cpp-sdk-cansub. Fetch it with:\n"
        "  git config core.longpaths true\n"
        "  git submodule update --init")
endif()

# EXCLUDE_FROM_ALL: only the static library is built, and only because libcansub links it.
add_subdirectory("${CANSUB_SDK_DIR}" sdk EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------------------------------
# libcansub: the connection layer on top of the SDK (sockets, I/O thread, channel setup, discovery),
# behind a plain C++ API. Nothing outside libcansub/src/sdk/ includes an SDK header.
# ---------------------------------------------------------------------------------------------------

add_library(libcansub STATIC
    libcansub/src/status.cpp
    libcansub/src/bit_timing.cpp
    libcansub/src/bus.cpp
    libcansub/src/sdk/socket.cpp
    libcansub/src/sdk/session.cpp
    libcansub/src/transport/json.cpp
    libcansub/src/transport/mdns.cpp
    libcansub/src/transport/discover.cpp
)

target_include_directories(libcansub
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/libcansub/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libcansub/src"
)

# bcrypt: the SDK's static library calls BCryptGenRandom (mbedTLS entropy) but does not declare the
# dependency, so a consumer has to name it.
target_link_libraries(libcansub
    PRIVATE cansub::cansub
    PUBLIC  ws2_32 iphlpapi bcrypt
)

# The unit tests reach into libcansub's internals (the SDK glue and the mDNS parser).
add_library(libcansub_internal INTERFACE)
target_include_directories(libcansub_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/libcansub/src")
target_link_libraries(libcansub_internal INTERFACE libcansub cansub::cansub)

# ---------------------------------------------------------------------------------------------------
# The RP1210 driver
# ---------------------------------------------------------------------------------------------------

add_library(rp1210_driver SHARED
    src/rp1210_api.cpp
    src/client.cpp
    src/ini.cpp
    src/j1939.cpp
    src/trace.cpp
    src/rp1210_codes.c
    # The .def file keeps the __stdcall exports undecorated, which RP1210 requires: host applications
    # resolve them by plain name.
    src/CANSUB32.def
)

set_target_properties(rp1210_driver PROPERTIES OUTPUT_NAME CANSUB32 PREFIX "")
target_include_directories(rp1210_driver PRIVATE include src)
target_link_libraries(rp1210_driver PRIVATE libcansub)

# The driver version is project(VERSION) above: RP1210_ReadVersion and ReadDetailedVersion report it,
# and the release workflow checks that the tag matches it.
target_compile_definitions(rp1210_driver PRIVATE
    RP1210_CANSUB_VERSION="${PROJECT_VERSION}"
    RP1210_CANSUB_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}"
    RP1210_CANSUB_VERSION_MINOR="${PROJECT_VERSION_MINOR}"
)

# The vendor INI declares the same version (Version=), so applications that show it agree.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/ini/CANSUB32.ini" _ini_version REGEX "^Version=")
string(STRIP "${_ini_version}" _ini_version)  # a CRLF checkout leaves a trailing \r
if(NOT _ini_version STREQUAL "Version=${PROJECT_VERSION}")
    message(FATAL_ERROR "ini/CANSUB32.ini says '${_ini_version}', but the project version is ${PROJECT_VERSION}. Update one of them.")
endif()

# The INI beside the DLL, so build/<arch>/ can be tested without installing anything.
add_custom_command(TARGET rp1210_driver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/ini/CANSUB32.ini" "${CMAKE_BINARY_DIR}/CANSUB32.ini")

# ---------------------------------------------------------------------------------------------------
# Tools shipped to users
# ---------------------------------------------------------------------------------------------------

# Standalone connection test. Links nothing of ours: it loads CANSUB32.dll by name at runtime, exactly
# as a diagnostic application does.
add_executable(rp1210_connect tools/rp1210_connect.cpp)

add_executable(cansub_discover tools/cansub_discover.cpp)
target_link_libraries(cansub_discover PRIVATE libcansub)

# Prints frames until interrupted. A development aid, not shipped.
add_executable(dump libcansub/examples/dump.cpp)
target_link_libraries(dump PRIVATE libcansub)

# ---------------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------------

enable_testing()

# No hardware needed. These run in CI.
add_executable(test_glue tests/unit/test_glue.cpp)
target_link_libraries(test_glue PRIVATE libcansub_internal)
add_test(NAME test_glue COMMAND test_glue)

add_executable(test_mdns tests/unit/test_mdns.cpp)
target_link_libraries(test_mdns PRIVATE libcansub_internal)
add_test(NAME test_mdns COMMAND test_mdns)

# The J1939 identifier and transport layer is free of libcansub and of the device by design.
add_executable(test_j1939 tests/unit/test_j1939.cpp src/j1939.cpp)
target_include_directories(test_j1939 PRIVATE src)
add_test(NAME test_j1939 COMMAND test_j1939)

# Needs a CANsub with CH1 and CH2 wired together: test_bus [host]. Not registered with CTest.
add_executable(test_bus tests/hardware/test_bus.cpp)
target_link_libraries(test_bus PRIVATE libcansub)
