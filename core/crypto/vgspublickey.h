#ifndef VGS_PUBLIC_KEY_H
#define VGS_PUBLIC_KEY_H

// The public half of the VFGS authoring keys, for readers. No private material here.
//
// A capture names the key that signed it, so several may be trusted at once and an
// old one can be retired without a format change: add the entry, keep verifying the
// files already in the field, drop it when nothing signed with it is left.

#include "vgscrypto.h"

namespace vgs {

struct TrustedKey {
  uint32_t id;
  PublicKey key;
};

inline constexpr TrustedKey TrustedKeys[] = {
  { 1, {
      0x92, 0xaa, 0xf0, 0xd4, 0x5e, 0xf3, 0x39, 0x37,
      0x7b, 0x96, 0x92, 0x94, 0x42, 0x5c, 0x51, 0x45,
      0x51, 0xee, 0xef, 0xfb, 0xb7, 0x55, 0xf3, 0xf6,
      0x76, 0xe0, 0xea, 0x04, 0x02, 0x02, 0xa9, 0xb0
  } }
};

/** The public key for an id, or null when the id is not trusted. */
inline const PublicKey *trustedKey(uint32_t id) {
  for (const auto &entry : TrustedKeys)
    if (entry.id == id)
      return &entry.key;
  return nullptr;
}

} // namespace vgs
#endif
