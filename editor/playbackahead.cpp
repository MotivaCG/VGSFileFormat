// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.

#include "playbackahead.h"
#include <algorithm>
#include <cmath>
#include <limits>

std::vector<PlaybackAhead::Upcoming> PlaybackAhead::upcoming(int current, double now, double frameSeconds,
                                                             const std::function<std::optional<int>(double)> &frameAt, int count) {
    std::vector<Upcoming> out{{current, now}};
    if (!(frameSeconds > 0) || count <= 1) return out;
    // Half a frame apart: a frame the clock shows lasts a whole one, so none falls between. From
    // now itself: the frame on screen is the clock's at its last tick, which may be a frame behind.
    const double step = frameSeconds / 2;
    for (int j = 0; j <= count * 4 + 8 && int(out.size()) < count; ++j) {
        const double at = now + j * step;
        const auto frame = frameAt(at);
        if (!frame) break;
        if (*frame == out.back().frame) continue;
        // Once round a loop at most: a frame is held for its next turn only.
        if (std::any_of(out.begin(), out.end(), [&](const Upcoming &u) { return u.frame == *frame; })) break;
        out.push_back({*frame, at});
    }
    return out;
}
double PlaybackAhead::spacing(double frameSeconds, double displaySeconds) {
    // Listed times are within half a frame of when a frame starts: allow for that.
    return frameSeconds > 0 && frameSeconds < displaySeconds ? displaySeconds - frameSeconds / 2 : 0;
}
int PlaybackAhead::lookahead(double frameSeconds, double spacing) const {
    if (!(frameSeconds > 0)) return 1;
    const double apart = std::min(std::ceil((spacing + frameSeconds / 2) / frameSeconds), 16.0);
    const double spanned = std::min(std::ceil(latency() / frameSeconds), 240.0);
    return (MaxFrames + 2) * int(std::max(1.0, apart)) + int(spanned) + 1;
}
void PlaybackAhead::invalidate() { frames_.clear(); ++epoch_; }
void PlaybackAhead::decoded(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return;
    latencies_[latencyNext_] = seconds;
    latencyNext_ = (latencyNext_ + 1) % latencies_.size();
    latencyCount_ = std::min(latencyCount_ + 1, latencies_.size());
}
double PlaybackAhead::latency() const {
    if (!latencyCount_) return 0;
    // The median: a chunk boundary's one slow decode does not make every frame look late.
    std::array<double, 8> recent = latencies_;
    const auto end = recent.begin() + latencyCount_;
    std::nth_element(recent.begin(), recent.begin() + latencyCount_ / 2, end);
    return recent[latencyCount_ / 2];
}
size_t PlaybackAhead::bytes() const {
    size_t total = 0;
    for (const auto &held : frames_) if (held.data) total += renderFrameBytes(*held.data);
    return total;
}
bool PlaybackAhead::holds(int frame) const {
    return std::any_of(frames_.begin(), frames_.end(), [&](const Held &held) { return held.frame == frame; });
}
// Frames still to come, and those whose time has come for the next tick to show.
void PlaybackAhead::keepUpcoming(const std::vector<Upcoming> &upcoming, double now) {
    frames_.erase(std::remove_if(frames_.begin(), frames_.end(), [&](const Held &held) {
        if (held.at <= now) return held.at < now - DueSeconds;
        return std::none_of(upcoming.begin(), upcoming.end(), [&](const Upcoming &u) { return u.frame == held.frame; });
    }), frames_.end());
}
std::optional<int> PlaybackAhead::next(const std::vector<Upcoming> &upcoming, double now, bool currentShown,
                                       const std::vector<int> &decoding, double spacing) {
    keepUpcoming(upcoming, now);
    if (int(frames_.size() + decoding.size()) >= MaxFrames || (!frames_.empty() && bytes() >= byteBudget_)) return std::nullopt;
    const double arrival = now + latency();
    const auto asked = [&](int frame) { return holds(frame) || std::find(decoding.begin(), decoding.end(), frame) != decoding.end(); };
    // Ready by its turn (`onTime`), or at least still on screen when it comes.
    auto pick = [&](bool onTime) -> std::optional<int> {
        double last = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < upcoming.size(); ++i) {
            const int frame = upcoming[i].frame;
            const double at = upcoming[i].at;
            if (i == 0 ? currentShown : frame == upcoming.front().frame) {last = std::max(last, at); continue;}
            if (asked(frame)) {last = std::max(last, at); continue;}
            if (at - last < spacing) continue;
            // On screen until the next one comes; the last listed, for as long as can be told.
            const double until = i + 1 < upcoming.size() ? upcoming[i + 1].at : std::numeric_limits<double>::infinity();
            if (onTime ? at >= arrival : until > arrival) return frame;
        }
        return std::nullopt;
    };
    if (currentShown) if (const auto frame = pick(true)) return frame;
    return pick(false);
}
bool PlaybackAhead::store(int frame, quint64 epoch, FramePtr data, const std::vector<Upcoming> &upcoming) {
    if (epoch != epoch_ || !data || holds(frame)) return false;
    const auto it = std::find_if(upcoming.begin(), upcoming.end(), [&](const Upcoming &u) { return u.frame == frame; });
    if (it == upcoming.end()) return false;
    frames_.push_back({frame, it->at, std::move(data)});
    return true;
}
FramePtr PlaybackAhead::take(int frame, double now, int *taken) {
    auto it = std::find_if(frames_.begin(), frames_.end(), [&](const Held &held) { return held.frame == frame; });
    if (it == frames_.end())
        for (auto held = frames_.begin(); held != frames_.end(); ++held)
            if (held->at <= now && held->at >= now - DueSeconds && (it == frames_.end() || held->at > it->at)) it = held;
    if (it == frames_.end()) return nullptr;
    if (taken) *taken = it->frame;
    const double at = it->at;
    FramePtr data = std::move(it->data);
    frames_.erase(std::remove_if(frames_.begin(), frames_.end(), [&](const Held &held) { return held.at <= at; }), frames_.end());
    return data;
}
