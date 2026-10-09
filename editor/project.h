#pragma once
#include <QJsonObject>
#include <array>
#include <map>
#include <vector>
#include <QJsonArray>
#include <QMatrix4x4>
#include <QMetaType>
#include <QString>
#include <QVector3D>
#include <QVector>
#include "capturesettings.h"

struct Transform {
    QVector3D position{0, 0, 0};
    QVector3D rotation{0, 0, 0}; // Euler XYZ in degrees; T * Rz * Ry * Rx * S.
    QVector3D scale{1, 1, 1};
    QVector3D shear{0, 0, 0}; // XY, XZ, YZ; preserves affine global scaling of rotated targets.
    QMatrix4x4 matrix() const;
    QMatrix4x4 rotationMatrix() const;
    Transform rotatedLocal(int axis, float degrees) const;
    static Transform fromMatrix(const QMatrix4x4 &matrix);
};
Q_DECLARE_METATYPE(Transform)
enum class TransformMode { None, Move, Rotate, Scale };
Q_DECLARE_METATYPE(TransformMode)
enum class CoordinateSpace { Global, Local };
enum class ViewPreset { Free, Front, Back, Left, Right, Top, Bottom };
Q_DECLARE_METATYPE(ViewPreset)
struct Camera {
    QVector3D target{0, 0, 0};
    float yaw = 25, pitch = 12, distance = 4, roll = 0;
    ViewPreset preset = ViewPreset::Free;
    bool orthographic = false;
    QMatrix4x4 viewMatrix() const;
    // Where the camera is: `distance` from the target, along yaw and pitch.
    QVector3D position() const;
};
enum class CropShape { Cylinder, Box };
struct CropVolume {
    bool enabled = false;
    CropShape shape = CropShape::Cylinder;
    Transform transform; // Independent world-space base pivot; local Y runs from 0 to height.
    // A cylinder is elliptic: radius along local X, radiusZ along local Z.
    float radius = 1, radiusZ = 1, height = 2;
    bool insideEllipse(float x,float z) const {return (x*x)/(radius*radius)+(z*z)/(radiusZ*radiusZ)<=1;}
    float width = 2, depth = 2;
    // Keep preserves what is inside; Remove deletes it, and wins where the two overlap.
    bool remove = false;
    // While this crop is edited, what it would delete is shown in red rather than hidden.
    // Editing preview only: it changes neither the normal view nor the export.
    bool showRemovedInRed = true;
    bool contains(const QVector3D &worldPosition) const;
};
Q_DECLARE_METATYPE(CropVolume)
enum class ModifierType { Crop, RemoveGreen, AnimateTransform, PurgeIsolated, Walk, BakeAntialiasing, PruneLowContribution, Audio, Colour, Erase };
struct GreenFilter {
    float minimumSaturation = 0.5f, hueTolerance = 45; // HSV: saturation 0..1, circular distance from 120 degrees.
    bool linearRgb = true; // Convert clamped sRGB base colour to linear RGB before HSV.
    bool matches(const QVector3D &sourceRgb) const;
};
struct TransformKeyframe {int frame=0;Transform offset;};
struct TransformAnimation {
    QVector<TransformKeyframe> keys;
    Transform evaluate(double frame) const;
    void setKey(int frame,const Transform &offset);
    void removeKey(int frame);
    QJsonArray json() const;
};
// A crop's pose and size at one frame. Its shape, Keep/Remove mode and edit preview are
// not animated: those stay the modifier's own.
struct CropKeyframe {int frame=0;Transform transform;float radius=1,radiusZ=1,height=2,width=2,depth=2;};
struct CropAnimation {
    // Static ignores the keys without losing them; either mode can be chosen at any time.
    bool animated=false;
    QVector<CropKeyframe> keys;
    // The static pose and size, kept aside while the crop is animated.
    CropVolume still;
    bool active() const {return animated && !keys.isEmpty();}
    // `base` supplies what is not animated. Pose and size interpolate linearly, rotation
    // along the shortest path; before the first key and after the last they hold.
    CropVolume evaluate(const CropVolume &base,double frame) const;
    void setKey(int frame,const CropVolume &crop);
    void removeKey(int frame);
    QJsonArray json() const;
};
struct IsolationFilter {
    int neighbour=4;
    double medianPercent=700;
};
// Prune low contribution: removes, chunk by chunk, up to `percent` of the splats that add
// least to images of the capture - but never one that covers more than `protectAbove`
// pixels of a 1080p view on average, so a capture where everything counts loses nothing.
struct PruneFilter {
    double percent=15;
    double protectAbove=0.25;
};
struct Modifier {
    QString id, name;
    ModifierType type = ModifierType::Crop;
    bool enabled = true;
    CropVolume crop;
    GreenFilter green;
    TransformAnimation animation;
    // Crop only. While animated, `crop` holds the pose shown at the current frame.
    CropAnimation cropAnimation;
    // The crop as it is when static: what is saved as its fixed pose and size.
    CropVolume staticCrop() const;
    IsolationFilter isolation;
    PruneFilter prune;
    // Audio: the track the capture plays with - a file, or with none the capture's own - and
    // where it sits: `audioOffset` seconds of the capture's timeline before its first sound
    // plays (negative: the track starts earlier). The editor plays it; exports carry it.
    QString audioFile;
    double audioOffset = 0;
    // Colour: exposure in stops, white balance (temperature: + warmer, tint: + greener) and
    // saturation (1 leaves it), all one 3x3 matrix on the colour - base and view-dependent
    // alike - so it keeps the capture's own representation. And despill, the green spill
    // removal Metadata and processing also offers, with the same settings: when on it
    // overrides that one, so the export despills once.
    double colourExposure = 0, colourTemperature = 0, colourTint = 0, colourSaturation = 1;
    // Opacity: every splat's opacity times this, at most 1 - the training-time opacity boost
    // (SuperSplat's and Spirula's) applied to a finished capture.
    double colourOpacity = 1;
    // None, only on export, or always - also in the viewport.
    enum DespillMode { DespillNone = 0, DespillOnExport = 1, DespillAlways = 2 };
    int colourDespill = DespillNone;
    bool colourRecoverSkin = true;
    double colourDespillStrength = 1, colourGreenGain = 0.97, colourViewChroma = 0.5;
    std::array<float,9> colourMatrix() const; // row-major
    // Erase: the splats picked by hand, which it removes. A splat is only itself within one
    // chunk of the source, so the picks are kept per source chunk, as sorted record indices
    // within it - the same records the preview and both exports read.
    std::map<int, std::vector<uint32_t>> erased;
    // While its selection is being edited the picks are shown, not removed. Never saved.
    bool showErased = false;
    // Walk: metres per second along +Z of the exported capture. Preview only; export
    // writes it to the header as a walking capture instead of moving the data.
    double walkSpeed = 1;
    // Whether the Walk speed is shown in km/h. Display only: walkSpeed stays in m/s.
    bool walkKmh = false;
    // Bake anti-aliasing: the viewing distance (m) and screen height (px) the capture is
    // prepared for, at the editor's 45 degree vertical field of view.
    double bakeDistance = 2.5;
    int bakeScreenHeight = 1080;
    // The world-space size every splat is widened by: the renderer's 0.3 px^2 low-pass,
    // as a distance at bakeDistance.
    double bakeSize() const;
    bool active() const { return enabled && (type!=ModifierType::Crop || crop.enabled); }
};
struct Project {
    Project();
    QString asset;
    Transform transform;
    Camera camera;
    QVector<Modifier> modifiers;
    QString selectedModifier;
    Modifier *modifier();
    const Modifier *modifier() const;
    CropVolume &crop(); // Selected crop properties; never used to evaluate the stack.
    const CropVolume &crop() const;
    QJsonArray modifierJson() const;
    QMatrix4x4 animationMatrix(double frame) const;
    Transform transformAtFrame(double frame) const;
    void setAnimatedPose(int frame,const Transform &pose);
    bool hasAnimation() const;
    bool hasAnimatedMotion() const;
    // The stack with every animated crop evaluated at a frame: what preview and export apply.
    QVector<Modifier> modifiersAtFrame(double frame) const;
    bool hasAnimatedCrop() const;
    // Refreshes the pose each animated crop shows to the one at this frame.
    void showCropsAtFrame(double frame);
    // The summed speed of the active Walk modifiers, and how far the capture has walked
    // at a time along +Z, from the start of the export range.
    double walkSpeed() const;
    double walkDistance(double seconds) const;
    // The size the active Bake anti-aliasing modifiers widen every splat by (the largest of
    // them), in world units; 0 when none is active.
    double antialiasingBake() const;
    // The active Audio modifier that decides the soundtrack (the last in the stack), or none.
    const Modifier *audioModifier() const;
    static QString newId();
    CaptureSettings captureSettings;
    double time = 0, in = 0, out = 0, speed = 1;
    double pointSize = 5;
    bool loop = true, grid = true;
    CoordinateSpace spaces[3] = {CoordinateSpace::Global,CoordinateSpace::Global,CoordinateSpace::Global};
    QJsonObject json(const QString &projectPath) const;
    static bool read(const QString &path, Project *project, QString *error);
    static bool fromJson(const QJsonObject &root,const QString &baseDirectory,Project *project,QString *error);
    bool write(const QString &path, QString *error) const;
private:
    CropVolume inactiveCrop_;
};
Q_DECLARE_METATYPE(Project)

