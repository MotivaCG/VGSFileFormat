// Reader subset of Gracia4DGSConverter/mintfile.cpp; see README.md.
#include "mintfile.h"
#include "mintskinrecovery.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QCoreApplication>
#include <QSaveFile>
#include <QtEndian>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// ---------------------------------------------------------------- primitives

quint64 readU64(const char* p) { quint64 v; std::memcpy(&v, p, 8); return qFromLittleEndian(v); }
quint32 readU32(const char* p) { quint32 v; std::memcpy(&v, p, 4); return qFromLittleEndian(v); }
quint16 readU16(const char* p) { quint16 v; std::memcpy(&v, p, 2); return qFromLittleEndian(v); }

double readF64(const char* p)
{
    quint64 bits = readU64(p);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

float readF32(const char* p)
{
    quint32 bits = readU32(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

float halfToFloat(quint16 h)
{
    const quint32 sign = quint32(h & 0x8000u) << 16;
    const quint32 exponent = (h >> 10) & 0x1Fu;
    const quint32 mantissa = h & 0x3FFu;

    quint32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal half: renormalise into a float exponent.
            quint32 e = 0, m = mantissa;
            while (!(m & 0x400u)) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

quint16 floatToHalf(float f)
{
    quint32 bits;
    std::memcpy(&bits, &f, 4);
    const quint32 sign = (bits >> 16) & 0x8000u;
    qint32 exponent = qint32((bits >> 23) & 0xFFu) - 127 + 15;
    quint32 mantissa = bits & 0x7FFFFFu;

    if (exponent >= 0x1F)
        return quint16(sign | 0x7C00u);                 // overflow to infinity
    if (exponent <= 0) {
        if (exponent < -10)
            return quint16(sign);                        // underflow to zero
        mantissa |= 0x800000u;
        const quint32 shift = quint32(14 - exponent);
        const quint32 rounded = (mantissa + (1u << (shift - 1))) >> shift;
        return quint16(sign | rounded);
    }
    // Round to nearest, ties away from zero. The GPU that the format was measured
    // against truncates instead, which shifts rotations by about 4e-4; that is far
    // below anything an export or a color edit cares about. See MINT_FORMAT.md 6.6.
    const quint32 rounded = (mantissa + 0x1000u) >> 13;
    if (rounded & 0x400u)
        return quint16(sign | (quint32(exponent + 1) << 10));
    return quint16(sign | (quint32(exponent) << 10) | rounded);
}

float readHalf(const char* p) { return halfToFloat(readU16(p)); }

// ------------------------------------------------------------ format helpers

// 21 bits per axis, sharing one scalar range. See MINT_FORMAT.md 6.4.
void unpackPosition(quint64 word, double lo, double hi, float* xyz)
{
    const double scale = (hi - lo) / 2097151.0;
    xyz[0] = float(lo + double((word >> 43) & 0x1FFFFFull) * scale);
    xyz[1] = float(lo + double((word >> 22) & 0x1FFFFFull) * scale);
    xyz[2] = float(lo + double((word >> 1) & 0x1FFFFFull) * scale);
}

// Smallest-three variant, stored wxyz, returned xyzw. See MINT_FORMAT.md 6.6.
void unpackQuaternion(quint32 word, float* xyzw)
{
    static const float kMax[3] = {1023.0f, 1023.0f, 511.0f};
    static const int kShift[3] = {20, 10, 1};
    static const quint32 kMask[3] = {1023u, 1023u, 511u};
    const float kInvRoot2 = 0.70710678118654752f;

    float kept[3];
    for (int j = 0; j < 3; ++j) {
        const quint32 q = (word >> kShift[j]) & kMask[j];
        kept[j] = (2.0f * float(q) / kMax[j] - 1.0f) * kInvRoot2;
    }

    const float sumSquares = kept[0] * kept[0] + kept[1] * kept[1] + kept[2] * kept[2];
    const float omitted = std::sqrt(std::max(0.0f, 1.0f - sumSquares));
    const int largest = int(word >> 30);

    float wxyz[4];
    for (int c = 0, j = 0; c < 4; ++c)
        wxyz[c] = (c == largest) ? omitted : kept[j++];

    const float sign = 1.0f - 2.0f * float(word & 1u);
    xyzw[0] = wxyz[1] * sign;
    xyzw[1] = wxyz[2] * sign;
    xyzw[2] = wxyz[3] * sign;
    xyzw[3] = wxyz[0] * sign;
}

// Splats are sorted by term count and the boundaries are cumulative, so a splat's
// terms are found by arithmetic rather than by a stored offset. See 6.5.
struct RankTable
{
    QVector<quint32> rank;    // 1-based number of terms
    QVector<quint64> offset;  // index of the splat's first term
    quint64 totalTerms = 0;
};

RankTable buildRanks(const char* boundaryBytes, int boundaryCount, quint64 splats)
{
    QVector<quint64> boundaries(boundaryCount);
    for (int j = 0; j < boundaryCount; ++j)
        boundaries[j] = readU32(boundaryBytes + 4 * j);

    QVector<quint64> starts(boundaryCount);
    starts[0] = 0;
    for (int j = 1; j < boundaryCount; ++j)
        starts[j] = boundaries[j - 1];

    QVector<quint64> rankStart(boundaryCount);
    rankStart[0] = 0;
    for (int j = 1; j < boundaryCount; ++j)
        rankStart[j] = rankStart[j - 1] + quint64(j) * (boundaries[j - 1] - starts[j - 1]);

    RankTable table;
    table.rank.resize(int(splats));
    table.offset.resize(int(splats));
    for (quint64 id = 0; id < splats; ++id) {
        int below = 0;
        while (below < boundaryCount && boundaries[below] <= id)
            ++below;
        const int r = below + 1;
        const int j = r - 1;
        table.rank[int(id)] = quint32(r);
        table.offset[int(id)] = rankStart[j] + quint64(r) * (id - starts[j]);
        table.totalTerms = std::max(table.totalTerms, table.offset[int(id)] + quint64(r));
    }
    return table;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

void writeGreenLimitedVariation(char* bytes, const float rgb[3], float strength)
{
    const float excess = rgb[1] - 0.5f * (rgb[0] + rgb[2]);
    const float removed = strength * excess;
    float out[3] = {rgb[0] + 0.7152f * removed,
                    rgb[1] - 0.2848f * removed,
                    rgb[2] + 0.7152f * removed};
    if (strength >= 1.0f) {
        // Independent half rounding would reintroduce a signed green component.
        // Quantize midpoint and R-B on a common representable grid instead, so
        // 2*G == R+B holds exactly in the stored coefficients, including negatives.
        const float magnitude = std::max({std::fabs(out[0]), std::fabs(out[1]), std::fabs(out[2])});
        const int exponent = magnitude > 0.0f ? std::max(-24, std::ilogb(magnitude) - 10) : -24;
        const float step = std::ldexp(1.0f, exponent);
        const float midpoint = std::round((0.5f * out[0] + 0.5f * out[2]) / step) * step;
        const float difference = std::round((0.5f * out[0] - 0.5f * out[2]) / step) * step;
        out[0] = midpoint + difference;
        out[1] = midpoint;
        out[2] = midpoint - difference;
    }
    for (int c = 0; c < 3; ++c) {
        const quint16 le = qToLittleEndian(floatToHalf(out[c]));
        std::memcpy(bytes + c * 2, &le, 2);
    }
}

// How much green a color carries above what its own red and blue predict. This is
// what the despill drives to zero, and what a residual green cast measures as.
float spillMetric(const float c[3], const MintDespillOptions& options)
{
    const float mx = std::max(std::max(c[0], c[1]), c[2]);
    const float mn = std::min(std::min(c[0], c[1]), c[2]);
    const float saturation = (mx > 0.0f) ? ((mx - mn) / mx) : 0.0f;
    const float w = options.gateBySaturation
                        ? std::pow(saturation, float(options.saturationExponent))
                        : 0.0f;
    const float rw = float(options.redWeight), bw = float(options.blueWeight);
    const float estimate =
        (1.0f - w) * (rw * c[0] + bw * c[2]) / (rw + bw) + w * std::max(c[0], c[2]);
    return c[1] - estimate;
}

// The refined adaptive despill of DESPILL.md 2.5. Input and output are sRGB in
// [0,1]; the caller is responsible for the clamp.
void despillColor(const float in[3], const MintDespillOptions& options, float out[3])
{
    float R = in[0], G = in[1], B = in[2];
    const float mx = std::max(std::max(R, G), B);
    const float mn = std::min(std::min(R, G), B);
    const float delta = mx - mn;
    const float saturation = (mx > 0.0f) ? (delta / mx) : 0.0f;

    // Stage one: desaturate inside the green wedge, leaving hue and value alone.
    if (delta > 0.0f) {
        float hue;
        if (R == mx)      hue = (G - B) / delta;
        else if (G == mx) hue = 2.0f + (B - R) / delta;
        else              hue = 4.0f + (R - G) / delta;
        hue *= 60.0f;
        if (hue < 0.0f) hue += 360.0f;

        if (hue >= 60.0f && hue <= 180.0f) {
            const float s = 1.0f - std::fabs(120.0f - hue) / 60.0f;
            const float factor = options.sineProfile
                                     ? std::sin(float(M_PI) * s * 0.5f)
                                     : std::sqrt(std::max(0.0f, s));
            const float sat = saturation * (1.0f - factor);

            const float v = mx;
            const int sector = int(hue / 60.0f) % 6;
            const float f = hue / 60.0f - std::floor(hue / 60.0f);
            const float p = v * (1.0f - sat);
            const float q = v * (1.0f - sat * f);
            const float t = v * (1.0f - sat * (1.0f - f));
            switch (sector) {
            case 0: R = v; G = t; B = p; break;
            case 1: R = q; G = v; B = p; break;
            case 2: R = p; G = v; B = t; break;
            case 3: R = p; G = q; B = v; break;
            case 4: R = t; G = p; B = v; break;
            default: R = v; G = p; B = q; break;
            }
        }
    }

    // Stage two: weighted-average despill with luminance give-back. The gate must
    // be applied to the give-back as well as to the detection, or a tiny detected
    // spill produces a huge correction. See the note in DESPILL.md 2.5.
    const float w = options.gateBySaturation
                        ? std::pow(saturation, float(options.saturationExponent))
                        : 0.0f;
    const float rw = float(options.redWeight), bw = float(options.blueWeight);
    const float both = rw + bw;
    auto estimate = [w, rw, bw, both](float r, float b) {
        return (1.0f - w) * (rw * r + bw * b) / both + w * std::max(r, b);
    };

    // Half the removed green is handed back, each channel taking a share inversely
    // proportional to its weight in the estimator.
    const float spill = G - estimate(R, B);
    if (spill > 0.0f) {
        const float r2 = std::min(1.0f, R + 0.5f * (both - rw) / both * spill);
        const float b2 = std::min(1.0f, B + 0.5f * (both - bw) / both * spill);
        R = r2;
        B = b2;
        G = estimate(r2, b2);
    }

    out[0] = R;
    out[1] = G;
    out[2] = B;
}

QString formatBytes(quint64 bytes)
{
    return QLocale().formattedDataSize(qint64(bytes));
}

} // namespace

// ============================================================ MintChunk

const MintBlock* MintChunk::sharedBlock() const
{
    for (const MintBlock& block : blocks)
        if (block.type == 3)
            return &block;
    return nullptr;
}

QVector<const MintBlock*> MintChunk::splatGroups() const
{
    QVector<const MintBlock*> groups;
    for (const MintBlock& block : blocks)
        if (block.type == 1)
            groups.append(&block);
    return groups;
}

// ============================================================ MintFile

MintFile::MintFile() = default;
MintFile::~MintFile() = default;

void MintFile::close()
{
    m_path.clear();
    m_data.clear();
    m_chunks.clear();
    m_payloadStart = 0;
    m_indexOffset = 0;
    m_fileSize = 0;
    m_headerOnly = false;
    m_duration = 0.0;
    m_modified = false;
}

bool MintFile::open(const QString& path, QString* error)
{
    return load(path, false, error);
}

bool MintFile::openHeader(const QString& path, QString* error)
{
    return load(path, true, error);
}

bool MintFile::load(const QString& path, bool headerOnly, QString* error)
{
    close();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Could not open %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.size() < 80) {
        if (error) *error = QObject::tr("%1 is too small to be a .mint file").arg(path);
        return false;
    }

    m_fileSize = quint64(file.size());
    m_headerOnly = headerOnly;

    if (headerOnly) {
        // The payload start is the third field, so 24 bytes settle how much of
        // the file the metadata occupies.
        const QByteArray prefix = file.read(24);
        if (prefix.size() != 24) {
            if (error) *error = QObject::tr("Could not read the header of %1").arg(path);
            return false;
        }
        quint64 payloadStart;
        std::memcpy(&payloadStart, prefix.constData() + 16, 8);
        payloadStart = qFromLittleEndian(payloadStart);
        if (payloadStart < 80 || payloadStart > m_fileSize || payloadStart > (64u << 20)) {
            if (error) *error = QObject::tr("%1 does not look like a .mint file").arg(path);
            return false;
        }
        file.seek(0);
        m_data = file.read(qint64(payloadStart));
        if (quint64(m_data.size()) != payloadStart) {
            if (error) *error = QObject::tr("Could not read the header of %1").arg(path);
            m_data.clear();
            return false;
        }
    } else {
        m_data = file.readAll();
        if (quint64(m_data.size()) != m_fileSize) {
            if (error) *error = QObject::tr("Could not read all of %1").arg(path);
            m_data.clear();
            return false;
        }
    }
    file.close();

    m_path = path;
    if (!parse(error)) {
        close();
        return false;
    }
    return true;
}

bool MintFile::parse(QString* error)
{
    const quint64 size = m_fileSize;
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };

    if (readU64(raw(0)) != 6)
        return fail(QObject::tr("Not a supported .mint container (format discriminator is not 6)"));

    const quint64 recordCount = readU64(raw(8));
    m_payloadStart = readU64(raw(16));
    if (m_payloadStart >= size)
        return fail(QObject::tr("Payload start lies outside the file"));

    // Records are variable length, so they have to be walked rather than indexed.
    quint64 cursor = 24;
    bool haveIndex = false;
    for (quint64 i = 0; i < recordCount; ++i) {
        if (cursor + 24 > m_payloadStart)
            return fail(QObject::tr("Top-level records run past the payload"));
        const quint64 kind = readU64(raw(cursor));
        if (kind == 1) {
            m_indexOffset = readU64(raw(cursor + 16));
            haveIndex = true;
            cursor += 24;
        } else if (kind == 2) {
            cursor += 32;   // optional payload, empty in every file seen so far
        } else {
            return fail(QObject::tr("Unknown top-level record type %1").arg(kind));
        }
    }
    if (!haveIndex || m_indexOffset + 56 > m_payloadStart)
        return fail(QObject::tr("The .mint file has no usable time index"));

    for (int i = 0; i < 6; ++i)
        m_bbox[i] = readF64(raw(m_indexOffset + 8 + quint64(i) * 8));

    // Offset/size slot pairs inside each block's type-specific header.
    struct Slot { const char* name; int offsetSlot; int sizeSlot; };
    static const Slot kShared[] = {
        {"scale_lut", 8, 16},
        {"sh_static_codebooks", 112, 120},
        {"sh_temporal_codebooks", 128, 136},
        {"sh0_trajectories", 168, 176},
        {"sh0_base_lut", 184, 192},
        {"opacity_trajectories", 208, 216},
        {"rotation_initial", 232, 240},
        {"rotation_delta_lut", 248, 256},
        {"rotation_delta_indices", 264, 272},
        {"position_trajectories", 304, 312},
        {"mesh_extent_lut", 320, 328},
    };
    static const Slot kGroup[] = {
        {"scale_indices", 56, 64},
        {"position_samples", 136, 144},
        {"sh_static_indices", 200, 208},
        {"sh_temporal_indices", 216, 224},
        {"lifetimes", 232, 240},
        {"rotation_samples", 248, 256},
        {"sh0_rq_indices", 264, 272},
        {"sh0_base_indices", 280, 288},
        {"opacity_rq_indices", 296, 304},
        {"rotation_base", 312, 320},
        {"rotation_rq_indices", 328, 336},
        {"rotation_rank_boundaries", 344, 352},
        {"position_base", 360, 368},
        {"position_rq_coefficients", 376, 384},
        {"position_rank_boundaries", 392, 400},
    };

    const quint64 chunkCount = readU64(raw(m_indexOffset));
    double time = 0.0;
    m_chunks.clear();
    m_chunks.reserve(int(chunkCount));

    for (quint64 c = 0; c < chunkCount; ++c) {
        const quint64 entry = m_indexOffset + 56 + c * 24;
        if (entry + 24 > m_payloadStart)
            return fail(QObject::tr("Chunk index entry %1 runs past the payload").arg(c));

        MintChunk chunk;
        chunk.index = int(c);
        chunk.start = time;
        chunk.duration = readF64(raw(entry));
        const quint64 blockCount = readU64(raw(entry + 8));
        quint64 descriptor = readU64(raw(entry + 16));

        for (quint64 b = 0; b < blockCount; ++b) {
            if (descriptor + 32 > m_payloadStart)
                return fail(QObject::tr("Block descriptor runs past the payload"));

            MintBlock block;
            block.descriptor = descriptor;
            block.type = readU64(raw(descriptor));
            block.offset = readU64(raw(descriptor + 8));
            block.size = readU64(raw(descriptor + 16));
            block.headerSize = readU64(raw(descriptor + 24));
            block.absolute = m_payloadStart + block.offset;

            if (descriptor + 32 + block.headerSize > m_payloadStart)
                return fail(QObject::tr("Block header crosses the payload boundary"));
            if (block.absolute + block.size > size)
                return fail(QObject::tr("Block payload runs past the end of the file"));

            const quint64 header = descriptor + 32;
            const Slot* slotTable = nullptr;  // not the obvious name: Qt defines that as a macro
            int slotCount = 0;
            if (block.type == 3) {
                slotTable = kShared;
                slotCount = int(sizeof(kShared) / sizeof(kShared[0]));
            } else if (block.type == 1) {
                slotTable = kGroup;
                slotCount = int(sizeof(kGroup) / sizeof(kGroup[0]));
            }
            for (int s = 0; s < slotCount; ++s) {
                MintArray array;
                array.offset = readU64(raw(header + quint64(slotTable[s].offsetSlot)));
                array.size = readU64(raw(header + quint64(slotTable[s].sizeSlot)));
                if (array.size && array.offset + array.size > block.size)
                    return fail(QObject::tr("Array %1 exceeds its block").arg(slotTable[s].name));
                array.absolute = block.absolute + array.offset;
                block.arrays.insert(QString::fromLatin1(slotTable[s].name), array);
            }

            if (block.type == 1) {
                block.intervals = readU64(raw(header + 8));
                block.splats = readU64(raw(header + 16));
                block.positionMin = readF64(raw(header + 24));
                block.positionMax = readF64(raw(header + 32));
                block.meshSamples = readU64(raw(header + 408));
                block.positionPerSample = !block.array("position_samples").isEmpty();
                block.rotationPerSample = !block.array("rotation_samples").isEmpty();
            } else if (block.type == 3) {
                block.shStaticEntries = readU64(raw(header + 144));
                block.shTemporalEntries = readU64(raw(header + 152));
                block.sh0Entries = readU64(raw(header + 160));
                block.opacityEntries = readU64(raw(header + 200));
                block.rotationEntries = readU64(raw(header + 224));
                block.trajectoryMin = readF64(raw(header + 280));
                block.trajectoryMax = readF64(raw(header + 288));
                block.positionEntries = readU64(raw(header + 296));
            }

            chunk.blocks.append(block);
            descriptor += 32 + block.headerSize;
        }

        // Every group in a chunk shares its interval count, and the shared block
        // is sized from it, so copy it across for convenience.
        if (MintBlock* shared = nullptr; true) {
            quint64 intervals = 0;
            for (const MintBlock& block : chunk.blocks)
                if (block.type == 1) { intervals = block.intervals; break; }
            for (MintBlock& block : chunk.blocks)
                if (block.type == 3) { block.intervals = intervals; shared = &block; }
            if (!shared)
                return fail(QObject::tr("Chunk %1 has no shared dictionary block").arg(c));
        }

        m_chunks.append(chunk);
        time += chunk.duration;
    }

    m_duration = time;
    if (m_chunks.isEmpty() || m_duration <= 0.0)
        return fail(QObject::tr("The .mint file contains no playable chunk"));
    return true;
}

quint64 MintFile::splatCount() const
{
    quint64 total = 0;
    if (!m_chunks.isEmpty())
        for (const MintBlock* group : m_chunks.first().splatGroups())
            total += group->splats;
    return total;
}

double MintFile::frameRate() const
{
    if (!m_chunks.isEmpty()) {
        const MintChunk& first = m_chunks.first();
        if (const MintBlock* shared = first.sharedBlock(); shared && shared->intervals && first.duration > 0.0)
            return double(shared->intervals) / first.duration;
    }
    return 30.0;
}

QString MintFile::frameRateProblem() const
{
    const double rate = frameRate();
    for (int c = 1; c < m_chunks.size(); ++c) {
        const MintChunk& chunk = m_chunks[c];
        const MintBlock* shared = chunk.sharedBlock();
        if (!shared || !shared->intervals || chunk.duration <= 0.0)
            continue;
        const double chunkRate = double(shared->intervals) / chunk.duration;
        if (std::abs(chunkRate - rate) > 1e-6 * rate)
            return QObject::tr("The capture changes frame rate: chunk %1 of %2 runs at %3 Hz "
                               "(%4 frames in %5 s), the ones before at %6 Hz. A .vgs or .pgs "
                               "has to keep one frame rate.")
                .arg(c + 1)
                .arg(m_chunks.size())
                .arg(QString::number(chunkRate, 'g', 5))
                .arg(shared->intervals)
                .arg(QString::number(chunk.duration, 'g', 5))
                .arg(QString::number(rate, 'g', 5));
    }
    return QString();
}

int MintFile::frameCount() const
{
    return std::max(1, int(std::lround(m_duration * frameRate())));
}

double MintFile::frameTime(int frame) const
{
    return double(frame) / frameRate();
}

QString MintFile::summary() const
{
    if (!isOpen())
        return QObject::tr("No file open.");

    return QObject::tr("%1  |  %2 splats  |  %3 chunk(s)  |  %4 s  |  %5 frames at %6 Hz")
        .arg(formatBytes(fileSize()))
        .arg(QLocale().toString(qulonglong(splatCount())))
        .arg(m_chunks.size())
        .arg(QString::number(m_duration, 'f', 3))
        .arg(frameCount())
        .arg(QString::number(frameRate(), 'g', 5));
}

const MintChunk* MintFile::chunkAt(double seconds, double* normalized) const
{
    // The runtime loops, and takes its time argument as float32. Matching that
    // matters only at sample boundaries, but it costs nothing to match it.
    double t = std::fmod(float(seconds), m_duration);
    if (t < 0.0)
        t += m_duration;

    for (const MintChunk& chunk : m_chunks) {
        if (t < chunk.start + chunk.duration) {
            if (normalized)
                *normalized = (t - chunk.start) / chunk.duration;
            return &chunk;
        }
    }
    if (normalized)
        *normalized = 1.0;
    return &m_chunks.last();
}

void MintFile::buildBasis(const MintBlock& shared, quint64 frame, float alpha, bool needPosition,
                          bool needRotation, SharedBasis* out) const
{
    const quint64 samples = shared.samples();
    const quint64 intervals = shared.intervals;

    // SH0: [entries][samples][3] f16, interpolated with alpha.
    {
        const quint64 n = shared.sh0Entries;
        const char* base = raw(shared.array("sh0_trajectories").absolute);
        out->sh0.resize(int(n * 3));
        for (quint64 i = 0; i < n; ++i) {
            const char* row = base + (i * samples) * 6;
            for (int c = 0; c < 3; ++c) {
                const float a = readHalf(row + frame * 6 + quint64(c) * 2);
                const float b = readHalf(row + (frame + 1) * 6 + quint64(c) * 2);
                out->sh0[int(i * 3 + quint64(c))] = a + (b - a) * alpha;
            }
        }
    }

    // Opacity: [entries][samples] f16.
    {
        const quint64 n = shared.opacityEntries;
        const char* base = raw(shared.array("opacity_trajectories").absolute);
        out->opacity.resize(int(n));
        for (quint64 i = 0; i < n; ++i) {
            const char* row = base + (i * samples) * 2;
            const float a = readHalf(row + frame * 2);
            const float b = readHalf(row + (frame + 1) * 2);
            out->opacity[int(i)] = a + (b - a) * alpha;
        }
    }

    if (needPosition) {
        const quint64 n = shared.positionEntries;
        const char* base = raw(shared.array("position_trajectories").absolute);
        out->position.resize(int(n * 3));
        for (quint64 i = 0; i < n; ++i) {
            const char* row = base + (i * samples) * 8;
            float a[3], b[3];
            unpackPosition(readU64(row + frame * 8), shared.trajectoryMin, shared.trajectoryMax, a);
            unpackPosition(readU64(row + (frame + 1) * 8), shared.trajectoryMin, shared.trajectoryMax, b);
            for (int c = 0; c < 3; ++c)
                out->position[int(i * 3 + quint64(c))] = a[c] + (b[c] - a[c]) * alpha;
        }
    }

    if (needRotation) {
        const quint64 n = shared.rotationEntries;
        const char* initial = raw(shared.array("rotation_initial").absolute);
        const char* lut = raw(shared.array("rotation_delta_lut").absolute);
        const quint8* indices =
            reinterpret_cast<const quint8*>(raw(shared.array("rotation_delta_indices").absolute));

        float deltaLut[256];
        for (int i = 0; i < 256; ++i)
            deltaLut[i] = readF32(lut + 4 * i);

        out->rotation.resize(int(n * 4));
        for (quint64 i = 0; i < n; ++i) {
            // The accumulator stays float32; only the stored samples are rounded
            // back to half. Rounding each step instead is visibly different.
            float acc[4];
            for (int c = 0; c < 4; ++c)
                acc[c] = readHalf(initial + (i * 4 + quint64(c)) * 2);

            const quint8* row = indices + i * intervals * 4;
            for (quint64 step = 0; step < frame; ++step)
                for (int c = 0; c < 4; ++c)
                    acc[c] += deltaLut[row[step * 4 + quint64(c)]];

            float q0[4], q1[4];
            for (int c = 0; c < 4; ++c) {
                q0[c] = halfToFloat(floatToHalf(acc[c]));
                q1[c] = halfToFloat(floatToHalf(acc[c] + deltaLut[row[frame * 4 + quint64(c)]]));
            }
            // Stored wxyz, emitted xyzw.
            static const int kOrder[4] = {1, 2, 3, 0};
            for (int c = 0; c < 4; ++c) {
                const int s = kOrder[c];
                const float v = q0[s] + (q1[s] - q0[s]) * alpha;
                out->rotation[int(i * 4 + quint64(c))] = halfToFloat(floatToHalf(v));
            }
        }
    }
}

void MintFile::decodeGroupColor(const MintBlock& group, const MintBlock& shared,
                                const SharedBasis& basis, QVector<float>* colorDc) const
{
    const quint64 n = group.splats;
    const quint64 stride = shared.sh0Entries / 5;   // 1024 rows per stage
    const char* lutBytes = raw(shared.array("sh0_base_lut").absolute);
    const char* baseIndices = raw(group.array("sh0_base_indices").absolute);
    const char* words = raw(group.array("sh0_rq_indices").absolute);

    float lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = readHalf(lutBytes + 2 * i);

    colorDc->resize(int(n * 3));
    const float c0 = float(sphericalHarmonicC0());
    for (quint64 i = 0; i < n; ++i) {
        const quint8* idx = reinterpret_cast<const quint8*>(baseIndices + i * 3);
        float sh0[3] = {lut[idx[0]], lut[idx[1]], lut[idx[2]]};

        const quint64 word = readU64(words + i * 8);
        for (int k = 0; k < 5; ++k) {
            const quint64 stage = (word >> (12 * k)) & 0xFFFull;
            const float* row = basis.sh0.constData() + (quint64(k) * stride + stage) * 3;
            for (int c = 0; c < 3; ++c)
                sh0[c] += row[c];
        }
        for (int c = 0; c < 3; ++c)
            (*colorDc)[int(i * 3 + quint64(c))] = 0.5f + c0 * sh0[c];
    }
}

bool MintFile::decode(double seconds, MintFrame* out, bool includeSh, QString* error) const
{
    if (!isOpen()) {
        if (error) *error = QObject::tr("No .mint file is open");
        return false;
    }
    if (m_headerOnly) {
        if (error) *error = QObject::tr("%1 was opened for its header only").arg(m_path);
        return false;
    }

    double normalized = 0.0;
    const MintChunk* chunk = chunkAt(seconds, &normalized);
    const MintBlock* shared = chunk->sharedBlock();
    const QVector<const MintBlock*> groups = chunk->splatGroups();
    if (!shared || groups.isEmpty()) {
        if (error) *error = QObject::tr("Chunk %1 has no splat data").arg(chunk->index);
        return false;
    }

    const quint64 intervals = groups.first()->intervals;
    const quint64 samples = intervals + 1;
    const float r = float(normalized);
    const float sampleTime = r * float(intervals);
    const quint64 frame = std::min<quint64>(quint64(std::floor(sampleTime)), intervals - 1);
    const float alpha = sampleTime - float(frame);

    bool needPosition = false, needRotation = false;
    for (const MintBlock* group : groups) {
        needPosition |= !group->positionPerSample;
        needRotation |= !group->rotationPerSample;
    }

    SharedBasis basis;
    buildBasis(*shared, frame, alpha, needPosition, needRotation, &basis);

    quint64 total = 0;
    for (const MintBlock* group : groups)
        total += group->splats;

    out->seconds = seconds;
    out->chunkIndex = chunk->index;
    out->sampleIndex = int(frame);
    out->sampleAlpha = alpha;
    out->count = total;
    out->position.resize(int(total * 3));
    out->rotation.resize(int(total * 4));
    out->scale.resize(int(total * 3));
    out->opacity.resize(int(total));
    out->colorDc.resize(int(total * 3));
    out->active.resize(int(total));
    out->shRest.clear();
    if (includeSh)
        out->shRest.resize(int(total * 45));

    float scaleLut[256];
    {
        const char* bytes = raw(shared->array("scale_lut").absolute);
        for (int i = 0; i < 256; ++i)
            scaleLut[i] = readF32(bytes + 4 * i);
    }

    quint64 written = 0;
    for (const MintBlock* group : groups) {
        const quint64 n = group->splats;

        // --- positions -------------------------------------------------------
        if (group->positionPerSample) {
            const char* base = raw(group->array("position_samples").absolute);
            for (quint64 i = 0; i < n; ++i) {
                const char* row = base + (i * samples) * 8;
                float a[3], b[3];
                unpackPosition(readU64(row + frame * 8), group->positionMin, group->positionMax, a);
                unpackPosition(readU64(row + (frame + 1) * 8), group->positionMin, group->positionMax, b);
                // This build interpolates per-sample positions with normalized
                // chunk time, not with the fraction inside the interval. It looks
                // like a defect, but it is what the runtime does. See 6.4.
                for (int c = 0; c < 3; ++c)
                    out->position[int((written + i) * 3 + quint64(c))] = a[c] + (b[c] - a[c]) * r;
            }
        } else {
            const char* baseBytes = raw(group->array("position_base").absolute);
            const RankTable ranks =
                buildRanks(raw(group->array("position_rank_boundaries").absolute), 4, n);
            const char* terms = raw(group->array("position_rq_coefficients").absolute);
            for (quint64 i = 0; i < n; ++i) {
                float p[3];
                unpackPosition(readU64(baseBytes + i * 8), group->positionMin, group->positionMax, p);
                const quint32 rank = ranks.rank[int(i)];
                const quint64 first = ranks.offset[int(i)];
                for (quint32 k = 0; k < rank; ++k) {
                    const quint32 packed = readU32(terms + (first + k) * 4);
                    const quint32 index = packed & 0xFFFFu;
                    // The high half is the weight's raw binary16 bits, not a
                    // normalised integer: real weights run well outside [-1,1].
                    const float weight = halfToFloat(quint16(packed >> 16));
                    const float* row = basis.position.constData() + quint64(index) * 3;
                    for (int c = 0; c < 3; ++c)
                        p[c] += weight * row[c];
                }
                for (int c = 0; c < 3; ++c)
                    out->position[int((written + i) * 3 + quint64(c))] = p[c];
            }
        }

        // --- rotations -------------------------------------------------------
        if (group->rotationPerSample) {
            const char* base = raw(group->array("rotation_samples").absolute);
            for (quint64 i = 0; i < n; ++i) {
                const char* row = base + (i * samples) * 4;
                float q0[4], q1[4];
                unpackQuaternion(readU32(row + frame * 4), q0);
                unpackQuaternion(readU32(row + (frame + 1) * 4), q1);
                float dot = 0.0f;
                for (int c = 0; c < 4; ++c) dot += q0[c] * q1[c];
                const float sign = (dot < 0.0f) ? -1.0f : 1.0f;
                float q[4], norm = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    q[c] = q0[c] + (q1[c] * sign - q0[c]) * alpha;
                    norm += q[c] * q[c];
                }
                norm = std::max(std::sqrt(norm), 1e-20f);
                for (int c = 0; c < 4; ++c)
                    out->rotation[int((written + i) * 4 + quint64(c))] = q[c] / norm;
            }
        } else {
            const char* baseBytes = raw(group->array("rotation_base").absolute);
            const RankTable ranks =
                buildRanks(raw(group->array("rotation_rank_boundaries").absolute), 5, n);
            const char* terms = raw(group->array("rotation_rq_indices").absolute);
            for (quint64 i = 0; i < n; ++i) {
                float base[4];
                unpackQuaternion(readU32(baseBytes + i * 4), base);

                float residual[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                const quint32 rank = ranks.rank[int(i)];
                const quint64 first = ranks.offset[int(i)];
                for (quint32 k = 0; k < rank; ++k) {
                    const quint16 index = readU16(terms + (first + k) * 2);
                    const float* row = basis.rotation.constData() + quint64(index) * 4;
                    for (int c = 0; c < 4; ++c)
                        residual[c] += row[c];
                }

                float normSq = 0.0f;
                for (int c = 0; c < 4; ++c) normSq += residual[c] * residual[c];

                float q[4];
                if (normSq < 1e-12f) {
                    for (int c = 0; c < 4; ++c) q[c] = base[c];
                } else {
                    const float inv = 1.0f / std::sqrt(normSq);
                    for (int c = 0; c < 4; ++c) residual[c] *= inv;
                    // normalize(residual * base), quaternions in xyzw.
                    const float* v = residual;
                    const float w = residual[3];
                    const float* bv = base;
                    const float bw = base[3];
                    q[0] = w * bv[0] + bw * v[0] + (v[1] * bv[2] - v[2] * bv[1]);
                    q[1] = w * bv[1] + bw * v[1] + (v[2] * bv[0] - v[0] * bv[2]);
                    q[2] = w * bv[2] + bw * v[2] + (v[0] * bv[1] - v[1] * bv[0]);
                    q[3] = w * bw - (v[0] * bv[0] + v[1] * bv[1] + v[2] * bv[2]);
                }
                float norm = 0.0f;
                for (int c = 0; c < 4; ++c) norm += q[c] * q[c];
                norm = std::max(std::sqrt(norm), 1e-20f);
                for (int c = 0; c < 4; ++c)
                    out->rotation[int((written + i) * 4 + quint64(c))] = q[c] / norm;
            }
        }

        // --- scale, opacity, lifetime, color ---------------------------------
        {
            const quint8* scaleIdx =
                reinterpret_cast<const quint8*>(raw(group->array("scale_indices").absolute));
            const char* opacityWords = raw(group->array("opacity_rq_indices").absolute);
            const quint8* lifetimes =
                reinterpret_cast<const quint8*>(raw(group->array("lifetimes").absolute));
            const quint64 opacityStride = shared->opacityEntries / 5;

            for (quint64 i = 0; i < n; ++i) {
                for (int c = 0; c < 3; ++c)
                    out->scale[int((written + i) * 3 + quint64(c))] = scaleLut[scaleIdx[i * 3 + quint64(c)]];

                const quint64 word = readU64(opacityWords + i * 8);
                float opacity = 0.0f;
                for (int k = 0; k < 5; ++k) {
                    const quint64 stage = (word >> (12 * k)) & 0xFFFull;
                    opacity += basis.opacity[int(quint64(k) * opacityStride + stage)];
                }
                out->opacity[int(written + i)] = clamp01(opacity);

                const quint8 begin = lifetimes[i * 2];
                const quint8 end = lifetimes[i * 2 + 1];
                out->active[int(written + i)] =
                    (frame >= begin && frame + 1 <= end) ? quint8(1) : quint8(0);
            }
        }

        {
            QVector<float> color;
            decodeGroupColor(*group, *shared, basis, &color);
            std::memcpy(out->colorDc.data() + written * 3, color.constData(), size_t(n * 3) * sizeof(float));
        }

        // --- higher-order spherical harmonics --------------------------------
        if (includeSh) {
            const char* staticBytes = raw(shared->array("sh_static_codebooks").absolute);
            const char* temporalBytes = raw(shared->array("sh_temporal_codebooks").absolute);
            const char* staticIdx = raw(group->array("sh_static_indices").absolute);
            const char* temporalIdx = raw(group->array("sh_temporal_indices").absolute);
            const quint64 ns = shared->shStaticEntries;
            const quint64 nt = shared->shTemporalEntries;
            // The temporal codebook is read at `frame` with no interpolation to the
            // next sample, unlike every other temporal table. See 6.8.
            const char* temporalFrame = temporalBytes + frame * 5 * 3 * nt * 3 * 2;
            static const int kShift[3] = {20, 10, 0};

            for (quint64 g = 0; g < 5; ++g) {
                // The index arrays are plane-major: all N words of group 0, then
                // all N of group 1, and so on.
                const char* sPlane = staticIdx + g * n * 4;
                const char* tPlane = temporalIdx + g * n * 4;
                for (quint64 i = 0; i < n; ++i) {
                    const quint32 sWord = readU32(sPlane + i * 4);
                    const quint32 tWord = readU32(tPlane + i * 4);
                    for (int k = 0; k < 3; ++k) {
                        const quint32 si = (sWord >> kShift[k]) & 0x3FFu;
                        const quint32 ti = (tWord >> kShift[k]) & 0x3FFu;
                        const char* sEntry = staticBytes + (((g * 3 + quint64(k)) * ns) + si) * 3 * 2;
                        const char* tEntry = temporalFrame + (((g * 3 + quint64(k)) * nt) + ti) * 3 * 2;
                        const int coefficient = int(g) * 3 + k;
                        for (int c = 0; c < 3; ++c) {
                            out->shRest[int((written + i) * 45 + quint64(coefficient) * 3 + quint64(c))] =
                                readHalf(sEntry + c * 2) + readHalf(tEntry + c * 2);
                        }
                    }
                }
            }
        }

        written += n;
    }

    return true;
}


bool MintFile::despill(const MintDespillOptions& options, const MintProgressFn& progress, QString* error)
{
    if (!isOpen()) {
        if (error) *error = QObject::tr("No .mint file is open");
        return false;
    }
    if (m_headerOnly) {
        if (error) *error = QObject::tr("%1 was opened for its header only").arg(m_path);
        return false;
    }

    const float c0 = float(sphericalHarmonicC0());
    const int totalSteps = m_chunks.size();
    const float strength = clamp01(float(options.strength));

    for (int ci = 0; ci < m_chunks.size(); ++ci) {
        const MintChunk& chunk = m_chunks[ci];
        const MintBlock* shared = chunk.sharedBlock();
        const QVector<const MintBlock*> groups = chunk.splatGroups();
        if (!shared || groups.isEmpty())
            continue;

        MintFrame originalSkinFrame;
        const double skinTime = chunk.start + chunk.duration * .5;
        if (options.recoverSkinColour && strength > 0.0f
            && !decode(skinTime, &originalSkinFrame, true, error)) return false;

        if (progress && !progress(100 * ci / std::max(1, totalSteps),
                                  QObject::tr("Despilling chunk %1 of %2").arg(ci + 1).arg(totalSteps))) {
            if (error) *error = QObject::tr("Cancelled");
            return false;
        }

        // Estimate the green gain from low-chroma surfaces: white trim, grey, cloth
        // in shadow. Whatever colour they come out is the illuminant's, so the gain
        // that makes their green match the mean of their red and blue removes it.
        // Very bright samples are skipped: near the top of the range the channels
        // compress and the ratio stops being meaningful.
        float greenGain = float(options.greenGain);
        if (options.autoGreenGain) {
            double sumGreen = 0.0, sumRedBlue = 0.0;
            const quint64 chunkIntervals = groups.first()->intervals;
            const float mid = float(chunkIntervals) * 0.5f;
            const quint64 frame = std::min<quint64>(quint64(std::floor(mid)), chunkIntervals - 1);
            SharedBasis basis;
            buildBasis(*shared, frame, mid - float(frame), false, false, &basis);
            for (const MintBlock* group : groups) {
                QVector<float> color;
                decodeGroupColor(*group, *shared, basis, &color);
                const quint64 n = group->splats;
                for (quint64 i = 0; i < n; ++i) {
                    const float r = clamp01(color[int(i * 3)]);
                    const float g = clamp01(color[int(i * 3 + 1)]);
                    const float b = clamp01(color[int(i * 3 + 2)]);
                    const float mx = std::max(std::max(r, g), b);
                    const float mn = std::min(std::min(r, g), b);
                    if (mx < 0.15f || mx > 0.80f)
                        continue;
                    if (mx <= 0.0f || (mx - mn) / mx > 0.20f)
                        continue;
                    sumGreen += g;
                    sumRedBlue += 0.5 * (double(r) + double(b));
                }
            }
            // Only ever darken green, and never by more than a tenth: anything
            // larger is not an illuminant, it is a broken capture.
            greenGain = (sumGreen > 0.0)
                            ? float(std::min(1.0, std::max(0.90, sumRedBlue / sumGreen)))
                            : 1.0f;
        }

        // Each chunk carries its own base LUT, so the nearest-value search has to
        // be rebuilt per chunk. Sorting once turns the per-splat lookup into a
        // binary search over 256 values.
        float lut[256];
        {
            const char* bytes = raw(shared->array("sh0_base_lut").absolute);
            for (int i = 0; i < 256; ++i)
                lut[i] = readHalf(bytes + 2 * i);
        }
        QVector<QPair<float, int>> sortedLut(256);
        for (int i = 0; i < 256; ++i)
            sortedLut[i] = qMakePair(lut[i], i);
        std::sort(sortedLut.begin(), sortedLut.end(),
                  [](const QPair<float, int>& a, const QPair<float, int>& b) { return a.first < b.first; });

        auto nearestIndex = [&sortedLut](float value) {
            auto it = std::lower_bound(sortedLut.constBegin(), sortedLut.constEnd(), value,
                                       [](const QPair<float, int>& e, float v) { return e.first < v; });
            if (it == sortedLut.constBegin())
                return it->second;
            if (it == sortedLut.constEnd())
                return (it - 1)->second;
            const auto prev = it - 1;
            return (value - prev->first <= it->first - value) ? prev->second : it->second;
        };
        auto floorIndex = [&sortedLut](float value) {
            auto it = std::upper_bound(sortedLut.constBegin(), sortedLut.constEnd(), value,
                                      [](float v, const QPair<float, int>& e) { return v < e.first; });
            return it == sortedLut.constBegin() ? it->second : (it - 1)->second;
        };

        // Visit every interval, including short-lived splats that nine global
        // samples can miss. Weight the fit by opacity while the splat is alive.
        const quint64 intervals = groups.first()->intervals;
        const int samplesPerInterval = std::max(1, int(std::ceil(
            double(std::max(1, options.samplesPerChunk)) / double(intervals))));
        const int sampleCount = int(intervals) * samplesPerInterval;
        // Two corrections, kept apart on purpose.
        //
        // The green gain models the illuminant. A capture shot inside a green dome is
        // lit by green light, and a coloured light multiplies everything; it is not
        // the additive local excess the despill operator is built for. It is applied
        // at full value and is NOT scaled by strength: how strong the local despill
        // should be says nothing about how green the room was.
        //
        // The despill then deals with what is left, which is genuinely local, and is
        // what strength scales.
        //
        // Fit the desired visible mean first. Then limit the green component of
        // temporal variation and compensate the base for that dictionary edit.
        QVector<QVector<float>> despillDelta(groups.size());
        QVector<QVector<float>> meanGreen(groups.size());
        QVector<QVector<float>> visibleWeight(groups.size());
        QVector<QVector<float>> meanBefore(groups.size()), meanAfter(groups.size());
        for (int g = 0; g < groups.size(); ++g) {
            despillDelta[g].fill(0.0f, int(groups[g]->splats * 3));
            meanGreen[g].fill(0.0f, int(groups[g]->splats));
            visibleWeight[g].fill(0.0f, int(groups[g]->splats));
            meanBefore[g].fill(0.0f, int(groups[g]->splats * 3));
            meanAfter[g].fill(0.0f, int(groups[g]->splats * 3));
        }

        for (int s = 0; s < sampleCount; ++s) {
            if (progress && !progress(100 * ci / std::max(1, totalSteps),
                                     QObject::tr("Despilling chunk %1 of %2").arg(ci + 1).arg(totalSteps))) {
                if (error) *error = QObject::tr("Cancelled");
                return false;
            }
            const float sampleTime = (float(s) + 0.5f) / float(samplesPerInterval);
            const quint64 frame = std::min<quint64>(quint64(std::floor(sampleTime)), intervals - 1);
            const float alpha = sampleTime - float(frame);

            SharedBasis basis;
            buildBasis(*shared, frame, alpha, false, false, &basis);

            for (int g = 0; g < groups.size(); ++g) {
                QVector<float> color;
                decodeGroupColor(*groups[g], *shared, basis, &color);
                const quint64 n = groups[g]->splats;
                float* delta = despillDelta[g].data();
                float* green = meanGreen[g].data();
                float* weights = visibleWeight[g].data();
                const auto* lifetimes = reinterpret_cast<const quint8*>(
                    raw(groups[g]->array("lifetimes").absolute));
                const char* opacityWords = raw(groups[g]->array("opacity_rq_indices").absolute);
                const quint64 opacityStride = shared->opacityEntries / 5;
                for (quint64 i = 0; i < n; ++i) {
                    if (frame < lifetimes[i * 2] || frame + 1 > lifetimes[i * 2 + 1])
                        continue;
                    const quint64 word = readU64(opacityWords + i * 8);
                    float opacity = 0.0f;
                    for (int k = 0; k < 5; ++k)
                        opacity += basis.opacity[int(quint64(k) * opacityStride
                                                     + ((word >> (12 * k)) & 0xFFFull))];
                    const float weight = clamp01(opacity);
                    if (weight == 0.0f)
                        continue;
                    weights[i] += weight;
                    // The operator is defined on sRGB in [0,1] and the HSV stage is
                    // meaningless outside it, but a little over 2% of splats fall
                    // outside. Derive the delta from the clamped color and apply it
                    // to the real one, which leaves their headroom alone.
                    const float raw[3] = {clamp01(color[int(i * 3)]),
                                          clamp01(color[int(i * 3 + 1)]),
                                          clamp01(color[int(i * 3 + 2)])};
                    green[i] += weight * raw[1];

                    // The illuminant comes off first, so the operator judges a colour
                    // that is no longer globally green.
                    const float in[3] = {raw[0], clamp01(raw[1] * greenGain), raw[2]};
                    float outColor[3];
                    despillColor(in, options, outColor);
                    for (int c = 0; c < 3; ++c) {
                        delta[i * 3 + quint64(c)] += weight * (outColor[c] - in[c]);
                        meanBefore[g][int(i * 3 + quint64(c))] += weight * color[int(i * 3 + quint64(c))];
                    }
                }
            }
        }

        if (strength > 0.0f) {
            // At full strength every temporal entry has zero green opponent.
            // Linear interpolation and sums then preserve that constraint at all
            // times, not only at the fit's sample times. Keep layout/padding intact.
            char* trajectory = mutableRaw(shared->array("sh0_trajectories").absolute);
            const quint64 entries = shared->sh0Entries * shared->samples();
            for (quint64 i = 0; i < entries; ++i) {
                float rgb[3];
                for (int c = 0; c < 3; ++c)
                    rgb[c] = readHalf(trajectory + i * 6 + c * 2);
                writeGreenLimitedVariation(trajectory + i * 6, rgb, strength);
            }

            // Measure the actually quantized result, not the ideal transform.
            for (int s = 0; s < sampleCount; ++s) {
                if (progress && !progress(100 * ci / std::max(1, totalSteps),
                                         QObject::tr("Limiting green in chunk %1 of %2").arg(ci + 1).arg(totalSteps))) {
                    if (error) *error = QObject::tr("Cancelled");
                    return false;
                }
                const float sampleTime = (float(s) + 0.5f) / float(samplesPerInterval);
                const quint64 frame = std::min<quint64>(quint64(std::floor(sampleTime)), intervals - 1);
                SharedBasis basis;
                buildBasis(*shared, frame, sampleTime - float(frame), false, false, &basis);
                for (int g = 0; g < groups.size(); ++g) {
                    QVector<float> color;
                    decodeGroupColor(*groups[g], *shared, basis, &color);
                    const auto* lifetimes = reinterpret_cast<const quint8*>(raw(groups[g]->array("lifetimes").absolute));
                    const char* opacityWords = raw(groups[g]->array("opacity_rq_indices").absolute);
                    for (quint64 i = 0; i < groups[g]->splats; ++i) {
                        if (frame < lifetimes[i * 2] || frame + 1 > lifetimes[i * 2 + 1])
                            continue;
                        const quint64 word = readU64(opacityWords + i * 8);
                        float opacity = 0.0f;
                        for (int k = 0; k < 5; ++k)
                            opacity += basis.opacity[int(quint64(k) * (shared->opacityEntries / 5)
                                                         + ((word >> (12 * k)) & 0xFFFull))];
                        const float weight = clamp01(opacity);
                        for (int c = 0; c < 3; ++c)
                            meanAfter[g][int(i * 3 + quint64(c))] += weight * color[int(i * 3 + quint64(c))];
                    }
                }
            }
        }

        for (int g = 0; g < groups.size(); ++g) {
            const quint64 n = groups[g]->splats;
            float* delta = despillDelta[g].data();
            float* green = meanGreen[g].data();
            for (quint64 i = 0; i < n; ++i) {
                const float weight = visibleWeight[g][int(i)];
                if (weight == 0.0f)
                    continue;
                green[i] /= weight;
                for (int c = 0; c < 3; ++c) {
                    delta[i * 3 + quint64(c)] /= weight;
                    meanBefore[g][int(i * 3 + quint64(c))] /= weight;
                    meanAfter[g][int(i * 3 + quint64(c))] /= weight;
                }
            }
        }

        // Fold the delta into the base indices. colorDc is an affine
        // function of the SH0 sum, so a color delta is a base delta divided by the
        // band-0 constant; the index is then whichever LUT entry lands nearest.
        for (int g = 0; g < groups.size(); ++g) {
            const MintBlock* group = groups[g];
            const quint64 n = group->splats;
            const float* acc = despillDelta[g].constData();
            const float* green = meanGreen[g].constData();
            char* indices = mutableRaw(group->array("sh0_base_indices").absolute);
            quint8* idx = reinterpret_cast<quint8*>(indices);

            for (quint64 i = 0; i < n; ++i) {
                for (int c = 0; c < 3 && visibleWeight[g][int(i)] > 0.0f; ++c) {
                    // A multiplicative gain cannot be stored exactly, because the base
                    // is added to time-varying stages; the splat's mean green over the
                    // chunk is the closest a single constant gets.
                    const float illuminant =
                        (c == 1) ? (greenGain - 1.0f) * green[i] : 0.0f;
                    const int index = int(i * 3 + quint64(c));
                    const float compensation = strength > 0.0f ? meanBefore[g][index] - meanAfter[g][index] : 0.0f;
                    const float delta = illuminant + strength * acc[index] + compensation;
                    if (delta == 0.0f)
                        continue;
                    const float current = lut[idx[i * 3 + quint64(c)]];
                    const float target = current + delta / c0;
                    idx[i * 3 + quint64(c)] = quint8(nearestIndex(target));
                }
                if (strength >= 1.0f) {
                    // Temporal and angular variations now have G=(R+B)/2. Enforce
                    // the same upper bound in the base LUT AFTER R/B quantization.
                    // Floor, never nearest: rounding upward can recreate green.
                    const float cap = 0.5f * (lut[idx[i * 3]] + lut[idx[i * 3 + 2]]);
                    idx[i * 3 + 1] = quint8(floorIndex(std::min(lut[idx[i * 3 + 1]], cap)));
                }
            }
        }

        // A splat's higher-order colour is the sum of a static and a temporal
        // codebook entry, and the render combines those linearly with the view
        // direction, so scaling the codebooks scales the view dependence exactly.
        // The arrays are fixed size, so nothing about the file's layout moves.
        if (strength > 0.0f || options.viewDependentScale != 1.0 || options.viewChromaScale != 1.0) {
            const float scale = float(options.viewDependentScale);
            const float chroma = float(options.viewChromaScale);
            const char* names[2] = {"sh_static_codebooks", "sh_temporal_codebooks"};
            for (int table = 0; table < 2; ++table) {
                const char* name = names[table];
                const MintArray array = shared->array(QString::fromLatin1(name));
                if (array.isEmpty())
                    continue;
                // A linear RGB transform commutes with SH evaluation and with the
                // sum of static/temporal entries. Reduce angular colour changes
                // while preserving their luminance, rather than flattening shading.
                // Use logical RGB entries, leaving the array's padding untouched.
                const quint64 entries = 5 * 3 * (table == 0
                    ? shared->shStaticEntries : shared->shTemporalEntries * shared->samples());
                char* bytes = mutableRaw(array.absolute);
                for (quint64 v = 0; v < entries; ++v) {
                    float rgb[3];
                    for (int c = 0; c < 3; ++c)
                        rgb[c] = readHalf(bytes + (v * 3 + quint64(c)) * 2);
                    const float grey = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
                    for (int c = 0; c < 3; ++c) {
                        const float value = chroma == 1.0f ? rgb[c] : grey + chroma * (rgb[c] - grey);
                        rgb[c] = value * scale;
                    }
                    writeGreenLimitedVariation(bytes + v * 6, rgb, strength);
                }
            }
        }

        if (options.recoverSkinColour && strength > 0.0f) {
            MintFrame corrected;
            if (!decode(skinTime, &corrected, false, error)) return false;
            QVector<float> delta;
            const auto reportSkin = [&](int, const QString& stage) {
                return !progress || progress(100*ci/std::max(1,totalSteps), stage);
            };
            if (!mintSkinRecovery(originalSkinFrame, corrected, &delta, reportSkin)) {
                if (error) *error = QObject::tr("Cancelled");
                return false;
            }
            int offset = 0;
            for (const MintBlock* group : groups) {
                auto* indices = reinterpret_cast<quint8*>(mutableRaw(group->array("sh0_base_indices").absolute));
                for (quint64 i = 0; i < group->splats; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        const float shift = strength*delta[(offset+int(i))*3+c];
                        if (shift != 0.0f) indices[i*3+c] = quint8(nearestIndex(lut[indices[i*3+c]]+shift/c0));
                    }
                    if (strength >= 1.0f) {
                        const float cap = .5f*(lut[indices[i*3]]+lut[indices[i*3+2]]);
                        indices[i*3+1] = quint8(floorIndex(std::min(lut[indices[i*3+1]],cap)));
                    }
                }
                offset += int(group->splats);
            }
        }
        m_modified = true;
    }

    if (progress)
        progress(100, QObject::tr("Despill complete"));
    return true;
}


// Bridge the same colour operator to decoded VGS arrays. This is an in-memory
// attribute view, not a synthesized MINT file; its layout never reaches a writer.
bool MintFile::despillLogical(vgs::DecodedChunk *logical,double secondsPerTick,
                             const MintDespillOptions &options,const MintProgressFn &progress,QString *error) {
    MintFile view;view.m_path="__logical_chunk__";MintChunk chunk;
    const auto intervals=logical->groups.front().intervals;
    chunk.duration=intervals*secondsPerTick;view.m_duration=chunk.duration;
    for (const auto &group : logical->groups) {
        MintBlock block;block.type=group.type ? 1 : 3;block.intervals=group.intervals;block.splats=group.splats;
        block.positionPerSample=(group.flags&1)!=0;block.rotationPerSample=(group.flags&2)!=0;
        block.positionMin=group.positionMin;block.positionMax=group.positionMax;
        block.trajectoryMin=group.trajectoryMin;block.trajectoryMax=group.trajectoryMax;
        block.shStaticEntries=group.counts[0];block.shTemporalEntries=group.counts[1];
        block.sh0Entries=group.counts[2];block.opacityEntries=group.counts[3];block.rotationEntries=group.counts[4];block.positionEntries=group.counts[5];
        chunk.blocks.append(block);
    }
    int planes=0;
    for (const auto &page : logical->pages) if (page.descriptor.attribute==vgs::ShStaticIndices) {planes=int(page.descriptor.spec.width);break;}
    if (!planes) {chunk.blocks[0].shStaticEntries=chunk.blocks[0].shTemporalEntries=1;}
    auto put=[&](uint32_t group,const QString &name,const QByteArray &bytes) {
        MintArray array;array.absolute=quint64(view.m_data.size());array.size=quint64(bytes.size());
        chunk.blocks[int(group)].arrays[name]=array;view.m_data.append(bytes);
    };
    for (const auto &page : logical->pages) {
        auto bytes=QByteArray(reinterpret_cast<const char *>(page.bytes.data()),qsizetype(page.bytes.size()));
        const auto attribute=page.descriptor.attribute,group=page.descriptor.group;
        if (attribute==vgs::ShStaticBook) bytes.append(QByteArray(15*chunk.blocks[0].shStaticEntries*6-bytes.size(),0));
        if (attribute==vgs::ShTemporalBook && planes<5) {
            const qsizetype sourceBlock=planes*3*chunk.blocks[0].shTemporalEntries*6,targetBlock=15*chunk.blocks[0].shTemporalEntries*6;
            QByteArray expanded((intervals+1)*targetBlock,0);
            for (quint64 sample=0;sample<=intervals;++sample) std::memcpy(expanded.data()+sample*targetBlock,bytes.constData()+sample*sourceBlock,size_t(sourceBlock));
            bytes=std::move(expanded);
        }
        if (attribute==vgs::ShStaticIndices || attribute==vgs::ShTemporalIndices) bytes.append(QByteArray(5*chunk.blocks[int(group)].splats*4-bytes.size(),0));
        put(group,QString::fromLatin1(vgs::attributeName(attribute)),bytes);
    }
    if (!planes) {
        put(0,"sh_static_codebooks",QByteArray(15*6,0));put(0,"sh_temporal_codebooks",QByteArray((intervals+1)*15*6,0));
        for (int group=1;group<chunk.blocks.size();++group) {
            put(group,"sh_static_indices",QByteArray(chunk.blocks[group].splats*5*4,0));put(group,"sh_temporal_indices",QByteArray(chunk.blocks[group].splats*5*4,0));
        }
    }
    view.m_chunks.append(chunk);
    if (!view.despill(options,progress,error)) return false;
    for (auto &page : logical->pages) {
        const char *bytes=view.raw(view.m_chunks[0].blocks[int(page.descriptor.group)].array(QString::fromLatin1(vgs::attributeName(page.descriptor.attribute))).absolute);
        if (page.descriptor.attribute==vgs::ShTemporalBook && planes<5) {
            const size_t kept=planes*3*chunk.blocks[0].shTemporalEntries*6,full=15*chunk.blocks[0].shTemporalEntries*6;
            for (quint64 sample=0;sample<=intervals;++sample) std::memcpy(page.bytes.data()+sample*kept,bytes+sample*full,kept);
        } else std::memcpy(page.bytes.data(),bytes,page.bytes.size());
    }
    return true;
}
