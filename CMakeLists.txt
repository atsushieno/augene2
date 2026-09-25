cmake_minimum_required(VERSION 3.28)
project(augene2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)

option(AUGENE2_BUILD_TESTS "Build augene2 tests" OFF)

set(ANTLR4_VERSION 4.13.2)
# The C++ runtime is pinned past 4.13.2, to the end of the header-hygiene fixes on
# antlr4's dev branch (antlr/antlr4#4738, #4791-#4794). 4.13.2 relies on transitive
# standard includes and does not build on MSVC 14.51 (high_resolution_clock without
# <chrono>) nor as C++23 against NDK r28 libc++ (unique_ptr of an incomplete
# antlr4::Token). The tool jar stays 4.13.2. Return to a release tag once one
# carries these fixes.
set(ANTLR4_RUNTIME_GIT_TAG 5435f14134eb699d7484d46d946df817dfe97297)
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
    GIT_TAG 0.4.0
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
    GIT_TAG ${ANTLR4_RUNTIME_GIT_TAG}
    GIT_REPOSITORY https://github.com/antlr/antlr4
    SOURCE_SUBDIR runtime/Cpp
    # WITH_STATIC_CRT defaults to ON, which builds only the runtime against the
    # static MSVC CRT; everything linking it uses the default dynamic CRT, and MSVC
    # refuses to mix them (LNK2038 "RuntimeLibrary mismatch").
    OPTIONS "ANTLR_BUILD_CPP_TESTS OFF" "ANTLR_BUILD_SHARED OFF" "ANTLR_BUILD_STATIC ON" "WITH_STATIC_CRT OFF"
    EXCLUDE_FROM_ALL
)
if(DEFINED _AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS AND NOT "${_AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_AUGENE2_OLD_ANTLR_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

CPMAddPackage(
    NAME            choc
    GIT_TAG         1857a38
    GIT_REPOSITORY  https://github.com/Tracktion/choc
    DOWNLOAD_ONLY YES
)

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
    PRIVATE
        ${choc_SOURCE_DIR}
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
