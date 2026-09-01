#include "config.h"

#if defined(WEBKIT_IOS6)

#include "CryptoEDKeyBridging.h"
#include "PlatformECKey.h"

namespace PAL::Crypto {

std::optional<PlatformECKey> PlatformECKey::importX963Pub(SpanConstUInt8, NamedCurve)
{
    return std::nullopt;
}

std::optional<PlatformECKey> PlatformECKey::importX963Private(SpanConstUInt8, NamedCurve)
{
    return std::nullopt;
}

std::optional<PlatformECKey> PlatformECKey::importCompressedPub(SpanConstUInt8, NamedCurve)
{
    return std::nullopt;
}

namespace EdKey {

CryptoOperationReturnValue privateToPublic(EdSigningAlgorithm, SpanConstUInt8)
{
    return { Error::UnsupportedAlgorithm, { } };
}

CryptoOperationReturnValue privateToPublicKeyAgreement(EdKeyAgreementAlgorithm, SpanConstUInt8)
{
    return { Error::UnsupportedAlgorithm, { } };
}

bool validateKeyPair(EdSigningAlgorithm, SpanConstUInt8, SpanConstUInt8)
{
    return false;
}

bool validateKeyPairKeyAgreement(EdKeyAgreementAlgorithm, SpanConstUInt8, SpanConstUInt8)
{
    return false;
}

} // namespace EdKey

} // namespace PAL::Crypto

#endif

// The operations that are performed, rather than refused.
//
// AES-GCM, HMAC, HKDF and AES key wrapping all reach CryptoKit upstream, and
// CryptoKit is Swift, which has no armv7 target - so this port used to alias
// every one of them to a stub returning an empty result and no error. A page
// encrypting a password before sending it received an empty buffer and sent
// that. Those four now run against OpenSSL, through plain C entry points in the
// compatibility library so this file needs no OpenSSL headers.
//
// The signature curves - Ed25519, X25519 and the elliptic-curve key import above
// - are still refused. They report an error rather than succeeding emptily.

#include "CryptoAlgorithmAESGCMCocoa.h"
#include "CryptoAlgorithmAESKWCocoaBridging.h"
#include "CryptoAlgorithmHKDFCocoaBridging.h"
#include "CryptoAlgorithmHMACCocoaBridging.h"

extern "C" {
int ios6AesGcmEncrypt(const unsigned char* key, size_t keyLength, const unsigned char* iv, size_t ivLength,
    const unsigned char* additional, size_t additionalLength, const unsigned char* plainText, size_t plainTextLength,
    size_t tagLength, unsigned char* out, size_t outCapacity, size_t* outLength);
int ios6AesGcmDecrypt(const unsigned char* key, size_t keyLength, const unsigned char* iv, size_t ivLength,
    const unsigned char* additional, size_t additionalLength, const unsigned char* cipherText, size_t cipherTextLength,
    size_t tagLength, unsigned char* out, size_t outCapacity, size_t* outLength);
int ios6Hmac(int digestIdentifier, const unsigned char* key, size_t keyLength,
    const unsigned char* data, size_t dataLength, unsigned char* out, size_t* outLength);
int ios6HkdfDeriveBits(int digestIdentifier, const unsigned char* key, size_t keyLength,
    const unsigned char* salt, size_t saltLength, const unsigned char* info, size_t infoLength,
    unsigned char* out, size_t outLength);
int ios6AesKeyWrap(const unsigned char* key, size_t keyLength, const unsigned char* plainText, size_t plainTextLength,
    unsigned char* out, size_t outCapacity, size_t* outLength);
int ios6AesKeyUnwrap(const unsigned char* key, size_t keyLength, const unsigned char* cipherText, size_t cipherTextLength,
    unsigned char* out, size_t outCapacity, size_t* outLength);
}

