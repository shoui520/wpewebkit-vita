include(platform/Cairo.cmake)
include(platform/FreeType.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)

list(APPEND WebCore_PRIVATE_LIBRARIES OpenSSL::Crypto)

list(APPEND WebCore_SOURCES
    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp
    platform/generic/ScrollbarsControllerGeneric.cpp
    platform/graphics/opentype/OpenTypeVerticalData.cpp
    platform/network/vita/DNSResolveQueueVita.cpp
    platform/network/vita/ResourceModelsVita.cpp
    platform/sql/vita/SQLiteVFSVita.cpp
    platform/text/Hyphenation.cpp
    platform/text/LocaleICU.cpp
    platform/unix/LoggingUnix.cpp
    platform/vita/SharedMemoryVita.cpp
)
list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/crypto/openssl"
    "${WEBCORE_DIR}/platform/graphics/opentype"
    "${WEBCORE_DIR}/platform/network/vita"
    "${WEBCORE_DIR}/platform/text/icu"
)
list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/network/vita/AuthenticationChallenge.h
    platform/network/vita/CertificateInfo.h
    platform/network/vita/ResourceError.h
    platform/network/vita/ResourceRequest.h
    platform/network/vita/ResourceResponse.h
)
