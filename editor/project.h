#pragma once
#include <QJsonObject>
#include <QMatrix4x4>
#include <QMetaType>
#include <QString>
#include <QVector3D>
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
struct Project {
    QString asset;
    Transform transform;
    Camera camera;
    CropVolume crop;
    CaptureSettings captureSettings;
    double time = 0, in = 0, out = 0, speed = 1;
    double pointSize = 2;
    bool loop = true, grid = true;
    CoordinateSpace spaces[3] = {CoordinateSpace::Global,CoordinateSpace::Global,CoordinateSpace::Global};
    QJsonObject json(const QString &projectPath) const;
    static bool read(const QString &path, Project *project, QString *error);
    static bool fromJson(const QJsonObject &root,const QString &baseDirectory,Project *project,QString *error);
    bool write(const QString &path, QString *error) const;
};
