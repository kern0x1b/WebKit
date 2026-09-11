/*
 * Digest implementation for the iOS 6 port.
 *
 * Upstream routes this through PALSwift, whose generated header comes from
 * Swift sources that cannot be built for armv7. CommonCrypto has provided the
 * same primitives since the first iOS release, so the digests are computed
 * directly against it.
 */

#include "config.h"
#include "CryptoDigest.h"

#include <CommonCrypto/CommonDigest.h>
#include <wtf/StdLibExtras.h>

namespace PAL::Crypto {

struct CryptoDigestContext {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CryptoDigestContext);

    CryptoDigest::Algorithm algorithm { CryptoDigest::Algorithm::SHA_1 };
    union {
        CC_SHA1_CTX sha1;
        CC_SHA256_CTX sha256;
        CC_SHA512_CTX sha512;
    } state;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CryptoDigestContext);

CryptoDigest::CryptoDigest()
    : m_context(WTF::makeUnique<CryptoDigestContext>())
{
}

CryptoDigest::~CryptoDigest() = default;

std::unique_ptr<CryptoDigest> CryptoDigest::create(CryptoDigest::Algorithm algorithm)
{
    std::unique_ptr<CryptoDigest> digest = WTF::makeUnique<CryptoDigest>();
    digest->m_context->algorithm = algorithm;

    switch (algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
        CC_SHA1_Init(&digest->m_context->state.sha1);
        break;
    case CryptoDigest::Algorithm::SHA_256:
        CC_SHA256_Init(&digest->m_context->state.sha256);
        break;
    case CryptoDigest::Algorithm::SHA_384:
        CC_SHA384_Init(&digest->m_context->state.sha512);
        break;
    case CryptoDigest::Algorithm::SHA_512:
        CC_SHA512_Init(&digest->m_context->state.sha512);
        break;
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        CC_SHA224_Init(&digest->m_context->state.sha256);
        break;
    }
    return digest;
}

void CryptoDigest::addBytes(std::span<const uint8_t> input)
{
    CC_LONG length = static_cast<CC_LONG>(input.size());
    switch (m_context->algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
        CC_SHA1_Update(&m_context->state.sha1, input.data(), length);
        break;
    case CryptoDigest::Algorithm::SHA_256:
        CC_SHA256_Update(&m_context->state.sha256, input.data(), length);
        break;
    case CryptoDigest::Algorithm::SHA_384:
        CC_SHA384_Update(&m_context->state.sha512, input.data(), length);
        break;
    case CryptoDigest::Algorithm::SHA_512:
        CC_SHA512_Update(&m_context->state.sha512, input.data(), length);
        break;
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        CC_SHA224_Update(&m_context->state.sha256, input.data(), length);
        break;
    }
}

Vector<uint8_t> CryptoDigest::computeHash()
{
    Vector<uint8_t> result;
    switch (m_context->algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
        result.grow(CC_SHA1_DIGEST_LENGTH);
        CC_SHA1_Final(result.mutableSpan().data(), &m_context->state.sha1);
        break;
    case CryptoDigest::Algorithm::SHA_256:
        result.grow(CC_SHA256_DIGEST_LENGTH);
        CC_SHA256_Final(result.mutableSpan().data(), &m_context->state.sha256);
        break;
    case CryptoDigest::Algorithm::SHA_384:
        result.grow(CC_SHA384_DIGEST_LENGTH);
        CC_SHA384_Final(result.mutableSpan().data(), &m_context->state.sha512);
        break;
    case CryptoDigest::Algorithm::SHA_512:
        result.grow(CC_SHA512_DIGEST_LENGTH);
        CC_SHA512_Final(result.mutableSpan().data(), &m_context->state.sha512);
        break;
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        result.grow(CC_SHA224_DIGEST_LENGTH);
        CC_SHA224_Final(result.mutableSpan().data(), &m_context->state.sha256);
        break;
    }
    return result;
}

} // namespace PAL::Crypto