// Shared preview/export semantics: union of active crops, then green removal.
class CompiledModifiers {
public:
    explicit CompiledModifiers(const Project &);
    explicit CompiledModifiers(const QVector<Modifier> &);
    bool keepsPosition(const QVector3D &world) const;
    bool removesColour(const QVector3D &sourceRgb) const;
    bool keeps(const QVector3D &world,const QVector3D &sourceRgb) const { return keepsPosition(world) && !removesColour(sourceRgb); }
    struct Crop {QMatrix4x4 inverse;CropVolume volume;};
    QVector<Crop> crops;
    QVector<GreenFilter> greens;
    QVector<IsolationFilter> isolations;
    QVector<PruneFilter> prunes;
    // Every active Colour modifier's matrix, in stack order, as one (row-major); identity
    // when there are none. And whether one of them despills, and how strongly.
    std::array<float,9> colour{1,0,0,0,1,0,0,0,1};
    bool colourChanges = false, despill = false, despillPreview = false, recoverSkin = true;
    float opacity = 1; // every active Colour modifier's opacity factor, multiplied
    double despillStrength = 1, greenGain = 0.97, viewChroma = 0.5;
    // Erase: the source chunks' records it removes, all active Erase modifiers together;
    // `showErased` when one of them shows its picks instead.
    std::map<int, std::vector<uint32_t>> erased;
    bool showErased = false;
    bool erases(int chunk, uint32_t record) const;
    // A chunk's records with nothing erased marked 1, for exports to filter with.
    std::vector<uint8_t> eraseKeep(int chunk, size_t records) const;
};
