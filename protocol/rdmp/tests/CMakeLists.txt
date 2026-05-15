cmake_minimum_required(VERSION 3.14)

# ---------------------------------------------------------------------------
# Fetch GoogleTest (header-only usage via FetchContent)
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent gtest from overriding our compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------

add_executable(rdmp_tests
    test_config.cpp
    test_common.cpp
    test_local_files_backend.cpp
    test_wire_format.cpp
    test_end_to_end.cpp
)

target_include_directories(rdmp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(rdmp_tests PRIVATE
    rdmp_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rdmp_tests)
