#ifndef VGS_CRYPTO_H
#define VGS_CRYPTO_H

// Ed25519 signatures and SHA-512 digests for the VFGS container, over TweetNaCl
// (public domain, vendored beside this file). Nothing here is written by hand: the
// algorithms are the reference implementation, and this header only gives the codec
// a small typed surface over them.
//
// Digests are SHA-512 truncated to its first 16 bytes. The truncation is deliberate -
// it is a size trade for structures the signature already covers, and reusing SHA-512
// rather than adding a second hash keeps one primitive on both sides of the wire,
// since Ed25519 needs SHA-512 anyway.
//
// Only the reading side is here: hashing and verifying. Producing a signature is in
// vgssign.h, which the decoder never compiles.

#include <array>
#include <cstddef>
#include <cstdint>

namespace vgs {

constexpr size_t PublicKeySize = 32;
constexpr size_t PrivateSeedSize = 32;
constexpr size_t SignatureSize = 64;
constexpr size_t DigestSize = 16;

using PublicKey = std::array<uint8_t, PublicKeySize>;
using PrivateSeed = std::array<uint8_t, PrivateSeedSize>;
using Signature = std::array<uint8_t, SignatureSize>;
using Digest = std::array<uint8_t, DigestSize>;

/** SHA-512 of a byte range, truncated to DigestSize. */
Digest digest(const uint8_t *data, size_t size);

/**
 * Verifies a detached Ed25519 signature. Returns false for a bad signature and for
 * malformed input; it never throws, so a caller can treat every failure the same way.
 */
bool verify(const uint8_t *data, size_t size, const Signature &, const PublicKey &);

} // namespace vgs
#endif
