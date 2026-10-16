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
#include <QVector3D>
#include <functional>
#include <memory>
#include <vector>

// Loop morph: the frames that carry the end of a playback range back to its start, so a
// loop plays without a jump. No splat is the same one in both frames, so each frame is
// deformed onto the other along an embedded deformation graph (Sumner et al. 2007) fitted as
// rigid as possible: pinned by the markers numbered on both frames, then drawn onto the
// other frame's surface by nearest points. Both then travel with their graph's nodes turning
// along the shortest arc and their positions solved as rigid as possible at every step, so a
// limb that turns swings round rather than cutting through. The end frame's splats fade out
// as the start frame's fade in, each taking on the colour of the other frame's nearest splat.
namespace loopmorph {

// Splats in one space: xyz, quaternion xyzw, scale xyz, base colour rgb (0.5 + C0 dc),
// opacity, and `shCoefficients` higher-order SH terms of three channels each.
struct Cloud {
    std::vector<float> position, rotation, scale, colour, opacity, sh;
    int shCoefficients = 0;
    size_t size() const { return opacity.size(); }
    void reserve(size_t splats);
    // Splat `i` of `from`, which has the same number of SH terms.
    void append(const Cloud &from, size_t i);
};
// The same point of the subject on the end frame and on the start frame.
struct Pair { QVector3D end, start; };

class Morph {
public:
    // Fits both deformations. Throws when either frame is empty, and "Loop morph canceled."
    // when `cancelled` says so; it is asked often, from this thread only.
    Morph(Cloud end, Cloud start, const std::vector<Pair> &pairs, const std::function<bool()> &cancelled = {});
    ~Morph();
    Morph(const Morph &) = delete;
    Morph &operator=(const Morph &) = delete;
    // `s` in [0,1]: 0 is the end frame as it is, 1 the start frame. Both frames' splats are
    // in it while either shows; one that has faded out entirely is left out.
    Cloud evaluate(double s) const;
    // Where the markers land, on average, from where they were picked on the other frame,
    // in the clouds' units: how well the fit honours them.
    double markerError() const;
    size_t nodes() const; // in both graphs
private:
    struct Data;
    std::unique_ptr<Data> d_;
};

// Where a point of one frame probably is on the other, from the marker pairs alone: the rigid
// motion that best fits them, plus their own misfit near the point, nearer pairs counting
// more. `toEnd` maps a Start point to End, otherwise the other way. False with fewer than
// three pairs or all of them in a line.
bool predict(const std::vector<Pair> &pairs, const QVector3D &from, bool toEnd, QVector3D *to);
// For each pair, how far its End point is from where the other pairs put it: a click that
// paired different points stands out. NaN where fewer than three others can say.
std::vector<float> deviations(const std::vector<Pair> &pairs);

// How much of each frame shows at `s`: the end frame fades out over the second half and
// more, the start frame in over the first, so the subject never thins in the middle.
float endOpacity(double s);
float startOpacity(double s);
}
