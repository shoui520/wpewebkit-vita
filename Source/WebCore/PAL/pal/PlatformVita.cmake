list(APPEND PAL_SOURCES
    crypto/mbedtls/CryptoDigestMbedTLS.cpp
    system/ClockGeneric.cpp
    system/Sound.cpp
    text/KillRing.cpp
)
list(APPEND PAL_LIBRARIES MbedTLS::Crypto)
