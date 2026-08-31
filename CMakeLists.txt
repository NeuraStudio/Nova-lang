cmake_minimum_required(VERSION 3.16)
project(Nova LANGUAGES CXX)

# ==============================================================================
# Nova build system
#
# Layout assumed (matches the frozen Nova folder structure). Every module
# path below is a CACHE variable, so if your tree differs you override it
# on the command line instead of editing this file:
#   cmake -S . -B build -DNOVA_LEXER_DIR=/some/other/path
#
# A module whose directory doesn't exist yet (or has no .cpp files in it)
# is SKIPPED, not a hard error — this matches Nova's phase-by-phase build
# philosophy: you build what exists today, and dirs added later just start
# picking up automatically on the next `cmake` re-run.
# ==============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE tooling
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(NOVA_ENABLE_LLVM_BACKEND "Build the LLVM-based backend"        ON)
option(NOVA_ENABLE_GCC_BACKEND  "Build the GCC-based backend"         ON)
option(NOVA_USE_LIBGCCJIT       "Link libgccjit instead of shelling out to gcc/cc at runtime" OFF)
option(NOVA_BUILD_RUNTIME       "Build libnova_runtime (linked into compiled Nova programs)" ON)
option(NOVA_BUILD_TESTS         "Build the tests/ tree if present"    OFF)
option(NOVA_WARNINGS_AS_ERRORS  "Treat compiler warnings as errors"   OFF)

# ------------------------------------------------------------------------------
# Source layout (override any of these to match your actual tree)
# ------------------------------------------------------------------------------
set(NOVA_LEXER_DIR        "${CMAKE_SOURCE_DIR}/compiler/lexer"        CACHE PATH "Lexer sources")
set(NOVA_PARSER_DIR       "${CMAKE_SOURCE_DIR}/compiler/parser"       CACHE PATH "Parser sources")
set(NOVA_AST_DIR          "${CMAKE_SOURCE_DIR}/compiler/ast"          CACHE PATH "AST sources")
set(NOVA_SEMANTIC_DIR     "${CMAKE_SOURCE_DIR}/compiler/semantic"     CACHE PATH "Semantic analyzer sources")
set(NOVA_IR_DIR           "${CMAKE_SOURCE_DIR}/compiler/ir"           CACHE PATH "IR generator sources")
set(NOVA_OPTIMIZER_DIR    "${CMAKE_SOURCE_DIR}/compiler/optimizer"    CACHE PATH "Optimizer sources")
set(NOVA_DISPATCHER_DIR   "${CMAKE_SOURCE_DIR}/compiler/dispatcher"   CACHE PATH "Command-dispatcher registry sources")
set(NOVA_BACKEND_LLVM_DIR "${CMAKE_SOURCE_DIR}/compiler/backend/llvm" CACHE PATH "LLVM backend sources")
set(NOVA_BACKEND_GCC_DIR  "${CMAKE_SOURCE_DIR}/compiler/backend/gcc"  CACHE PATH "GCC backend sources")
set(NOVA_RUNTIME_DIR      "${CMAKE_SOURCE_DIR}/runtime"               CACHE PATH "Runtime sources (nova_rt.cpp, nova_rt.hpp, nova_bridge.cpp)")

