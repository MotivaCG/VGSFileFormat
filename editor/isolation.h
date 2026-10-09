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
#include "project.h"
#include <functional>
#include <vector>

// Adapted from SMNForge's pruneIsolation: d_N excludes the query point itself.
// The global upper median is measured only on surviving finite world centres.
void applyIsolation(const std::vector<QVector3D> &positions,std::vector<uint8_t> &keep,
                    const QVector<IsolationFilter> &,const std::function<bool()> &cancelled = {});
