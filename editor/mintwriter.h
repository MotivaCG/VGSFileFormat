// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "vgscodec.h"
#include <functional>

// Rebuild a Gracia format-6 container from edited encoding-0 attributes.
// MINT holds raw padded arrays and always carries fifteen higher SH slots.
void writeMintSequence(const vgs::Header &,
                       const std::function<vgs::DecodedChunk(size_t)> &,
                       const vgs::WriteSink &,const vgs::Progress & = {});