# ------------------------------------------------------------------------------
# Helper: build a STATIC lib from every .cpp/.cc under a dir; skip cleanly
# if the dir doesn't exist yet or is empty.
# ------------------------------------------------------------------------------
function(nova_add_module out_found_var target_name dir)
    if(EXISTS "${dir}")
        file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${dir}/*.cpp" "${dir}/*.cc")
    else()
        set(_srcs "")
    endif()

    if(_srcs)
        add_library(${target_name} STATIC ${_srcs})
        target_include_directories(${target_name} PUBLIC
            "${dir}"
            "${CMAKE_SOURCE_DIR}/compiler"
        )
        target_compile_features(${target_name} PUBLIC cxx_std_17)
        if(NOVA_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Werror)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra)
        endif()
        message(STATUS "Nova module '${target_name}': ${dir} (${_srcs_count} files)")
        list(LENGTH _srcs _n)
        message(STATUS "  -> ${_n} source file(s)")
        set(${out_found_var} TRUE PARENT_SCOPE)
    else()
        message(STATUS "Nova module '${target_name}': SKIPPED (no sources under ${dir})")
        set(${out_found_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

nova_add_module(NOVA_HAVE_LEXER      nova_lexer      "${NOVA_LEXER_DIR}")
nova_add_module(NOVA_HAVE_PARSER     nova_parser     "${NOVA_PARSER_DIR}")
nova_add_module(NOVA_HAVE_AST        nova_ast        "${NOVA_AST_DIR}")
nova_add_module(NOVA_HAVE_SEMANTIC   nova_semantic   "${NOVA_SEMANTIC_DIR}")
nova_add_module(NOVA_HAVE_IR         nova_ir         "${NOVA_IR_DIR}")
nova_add_module(NOVA_HAVE_OPTIMIZER  nova_optimizer  "${NOVA_OPTIMIZER_DIR}")
nova_add_module(NOVA_HAVE_DISPATCHER nova_dispatcher "${NOVA_DISPATCHER_DIR}")

set(NOVA_CORE_LIBS "")
if(NOVA_HAVE_LEXER)      list(APPEND NOVA_CORE_LIBS nova_lexer)      endif()
if(NOVA_HAVE_PARSER)     list(APPEND NOVA_CORE_LIBS nova_parser)     endif()
if(NOVA_HAVE_AST)        list(APPEND NOVA_CORE_LIBS nova_ast)        endif()
if(NOVA_HAVE_SEMANTIC)   list(APPEND NOVA_CORE_LIBS nova_semantic)   endif()
if(NOVA_HAVE_IR)         list(APPEND NOVA_CORE_LIBS nova_ir)         endif()
if(NOVA_HAVE_OPTIMIZER)  list(APPEND NOVA_CORE_LIBS nova_optimizer)  endif()
if(NOVA_HAVE_DISPATCHER) list(APPEND NOVA_CORE_LIBS nova_dispatcher) endif()

if(NOT NOVA_HAVE_LEXER OR NOT NOVA_HAVE_PARSER OR NOT NOVA_HAVE_AST)
    message(WARNING
        "Lexer, Parser, and AST are the minimum needed for a usable novac. "
        "At least one of them has no sources yet — novac will still be "
        "configured, but will fail to link until those modules exist.")
endif()

# ==============================================================================
# LLVM backend
# find_package(LLVM CONFIG) instead of hardcoded paths — CMAKE_PREFIX_PATH
# hints below cover the common install locations per platform; override
# with -DLLVM_DIR=/path/to/lib/cmake/llvm if yours isn't found.
# ==============================================================================
if(NOVA_ENABLE_LLVM_BACKEND)
    list(APPEND CMAKE_PREFIX_PATH
        $ENV{PREFIX}                       # Termux: $PREFIX = .../com.termux/files/usr
        /usr/lib/llvm-18 /usr/lib/llvm-17 /usr/lib/llvm-16 /usr/lib/llvm-15
        /usr/local/opt/llvm                # macOS Homebrew (Intel)
        /opt/homebrew/opt/llvm             # macOS Homebrew (Apple Silicon)
    )
    find_package(LLVM CONFIG QUIET)

    if(NOT LLVM_FOUND)
        message(FATAL_ERROR
            "LLVM development package not found via find_package(LLVM CONFIG).\n"
            "  Termux/Android : pkg install llvm\n"
            "  Debian/Ubuntu  : sudo apt install llvm-dev clang\n"
            "  Fedora         : sudo dnf install llvm-devel\n"
            "  macOS (brew)   : brew install llvm\n"
            "Or point CMake at it directly: -DLLVM_DIR=/path/to/lib/cmake/llvm\n"
            "To build without the LLVM backend: -DNOVA_ENABLE_LLVM_BACKEND=OFF")
    endif()

    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
    include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})

    llvm_map_components_to_libnames(NOVA_LLVM_LIBS
        core support irreader bitwriter
        mcjit orcjit native
        AArch64 X86
    )

    nova_add_module(NOVA_HAVE_BACKEND_LLVM nova_backend_llvm "${NOVA_BACKEND_LLVM_DIR}")
    if(NOVA_HAVE_BACKEND_LLVM)
        target_link_libraries(nova_backend_llvm PUBLIC ${NOVA_LLVM_LIBS})
        list(APPEND NOVA_CORE_LIBS nova_backend_llvm)
    else()
        message(STATUS "NOVA_ENABLE_LLVM_BACKEND=ON but no sources under ${NOVA_BACKEND_LLVM_DIR} yet — LLVM is linked, backend target skipped.")
    endif()
endif()

# ==============================================================================
# GCC backend
# Two modes:
#  - NOVA_USE_LIBGCCJIT=ON : link libgccjit directly (needs libgccjit-dev,
#    often unavailable on Android/Termux)
#  - default               : backend shells out to an installed gcc/cc/clang
#    at runtime — no extra link-time dependency, works everywhere
# ==============================================================================
if(NOVA_ENABLE_GCC_BACKEND)
    nova_add_module(NOVA_HAVE_BACKEND_GCC nova_backend_gcc "${NOVA_BACKEND_GCC_DIR}")
    if(NOVA_HAVE_BACKEND_GCC)
        if(NOVA_USE_LIBGCCJIT)
            find_path(GCCJIT_INCLUDE_DIR libgccjit.h)
            find_library(GCCJIT_LIBRARY gccjit)
            if(GCCJIT_INCLUDE_DIR AND GCCJIT_LIBRARY)
                target_include_directories(nova_backend_gcc PUBLIC ${GCCJIT_INCLUDE_DIR})
                target_link_libraries(nova_backend_gcc PUBLIC ${GCCJIT_LIBRARY})
                target_compile_definitions(nova_backend_gcc PUBLIC NOVA_GCCJIT_AVAILABLE=1)
                message(STATUS "GCC backend: using libgccjit (${GCCJIT_LIBRARY})")
            else()
                message(WARNING "NOVA_USE_LIBGCCJIT=ON but libgccjit was not found — falling back to shelling out to gcc/cc.")
                target_compile_definitions(nova_backend_gcc PUBLIC NOVA_GCCJIT_AVAILABLE=0)
            endif()
        else()
            target_compile_definitions(nova_backend_gcc PUBLIC NOVA_GCCJIT_AVAILABLE=0)
        endif()

        find_program(NOVA_GCC_EXECUTABLE NAMES gcc cc clang)
        if(NOVA_GCC_EXECUTABLE)
            message(STATUS "GCC backend: shell-out compiler = ${NOVA_GCC_EXECUTABLE}")
            target_compile_definitions(nova_backend_gcc PUBLIC NOVA_GCC_EXECUTABLE="${NOVA_GCC_EXECUTABLE}")
        else()
            message(WARNING "No gcc/cc/clang found on PATH — the shell-out GCC backend path will fail at runtime until one is installed.")
        endif()

        list(APPEND NOVA_CORE_LIBS nova_backend_gcc)
    endif()
endif()

# ==============================================================================
# novac — the compiler driver executable
# ==============================================================================
set(NOVA_MAIN_CANDIDATE "${CMAKE_SOURCE_DIR}/compiler/main.cpp")
if(EXISTS "${NOVA_MAIN_CANDIDATE}")
    add_executable(novac "${NOVA_MAIN_CANDIDATE}")
    target_link_libraries(novac PRIVATE ${NOVA_CORE_LIBS})
    target_include_directories(novac PRIVATE "${CMAKE_SOURCE_DIR}/compiler")
    install(TARGETS novac RUNTIME DESTINATION bin)
else()
    message(STATUS "No compiler/main.cpp yet — novac executable target skipped (module libraries above still build).")
endif()

# ==============================================================================
# libnova_runtime — statically linked into every COMPILED Nova program
# (this is nova_rt.cpp / nova_rt.hpp / nova_bridge.cpp)
# ==============================================================================
if(NOVA_BUILD_RUNTIME)
    find_package(Threads REQUIRED)
    file(GLOB NOVA_RUNTIME_SOURCES CONFIGURE_DEPENDS "${NOVA_RUNTIME_DIR}/*.cpp")
    if(NOVA_RUNTIME_SOURCES)
        add_library(nova_runtime STATIC ${NOVA_RUNTIME_SOURCES})
        target_include_directories(nova_runtime PUBLIC "${NOVA_RUNTIME_DIR}")
        target_compile_features(nova_runtime PUBLIC cxx_std_17)
        target_link_libraries(nova_runtime PUBLIC Threads::Threads)
        install(TARGETS nova_runtime ARCHIVE DESTINATION lib)
        install(FILES "${NOVA_RUNTIME_DIR}/nova_rt.hpp" DESTINATION include)
        message(STATUS "libnova_runtime will be built from ${NOVA_RUNTIME_DIR}")
    else()
        message(STATUS "NOVA_BUILD_RUNTIME=ON but no .cpp found under ${NOVA_RUNTIME_DIR} — put nova_rt.cpp and nova_bridge.cpp there.")
    endif()
endif()

# ==============================================================================
# nova_ecosystem — optional domain module (MAP 11 Web / MAP 12 SQLite3 / MAP 13 GUI)
# Previously this module had no CMake target at all: it was compiled and
# linked by a pre-build Python patch script instead. That's the single
# biggest reason such a script existed. This target replaces it — real
# dependency discovery, real per-platform link libraries, and a clean skip
# (not a hard failure) when a dependency isn't installed, matching this
# file's existing "build what exists today" philosophy.
# ==============================================================================
option(NOVA_BUILD_ECOSYSTEM "Build nova_ecosystem (Web/SQLite3/GUI domain module)" ON)
set(NOVA_ECOSYSTEM_DIR "${NOVA_RUNTIME_DIR}/ecosystem" CACHE PATH "nova_ecosystem.cpp location")
set(NOVA_ECOSYSTEM_BUILT FALSE)

if(NOVA_BUILD_ECOSYSTEM)
    set(_nova_eco_src "${NOVA_ECOSYSTEM_DIR}/nova_ecosystem.cpp")
    if(EXISTS "${_nova_eco_src}")
        find_package(SQLite3 QUIET)
        set(_nova_eco_ok TRUE)
        set(_nova_eco_link "")
        set(_nova_eco_includes "")

        if(SQLite3_FOUND)
            # Using the SQLite3_INCLUDE_DIRS/SQLite3_LIBRARIES variables
            # rather than an imported target name (target name/namespace
            # for this module has changed across CMake versions) — these
            # variables are the stable, documented part of FindSQLite3
            # back to CMake 3.14.
            list(APPEND _nova_eco_link ${SQLite3_LIBRARIES})
            list(APPEND _nova_eco_includes ${SQLite3_INCLUDE_DIRS})
        else()
            message(WARNING "nova_ecosystem: SQLite3 dev package not found — SKIPPING nova_ecosystem.\n"
                             "  Debian/Ubuntu : sudo apt install libsqlite3-dev\n"
                             "  Termux        : pkg install sqlite\n"
                             "  macOS (brew)  : brew install sqlite3")
            set(_nova_eco_ok FALSE)
        endif()

        if(WIN32)
            list(APPEND _nova_eco_link ws2_32 user32 gdi32)
        else()
            find_package(X11 QUIET)
            if(X11_FOUND)
                list(APPEND _nova_eco_link ${X11_LIBRARIES})
                list(APPEND _nova_eco_includes ${X11_INCLUDE_DIR})
            else()
                message(WARNING "nova_ecosystem: X11 dev package not found — SKIPPING nova_ecosystem.\n"
                                 "  Debian/Ubuntu : sudo apt install libx11-dev\n"
                                 "  Termux        : pkg install libx11 (needs a Termux:X11 or VNC display to actually run windows)")
                set(_nova_eco_ok FALSE)
            endif()
        endif()

        if(_nova_eco_ok)
            find_package(Threads REQUIRED)
            add_library(nova_ecosystem STATIC "${_nova_eco_src}")
            target_include_directories(nova_ecosystem PUBLIC "${NOVA_RUNTIME_DIR}" ${_nova_eco_includes})
            target_compile_features(nova_ecosystem PUBLIC cxx_std_17)
            target_link_libraries(nova_ecosystem PUBLIC ${_nova_eco_link} Threads::Threads)
            if(TARGET nova_runtime)
                target_link_libraries(nova_ecosystem PUBLIC nova_runtime)
            endif()
            install(TARGETS nova_ecosystem ARCHIVE DESTINATION lib)
            set(NOVA_ECOSYSTEM_BUILT TRUE)
            message(STATUS "nova_ecosystem: enabled (SQLite3 + ${CMAKE_SYSTEM_NAME} GUI backend)")
        endif()
    else()
        message(STATUS "NOVA_BUILD_ECOSYSTEM=ON but no nova_ecosystem.cpp found under ${NOVA_ECOSYSTEM_DIR} — skipped.")
    endif()
endif()

# ==============================================================================
# Tests (optional, off by default)
# ==============================================================================
if(NOVA_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

# ==============================================================================
# Summary
# ==============================================================================
message(STATUS "")
message(STATUS "=== Nova build summary ===")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  System            : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  LLVM backend      : ${NOVA_ENABLE_LLVM_BACKEND}")
message(STATUS "  GCC backend       : ${NOVA_ENABLE_GCC_BACKEND} (libgccjit=${NOVA_USE_LIBGCCJIT})")
message(STATUS "  Runtime library   : ${NOVA_BUILD_RUNTIME}")
message(STATUS "  Ecosystem module  : requested=${NOVA_BUILD_ECOSYSTEM} built=${NOVA_ECOSYSTEM_BUILT}")
message(STATUS "===========================")
message(STATUS "")
