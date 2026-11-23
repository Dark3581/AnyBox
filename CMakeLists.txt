cmake_minimum_required(VERSION 3.20)
project(AnyBox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ANYBOX_RELINKER_ONLY "Build only the relinker and its tests" OFF)
include(CTest)

find_package(Python3 COMPONENTS Interpreter)

set(ANYBOX_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(ANYBOX_LIBS_DIR ${CMAKE_BINARY_DIR}/libs)
file(MAKE_DIRECTORY ${ANYBOX_GENERATED_DIR} ${ANYBOX_LIBS_DIR})

include(cmake/Version.cmake)
include(cmake/LibraryMap.cmake)
include(cmake/Shaders.cmake)

add_subdirectory(core/relinker)

if(NOT ANYBOX_RELINKER_ONLY)
    if(NOT WIN32)
        message(FATAL_ERROR "The AnyBox libraries are Windows DLLs. Configure with -DANYBOX_RELINKER_ONLY=ON on this host, or build on Windows.")
    endif()
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/CMakeLists.txt)
        message(FATAL_ERROR "3rdparty/Vulkan-Headers is missing: run git submodule update --init")
    endif()
    add_subdirectory(3rdparty/Vulkan-Headers EXCLUDE_FROM_ALL)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/3rdparty/zlib/inflate.c)
        message(FATAL_ERROR "3rdparty/zlib is missing: run git submodule update --init")
    endif()
    add_library(abx_zlib STATIC
        3rdparty/zlib/adler32.c 3rdparty/zlib/crc32.c 3rdparty/zlib/inffast.c 3rdparty/zlib/inflate.c 3rdparty/zlib/inftrees.c 3rdparty/zlib/zutil.c)
    target_include_directories(abx_zlib PUBLIC ${CMAKE_SOURCE_DIR}/3rdparty/zlib)
    set_target_properties(abx_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_subdirectory(core/libs)
endif()

if(BUILD_TESTING AND Python3_FOUND)
    file(GLOB ANYBOX_TOOL_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/tests/test_*.py)
    foreach(test_file IN LISTS ANYBOX_TOOL_TESTS)
        get_filename_component(test_name ${test_file} NAME_WE)
        add_test(NAME tools_${test_name} COMMAND ${Python3_EXECUTABLE} ${test_file} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()
