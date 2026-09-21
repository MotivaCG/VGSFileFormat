#include "vgssign.h"

#include "vgskeys.h"
#include "vgspublickey.h"

#include <vector>

extern "C" {
#include "tweetnacl.h"
}

namespace vgs {

Signature sign(const uint8_t *data, size_t size, const PrivateSeed &seed,
               const PublicKey &publicKey) {
  // An Ed25519 secret key is the seed followed by the public key.
  uint8_t secret[64];
  for (size_t i = 0; i < PrivateSeedSize; ++i)
    secret[i] = seed[i];
  for (size_t i = 0; i < PublicKeySize; ++i)
    secret[PrivateSeedSize + i] = publicKey[i];

  // crypto_sign writes signature || message; only the signature is kept, since the
  // message is the file itself and is not about to be stored twice.
  std::vector<uint8_t> signed_(size + SignatureSize);
  unsigned long long length = 0;
  crypto_sign(signed_.data(), &length, data, static_cast<unsigned long long>(size),
              secret);
  Signature out{};
  for (size_t i = 0; i < SignatureSize; ++i)
    out[i] = signed_[i];
  return out;
}

Signer authoringSigner() {
  const PublicKey *key = trustedKey(AuthoringKeyId);
  if (!key)
    throw Error("the authoring key id has no public key");
  const PublicKey publicKey = *key;
  return {AuthoringKeyId, [publicKey](const uint8_t *data, size_t size) {
            return sign(data, size, AuthoringPrivateSeed, publicKey);
          }};
}

} // namespace vgs
