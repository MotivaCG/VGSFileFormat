#ifndef MINTFILE_H
#define MINTFILE_H

#include <QByteArray>
#include <QHash>
#include <QString>

#include <QVector>
#include "vgscodec.h"

#include <functional>
#include <string>
#include <vector>

// Reader, editor and writer for Gracia .mint volumetric captures.
//
// The container and the attribute reconstruction are specified in MINT_FORMAT.md,
// which this class implements; the despill operator is specified in DESPILL.md.
// Both live next to this file. Where a comment below says "see 6.4" it means a
// section of MINT_FORMAT.md.
//
// Only the observed format-6 container is supported, and the input is assumed to
// be a well-formed file from Gracia: open() validates structure enough to fail
// cleanly on something that is not a .mint, but it is not a hardened parser for
// untrusted input.

// One offset/size pair out of a block header. The absolute address is resolved at
// parse time so callers never have to track which of the format's three offset
// bases a given field used.
struct MintArray
{
    quint64 offset = 0;    // relative to its own block's payload
    quint64 size = 0;      // bytes reserved, padding included
    quint64 absolute = 0;  // resolved offset into the file

    bool isEmpty() const { return size == 0; }
};

struct MintBlock
{
    quint64 type = 0;
    quint64 descriptor = 0;
    quint64 offset = 0;
    quint64 absolute = 0;
    quint64 size = 0;
    quint64 headerSize = 0;
    QHash<QString, MintArray> arrays;

    // Type 1 only: one group of splats sharing an encoding mode combination.
    quint64 intervals = 0;
    quint64 splats = 0;
    quint64 meshSamples = 0;
    double positionMin = 0.0;
    double positionMax = 0.0;
    bool positionPerSample = false;
    bool rotationPerSample = false;

    // Type 3 only: the shared dictionaries for one chunk.
    quint64 shStaticEntries = 0;
    quint64 shTemporalEntries = 0;
    quint64 sh0Entries = 0;
    quint64 opacityEntries = 0;
    quint64 rotationEntries = 0;
    quint64 positionEntries = 0;
    double trajectoryMin = 0.0;
    double trajectoryMax = 0.0;

    // A trajectory table has one more sample than it has intervals: the last
    // interval needs the sample that closes it. See 3.7.
    quint64 samples() const { return intervals + 1; }

    MintArray array(const QString& name) const { return arrays.value(name); }
};

struct MintChunk
{
    int index = 0;
    double start = 0.0;
    double duration = 0.0;
    QVector<MintBlock> blocks;

    const MintBlock* sharedBlock() const;
    QVector<const MintBlock*> splatGroups() const;
};

// Every splat's attributes at one instant, in the asset's local space, before any
// scene transform and before the view-dependent SH evaluation. Rows concatenate
// the groups in descriptor order.
struct MintFrame
{
    double seconds = 0.0;
    int chunkIndex = 0;
    int sampleIndex = 0;
    float sampleAlpha = 0.0f;
    quint64 count = 0;

    QVector<float> position;   // count * 3
    QVector<float> rotation;   // count * 4, xyzw
    QVector<float> scale;      // count * 3, directly usable, no exp()
    QVector<float> opacity;    // count, already in [0,1], no sigmoid
    QVector<float> colorDc;    // count * 3
    QVector<float> shRest;     // count * 45 as [splat][coefficient][channel], or empty
    QVector<quint8> active;    // count, 1 while the splat's lifetime covers this interval
};

using MintProgressFn = std::function<bool(int, const QString &)>;

struct MintDespillOptions
{
    double redWeight = 2.0;
    double blueWeight = 1.0;
    bool gateBySaturation = false;  // enable to protect saturated yellow/cyan
    double saturationExponent = 3.0;
    bool sineProfile = true;
    // Also suppresses green/magenta temporal and angular variation. At 1, the
    // stored base and variations enforce G <= (R+B)/2 before channel clamping
    // (within float arithmetic error), independently of greenGain/viewChromaScale.
    double strength = 1.0;

    // Minimum sample budget. Every interval is sampled at least once; only live
    // samples contribute, weighted by opacity. More samples refine interpolation.
    int samplesPerChunk = 9;

    // Explicit illuminant correction, applied before the local operator. A
    // constant base offset approximates this gain over the visible lifetime.
    // Auto estimation assumes neutral surfaces and is disabled by default.
    bool autoGreenGain = false;
    double greenGain = 0.97;

    // Scale all higher-order SH channels together (0 removes angular shading).
    double viewDependentScale = 1.0;

