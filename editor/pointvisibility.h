// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#pragma once
#include "captureworker.h"

// One CPU visibility predicate for viewport picking/focus, fallback rendering and MCP
// sampling. GPU rendering uses the same compiled stack and decoded mask.
inline bool keepsPreviewPoint(const PointVertex &point, const QMatrix4x4 &model,
                              const CompiledModifiers &modifiers, bool greensApplied,
                              bool showRemovedCrop = false) {
    const bool masked=greensApplied || !modifiers.isolations.isEmpty() || !modifiers.prunes.isEmpty() || !modifiers.erased.empty();
    if (masked && point.modifierVisibility<.5f) return false;
    return (showRemovedCrop || modifiers.keepsPosition(model.map({point.position[0],point.position[1],point.position[2]})))
        && (greensApplied || !modifiers.removesColour({point.color[0],point.color[1],point.color[2]}));
}
