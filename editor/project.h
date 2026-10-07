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
    float radius = 1, height = 2;
    float width = 2, depth = 2;
    bool contains(const QVector3D &worldPosition) const;
};
Q_DECLARE_METATYPE(CropVolume)
enum class ModifierType { Crop, RemoveGreen, AnimateTransform, PurgeIsolated };
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
    IsolationFilter isolation;
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
