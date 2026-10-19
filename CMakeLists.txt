cmake_minimum_required(VERSION 3.16)
project(sohamalloc VERSION 0.1.0 LANGUAGES C CXX)

option(SOHAM_DEBUG "Internal assertions" OFF)
option(SOHAM_BUILD_TESTS "Add the tests/ directory if it has a CMakeLists.txt" ON)
set(SOHAM_SANITIZE "" CACHE STRING "Sanitizer for the prefixed library: thread|address|undefined")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(SOHAM_SOURCES src/sohamalloc.c src/os_posix.c src/os_windows.c)

set(SOHAM_C_FLAGS
  -Wall -Wextra -fvisibility=hidden
  -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc -fno-builtin-realloc
  -fno-stack-protector)
if(NOT WIN32)
  list(APPEND SOHAM_C_FLAGS -ftls-model=initial-exec)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  list(APPEND SOHAM_C_FLAGS -fno-semantic-interposition)
endif()

set(SOHAM_DEFS "")
if(SOHAM_DEBUG)
  list(APPEND SOHAM_DEFS SOHAM_DEBUG=1)
endif()

find_package(Threads REQUIRED)

function(soham_common tgt)
  target_include_directories(${tgt} PUBLIC ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:C>:${SOHAM_C_FLAGS}>)
  target_compile_definitions(${tgt} PRIVATE ${SOHAM_DEFS})
  target_link_libraries(${tgt} PUBLIC Threads::Threads)
endfunction()

# Override libraries: malloc family + C++ operators.
add_library(sohamalloc SHARED ${SOHAM_SOURCES} src/sohamalloc_cxx.cpp)
soham_common(sohamalloc)
target_compile_definitions(sohamalloc PRIVATE SOHAM_OVERRIDE=1)
target_compile_options(sohamalloc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -fvisibility=hidden>)

add_library(sohamalloc_static STATIC ${SOHAM_SOURCES} src/sohamalloc_cxx.cpp)
soham_common(sohamalloc_static)
target_compile_definitions(sohamalloc_static PRIVATE SOHAM_OVERRIDE=1)
set_target_properties(sohamalloc_static PROPERTIES OUTPUT_NAME sohamalloc)

# Prefixed library: soham_* only (sanitizer testing of the allocator itself).
add_library(sohamalloc_prefixed STATIC ${SOHAM_SOURCES})
soham_common(sohamalloc_prefixed)
target_compile_definitions(sohamalloc_prefixed PRIVATE SOHAM_OVERRIDE=0)
set_target_properties(sohamalloc_prefixed PROPERTIES OUTPUT_NAME sohamalloc-prefixed)
if(SOHAM_SANITIZE)
  target_compile_options(sohamalloc_prefixed PRIVATE -fsanitize=${SOHAM_SANITIZE} -fno-omit-frame-pointer -g)
endif()

if(SOHAM_BUILD_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
