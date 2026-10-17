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
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Loop morph: the frames that carry the end of a playback range back to its start, so a
// loop plays without a jump. No splat is the same one in both frames, so each frame is
// deformed onto the other along an embedded deformation graph (Sumner et al. 2007) fitted as
// rigid as possible: pinned by the markers numbered on both frames, then drawn onto the
// other frame's surface by nearest points. Both then travel with their graph's nodes turning
// along the shortest arc and their positions solved as rigid as possible at every step, so a
// limb that turns swings round rather than cutting through. The End frame fades into the
// Start frame, each splat taking on the colour of the other frame's most alike one.
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
// A source frame's splats as followed from frame to frame: where each is, its colour, and
// which record of which source chunk it is. Within a chunk a record is the same splat in
// every frame; across chunks nothing is.
struct Track {
    std::vector<float> position, colour;
    std::vector<uint32_t> record;
    long long chunk = -1;
    size_t size() const { return record.size(); }
};

class Morph {
public:
    // Fits both deformations. Throws when either frame is empty, and "Loop morph canceled."
    // when `cancelled` says so; it is asked often, from this thread only.
    Morph(Cloud end, Cloud start, const std::vector<Pair> &pairs, const std::function<bool()> &cancelled = {});
    ~Morph();
    Morph(const Morph &) = delete;
    Morph &operator=(const Morph &) = delete;
    // `s` in [0,1]: 0 is the end frame as it is, 1 the start frame. A splat that does not
    // show at `s` is left out. `source`, when given, says where each splat came from: i for
    // the End frame's i-th, the End frame's count plus j for the Start frame's j-th.
    Cloud evaluate(double s, std::vector<uint32_t> *source = nullptr) const;

    // A morph that overlaps the range's last frames rather than following End: the capture
    // plays on and takes the End-to-Start correction on gradually. Each deformation graph is
    // followed back through the frames before its own: `back[0]` is that frame (End, or
    // Start) and `back[i]` the one i frames earlier, all in the clouds' space. Within a chunk
    // each node moves as its splats do; at a chunk change it goes on as it was moving and is
    // then fitted onto the earlier frame's surface. Throws "Loop morph canceled." likewise.
    void follow(bool start, const std::vector<Track> &back, const std::function<bool()> &cancelled = {});
    // How many frames back each graph has been followed.
    size_t followed(bool start) const;
    // A frame of an overlapping morph, `back` frames before End: `end` is the capture's own
    // frame there, carried `s` of the way to Start by the correction moved along with it.
    // `start`, given, is the frame as many frames before Start, the morph then blending the
    // two motions as well (a lead-in); without it the Start frame itself is the target.
    // Sources number `end`'s splats first, then `start`'s (or the Start frame's).
    struct Live {
        size_t back = 0;
        const Cloud *end = nullptr, *start = nullptr;
    };
    Cloud evaluate(double s, const Live &live, std::vector<uint32_t> *source = nullptr) const;
    // Where a point of the End frame, in the clouds' space, is carried to at `s`, `back`
    // frames before End (0 for a morph that follows End) - the marker on the morph's frame.
    QVector3D carry(const QVector3D &onEnd, double s, size_t back = 0, bool leadIn = false) const;
    // Keeps each frame's shape - which splats show, where, turned how and how opaque, some 36
    // bytes a splat - once worked out, so the morph plays again without deforming anything:
    // about 11 MB a frame for 300 000 splats. Colour and SH are mixed afresh each time. The
    // cache goes with this object, which is fitted again whenever anything it depends on
    // changes. Off by default (an export evaluates each frame once).
    void setCaching(bool caching);
    // Where the markers land, on average, from where they were picked on the other frame,
    // in the clouds' units: how well the fit honours them.
    double markerError() const;
    size_t nodes() const; // in both graphs
private:
    struct Data;
    struct Shape;
    std::shared_ptr<const Shape> shape(double s, const Live &live) const;
    Cloud mix(double s, const Shape &shape, const Live &live, std::vector<uint32_t> *source) const;
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

// The opacity a splat of the End or the Start frame has at `s`, from its own `alpha`. Between
// 0.2 and 0.8 of the way the End frame hands its optical depth over to the Start frame: each
// keeps 1-(1-alpha)^share, the two shares adding up to one, so where the frames lie on each
// other they stop as much light together as either alone. Scaling the opacities instead lets
// light through the middle of the fade, and what is inside shows: the subject reads paler.
float endOpacity(double s, float alpha);
float startOpacity(double s, float alpha);
}
