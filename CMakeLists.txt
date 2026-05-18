cmake_minimum_required(VERSION 3.28)
project(augene2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)

option(AUGENE2_BUILD_TESTS "Build augene2 tests" OFF)

set(ANTLR4_VERSION 4.13.2)
set(ANTLR4_JAR_PATH "${CMAKE_BINARY_DIR}/tools/antlr-${ANTLR4_VERSION}-complete.jar")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
if(NOT EXISTS "${ANTLR4_JAR_PATH}")
    file(DOWNLOAD
        "https://www.antlr.org/download/antlr-${ANTLR4_VERSION}-complete.jar"
        "${ANTLR4_JAR_PATH}"
        SHOW_PROGRESS
    )
endif()
set(ANTLR_EXECUTABLE "${ANTLR4_JAR_PATH}" CACHE FILEPATH "ANTLR tool jar")
find_package(ANTLR REQUIRED)

set(_AUGENE2_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
CPMAddPackage(
    NAME midicci
    GIT_TAG 7c0c6a5
    GIT_REPOSITORY https://github.com/atsushieno/midicci
    OPTIONS "MIDICCI_SKIP_TOOLS ON" "BUILD_TESTS OFF" "BUILD_EXAMPLES OFF"
    EXCLUDE_FROM_ALL
)
if(DEFINED _AUGENE2_OLD_BUILD_SHARED_LIBS AND NOT "${_AUGENE2_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_AUGENE2_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

set(_AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
CPMAddPackage(
    NAME antlr4_runtime
    GIT_TAG ${ANTLR4_VERSION}
    GIT_REPOSITORY https://github.com/antlr/antlr4
    SOURCE_SUBDIR runtime/Cpp
    OPTIONS "ANTLR_BUILD_CPP_TESTS OFF" "ANTLR_BUILD_SHARED OFF" "ANTLR_BUILD_STATIC ON"
    EXCLUDE_FROM_ALL
)
if(DEFINED _AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS AND NOT "${_AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

set(AUGENE2_ANTLR_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/antlr")
ANTLR_TARGET(Augene2Lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/grammar/MugeneLexer.g4
    LEXER
    OUTPUT_DIRECTORY ${AUGENE2_ANTLR_OUTPUT_DIR}
)
ANTLR_TARGET(Augene2Parser
    ${CMAKE_CURRENT_SOURCE_DIR}/grammar/MugeneParser.g4
    PARSER
    VISITOR
    OUTPUT_DIRECTORY ${AUGENE2_ANTLR_OUTPUT_DIR}
    DEPENDS_ANTLR Augene2Lexer
)

add_library(mugene2 STATIC
    src/mugene2/mugene2.cpp
    src/mugene2/detail/compiler_internal.cpp
    src/mugene2/detail/antlr_smoke.cpp
    ${ANTLR_Augene2Lexer_CXX_OUTPUTS}
    ${ANTLR_Augene2Parser_CXX_OUTPUTS}
)

target_include_directories(mugene2
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${midicci_SOURCE_DIR}/include
    PRIVATE
        ${AUGENE2_ANTLR_OUTPUT_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mugene2
)

target_compile_definitions(mugene2
    PRIVATE
        AUGENE2_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(mugene2
    PUBLIC
        umppi_static
    PRIVATE
        antlr4_static
)

add_library(mugene2::mugene2 ALIAS mugene2)

add_executable(mugene2-cli
    src/mugene2_cli/main.cpp
)

target_link_libraries(mugene2-cli
    PRIVATE
        mugene2
)

add_library(augene2 STATIC
    src/augene2/project_compiler.cpp
    src/augene2/storage.cpp
)

target_include_directories(augene2
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(augene2
    PUBLIC
        mugene2
)

add_library(augene2::augene2 ALIAS augene2)

add_executable(augene2-cli
    src/augene2_cli/main.cpp
)

target_link_libraries(augene2-cli
    PRIVATE
        augene2
)
