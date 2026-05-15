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

# ---------------------------------------------------------------------------
# Shared library: protocol core (common utilities + backends)
# ---------------------------------------------------------------------------

add_library(rdmp_core STATIC
    src/rdmp_common.cpp
    src/rdmp_s3.cpp
    src/rdmp_local_files.cpp
    src/rdmp_backend.cpp
)

target_include_directories(rdmp_core PUBLIC include)

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
# Install targets (optional)
# ---------------------------------------------------------------------------

install(TARGETS rdmp_client rdmp_server RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/rdmp)
