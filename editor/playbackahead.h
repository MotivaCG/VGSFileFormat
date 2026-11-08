// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#pragma once

#include "captureworker.h"
#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

// Frames decoded ahead of the playback clock, on the GUI side; the editor and the viewer share it.
// While a capture plays, the frames the clock is going to show are asked of the worker before
// their turn and kept here until it comes, so a frame is on screen the moment its time comes
// instead of being decoded only then. Up to two requests are with the worker at once, so it does
// not wait for the GUI thread, which a display's refresh can hold for a whole frame.
//
// Which frames: those the clock will show, in its order - a loop's wrap is followed, and when
// frames come faster than the display refreshes, only one a refresh. A frame is asked for when it
// will be ready by its turn, judged from how long requests have been taking; when decoding falls
// behind the clock it aims at where the clock will be, and frames that would arrive too late to
// be shown are not decoded at all.
//
// Frames are timeline frame numbers. Held frames hold the project as it was when they were asked
// for: anything that changes what a frame looks like, a seek or a stop calls invalidate().
class PlaybackAhead {
public:
    // At most this many frames ahead, held or being decoded, and no more than about this much
    // memory unless that is a single frame: a frame of a large capture with full SH is a few
    // hundred megabytes.
    static constexpr int MaxFrames = 5;
    static constexpr size_t ByteBudget = 768ull * 1024 * 1024;
    // Requests with the worker at once while playing.
    static constexpr int MaxRequests = 2;
    explicit PlaybackAhead(size_t byteBudget = ByteBudget) : byteBudget_(byteBudget) {}

    // A frame the clock will show and when, in seconds of playback; the current one first, at now.
    struct Upcoming { int frame; double at; };
    // `frameAt` gives the frame at a playback instant, nullopt once playback stops there. It is
    // sampled half a frame's duration apart (`frameSeconds`, at the playback speed), so no frame
    // the clock shows is missed; at most `count` frames are listed, and no frame twice: a short
    // loop is listed once round.
    static std::vector<Upcoming> upcoming(int current, double now, double frameSeconds,
                                          const std::function<std::optional<int>(double)> &frameAt, int count);
    // How far apart, in seconds of playback, frames worth decoding are: a display refreshing
    // every `displaySeconds` shows one frame a refresh at most.
    static double spacing(double frameSeconds, double displaySeconds);
    // How many upcoming frames are worth listing: those held at most at that spacing, and those
    // that pass while one is decoded.
    int lookahead(double frameSeconds, double spacing = 0) const;

    void invalidate();
    quint64 epoch() const { return epoch_; }

    // How long a request took from being made to arriving, while playing.
    void decoded(double seconds);
    // What a request is expected to take: the median of the recent ones, 0 before any.
    double latency() const;

    // The frame to ask for next, if any; none when enough are held or being decoded (`decoding`).
    // With the current frame on screen, the first one that will be ready by its turn, or failing
    // that the first still on screen by then; without it, the first still on screen by then.
    // Frames closer than `spacing` to one held or asked for are passed over. Held frames the
    // clock will no longer show go first.
    std::optional<int> next(const std::vector<Upcoming> &upcoming, double now, bool currentShown,
                            const std::vector<int> &decoding = {}, double spacing = 0);
    // A frame asked for ahead arrives. Kept when it was asked for in this epoch and the clock is
    // still to show it; false when it is not kept.
    bool store(int frame, quint64 epoch, FramePtr data, const std::vector<Upcoming> &upcoming);
    // The frame to show for the clock's current frame, `now`: the one held for it, or else the
    // latest held one whose time has come - the clock's ticks need not land on every frame. It
    // leaves, and so do frames held for before it. Null when there is none, which drops nothing.
    FramePtr take(int frame, double now, int *taken = nullptr);
    // A frame went on screen without take() (it arrived as the clock reached it): held frames
    // whose time had come by `now` are older than it and go.
    void shownAt(double now);
    // How long a held frame whose time has passed is still worth showing.
    static constexpr double DueSeconds = 0.05;

    size_t count() const { return frames_.size(); }
    size_t bytes() const;
    bool holds(int frame) const;

private:
    // `at`: when the clock comes to it, in seconds of playback.
    struct Held { int frame; double at; FramePtr data; };
    std::deque<Held> frames_;
    quint64 epoch_ = 1;
    size_t byteBudget_;
    std::array<double, 8> latencies_{};
    size_t latencyCount_ = 0, latencyNext_ = 0;
    void keepUpcoming(const std::vector<Upcoming> &upcoming, double now);
};
