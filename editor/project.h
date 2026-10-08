#pragma once
#include <QJsonObject>
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
enum class ModifierType { Crop, RemoveGreen, AnimateTransform, PurgeIsolated, Walk, BakeAntialiasing };
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
};
