/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
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
#include "CryptoAlgorithmHKDF.h"

#include "CryptoAlgorithmHkdfParams.h"
#include "CryptoKeyRaw.h"
#include "ExceptionOr.h"
#include "OpenSSLUtilities.h"
#include <openssl/kdf.h>
#include <limits>

namespace WebCore {

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmHKDF::platformDeriveBits(const CryptoAlgorithmHkdfParams& parameters, const CryptoKeyRaw& key, size_t length)
{
    auto algorithm = digestAlgorithm(parameters.hashIdentifier);
    if (!algorithm)
        return Exception { ExceptionCode::NotSupportedError };

    auto& salt = parameters.saltVector();
    auto& info = parameters.infoVector();
    if (key.key().size() > static_cast<size_t>(std::numeric_limits<int>::max()) || salt.size() > static_cast<size_t>(std::numeric_limits<int>::max()) || info.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return Exception { ExceptionCode::OperationError };

    // OpenSSL3 distinguishes a missing buffer from an empty octet string.
    // The explicit zero lengths ensure this byte is never part of the input.
    constexpr uint8_t empty = 0;
    auto* keyData = key.key().isEmpty() ? &empty : key.key().span().data();
    auto* saltData = salt.isEmpty() ? &empty : salt.span().data();
    auto context = EvpPKeyCtxPtr(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0
        || EVP_PKEY_CTX_set_hkdf_md(context.get(), algorithm) <= 0
        || EVP_PKEY_CTX_set1_hkdf_key(context.get(), keyData, key.key().size()) <= 0
        || EVP_PKEY_CTX_set1_hkdf_salt(context.get(), saltData, salt.size()) <= 0
        || EVP_PKEY_CTX_add1_hkdf_info(context.get(), info.span().data(), info.size()) <= 0)
        return Exception { ExceptionCode::OperationError };

    Vector<uint8_t> output(length / 8);
    size_t outputSize = output.size();
    if (EVP_PKEY_derive(context.get(), output.mutableSpan().data(), &outputSize) <= 0 || outputSize != output.size())
        return Exception { ExceptionCode::OperationError };

    return output;
}

} // namespace WebCore
