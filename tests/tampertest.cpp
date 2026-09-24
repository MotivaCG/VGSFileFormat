// Every way of altering a capture must be refused, and refused identically.
//
// The signature covers the whole structural region: the fixed header, every table, and
// the metadata. This walks that region and changes one byte at a time in each part of it,
// then requires the reader to refuse with exactly "invalid 4dgs capture" - not a parse
// error, not a size mismatch, not a different message for each kind of damage.
//
// Identical messages are the point rather than a detail. Which check failed is of no use
// to a caller and of some use to whoever is trying to get past them.

#include "vgsdecoder/vgsdecoder.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;

std::vector<uint8_t> readFile(const char *path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(2);
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

/** Flips one bit at `at` and requires the result to be refused with `expected`. */
void mustRefuseWith(const char *what, std::vector<uint8_t> bytes, size_t at,
                    const std::string &expected) {
  ++checks;
  if (at >= bytes.size()) {
    std::printf("FAIL  %s: offset %zu is past the end of the capture\n", what, at);
    ++failures;
    return;
  }
  bytes[at] ^= 0x01;

  try {
    vgsdec::Capture capture = vgsdec::Capture::openMemory(bytes.data(), bytes.size());
    // Opening is what authenticates, so reaching here is already the failure. Reading
    // something from it makes the report concrete rather than abstract.
    std::printf("FAIL  %s: altered byte %zu was accepted (title \"%s\")\n", what, at,
                capture.metadata().title.c_str());
    ++failures;
  } catch (const std::exception &error) {
    if (std::string(error.what()) == expected) {
      std::printf("ok    %s: refused with \"%s\"\n", what, expected.c_str());
    } else {
      std::printf("FAIL  %s: refused, but said \"%s\" instead of \"%s\"\n", what,
                  error.what(), expected.c_str());
      ++failures;
    }
  }
}

/** Everything past the first eight bytes must fail as an authenticity failure. */
void mustRefuse(const char *what, std::vector<uint8_t> bytes, size_t at) {
  mustRefuseWith(what, std::move(bytes), at, vgsdec::InvalidCapture);
}

void mustAccept(const char *what, const std::vector<uint8_t> &bytes) {
  ++checks;
  try {
    vgsdec::Capture capture = vgsdec::Capture::openMemory(bytes.data(), bytes.size());
    capture.setTime(0);
    std::printf("ok    %s: accepted and decodes\n", what);
  } catch (const std::exception &error) {
    std::printf("FAIL  %s: the untouched capture was refused: %s\n", what, error.what());
    ++failures;
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: tampertest <capture.vgs>\n");
    return 2;
  }

  const std::vector<uint8_t> original = readFile(argv[1]);
  mustAccept("untouched", original);

  // The fixed header is the first 192 bytes: magic, version, sizes, timing, bounds, the
  // playback mode, the motion. The signature block sits at signedSize and the payload
  // after it.
  const uint64_t structural = vgsdec::Capture::structuralSize(original.data(), original.size());

  // The first eight bytes answer "what is this", not "is this genuine", and they get
  // their own messages on purpose: a loader choosing between formats needs to hear that
  // this is not one of ours, and an application meeting a newer capture needs to be able
  // to say "update" rather than "broken". Neither leaks anything a caller does not have.
  mustRefuseWith("magic", original, 0, "not a VFGS file");
  mustRefuseWith("version", original, 4, "unsupported VGS version");
  mustRefuse("a size in the fixed header", original, 24);
  mustRefuse("the timebase", original, 96);
  mustRefuse("a bound", original, 128);
  mustRefuse("the playback mode", original, 176);
  mustRefuse("the motion type", original, 180);
  mustRefuse("the moving speed", original, 184);
  mustRefuse("the last byte of the fixed header", original, 191);

  // Past the fixed header and before the signature: the tables and the metadata, which
  // is where a capture says what it is. Altering a title has to fail as hard as altering
  // a chunk offset.
  const size_t afterHeader = 192;
  const size_t beforeSignature = size_t(structural) - 80;
  mustRefuse("the first table byte", original, afterHeader);
  mustRefuse("the middle of the tables", original, (afterHeader + beforeSignature) / 2);
  mustRefuse("the last byte before the signature", original, beforeSignature - 1);

  // The signature itself, and the key it names.
  mustRefuse("the signature", original, size_t(structural) - 40);
  mustRefuse("the key id", original, beforeSignature + 4);

  // A capture cut short, which is what a failed download looks like.
  {
    ++checks;
    std::vector<uint8_t> truncated(original.begin(),
                                   original.begin() + std::ptrdiff_t(structural) - 1);
    try {
      vgsdec::Capture::openMemory(truncated.data(), truncated.size());
      std::printf("FAIL  truncated: accepted a capture cut short of its signature\n");
      ++failures;
    } catch (const std::exception &error) {
      if (std::string(error.what()) == vgsdec::InvalidCapture) {
        std::printf("ok    truncated: refused\n");
      } else {
        std::printf("FAIL  truncated: said \"%s\" instead of \"%s\"\n", error.what(),
                    vgsdec::InvalidCapture);
        ++failures;
      }
    }
  }

  // Altering the payload is caught later, when the chunk is decoded rather than when the
  // file is opened: the signature covers the table of digests, and the digest covers the
  // chunk. The guarantee still reaches the frames, just not at open time.
  {
    ++checks;
    std::vector<uint8_t> altered = original;
    vgsdec::Capture reference = vgsdec::Capture::openMemory(original.data(), original.size());
    const uint64_t at = reference.chunk(0).offset + reference.chunk(0).size / 2;
    altered[size_t(at)] ^= 0x01;
    try {
      vgsdec::Capture capture = vgsdec::Capture::openMemory(altered.data(), altered.size());
      capture.setTime(0);
      std::printf("FAIL  payload: an altered chunk decoded without complaint\n");
      ++failures;
    } catch (const std::exception &) {
      std::printf("ok    payload: altering a chunk is caught when it is decoded\n");
    }
  }

  std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
