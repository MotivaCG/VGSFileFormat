#ifndef VGS_PCODEC_BRIDGE_H
#define VGS_PCODEC_BRIDGE_H
#include "mgscodec.h"
namespace mgs {
Bytes pcoCompress(const uint8_t* data, size_t size, unsigned kind);
void pcoDecompress(const uint8_t* data, size_t size, uint8_t* out, size_t bytes, unsigned kind);
}
#endif
