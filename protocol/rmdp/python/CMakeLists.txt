cmake_minimum_required(VERSION 3.14)

# ---------------------------------------------------------------------------
# Python + Boost.Python
# ---------------------------------------------------------------------------

find_package(Python3 REQUIRED COMPONENTS Development)
find_package(Boost 1.71 QUIET COMPONENTS python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR})

if(NOT Boost_python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_FOUND AND NOT Boost_FOUND)
    message(STATUS "Boost.Python not found – skipping rdmp Python bindings")
    return()
endif()

# Check that the python component was actually found
if(NOT TARGET Boost::python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR})
    message(STATUS "Boost::python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR} target not available – skipping Python bindings")
    return()
endif()

# ---------------------------------------------------------------------------
# rdmp Python extension module
# ---------------------------------------------------------------------------

add_library(rdmp_python MODULE
    rdmp_python.cpp
    # Include client and server sources directly so all symbols resolve.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rdmp_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rdmp_server.cpp
)

# Boost.Python target name varies by Python version
set(BOOST_PYTHON_LIB
    Boost::python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR})

target_include_directories(rdmp_python PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${Python3_INCLUDE_DIRS}
)

target_link_libraries(rdmp_python PRIVATE
    rdmp_core
    ${BOOST_PYTHON_LIB}
    Python3::Python
)

# Python extension modules must not have a "lib" prefix and should have the
# platform ABI suffix (.so on Linux).
set_target_properties(rdmp_python PROPERTIES
    PREFIX ""
    OUTPUT_NAME "rdmp"
    SUFFIX "${Python3_SOABI}.so"
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

install(TARGETS rdmp_python
    LIBRARY DESTINATION ${Python3_SITEARCH}
    MODULE  DESTINATION ${Python3_SITEARCH}
)
