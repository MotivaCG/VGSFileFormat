#pragma once
#include "captureworker.h"
#include "project.h"
#include <array>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
class ViewCube;
class QLabel;
class QOpenGLFramebufferObject;

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
    void activateTransformShortcut(TransformMode mode);
    void setCamera(const Camera &camera);
    Camera camera() const { return camera_; }
    void setViewPreset(ViewPreset preset, bool orthographic = true);
    bool handleViewKey(int key, Qt::KeyboardModifiers modifiers);
    void setCrop(const CropVolume &crop);
    void setModifiers(const QVector<Modifier> &modifiers);
    CropVolume crop() const { return crop_; }
    void setCropEditing(bool enabled);
    bool cropEditing() const { return cropEditing_; }
    Transform editableTransform() const;
    Transform displayedTransform() const;
    void setDisplayedComponent(int group,int axis,float value);
    void setCoordinateSpace(TransformMode mode,CoordinateSpace space);
    CoordinateSpace coordinateSpace(TransformMode mode) const;
    // What the scene chose, before a crop being edited forces Scale to Local.
    CoordinateSpace chosenCoordinateSpace(TransformMode mode) const;
    void setPointSize(float size);
    void setDisplayControls(QWidget *controls);
    void setPlaybackTime(double seconds,double duration);
    void setGrid(bool enabled);
    // Preview of a walking capture: it stays where it is and the floor slides under it,
    // by `distance` along -Z, with a finer grid to read the feet against.
    void setFloorScroll(bool walking, double distance);
    // Display-only viewport contrast (Dark by default); never stored in projects or presets.
    void setLightBackground(bool light);
    bool lightBackground() const { return lightBackground_; }
    void fit(const QVector3D &minimum, const QVector3D &maximum);
    bool focusVisible();
    bool setGhost(bool enabled);
    void setGhostOpacity(float opacity);
    float ghostOpacity() const {return ghostOpacity_;}
    bool ghostEnabled() const {return ghostEnabled_;}
    size_t ghostPointCount() const {return ghostPoints_.size();}
    double ghostTime() const {return ghostTime_;}
    QString renderError() const { return error_; }
signals:
    void cameraChanged();
    void renderFailed(QString error);
    void transformEdited(Transform transform);
    void transformModeChanged(TransformMode mode);
    void cropEdited(CropVolume crop);
    void frameRequested();
    void ghostChanged(bool enabled);
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
    std::vector<QVector3D> visibleWorldPoints() const;
    void drawGhost(const QMatrix4x4 &viewProjection,const QSize &pixels,float dpr);
    Transform worldTransformed(const Transform &start,const QMatrix4x4 &delta) const;
    FramePtr frame_;
    Transform transform_;
    Camera camera_;
    ViewCube *viewCube_;
    QLabel *statistics_;
    QString playbackTimeText_;
    QWidget *displayControls_ = nullptr;
    CropVolume crop_;
    QVector<Modifier> modifiers_;
    bool modifierStack_ = false, cpuFiltered_ = false;
    GLuint modifierBuffer_ = 0, modifierTexture_ = 0;
    bool cropEditing_ = false;
    QPoint lastMouse_;
    bool walking_ = false;
    double walkDistance_ = 0;
    bool grid_ = true, frameDirty_ = true, initialized_ = false, lightBackground_ = false;
    QColor overlayText() const;
    GLuint vao_ = 0, buffer_ = 0, shBuffer_ = 0, shTexture_ = 0;
    GLuint gridVao_ = 0, gridBuffer_ = 0, fineVao_ = 0, fineBuffer_ = 0;
    int fineVertices_ = 0;
    int gridVertices_ = 0, shCoefficients_ = 0;
    std::unique_ptr<QOpenGLShaderProgram> pointShader_, gridShader_;
    QString error_;
    double uploadMs_ = 0;
    float pointSize_ = 5;
    struct GhostPoint {float position[3];};
    std::vector<GhostPoint> ghostPoints_;
    bool ghostEnabled_=false,ghostDirty_=false;
    double ghostTime_=0;
    float ghostOpacity_=0.15f;
    GLuint ghostVao_=0,ghostBuffer_=0,ghostCompositeVao_=0;
    std::unique_ptr<QOpenGLShaderProgram> ghostShader_,ghostCompositeShader_;
    std::unique_ptr<QOpenGLFramebufferObject> ghostFramebuffer_;
    GLuint gizmoVao_ = 0, gizmoBuffer_ = 0;
    TransformMode mode_ = TransformMode::None;
    CoordinateSpace spaces_[3] = {CoordinateSpace::Global,CoordinateSpace::Global,CoordinateSpace::Global};
    int hoverHandle_ = -1, dragHandle_ = -1; // 0-2 axes, 3 centre, 4-6 Move planes by their normal axis
    std::array<QVector3D,4> planeHandle(int normal,const QVector3D &pivot,float length) const;
    bool dragging_ = false, ignoreLeftUntilRelease_ = false, dragConstraintValid_ = false;
    Transform dragStart_;
    CropVolume dragStartCrop_;
    QVector3D dragPivot_, dragAxis_, dragPlaneNormal_, dragPlaneStart_, dragRotationVector_;
    QPointF dragScreenStart_, dragScreenAxis_;
    float dragLength_ = 1, dragParameter_ = 0, dragRotationAngle_ = 0;
};
