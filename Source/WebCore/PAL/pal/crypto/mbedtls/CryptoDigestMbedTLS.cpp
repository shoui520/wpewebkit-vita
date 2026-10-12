/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CryptoDigest.h"

#include <array>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

namespace {
struct SHA1Functions {
    static constexpr auto init = mbedtls_sha1_init;
    static constexpr auto free = mbedtls_sha1_free;
    static constexpr auto starts = mbedtls_sha1_starts;
    static constexpr auto update = mbedtls_sha1_update;
    static constexpr auto final = mbedtls_sha1_finish;
    static constexpr size_t digestLength = 20;
};
template<bool is224> struct SHA256Functions {
    static constexpr auto init = mbedtls_sha256_init;
    static constexpr auto free = mbedtls_sha256_free;
    static int starts(mbedtls_sha256_context* context) { return mbedtls_sha256_starts(context, is224); }
    static constexpr auto update = mbedtls_sha256_update;
    static constexpr auto final = mbedtls_sha256_finish;
    static constexpr size_t digestLength = is224 ? 28 : 32;
};
template<bool is384> struct SHA512Functions {
    static constexpr auto init = mbedtls_sha512_init;
    static constexpr auto free = mbedtls_sha512_free;
    static int starts(mbedtls_sha512_context* context) { return mbedtls_sha512_starts(context, is384); }
    static constexpr auto update = mbedtls_sha512_update;
    static constexpr auto final = mbedtls_sha512_finish;
    static constexpr size_t digestLength = is384 ? 48 : 64;
};
}

namespace PAL {

struct CryptoDigestContext {
    virtual ~CryptoDigestContext() = default;
    virtual void addBytes(std::span<const uint8_t> input) = 0;
    virtual Vector<uint8_t> computeHash() = 0;
};

template <typename SHAContext, typename SHAFunctions>
struct CryptoDigestContextImpl : public CryptoDigestContext {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CryptoDigestContextImpl);

    static std::unique_ptr<CryptoDigestContext> create()
    {
        return makeUnique<CryptoDigestContextImpl>();
    }

    CryptoDigestContextImpl()
    {
        SHAFunctions::init(&m_context);
        RELEASE_ASSERT(!SHAFunctions::starts(&m_context));
    }

    ~CryptoDigestContextImpl() override
    {
        SHAFunctions::free(&m_context);
    }

    void addBytes(std::span<const uint8_t> input) override
    {
        RELEASE_ASSERT(!SHAFunctions::update(&m_context, input.data(), input.size()));
    }

    Vector<uint8_t> computeHash() override
    {
        std::array<uint8_t, 64> output { };
        RELEASE_ASSERT(!SHAFunctions::final(&m_context, output.data()));
        return Vector<uint8_t>(std::span(output).first(SHAFunctions::digestLength));
    }

private:
    SHAContext m_context;
};

CryptoDigest::CryptoDigest() = default;

CryptoDigest::~CryptoDigest() = default;

static std::unique_ptr<CryptoDigestContext> createCryptoDigest(CryptoDigest::Algorithm algorithm)
{
    switch (algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
        return CryptoDigestContextImpl<mbedtls_sha1_context, SHA1Functions>::create();
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        return CryptoDigestContextImpl<mbedtls_sha256_context, SHA256Functions<true>>::create();
    case CryptoDigest::Algorithm::SHA_256:
        return CryptoDigestContextImpl<mbedtls_sha256_context, SHA256Functions<false>>::create();
    case CryptoDigest::Algorithm::SHA_384:
        return CryptoDigestContextImpl<mbedtls_sha512_context, SHA512Functions<true>>::create();
    case CryptoDigest::Algorithm::SHA_512:
        return CryptoDigestContextImpl<mbedtls_sha512_context, SHA512Functions<false>>::create();
    }
    return nullptr;
}

std::unique_ptr<CryptoDigest> CryptoDigest::create(CryptoDigest::Algorithm algorithm)
{
    std::unique_ptr<CryptoDigest> digest = WTF::makeUnique<CryptoDigest>();
    digest->m_context = createCryptoDigest(algorithm);
    return digest;
}

void CryptoDigest::addBytes(std::span<const uint8_t> input)
{
    ASSERT(m_context);
    m_context->addBytes(input);
}

Vector<uint8_t> CryptoDigest::computeHash()
{
    ASSERT(m_context);
    return m_context->computeHash();
}

} // namespace PAL
