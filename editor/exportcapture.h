#pragma once
#include "project.h"
#include "vgsframe.h"
#include <functional>
#include <array>

struct ExportResult {
    int frames = 0;
    quint64 kept = 0, removed = 0;
    QStringList notes;
};
using ExportProgress = std::function<bool(int, const QString &)>;

// Source attributes are immutable. Crop tests transformed means in world space.
vgs::Frame bakeExportFrame(const vgs::Frame &, const Project &, int degree,
                          const ExportProgress & = {},double frameRate = 30);
ExportResult exportCaptureFile(const Project &, const QString &destination,
                               const ExportProgress & = {});

// One edited instant as a 3D Gaussian Splatting .ply (INRIA convention, binary little
// endian): what Export capture bakes at that time - transform, modifiers, colour
// processing - with the export SH degree's bands in f_rest.
ExportResult exportFramePly(const Project &, double seconds, const QString &destination,
                            const ExportProgress & = {});
void writePly(const vgs::Frame &, const QString &destination);

// Assemble standard encoding-0 attributes from an already baked native frame.
vgs::DecodedChunk packExportFrame(const vgs::Frame &, int degree);
std::array<double,256> exportShTransform(const Transform &);
