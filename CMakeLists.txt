cmake_minimum_required(VERSION 3.20)
project(CapySR CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8 /bigobj /wd4100 /wd4267 /wd4244 /wd4065)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(PROTOC_EXECUTABLE protoc REQUIRED)

# ---------------------------------------------------------------- codegen ----
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_SOURCE_DIR}/src/proto/gen)
set(PROTO_SHARDS 8)

file(GLOB PROTO_FILES CONFIGURE_DEPENDS ${PROTO_DIR}/*.proto)

set(PROTO_GEN_SOURCES ${PROTO_GEN_DIR}/protos.h)
math(EXPR LAST_SHARD "${PROTO_SHARDS} - 1")
foreach(i RANGE ${LAST_SHARD})
    list(APPEND PROTO_GEN_SOURCES ${PROTO_GEN_DIR}/protos_${i}.cpp)
endforeach()

add_custom_command(
    OUTPUT ${PROTO_GEN_SOURCES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_proto.py
            --proto-dir ${PROTO_DIR}
            --seeds ${PROTO_DIR}/seeds.txt
            --out-dir ${PROTO_GEN_DIR}
            --protoc ${PROTOC_EXECUTABLE}
            --shards ${PROTO_SHARDS}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_proto.py ${PROTO_DIR}/seeds.txt ${PROTO_FILES}
    COMMENT "Generating protocol structs"
    VERBATIM)

set(CMDID_GEN_SOURCES ${CMAKE_SOURCE_DIR}/src/net/cmd_ids.h ${CMAKE_SOURCE_DIR}/src/net/cmd_ids.cpp)
add_custom_command(
    OUTPUT ${CMDID_GEN_SOURCES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_cmdids.py
            --input ${PROTO_DIR}/cmdids.txt --out-dir ${CMAKE_SOURCE_DIR}/src/net
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_cmdids.py ${PROTO_DIR}/cmdids.txt
    COMMENT "Generating command ids"
    VERBATIM)

# ------------------------------------------------------------ third party ----
add_library(ikcp STATIC third_party/ikcp/ikcp.c)
target_include_directories(ikcp PUBLIC third_party/ikcp)
if(MSVC)
    target_compile_options(ikcp PRIVATE /W0)
else()
    target_compile_options(ikcp PRIVATE -w)
endif()

# ----------------------------------------------------------------- server ----
file(GLOB_RECURSE CAPYSR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_SOURCE_DIR}/src/data/*.cpp
    ${CMAKE_SOURCE_DIR}/src/game/*.cpp
    ${CMAKE_SOURCE_DIR}/src/http/*.cpp
    ${CMAKE_SOURCE_DIR}/src/net/*.cpp
    ${CMAKE_SOURCE_DIR}/src/sdk/*.cpp)
list(REMOVE_ITEM CAPYSR_SOURCES ${CMAKE_SOURCE_DIR}/src/net/cmd_ids.cpp)

# The generated headers are included by anything that touches the protocol, so every
# target has to wait for codegen -- not just the one that lists it as a source.
add_custom_target(capysr_codegen DEPENDS ${PROTO_GEN_SOURCES} ${CMDID_GEN_SOURCES})

add_library(capysr_core STATIC
    ${CAPYSR_SOURCES}
    ${PROTO_GEN_SOURCES}
    ${CMDID_GEN_SOURCES})
add_dependencies(capysr_core capysr_codegen)

target_include_directories(capysr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json)

target_link_libraries(capysr_core PUBLIC ikcp)
if(WIN32)
    target_link_libraries(capysr_core PUBLIC ws2_32 mswsock dbghelp)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(capysr_core PUBLIC Threads::Threads)
endif()

add_executable(capysr src/main.cpp)
add_dependencies(capysr capysr_codegen)
target_link_libraries(capysr PRIVATE capysr_core)

# ------------------------------------------------------------------ tests ----
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
if(TEST_SOURCES)
    add_executable(capysr_tests ${TEST_SOURCES})
    add_dependencies(capysr_tests capysr_codegen)
    target_include_directories(capysr_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(capysr_tests PRIVATE capysr_core)
    enable_testing()
    add_test(NAME capysr_tests COMMAND capysr_tests)
endif()
