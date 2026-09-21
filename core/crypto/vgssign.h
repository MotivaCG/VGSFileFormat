#ifndef VGS_SIGN_H
#define VGS_SIGN_H

// The authoring half of the signature. Only a program that writes captures links this:
// it is the one translation unit that sees the private key, and it is deliberately not
// part of the codec library, which is compiled into the WebAssembly decoder browsers
// download.

#include "vgscodec.h"
#include "vgscrypto.h"

namespace vgs {

/**
 * Detached Ed25519 signature of a byte range. Ed25519 signing needs the public key as
 * well as the seed - it is part of the secret key - so the caller passes both rather
 * than having this derive one from the other.
 */
Signature sign(const uint8_t *data, size_t size, const PrivateSeed &,
               const PublicKey &);

/** Signs with the current authoring key, for EncodeOptions::signer. */
Signer authoringSigner();

} // namespace vgs
#endif
