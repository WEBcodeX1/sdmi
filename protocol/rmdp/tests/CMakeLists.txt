cmake_minimum_required(VERSION 3.14)

# ---------------------------------------------------------------------------
# Boost.Test
# ---------------------------------------------------------------------------

find_package(Boost 1.71 REQUIRED COMPONENTS unit_test_framework)

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

target_compile_definitions(rdmp_tests PRIVATE
    BOOST_TEST_DYN_LINK
)

target_link_libraries(rdmp_tests PRIVATE
    rdmp_core
    Boost::unit_test_framework
)

include(CTest)
add_test(NAME rdmp_tests COMMAND rdmp_tests --log_level=message)
