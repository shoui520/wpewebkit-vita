if (NOT VITA OR NOT WTF_CPU_ARM OR NOT ARM_THUMB2_DETECTED)
    message(FATAL_ERROR "The Vita port requires a Vita ARMv7 Thumb-2 toolchain")
endif ()
if (NOT VITA_C_RUNTIME STREQUAL "SceLibcPosix" OR NOT VITA_RUNTIME_LINKAGE_CONFIGURED)
    message(FATAL_ERROR "The Vita port requires the native SceLibcPosix CRT, imports and pthread module")
endif ()

check_cxx_source_compiles("
    #include <setjmp.h>
    #if !defined(__vita__) || !defined(__thumb2__) || !defined(__ARM_PCS_VFP) || __ARM_ARCH < 7
    #error Vita ARMv7 Thumb-2 hard-float ABI is required
    #endif
    #if VITA_LIBC_JUMP_ABI != 104
    #error The versioned native hard-float jump ABI is required
    #endif
    enum NativeEnum { NativeEnumValue = 1 };
    static_assert(sizeof(void*) == 4 && sizeof(NativeEnum) == 4);
    static_assert(sizeof(jmp_buf) == 104 && alignof(jmp_buf) == 8);
    int main() { jmp_buf state; return setjmp(state); }
" VITA_WEBCORE_ABI_SUPPORTED)
if (NOT VITA_WEBCORE_ABI_SUPPORTED)
    message(FATAL_ERROR "Vita WebCore requires 32-bit enums and the matching native HF104 runtime headers")
endif ()

find_package(Threads REQUIRED)
set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_LIBRARIES "")
find_path(KUBRIDGE_INCLUDE_DIR NAMES kubridge.h REQUIRED)
find_library(KUBRIDGE_LIBRARY NAMES kubridge_stub REQUIRED)
find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)
find_package(Cairo 1.16 REQUIRED)
find_package(Freetype REQUIRED)
find_package(Fontconfig REQUIRED)
find_package(HarfBuzz 2.7.4 REQUIRED COMPONENTS ICU)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP REQUIRED COMPONENTS demux)
find_package(LibXml2 2.9.13 REQUIRED)
find_package(SQLite3 REQUIRED)
if (TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif ()
find_package(ZLIB REQUIRED)
find_package(EXPAT REQUIRED)
find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)
find_library(PIXMAN_LIBRARY NAMES pixman-1 REQUIRED)
find_library(SHARPYUV_LIBRARY NAMES sharpyuv REQUIRED)
find_library(BROTLIDEC_LIBRARY NAMES brotlidec)
find_library(BROTLICOMMON_LIBRARY NAMES brotlicommon)
# System ports normally use shared libraries with recorded dependencies. Vita
# links static archives, so retain those dependencies in the imported targets.
target_link_libraries(Freetype::Freetype INTERFACE PNG::PNG ZLIB::ZLIB)
if (BROTLIDEC_LIBRARY AND BROTLICOMMON_LIBRARY)
    target_link_libraries(Freetype::Freetype INTERFACE "${BROTLIDEC_LIBRARY}" "${BROTLICOMMON_LIBRARY}")
endif ()
target_link_libraries(Fontconfig::Fontconfig INTERFACE Freetype::Freetype EXPAT::EXPAT)
target_link_libraries(Cairo::Cairo INTERFACE Fontconfig::Fontconfig Freetype::Freetype PNG::PNG ZLIB::ZLIB "${PIXMAN_LIBRARY}")
target_link_libraries(HarfBuzz::HarfBuzz INTERFACE Freetype::Freetype ICU::uc)
target_link_libraries(HarfBuzz::ICU INTERFACE HarfBuzz::HarfBuzz ICU::uc)
target_link_libraries(LibXml2::LibXml2 INTERFACE ICU::uc Threads::Threads)
target_link_libraries(WebP::libwebp INTERFACE "${SHARPYUV_LIBRARY}" Threads::Threads)
target_link_libraries(SQLite::SQLite3 INTERFACE Threads::Threads)

