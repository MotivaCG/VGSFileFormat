#ifndef MINTSKINRECOVERY_H
#define MINTSKINRECOVERY_H

#include "mintfile.h"

// Optional material-specific colour recovery. Requires positions in metres.
// Returns RGB deltas; opacity, geometry and directional shading are not changed.
bool mintSkinRecovery(const MintFrame& original, const MintFrame& corrected,
                      QVector<float>* deltas, const MintProgressFn& progress);

#endif
