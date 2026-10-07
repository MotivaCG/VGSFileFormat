#include "mgscodec.h"
#include <cstdio>
#include <cstring>
#include <future>

namespace {
void require(bool value, const char* message) {
  if (!value) throw mgs::Error(message);
}
void test(const mgs::AttributeSpec& spec, const mgs::Bytes& raw) {
  for (int model = 0; model < mgs::attributeModelCount(spec); ++model) {
    const auto standard = mgs::encodeAttribute(spec, model, raw.data(), raw.size());
    const auto searched = mgs::encodeAttribute(spec, model, raw.data(), raw.size(), mgs::EntropySearch::Thorough);
    require(searched.size() <= standard.size(), "search grew payload");
    require(mgs::decodeAttribute(spec, model, standard.data(), standard.size()) == raw, "standard values changed");
    mgs::DecodeStats stats;
    require(mgs::decodeAttribute(spec, model, searched.data(), searched.size(), &stats) == raw, "searched values changed");
    require(stats.streams > 0 && stats.symbols > 0 && stats.tablePayloadBytes > 0 && stats.maxTableBytes > 0,
            "missing stream/table diagnostics");
    require(mgs::encodeAttribute(spec, model, raw.data(), raw.size()) == standard, "search setting leaked");
  }
}
}

int main() {
  try {
    mgs::AttributeSpec half;
    half.kind = 0; half.rows = 65536;
    mgs::Bytes raw(65536 * 2);
    for (uint32_t i = 0; i < 65536; ++i) {
      raw[i * 2] = uint8_t(i); raw[i * 2 + 1] = uint8_t(i >> 8);
    }
    // All f16 patterns, including signed zeros, infinities and every NaN. Scales
    // below 16 bits cannot normalize this alphabet and must be skipped, not hang.
    test(half, raw);
    mgs::AttributeSpec packed;
    packed.kind = 2; packed.width = 4; packed.fields = {21, 11}; packed.rows = 8192;
    mgs::Bytes words(size_t(packed.rows) * 4);
    uint32_t random = 12345;
    for (size_t i = 0; i < size_t(packed.rows); ++i) {
      random = random * 1664525u + 1013904223u;
      const uint32_t value = i % 19 ? uint32_t(i % 7) : random;
      std::memcpy(words.data() + i * 4, &value, 4);
    }
    test(packed, words);
    mgs::AttributeSpec channels;
    channels.kind = 1; channels.family = 3; channels.fields = {0, 0, 0}; channels.rows = 4097;
    mgs::Bytes colors(size_t(channels.rows) * 3);
    for (size_t i = 0; i < size_t(channels.rows); ++i) {
      colors[i * 3] = uint8_t(i % 2 ? 255 : 0);
      colors[i * 3 + 1] = uint8_t(i % 13);
      colors[i * 3 + 2] = uint8_t(i % 13);
    }
    // Many empty contexts and sparse/dense frequency representations.
    test(channels, colors);
    half.rows = 33 * 40; half.family = 1; half.samples = 33;
    raw.resize(size_t(half.rows) * 2);
    for (size_t i = 0; i < size_t(half.rows); ++i) {
      const uint16_t value = uint16_t(0x3c00 + i % 33);
      raw[i * 2] = uint8_t(value); raw[i * 2 + 1] = uint8_t(value >> 8);
    }
    test(half, raw);
    half.family = 2; half.entries = 40;
    test(half, raw);
    for (uint64_t samples : {uint64_t(1), uint64_t(33), uint64_t(65536)}) {
      half.family = 1; half.samples = samples; half.rows = samples * (samples == 65536 ? 1 : 3);
      raw.resize(size_t(half.rows) * 2);
      for (size_t i = 0; i < size_t(half.rows); ++i) {
        const uint16_t value = uint16_t(i);
        raw[i * 2] = uint8_t(value); raw[i * 2 + 1] = uint8_t(value >> 8);
      }
      test(half, raw);
    }
    mgs::AttributeSpec runs;
    runs.kind = 1; runs.family = 4; runs.fields = {0, 0}; runs.intervals = 33; runs.rows = 33 * 150;
    mgs::Bytes deltas(size_t(runs.rows) * 2);
    for (size_t i = 0; i < deltas.size(); ++i) deltas[i] = uint8_t((i / 5) % 7);
    test(runs, deltas);
    mgs::AttributeSpec rotations;
    rotations.kind = 2; rotations.family = 7; rotations.width = 4;
    rotations.fields = {1, 9, 10, 10, 2}; rotations.samples = 33; rotations.rows = 33 * 150;
    mgs::Bytes quats(size_t(rotations.rows) * 4);
    for (size_t i = 0; i < size_t(rotations.rows); ++i) {
      const uint32_t value = uint32_t((i / 3) % 2) | uint32_t(i % 512) << 1 |
        uint32_t((i * 3) % 1024) << 10 | uint32_t((i * 7) % 1024) << 20 | uint32_t((i / 7) % 4) << 30;
      std::memcpy(quats.data() + i * 4, &value, 4);
    }
    // Run starts, changing dropped components and sign flips across rANS states.
    test(rotations, quats);
    for (uint64_t n : {uint64_t(1), uint64_t(4095), uint64_t(4096)}) {
      mgs::AttributeSpec scalar; scalar.rows = n;
      test(scalar, mgs::Bytes(size_t(n), 255));
    }
    // Calls with different effort on independent threads, plus exception restoration.
    auto worker = std::async(std::launch::async, [&] { test(packed, words); });
    test(channels, colors);
    worker.get();
    const auto before = mgs::encodeAttribute(channels, 0, colors.data(), colors.size());
    bool rejected = false;
    try { mgs::encodeAttribute(channels, 0, colors.data(), 0, mgs::EntropySearch::Thorough); }
    catch (const mgs::Error&) { rejected = true; }
    require(rejected, "invalid input accepted");
    require(mgs::encodeAttribute(channels, 0, colors.data(), colors.size()) == before, "exception leaked search setting");
    std::puts("entropy search: exact values, non-growing payloads, profiling and isolation passed");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