find_path(MBEDTLS_INCLUDE_DIR NAMES mbedtls/sha512.h REQUIRED)
find_library(MBEDTLS_CRYPTO_LIBRARY NAMES mbedcrypto REQUIRED)
add_library(MbedTLS::Crypto UNKNOWN IMPORTED)
set_target_properties(MbedTLS::Crypto PROPERTIES
    IMPORTED_LOCATION "${MBEDTLS_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
)

set(PROJECT_VERSION_MAJOR 2)
set(PROJECT_VERSION_MINOR 52)
set(PROJECT_VERSION_MICRO 6)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})
set(CMAKE_POSITION_INDEPENDENT_CODE FALSE)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
add_compile_definitions(BUILDING_VITA__ _GNU_SOURCE=1 U_STATIC_IMPLEMENTATION)

WEBKIT_OPTION_BEGIN()
WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Build JavaScriptCore as a static library" PUBLIC ON)
WEBKIT_OPTION_DEFINE(ENABLE_JSC_GLIB_API "Build the JavaScriptCore GLib API" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_LIBBACKTRACE "Use libbacktrace" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_SYSTEM_UNIFDEF "Use a system-provided unifdef" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(ENABLE_FUZZILLI "Build Fuzzilli support" PRIVATE OFF)
foreach (_feature ENABLE_JIT ENABLE_DFG_JIT USE_SYSTEM_MALLOC)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(${_feature} PRIVATE ON)
endforeach ()
foreach (_feature
    ENABLE_API_TESTS ENABLE_CONTEXT_MENUS ENABLE_FTL_JIT ENABLE_C_LOOP
    ENABLE_FTPDIR ENABLE_FULLSCREEN_API ENABLE_GEOLOCATION ENABLE_GPU_PROCESS
    ENABLE_IMAGE_DIFF ENABLE_JAVASCRIPT_SHELL ENABLE_LAYOUT_TESTS
    ENABLE_MEDIA_SOURCE ENABLE_MEDIA_STREAM ENABLE_MEDIA_RECORDER
    ENABLE_REMOTE_INSPECTOR ENABLE_RESOURCE_USAGE ENABLE_SAMPLING_PROFILER
    ENABLE_WEBGL ENABLE_WEBGPU ENABLE_WEB_AUDIO ENABLE_WEB_RTC
    ENABLE_XSLT USE_AVIF USE_ISO_MALLOC USE_JPEGXL USE_LCMS
    USE_SKIA USE_WOFF2)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(${_feature} PRIVATE OFF)
endforeach ()
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DARK_MODE_CSS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GAMEPAD PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_OFFSCREEN_CANVAS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_TOUCH_EVENTS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VARIATION_FONTS PRIVATE ON)
WEBKIT_OPTION_END()

if (NOT ENABLE_STATIC_JSC OR NOT USE_SYSTEM_MALLOC OR USE_SKIA)
    message(FATAL_ERROR "The Vita port requires static JavaScriptCore, native allocation and Cairo")
endif ()

# FreeType decodes WOFF2 itself when it is built with brotli.
include(WOFF2Checks)
if (FREETYPE_WOFF2_SUPPORT_IS_AVAILABLE)
    SET_AND_EXPOSE_TO_BUILD(HAVE_WOFF_SUPPORT ON)
endif ()

set(ENABLE_WEBCORE ON)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_TOOLS OFF)
set(WTF_LIBRARY_TYPE STATIC)
set(JavaScriptCore_LIBRARY_TYPE STATIC)
set(PAL_LIBRARY_TYPE STATIC)
set(WebCore_LIBRARY_TYPE STATIC)
set(LOWERCASE_EVENT_LOOP_TYPE generic)
SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
SET_AND_EXPOSE_TO_BUILD(USE_CAIRO 1)
SET_AND_EXPOSE_TO_BUILD(USE_FREETYPE 1)
SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ 1)
SET_AND_EXPOSE_TO_BUILD(USE_FONT_VARIANT_VIA_FEATURES 1)
SET_AND_EXPOSE_TO_BUILD(USE_GLIB 0)
SET_AND_EXPOSE_TO_BUILD(USE_SOUP 0)
SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL 1)
