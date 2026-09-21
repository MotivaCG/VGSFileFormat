#ifndef VGS_KEYS_H
#define VGS_KEYS_H

// Signing keys for the VFGS authoring pipeline. Generated once with the operating
// system's random generator; they are not derived from anything and are not shared
// with any other project.
//
// THIS HEADER HOLDS A PRIVATE KEY. It belongs to the authoring side only. A reader
// links vgspublickey.h instead, which carries the public half and nothing else.
//
// Rotating keys does not touch the container format: a file records the key id it was
// signed with, so a new pair is a new id here and an extra entry in the reader's table.

// A build that is not the authoring pipeline must not reach this file at all, so the
// mistake is a compile error rather than a private key quietly ending up in a library
// somebody downloads. VGS_AUTHORING is set by the encoder target and by nothing else.
#ifndef VGS_AUTHORING
#error "vgskeys.h holds the private signing key and belongs to the encoder only"
#endif

#include "vgscrypto.h"

namespace vgs {

// The key pair a fresh capture is signed with.
constexpr uint32_t AuthoringKeyId = 1;

constexpr PrivateSeed AuthoringPrivateSeed = {
    0x87, 0x6d, 0x5b, 0xd3, 0xc8, 0x66, 0xb6, 0xb4,
    0x7e, 0xc9, 0x63, 0xf3, 0x12, 0x05, 0x2c, 0x1a,
    0xc9, 0x0d, 0x97, 0x21, 0x90, 0x2c, 0x2e, 0x6b,
    0x14, 0xce, 0xc8, 0xb1, 0x82, 0x43, 0xf5, 0xc9
};

} // namespace vgs
#endif
