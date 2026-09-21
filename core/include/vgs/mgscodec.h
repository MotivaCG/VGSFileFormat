#ifndef MGSCODEC_H
#define MGSCODEC_H

// .mgs: lossless recoding of Gracia format-6 .mint captures.
//
// A .mgs rebuilds the original .mint byte for byte, so the Gracia runtime reads the
// result unchanged. Only the chunk payloads are recoded; the metadata region and any
// trailing bytes are stored as they are.
//
// Every chunk is coded on its own, because the runtime reads one whole chunk per
// request. Inside a chunk, each known array is split into its integer fields and
// entropy coded with static rANS; the encoder tries a few models per array and keeps
// the smallest. Bytes no model covers go to a residue stream, so an unexpected layout
// costs size, never correctness.
//
// A chunk payload starts with a table of what it holds and where each piece lands in
// the rebuilt chunk. A reader can therefore hand items to parallel workers and place
// their bytes without knowing anything about .mint; only decodeItem() needs the
// metadata, to plan how each item is laid out.
//
// Plain C++17 with no Qt, so the same file builds into the converter and into the
// WebAssembly decoder the viewer uses. Errors are thrown as mgs::Error.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mgs {

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- container

struct ChunkEntry
{
    uint64_t mintStart = 0;  // where the chunk's bytes sit in the .mint
    uint64_t mintSize = 0;
    uint64_t mgsStart = 0;   // where its payload sits in the .mgs
    uint64_t mgsSize = 0;
};

struct Header
{
    uint64_t mintSize = 0;
    uint64_t metadataStart = 0, metadataSize = 0;  // .mint bytes [0, metadataSize)
    uint64_t tailStart = 0, tailSize = 0;          // .mint bytes after the last chunk
    std::vector<ChunkEntry> chunks;
    uint64_t headerSize = 0;                       // bytes before the first payload
};

// Parses the fixed part of a .mgs. `size` may be just the leading bytes: returns false
// when more are needed, with *needed set to how many.
bool readHeader(const uint8_t* data, size_t size, Header* out, size_t* needed);

// Whole-file conversion. `progress(done, total)` is called after each chunk; return
// false from it to cancel, which throws mgs::Error("cancelled").
Bytes encodeFile(const uint8_t* mint, size_t size,
                 const std::function<bool(int done, int total)>& progress = {});
Bytes decodeFile(const uint8_t* mgs, size_t size);

// ---------------------------------------------------------------- one chunk, in pieces

struct Span
{
    uint64_t offset = 0;  // within the rebuilt chunk
    uint64_t size = 0;
};

// The table at the start of a chunk payload.
struct ChunkTable
{
    std::vector<Span> rawSpans;           // copied from rawBytes, in order
    const uint8_t* rawBytes = nullptr;

    struct Item
    {
        Span out;                         // where the decoded bytes go
        const uint8_t* payload = nullptr;
        uint64_t payloadSize = 0;
    };
    std::vector<Item> items;

    std::vector<Span> residueSpans;       // filled, in order, from the residue stream
    const uint8_t* residue = nullptr;
    uint64_t residueSize = 0;

    // The leading bytes a planner needs: the table's raw part (rank boundaries).
    const uint8_t* head = nullptr;
    uint64_t headSize = 0;
};

// Pointers in the table point into `payload`, which must outlive it.
ChunkTable readChunkTable(const uint8_t* payload, size_t size);

// Decodes items of one chunk. Construct once per worker from the .mint metadata
// (Header::metadata bytes); plan() is cheap to repeat for the same chunk.
class ItemDecoder
{
public:
    ItemDecoder(const uint8_t* metadata, size_t size);
    ~ItemDecoder();
    ItemDecoder(const ItemDecoder&) = delete;
    ItemDecoder& operator=(const ItemDecoder&) = delete;

    size_t chunkCount() const;

    // Plans chunk `index` from its table head (ChunkTable::head).
    void plan(size_t index, const uint8_t* head, size_t headSize);
    size_t itemCount() const;
    uint64_t itemSize(size_t item) const;

    // Decodes one planned item into `out`, which must hold itemSize(item) bytes.
    void decodeItem(size_t item, const uint8_t* payload, size_t size, uint8_t* out) const;

private:
    struct State;
    State* m_state;
};

// The integer columns of one chunk, as the encoder sees them before any model or entropy
// coding. Only for measuring other coders on the same data; the format does not use it.
struct ColumnDump
{
    std::string item;   // the .mint array it came from
    int column = 0;
    std::vector<int32_t> values;
};
std::vector<ColumnDump> dumpColumns(const uint8_t* mint, size_t size, size_t chunk);

// Container-independent numerical coding shared with VGS. These describe logical
// arrays, never .mint offsets or metadata. MGS's version-4 bitstream is unchanged.
struct AttributeSpec {
    uint32_t kind = 1, family = 0, width = 0;
    uint64_t rows = 0, samples = 0, intervals = 0, entries = 0;
    std::vector<int> fields = {0};
    std::vector<uint8_t> ranks; // one term count per splat, only for term arrays
};
struct SourceAttribute {
    std::string name;
    uint64_t offset = 0, size = 0; // absolute address in the input, encoder only
    AttributeSpec spec;
};
std::vector<SourceAttribute> sourceAttributes(const uint8_t* mint, size_t size, size_t chunk);
uint64_t attributeSize(const AttributeSpec& spec);
int attributeModelCount(const AttributeSpec& spec);
Bytes encodeAttribute(const AttributeSpec& spec, int model, const uint8_t* data, size_t size);
Bytes decodeAttribute(const AttributeSpec& spec, int model, const uint8_t* data, size_t size);

// Decodes a residue stream into `out` (the total size of the residue spans).
void decodeResidue(const uint8_t* payload, size_t size, uint8_t* out, size_t count);

// Convenience for a single thread: the whole chunk.
Bytes decodeChunk(const uint8_t* metadata, size_t metadataSize, size_t index,
                  const uint8_t* payload, size_t size);

} // namespace mgs

#endif // MGSCODEC_H
