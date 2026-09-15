cmake_minimum_required(VERSION 3.16)

project(faceTSS
    VERSION 0.2.0
    DESCRIPTION "FACE Transport Services Segment over nng + FlatBuffers (C)"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # for typeof/thread bits where handy; code is C11-clean
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FACETSS_BUILD_SHARED "Build shared libraries (.so)" ON)
option(FACETSS_BUILD_TESTS "Build C tests" ON)
option(FACETSS_BUILD_EXAMPLES "Build C examples" ON)
option(FACETSS_USE_SYSTEM_NNG "Use system-installed nng instead of vendored" OFF)
option(FACETSS_USE_SYSTEM_FLATCC "Use system-installed flatcc runtime instead of vendored" OFF)
option(FACETSS_ENABLE_WERROR "Treat warnings as errors" OFF)

if(FACETSS_ENABLE_WERROR)
    if(MSVC)
        add_compile_options(/W4 /WX)
    else()
        add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
    endif()
else()
    if(NOT MSVC)
        add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    endif()
endif()

include(FetchContent)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Third party: nng -> libnng.so
# ---------------------------------------------------------------------------
if(FACETSS_USE_SYSTEM_NNG)
    find_package(nng CONFIG REQUIRED)
else()
    set(NNG_TESTS OFF CACHE BOOL "" FORCE)
    set(NNG_ENABLE_SQLITE OFF CACHE BOOL "" FORCE)
    set(NNG_ENABLE_TLS OFF CACHE BOOL "" FORCE)
    # Keep the vendored build quiet-ish and shared.
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        nng
        URL https://github.com/nanomsg/nng/archive/refs/tags/v1.12.3.zip
        URL_HASH SHA256=c1705560644dabd26cf7fa9d6ef720d96d48c1d9df548b242b8ede3a49ad04e0
    )
    FetchContent_MakeAvailable(nng)
endif()

# ---------------------------------------------------------------------------
# Third party: flatcc runtime -> libflatccrt.so
#
# flatcc builds several libs; the runtime (builder/emitter/refmap/verifier)
# is `flatccrt`. We expose it under the required soname `libflatccrt.so`.
# ---------------------------------------------------------------------------
if(FACETSS_USE_SYSTEM_FLATCC)
    find_library(FLATCCRT_LIB flatccrt REQUIRED)
    add_library(flatccrt_iface INTERFACE)
    target_link_libraries(flatccrt_iface INTERFACE ${FLATCCRT_LIB})
    # Headers are expected alongside (system package provides them).
    add_library(flatccrt_alias ALIAS flatccrt_iface)
    set(FLATCCRT_TARGET flatccrt_iface)
    set(FLATCC_INCLUDE_DIRS "")
else()
    set(FLATCC_TEST OFF CACHE BOOL "" FORCE)
    set(FLATCC_CXX_TEST OFF CACHE BOOL "" FORCE)
    set(FLATCC_INSTALL OFF CACHE BOOL "" FORCE)
    set(FLATCC_COVERAGE OFF CACHE BOOL "" FORCE)
    set(FLATCC_DEBUG_CLANG_SANITIZE OFF CACHE BOOL "" FORCE)
    # Runtime-only still defines the flatccrt target we need.
    set(FLATCC_RTONLY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        flatcc
        URL https://github.com/dvidelabs/flatcc/archive/refs/tags/v0.6.3.zip
        URL_HASH SHA256=ca2a1b08a598bd4c510f8de7fd7c459f758df0e28480ae1292c894222e664268
    )
    FetchContent_MakeAvailable(flatcc)
    set(FLATCCRT_TARGET flatccrt)
    set(FLATCC_INCLUDE_DIRS ${flatcc_SOURCE_DIR}/include)
endif()

# ---------------------------------------------------------------------------
# libTSS.so
# ---------------------------------------------------------------------------
add_library(TSS SHARED
    c/src/config.c
    c/src/config_service.c
    c/src/csp.c
    c/src/envelope.c
    c/src/marshalling.c
    c/src/qos.c
    c/src/tpm.c
    c/src/transport.c
    c/src/tss.c
    c/src/typed.c
)
add_library(FaceTSS::TSS ALIAS TSS)

target_include_directories(TSS
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(NOT FACETSS_USE_SYSTEM_FLATCC)
    target_include_directories(TSS PRIVATE ${FLATCC_INCLUDE_DIRS})
endif()

target_link_libraries(TSS
    PUBLIC nng
    PRIVATE ${FLATCCRT_TARGET}
)

set_target_properties(TSS PROPERTIES
    OUTPUT_NAME "TSS"
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    C_VISIBILITY_PRESET default
)

find_package(Threads REQUIRED)
target_link_libraries(TSS PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(TSS PRIVATE bcrypt)
endif()

# ---------------------------------------------------------------------------
# Tests + examples
# ---------------------------------------------------------------------------
if(FACETSS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(c/tests)
endif()

if(FACETSS_BUILD_EXAMPLES)
    add_subdirectory(c/examples)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS TSS
    EXPORT FaceTSSTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY c/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES c/schemas/tss_envelope.fbs c/schemas/position_report.fbs
    DESTINATION ${CMAKE_INSTALL_DATADIR}/faceTSS/schemas)

# Also install the two dependency .so files when we built them, so the
# install tree (and the build tree - see below) holds all three .so files.
if(NOT FACETSS_USE_SYSTEM_NNG)
    install(TARGETS nng
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(NOT FACETSS_USE_SYSTEM_FLATCC)
    install(TARGETS flatccrt
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# Convenience: stage the dependency .so files next to libTSS.so in the
# build tree so `ls *.so*` shows all three deliverables in one place.
# nng carries a versioned soname (libnng.so.1 -> libnng.so.1.12.3); stage the
# full chain plus a plain libnng.so symlink for linkers that ask for -lnng.
if(NOT FACETSS_USE_SYSTEM_NNG)
    add_custom_command(TARGET TSS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nng> $<TARGET_FILE_DIR:TSS>/libnng.so.1.12.3
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            libnng.so.1.12.3 $<TARGET_FILE_DIR:TSS>/libnng.so.1
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            libnng.so.1 $<TARGET_FILE_DIR:TSS>/libnng.so
        COMMENT "Staging libnng.so.1.12.3 (+soname/dev symlinks) next to libTSS.so")
endif()
if(NOT FACETSS_USE_SYSTEM_FLATCC)
    add_custom_command(TARGET TSS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:flatccrt> $<TARGET_FILE_DIR:TSS>
        COMMENT "Staging libflatccrt.so next to libTSS.so")
endif()

# C++ binding: FACE::TSS C++ interfaces over the C library
# ---------------------------------------------------------------------------
add_library(TSS_CPP STATIC
    cpp/src/typed_ts.cpp
)
add_library(FaceTSS::TSS_CPP ALIAS TSS_CPP)

target_include_directories(TSS_CPP
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/c/include
)
target_link_libraries(TSS_CPP PUBLIC FaceTSS::TSS)
set_target_properties(TSS_CPP PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)
