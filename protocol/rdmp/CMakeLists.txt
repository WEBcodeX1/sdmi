cmake_minimum_required(VERSION 3.14)
project(rdmp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# External dependencies
# ---------------------------------------------------------------------------

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# On some systems (e.g. Debian/Ubuntu) curl headers are installed under the
# multiarch-qualified include directory; ensure they are discoverable.
if(CURL_FOUND AND NOT EXISTS "${CURL_INCLUDE_DIR}/curl/curl.h")
    find_path(CURL_INCLUDE_DIR NAMES curl/curl.h
        PATHS /usr/include/x86_64-linux-gnu
              /usr/local/include
        NO_DEFAULT_PATH)
    if(CURL_INCLUDE_DIR)
        message(STATUS "Using curl include dir: ${CURL_INCLUDE_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Shared library: protocol core (common utilities + backends)
# ---------------------------------------------------------------------------

add_library(rdmp_core STATIC
    src/rdmp_common.cpp
    src/rdmp_s3.cpp
    src/rdmp_local_files.cpp
    src/rdmp_backend.cpp
)

# Position-independent code is required so rdmp_core can be linked into the
# Python extension module (a shared library).
set_target_properties(rdmp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(rdmp_core PUBLIC include)
# Ensure the multiarch curl include path is available when headers live under
# a qualified directory (e.g. /usr/include/x86_64-linux-gnu on Debian/Ubuntu).
if(CURL_INCLUDE_DIR)
    target_include_directories(rdmp_core PRIVATE "${CURL_INCLUDE_DIR}")
endif()

target_link_libraries(rdmp_core PUBLIC
    CURL::libcurl
    OpenSSL::Crypto
)

# ---------------------------------------------------------------------------
# rdmp_client executable
# ---------------------------------------------------------------------------

add_executable(rdmp_client
    src/rdmp_client.cpp
    src/rdmp_client_main.cpp
)

target_include_directories(rdmp_client PRIVATE include)
target_link_libraries(rdmp_client PRIVATE rdmp_core)

# ---------------------------------------------------------------------------
# rdmp_server executable
# ---------------------------------------------------------------------------

add_executable(rdmp_server
    src/rdmp_server.cpp
    src/rdmp_server_main.cpp
)

target_include_directories(rdmp_server PRIVATE include)
target_link_libraries(rdmp_server PRIVATE rdmp_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Python bindings (Boost.Python)
# ---------------------------------------------------------------------------

find_package(Python3 COMPONENTS Development QUIET)
if(Python3_FOUND)
    add_subdirectory(python)
else()
    message(STATUS "Python3 not found – skipping Boost.Python bindings")
endif()

# ---------------------------------------------------------------------------
# Install targets (optional)
# ---------------------------------------------------------------------------

install(TARGETS rdmp_client rdmp_server RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/rdmp)