    // Retain this fraction of angular chroma, preserving weighted RGB luminance.
    // 1 retains the angular colour allowed by strength; 0 makes it neutral.
    // This also reduces genuine coloured reflections. Base colour is unaffected.
    double viewChromaScale = 0.5;

    // Optional material-specific recovery from nearby clean skin. Assumes metre
    // coordinates; disable for non-human captures or genuine skin-coloured props.
    bool recoverSkinColour = true;
};


class MintFile
{
public:
    MintFile();
    ~MintFile();

    MintFile(const MintFile&) = delete;
    MintFile& operator=(const MintFile&) = delete;

    // The whole file is held in memory. The largest capture seen so far is 447 MB,
    // which is the price of being able to edit and write back without a second
    // parse; if that ever stops being acceptable, the edits are confined to a few
    // byte ranges and could be streamed instead.
    bool open(const QString& path, QString* error);

    // Reads only the metadata region, a few kilobytes, and stops. Enough for
    // summary(), chunks() and frameCount(); decoding, editing and saving all
    // refuse until the file is opened properly. This is what a file dialog
    // should call: a full open of a 400 MB capture would stall the GUI thread
    // for seconds just to fill in a label.
    bool openHeader(const QString& path, QString* error);

    void close();
    bool isOpen() const { return !m_path.isEmpty(); }
    bool isHeaderOnly() const { return m_headerOnly; }

    QString path() const { return m_path; }
    quint64 fileSize() const { return m_fileSize; }
    quint64 payloadStart() const { return m_payloadStart; }
    double duration() const { return m_duration; }
    const QVector<MintChunk>& chunks() const { return m_chunks; }
    const double* boundingBox() const { return m_bbox; }  // minXYZ then maxXYZ
    quint64 splatCount() const;
    bool isModified() const { return m_modified; }

    // A one-paragraph description for a dialog: size, splats, chunks, duration.
    QString summary() const;

    // Reconstructs every splat at this instant. Times outside the animation wrap,
    // matching the runtime. includeSh fills shRest, which costs noticeably more.
    bool decode(double seconds, MintFrame* out, bool includeSh, QString* error) const;

    // The capture's own frame rate, from its first chunk: intervals over duration.
    // 30 for the captures seen so far, 25 for some; 30 when there is nothing to go by.
    double frameRate() const;
    // Empty when every chunk keeps the first one's frame rate; otherwise which chunk
    // does not, in words for the user. A .vgs or .pgs holds one frame rate, so such a
    // capture cannot be written as one; .mint, .mgs and .ply do not care.
    QString frameRateProblem() const;
    // How many frames the animation holds at that rate, and the time of one of them.
    int frameCount() const;
    double frameTime(int frame) const;

    bool despill(const MintDespillOptions &, const MintProgressFn &, QString *error);
    static bool despillLogical(vgs::DecodedChunk *, double secondsPerTick,
                              const MintDespillOptions &, const MintProgressFn &, QString *error);
    const QByteArray &bytes() const { return m_data; }

    static double sphericalHarmonicC0() { return 0.28209479177387814; }

private:
    bool parse(QString* error);
    const MintChunk* chunkAt(double seconds, double* normalized) const;

    // Fills the per-trajectory tables the groups index into, evaluated at one
    // instant. Shared by decode() and despill() so the interpolation rules only
    // exist in one place.
    struct SharedBasis
    {
        QVector<float> sh0;       // sh0Entries * 3
        QVector<float> opacity;   // opacityEntries
        QVector<float> position;  // positionEntries * 3
        QVector<float> rotation;  // rotationEntries * 4, xyzw
    };
    void buildBasis(const MintBlock& shared, quint64 frame, float alpha, bool needPosition,
                    bool needRotation, SharedBasis* out) const;

    // Resolves colorDc for one group at one instant. The despill needs only this
    // slice of the reconstruction, so it does not pay for positions or rotations.
    void decodeGroupColor(const MintBlock& group, const MintBlock& shared,
                          const SharedBasis& basis, QVector<float>* colorDc) const;

    const char* raw(quint64 absolute) const { return m_data.constData() + absolute; }
    char* mutableRaw(quint64 absolute) { return m_data.data() + absolute; }

    bool load(const QString& path, bool headerOnly, QString* error);

    QString m_path;
    QByteArray m_data;
    quint64 m_fileSize = 0;
    bool m_headerOnly = false;
    quint64 m_payloadStart = 0;
    quint64 m_indexOffset = 0;
    double m_duration = 0.0;
    double m_bbox[6] = {0, 0, 0, 0, 0, 0};
    QVector<MintChunk> m_chunks;
    bool m_modified = false;
};

#endif // MINTFILE_H
