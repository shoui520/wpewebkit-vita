find_package(Threads REQUIRED)

if (VITA)
    find_path(KUBRIDGE_INCLUDE_DIR NAMES kubridge.h REQUIRED)
    find_library(KUBRIDGE_LIBRARY NAMES kubridge_stub REQUIRED)
    # vita-elf-create relocates static executables and rejects PIC GOT relocations.
    set(CMAKE_POSITION_INDEPENDENT_CODE FALSE)
    # Match the POSIX feature visibility used by the configure checks.
    add_compile_definitions(_GNU_SOURCE=1)
    # libstdc++ probes pthread_cancel through a weak reference and calls other
    # pthread functions weakly. A normal static archive link leaves these out,
    # causing std::call_once to fail even when pthread_create is linked.
    if (VITA_C_RUNTIME STREQUAL "SceLibcPosix")
        if (NOT VITA_RUNTIME_LINKAGE_CONFIGURED)
            message(FATAL_ERROR "SceLibcPosix requires a toolchain configured with its CRT, runtime imports, and pthread module")
        endif ()
        check_cxx_source_compiles("
            #include <sys/mman.h>
            #include <sys/uio.h>
            int main() { }
        " VITA_RUNTIME_HEADERS_SUPPORTED)
        if (NOT VITA_RUNTIME_HEADERS_SUPPORTED)
            message(FATAL_ERROR "SceLibcPosix requires its POSIX header overlay")
        endif ()
        # The runtime toolchain supplies whole-archive pthread module imports
        # for every executable, including CMake's compiler and feature checks.
        set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_LIBRARIES "")
    else ()
        set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_LIBRARIES
            "-Wl,--whole-archive;-lpthread;-Wl,--no-whole-archive")
    endif ()
endif ()

if (MSVC)
    include(OptionsMSVC)
else ()
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif ()

add_definitions(-DBUILDING_JSCONLY__)

set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_MICRO 0)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})

WEBKIT_OPTION_BEGIN()
set(ENABLE_STATIC_JSC_DEFAULT OFF)
if (VITA)
    set(ENABLE_STATIC_JSC_DEFAULT ON)
endif ()
WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Whether to build JavaScriptCore as a static library." PUBLIC ${ENABLE_STATIC_JSC_DEFAULT})
WEBKIT_OPTION_DEFINE(USE_LIBBACKTRACE "Whether to enable usage of libbacktrace." PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE OFF)
if (NOT WIN32)
    WEBKIT_OPTION_DEFINE(ENABLE_FUZZILLI "Whether to build JavaScriptCore with support for Fuzzilli." PUBLIC OFF)
endif ()
if (WIN32)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE OFF)
endif ()
WEBKIT_OPTION_DEFINE(ENABLE_JSC_GLIB_API "Whether to enable the JavaScriptCore GLib API." PUBLIC OFF)
WEBKIT_OPTION_DEFINE(USE_SYSTEM_UNIFDEF "Whether to use a system-provided unifdef" PRIVATE OFF)

if (VITA)
    if (NOT WTF_CPU_ARM OR NOT ARM_THUMB2_DETECTED)
        message(FATAL_ERROR "Vita JavaScriptCore requires an ARMv7 Thumb-2 toolchain")
    endif ()
    check_cxx_source_compiles("
        #if !defined(__vita__) || !defined(__thumb2__) || !defined(__ARM_PCS_VFP) || __ARM_ARCH < 7
        #error Vita ARMv7 Thumb-2 hard-float ABI is required
        #endif
        static_assert(sizeof(void*) == 4);
        int main() { }
    " VITA_JSC_ABI_SUPPORTED)
    if (NOT VITA_JSC_ABI_SUPPORTED)
        message(FATAL_ERROR "Vita JavaScriptCore requires the 32-bit ARMv7 Thumb-2 hard-float ABI")
    endif ()
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_JIT PRIVATE ON)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DFG_JIT PRIVATE ON)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FTL_JIT PRIVATE OFF)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_C_LOOP PRIVATE OFF)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SAMPLING_PROFILER PRIVATE OFF)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBASSEMBLY PRIVATE OFF)
    # Use the platform runtime allocator instead of bmalloc's VM assumptions.
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE ON)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_ISO_MALLOC PRIVATE OFF)
endif ()

