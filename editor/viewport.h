#pragma once
#include "captureworker.h"
#include "project.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
class ViewCube;

class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit Viewport(QWidget *parent = nullptr);
    ~Viewport() override;
    void setFrame(FramePtr frame);
    void setTransform(const Transform &transform);
    Transform transform() const { return transform_; }
    void setTransformMode(TransformMode mode);
    TransformMode transformMode() const { return mode_; }
    void toggleTransformMode(TransformMode mode);
    void setCamera(const Camera &camera);
    Camera camera() const { return camera_; }
    void setViewPreset(ViewPreset preset, bool orthographic = true);
    bool handleViewKey(int key, Qt::KeyboardModifiers modifiers);
    void setCrop(const CropVolume &crop);
    CropVolume crop() const { return crop_; }
    void setCropEditing(bool enabled);
    bool cropEditing() const { return cropEditing_; }
    Transform editableTransform() const;
    Transform displayedTransform() const;
    void setDisplayedComponent(int group,int axis,float value);
    void setCoordinateSpace(TransformMode mode,CoordinateSpace space);
    CoordinateSpace coordinateSpace(TransformMode mode) const;
    void setPointSize(float size);
    void setGrid(bool enabled);
    void fit(const QVector3D &minimum, const QVector3D &maximum);
    QString renderError() const { return error_; }
signals:
    void cameraChanged();
    void renderFailed(QString error);
    void transformEdited(Transform transform);
    void transformModeChanged(TransformMode mode);
    void cropEdited(CropVolume crop);
    void frameRequested();
protected:
    bool event(QEvent *event) override;
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void cleanup();
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix() const;
    QVector3D axisDirection(int axis) const;
    float gizmoLength() const;
    bool projectPoint(const QVector3D &world, QPointF *screen) const;
    void mouseRay(const QPointF &screen, QVector3D *origin, QVector3D *direction) const;
    bool axisParameter(const QPointF &screen, const QVector3D &pivot, const QVector3D &axis, float *parameter) const;
    bool planePoint(const QPointF &screen, const QVector3D &pivot, const QVector3D &normal, QVector3D *point) const;
    int pickHandle(const QPointF &screen) const;
    void drawGizmo(const QMatrix4x4 &viewProjection);
    void beginManipulation(int handle, const QPointF &screen);
    void updateManipulation(const QPointF &screen);
    void cancelManipulation();
    QMatrix4x4 editableParent() const;
    QVector3D editablePivot() const;
    void applyEditableTransform(const Transform &transform);
    void orbit(float yawDelta, float pitchDelta);
    void drawCrop(const QMatrix4x4 &viewProjection);
    Transform worldTransformed(const Transform &start,const QMatrix4x4 &delta) const;
    FramePtr frame_;
    Transform transform_;
    Camera camera_;
    ViewCube *viewCube_;
    CropVolume crop_;
    bool cropEditing_ = false;
    QPoint lastMouse_;
    bool grid_ = true, frameDirty_ = true, initialized_ = false;
    GLuint vao_ = 0, buffer_ = 0, shBuffer_ = 0, shTexture_ = 0;
    GLuint gridVao_ = 0, gridBuffer_ = 0;
    int gridVertices_ = 0, shCoefficients_ = 0;
    std::unique_ptr<QOpenGLShaderProgram> pointShader_, gridShader_;
    QString error_;
    double uploadMs_ = 0;
    float pointSize_ = 2;
    GLuint gizmoVao_ = 0, gizmoBuffer_ = 0;
    TransformMode mode_ = TransformMode::None;
    CoordinateSpace spaces_[3] = {CoordinateSpace::Global,CoordinateSpace::Global,CoordinateSpace::Global};
    int hoverHandle_ = -1, dragHandle_ = -1;
    bool dragging_ = false, ignoreLeftUntilRelease_ = false, dragConstraintValid_ = false;
    Transform dragStart_;
    QVector3D dragPivot_, dragAxis_, dragPlaneNormal_, dragPlaneStart_, dragRotationVector_;
    QPointF dragScreenStart_, dragScreenAxis_;
    float dragLength_ = 1, dragParameter_ = 0, dragRotationAngle_ = 0;
};
