#include "vgscrypto.h"

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "tweetnacl.h"

// TweetNaCl asks the platform for randomness when it generates a key pair. This build
// never does: keys are generated once, outside, and arrive here as fixed bytes. The
// symbol still has to resolve, so it aborts rather than quietly returning zeros - a
// caller reaching it would be asking for a key pair this codec has no business making.
void randombytes(unsigned char *, unsigned long long);
void randombytes(unsigned char *, unsigned long long) { std::abort(); }
}

namespace vgs {

Digest digest(const uint8_t *data, size_t size) {
  uint8_t full[64];
  // TweetNaCl's crypto_hash is SHA-512. An empty range is legal and hashes to the
  // usual SHA-512 of no bytes.
  crypto_hash(full, data, static_cast<unsigned long long>(size));
  Digest out{};
  for (size_t i = 0; i < DigestSize; ++i)
    out[i] = full[i];
  return out;
}

bool verify(const uint8_t *data, size_t size, const Signature &signature,
            const PublicKey &publicKey) {
  // crypto_sign_open takes the combined form, so the two halves are put back together
  // for the call. The structural region is kilobytes, not the payload, so this copy is
  // bounded and happens once per file.
  std::vector<uint8_t> combined;
  try {
    combined.resize(size + SignatureSize);
  } catch (...) {
    return false;
  }
  for (size_t i = 0; i < SignatureSize; ++i)
    combined[i] = signature[i];
  if (size)
    std::memcpy(combined.data() + SignatureSize, data, size);

  std::vector<uint8_t> message(combined.size());
  unsigned long long length = 0;
  return crypto_sign_open(message.data(), &length, combined.data(),
                          static_cast<unsigned long long>(combined.size()),
                          publicKey.data()) == 0;
}

} // namespace vgs