namespace PAL::Crypto {

static int digestIdentifier(CryptoDigestHashFunction function)
{
    switch (function) {
    case CryptoDigestHashFunction::SHA_1: return 0;
    case CryptoDigestHashFunction::DEPRECATED_SHA_224: return 1;
    case CryptoDigestHashFunction::SHA_256: return 2;
    case CryptoDigestHashFunction::SHA_384: return 3;
    case CryptoDigestHashFunction::SHA_512: return 4;
    }
    return -1;
}

// An empty vector's data() is null, and passing null to OpenSSL where it expects
// a buffer is undefined even when the length is zero.
static const unsigned char* bytesOf(const VectorUInt8& vector)
{
    static const unsigned char nothing[1] = { 0 };
    return vector.isEmpty() ? nothing : vector.span().data();
}

// The specification's default is a 128-bit tag, and the caller passes zero when
// the page did not ask for a particular size.
static size_t tagLengthOrDefault(size_t requested)
{
    return requested ? requested : 16;
}

static Expected<VectorUInt8, Error> encryptGCM(const VectorUInt8& iv, const VectorUInt8& key,
    const VectorUInt8& plainText, const VectorUInt8& additionalData, size_t desiredTagLengthInBytes)
{
    size_t tagLength = tagLengthOrDefault(desiredTagLengthInBytes);
    if (tagLength > 16)
        return makeUnexpected(Error::WrongTagSize);

    VectorUInt8 out(plainText.size() + tagLength);
    size_t produced = 0;
    if (!ios6AesGcmEncrypt(bytesOf(key), key.size(), bytesOf(iv), iv.size(),
            bytesOf(additionalData), additionalData.size(), bytesOf(plainText), plainText.size(),
            tagLength, out.mutableSpan().data(), out.size(), &produced))
        return makeUnexpected(Error::EncryptionFailed);
    out.shrink(produced);
    return out;
}

Expected<VectorUInt8, Error> encryptAESGCM(const VectorUInt8& iv, const VectorUInt8& key,
    const VectorUInt8& plainText, const VectorUInt8& additionalData, size_t desiredTagLengthInBytes)
{
    return encryptGCM(iv, key, plainText, additionalData, desiredTagLengthInBytes);
}

Expected<VectorUInt8, Error> encryptCryptoKitAESGCM(const VectorUInt8& iv, const Vector<uint8_t>& key,
    const VectorUInt8& plainText, const VectorUInt8& additionalData, size_t desiredTagLengthInBytes)
{
    return encryptGCM(iv, key, plainText, additionalData, desiredTagLengthInBytes);
}

Expected<VectorUInt8, Error> decyptAESGCM(const VectorUInt8& iv, const VectorUInt8& key,
    const VectorUInt8& cipherText, const VectorUInt8& additionalData, size_t desiredTagLengthInBytes)
{
    size_t tagLength = tagLengthOrDefault(desiredTagLengthInBytes);
    if (tagLength > 16)
        return makeUnexpected(Error::WrongTagSize);
    if (cipherText.size() < tagLength)
        return makeUnexpected(Error::EncryptionFailed);

    VectorUInt8 out(cipherText.size() - tagLength);
    size_t produced = 0;
    if (!ios6AesGcmDecrypt(bytesOf(key), key.size(), bytesOf(iv), iv.size(),
            bytesOf(additionalData), additionalData.size(), bytesOf(cipherText), cipherText.size(),
            tagLength, out.isEmpty() ? nullptr : out.mutableSpan().data(), out.size(), &produced))
        return makeUnexpected(Error::EncryptionFailed);
    out.shrink(produced);
    return out;
}

Expected<VectorUInt8, Error> signHMACCryptoKit(const VectorUInt8& key, const VectorUInt8& data,
    CryptoDigestHashFunction function)
{
    int identifier = digestIdentifier(function);
    if (identifier < 0)
        return makeUnexpected(Error::UnsupportedAlgorithm);

    VectorUInt8 out(64);
    size_t produced = 0;
    if (!ios6Hmac(identifier, bytesOf(key), key.size(), bytesOf(data), data.size(),
            out.mutableSpan().data(), &produced))
        return makeUnexpected(Error::EncryptionFailed);
    out.shrink(produced);
    return out;
}

Expected<bool, Error> verifyHMACCryptoKit(const VectorUInt8& signature, const VectorUInt8& key,
    const VectorUInt8& data, CryptoDigestHashFunction function)
{
    auto expected = signHMACCryptoKit(key, data, function);
    if (!expected)
        return makeUnexpected(expected.error());

    // Compared in constant time: an early exit on the first differing byte tells
    // a caller how much of a guessed signature was right.
    const auto& computed = expected.value();
    if (computed.size() != signature.size())
        return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < computed.size(); i++)
        difference |= computed[i] ^ signature[i];
    return !difference;
}

Expected<VectorUInt8, Error> deriveBitsHKDFCryptoKit(const VectorUInt8& key, const VectorUInt8& salt,
    const VectorUInt8& info, size_t lengthInBits, CryptoDigestHashFunction function)
{
    int identifier = digestIdentifier(function);
    if (identifier < 0)
        return makeUnexpected(Error::UnsupportedAlgorithm);
    if (!lengthInBits)
        return VectorUInt8 { };
    // The length arrives in bits, straight from the page's deriveBits call, and
    // the derivation produces bytes. Taking it for bytes returns eight times the
    // material asked for, which a caller will happily use.
    if (lengthInBits % 8)
        return makeUnexpected(Error::InvalidArgument);

    size_t lengthInBytes = lengthInBits / 8;
    VectorUInt8 out(lengthInBytes);
    if (!ios6HkdfDeriveBits(identifier, bytesOf(key), key.size(), bytesOf(salt), salt.size(),
            bytesOf(info), info.size(), out.mutableSpan().data(), lengthInBytes))
        return makeUnexpected(Error::EncryptionFailed);
    return out;
}

Expected<VectorUInt8, Error> wrapKeyAESKWCryptoKit(const VectorUInt8& key, const VectorUInt8& data)
{
    VectorUInt8 out(data.size() + 8);
    size_t produced = 0;
    if (!ios6AesKeyWrap(bytesOf(key), key.size(), bytesOf(data), data.size(),
            out.mutableSpan().data(), out.size(), &produced))
        return makeUnexpected(Error::EncryptionFailed);
    out.shrink(produced);
    return out;
}

Expected<VectorUInt8, Error> unwrapKeyAESKWCryptoKit(const VectorUInt8& key, const VectorUInt8& data)
{
    if (data.size() < 8)
        return makeUnexpected(Error::EncryptionFailed);
    VectorUInt8 out(data.size() - 8);
    size_t produced = 0;
    if (!ios6AesKeyUnwrap(bytesOf(key), key.size(), bytesOf(data), data.size(),
            out.isEmpty() ? nullptr : out.mutableSpan().data(), out.size(), &produced))
        return makeUnexpected(Error::EncryptionFailed);
    out.shrink(produced);
    return out;
}

} // namespace PAL::Crypto
