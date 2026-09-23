#ifndef VGSENCODER_H
#define VGSENCODER_H

// Writing VFGS captures (.vgs and .pgs) from a MINT capture.
//
// One object, filled in with setters and then run:
//
//     vgsenc::Encoder encoder;
//     encoder.setInputFile("boxing.mint");
//     encoder.setCoding(vgsenc::Coding::Compressed);
//     encoder.setSphericalHarmonicDegree(2);
//     encoder.setAuthor("SMN|The4DSCanner");
//     encoder.setProgressCallback([](int percent, const char *stage) {
//       printf("%3d%% %s\n", percent, stage);
//       return true;                       // false cancels the encode
//     });
//     if (!encoder.write("boxing.vgs"))
//       fprintf(stderr, "%s\n", encoder.lastError().c_str());
//
// There is no setter for the identifier: it is derived from the metadata and the
// contents when the file is written, so the same take always names itself the same way
// and two different takes never collide. Read it back with uuid() afterwards.
//
// Nothing here throws. Every call that can fail returns false and leaves a description
// in lastError(), which is also what makes the flat C interface in vgsencoder_c.h a
// wrapper rather than a translation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vgsenc {

/** How the attribute pages are stored: .vgs is entropy coded, .pgs is not. */
enum class Coding { Compressed, Plain };

/** What an audio track is, since the container stores it as delivered. */
enum class AudioFormat { Mp3 = 1, Aac = 2, Opus = 3, Wav = 4 };

/**
 * How a player should run the capture unless its user chooses otherwise: once, holding
 * the last frame; over and over; or there and back. Loop unless set.
 */
enum class PlaybackMode { Once = 0, Loop = 1, PingPong = 2 };

/** What a thumbnail is. */
enum class ImageFormat { Png = 1, Jpeg = 2, Webp = 3 };

/**
 * Reports progress and can stop the work. Return false to cancel: the encoder then
 * fails with "cancelled" and writes nothing. `stage` is a short description of what is
 * happening, for a status line.
 */
using ProgressFn = std::function<bool(int percent, const char *stage)>;

class Encoder {
public:
  Encoder();
  ~Encoder();
  Encoder(Encoder &&) noexcept;
  Encoder &operator=(Encoder &&) noexcept;

  // ---- input ------------------------------------------------------------------

  /** The MINT capture to convert. Read when the encode runs, not here. */
  bool setInputFile(const std::string &path);
  /** The same, from memory. The bytes are borrowed and must outlive the encode. */
  void setInputMemory(const uint8_t *data, size_t size);

  // ---- what to write ----------------------------------------------------------

  void setCoding(Coding);
  /**
   * Highest spherical harmonic degree to keep, 0 to 3. The source carries 3; writing
   * less drops whole planes of coefficients, which is where about a third of the file
   * lives. Values above 3 are clamped.
   */
  void setSphericalHarmonicDegree(uint32_t degree);
  /** Where this capture begins on an external timeline, for syncing with other media. */
  void setStartTick(uint64_t);
  /** How players should run it by default. Loop unless set. */
  void setPlaybackMode(PlaybackMode);
  /** Rows per page. The default suits every capture seen so far; 1024 to 1048576. */
  void setPageRows(uint32_t);

  // ---- metadata ---------------------------------------------------------------
  //
  // All strings are UTF-8 and at most 1024 bytes. The identifier is not here: see uuid().

  /** The catalogue identifier, yours to choose. Defaults to the input file's name. */
  void setId(std::string);
  /** Defaults to the input file's name. */
  void setTitle(std::string);
  void setAuthor(std::string);
  void setProjectName(std::string);
  void setTakeName(std::string);
  void setCaptureStudio(std::string);
  void setCopyright(std::string);
  void setSoftwareName(std::string);
  void setSoftwareVersion(std::string);
  /** At most 64 tags. */
  void addTag(std::string);
  void clearTags();

  // ---- optional payloads ------------------------------------------------------

  /** A sound track stored as delivered: nothing is transcoded or re-timed. */
  bool setAudioFile(const std::string &path);
  void setAudio(const uint8_t *data, size_t size, AudioFormat);
  bool setThumbnailFile(const std::string &path);
  void setThumbnail(const uint8_t *data, size_t size, ImageFormat);
  /** Free-form JSON, for whatever the standard fields do not describe. */
  bool setMetadataJson(const std::string &utf8Json);
  /** A second block, kept apart: one belongs to whoever produced the capture, the
   *  other to whoever uses it, and neither has to parse the other's. */
  bool setMetadataJson2(const std::string &utf8Json);

  // ---- running it -------------------------------------------------------------

  void setProgressCallback(ProgressFn);
  /**
   * Asks a running encode to stop. Safe to call from another thread; the encode ends
   * at its next progress point and reports "cancelled".
   */
  void cancel();

  /** Encodes and writes to `path`. Writes to a temporary and renames, so an interrupted
   *  run cannot leave a half written file where a capture should be. */
  bool write(const std::string &path);
  /** Encodes into memory instead. */
  bool encode(std::vector<uint8_t> &out);

  // ---- afterwards -------------------------------------------------------------

  /** The derived identifier of the capture just written, 16 bytes. */
  const std::array<uint8_t, 16> &uuid() const;
  /** The same as 8-4-4-4-12 hexadecimal. */
  std::string uuidText() const;
  /** Bytes written by the last successful run. */
  uint64_t outputSize() const;
  /** Why the last call returned false. Empty after a successful one. */
  const std::string &lastError() const;

  /** The format version this build writes. */
  static uint32_t formatVersion();

private:
  struct State;
  std::unique_ptr<State> state;
};

} // namespace vgsenc
#endif