WEBKIT_OPTION_END()

if (VITA AND NOT ENABLE_STATIC_JSC)
    message(FATAL_ERROR "Vita JavaScriptCore requires static libraries")
endif ()

set(ALL_EVENT_LOOP_TYPES
    GLib
    Generic
)

set(DEFAULT_EVENT_LOOP_TYPE "Generic")

set(EVENT_LOOP_TYPE ${DEFAULT_EVENT_LOOP_TYPE} CACHE STRING "Implementation of event loop to be used in JavaScriptCore (one of ${ALL_EVENT_LOOP_TYPES})")

set(ENABLE_WEBCORE OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_WEBGL OFF)
set(ENABLE_WEBGPU OFF)

if (WIN32 OR VITA)
    set(ENABLE_API_TESTS OFF)
else ()
    set(ENABLE_API_TESTS ON)
endif ()

if ((WTF_CPU_ARM OR WTF_CPU_MIPS) AND NOT VITA)
    SET_AND_EXPOSE_TO_BUILD(USE_CAPSTONE TRUE)
endif ()

if (NOT ENABLE_STATIC_JSC)
    set(JavaScriptCore_LIBRARY_TYPE SHARED)
    set(bmalloc_LIBRARY_TYPE OBJECT)
    set(WTF_LIBRARY_TYPE OBJECT)
endif ()

if (WIN32)
    add_definitions(-D_WINDOWS -DNTDDI_VERSION=0x0A000006 -D_WIN32_WINNT=0x0A00)

    add_definitions(-DNOMINMAX)
    add_definitions(-DUNICODE -D_UNICODE)
    add_definitions(-DNOCRYPT)

    # For fileno, wcsicmp, getpid and strdup.
    # https://learn.microsoft.com/en-us/previous-versions/ms235384(v=vs.100)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)

    # FIXME: warning STL4042: std::float_denorm_style, std::numeric_limits::has_denorm, and std::numeric_limits::has_denorm_loss are deprecated in C++23.
    add_definitions(-D_SILENCE_CXX23_DENORM_DEPRECATION_WARNING)

    if (DEFINED ENV{WEBKIT_IGNORE_PATH})
        set(CMAKE_IGNORE_PATH $ENV{WEBKIT_IGNORE_PATH})
    endif ()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

    set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)
endif ()

if (ENABLE_JSC_GLIB_API)
    include(GNUInstallDirs)

    SET_AND_EXPOSE_TO_BUILD(ENABLE_2022_GLIB_API TRUE)

    set(EVENT_LOOP_TYPE "GLib")
    set(JavaScriptCore_PKGCONFIG_FILE ${CMAKE_BINARY_DIR}/Source/JavaScriptCore/javascriptcoreglib-${PROJECT_VERSION}.pc)
    set(JavaScriptCore_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/javascriptcoreglib-${PROJECT_VERSION}")

    add_definitions(-DJSC_GLIB_API_ENABLED)
    add_definitions(-DGETTEXT_PACKAGE="JSCGlib")
endif ()

string(TOLOWER ${EVENT_LOOP_TYPE} LOWERCASE_EVENT_LOOP_TYPE)
if (LOWERCASE_EVENT_LOOP_TYPE STREQUAL "glib")
    find_package(GLib 2.70.0 REQUIRED COMPONENTS GioUnix Object)
    SET_AND_EXPOSE_TO_BUILD(USE_GLIB 1)
    SET_AND_EXPOSE_TO_BUILD(USE_GLIB_EVENT_LOOP 1)
    SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
else ()
    SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
    SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
endif ()

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)
if (APPLE)
    add_definitions(-DU_DISABLE_RENAMING=1)
endif ()

if (USE_LIBBACKTRACE)
    find_package(LibBacktrace)
    if (NOT LIBBACKTRACE_FOUND)
        message(FATAL_ERROR "libbacktrace is required for USE_LIBBACKTRACE")
    endif ()
endif ()
