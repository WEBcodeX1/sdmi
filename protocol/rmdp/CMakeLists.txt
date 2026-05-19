cmake_minimum_required(VERSION 3.14)
project(rmdp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# External dependencies
# ---------------------------------------------------------------------------

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Shared library: protocol core (common utilities + backends)
# ---------------------------------------------------------------------------

add_library(rmdp_core STATIC
    src/rmdp_common.cpp
    src/rmdp_s3.cpp
    src/rmdp_local_files.cpp
    src/rmdp_backend.cpp
)

# Position-independent code is required so rmdp_core can be linked into the
# Python extension module (a shared library).
set_target_properties(rmdp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(rmdp_core PUBLIC include)

target_link_libraries(rmdp_core PUBLIC
    CURL::libcurl
    OpenSSL::Crypto
)

# ---------------------------------------------------------------------------
# rmdp_client executable
# ---------------------------------------------------------------------------

add_executable(rmdp_client
    src/rmdp_client.cpp
    src/rmdp_client_main.cpp
)

target_include_directories(rmdp_client PRIVATE include)
target_link_libraries(rmdp_client PRIVATE rmdp_core)

# ---------------------------------------------------------------------------
# rmdp_server executable
# ---------------------------------------------------------------------------

add_executable(rmdp_server
    src/rmdp_server.cpp
    src/rmdp_server_main.cpp
)

target_include_directories(rmdp_server PRIVATE include)
target_link_libraries(rmdp_server PRIVATE rmdp_core)

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

install(TARGETS rmdp_client rmdp_server RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/rmdp)
