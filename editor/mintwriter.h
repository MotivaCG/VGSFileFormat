#pragma once
#include "vgscodec.h"
#include <functional>

// Rebuild a Gracia format-6 container from edited encoding-0 attributes.
// MINT holds raw padded arrays and always carries fifteen higher SH slots.
void writeMintSequence(const vgs::Header &,
                       const std::function<vgs::DecodedChunk(size_t)> &,
                       const vgs::WriteSink &,const vgs::Progress & = {});
