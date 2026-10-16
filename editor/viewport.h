// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "captureworker.h"
#include "project.h"
#include <QElapsedTimer>
#include <QTimer>
#include <array>
#include <QPainterPath>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QVector2D>
class ViewCube;
class QLabel;
class QOpenGLFramebufferObject;
class SplatSorter;

class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit Viewport(QWidget *parent = nullptr);
    ~Viewport() override;
    void setFrame(FramePtr frame);
    FramePtr frame() const { return frame_; }
    quint64 frameRevision() const { return frameRevision_; }
    QJsonObject samplePoints(int limit, bool includeRemoved) const;
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
    // Display only: draw each record as its Gaussian (sorted, blended) instead of a point.
    void setSplatRendering(bool splats);
    bool splatRendering() const { return splats_; }
    // Spherical-harmonic bands the splats evaluate, 0 (base colour) to 3, or -1 for all the
    // capture has; points use them all.
    void setSplatShDegree(int degree);
    // Whether the capture was trained with anti-aliasing, and its splats are drawn with the
    // opacity compensation that goes with it (the VGS AntialiasedSplats render hint).
    void setSplatAntialiasing(bool antialiased);
    bool splatAntialiasing() const { return splatAntialiasing_; }
    int splatShDegree() const { return splatShDegree_; }
    void setDisplayControls(QWidget *controls);
    void setPlaybackTime(double seconds,double duration);
    void setPlaybackRate(bool playing, double targetFps);
    // Display aids, each on its own: the floor grid, the coloured X/Y/Z axes and a grey
    // chevron on the floor just past a metre along +Z, pointing that way (where the capture faces).
    void setGrid(bool enabled);
    // The viewer's: no transform keys, only the navigation hint.
    void setViewerMode(bool viewer) { viewerMode_ = viewer; if (!viewer) {orbitInertiaTimer_.stop();orbitDragging_=false;} update(); }
    void setAxes(bool enabled);
    void setFrontMarker(bool enabled);
    // Preview of a walking capture: it stays where it is and the floor slides under it,
    // by `distance` along -Z, with a finer grid to read the feet against.
    void setFloorScroll(bool walking, double distance);
    // Display-only viewport contrast (Dark by default); never stored in projects or presets.
    void setLightBackground(bool light);
    bool lightBackground() const { return lightBackground_; }
    void fit(const QVector3D &minimum, const QVector3D &maximum);
    bool focusVisible();
    // The world point drawn under a viewport position, as a viewer sees it: for splats, where
    // the splats in front add up to half opacity along that pixel; for points, the nearest
    // point under the cursor. False over empty space.
    bool pickPoint(const QPointF &screen, QVector3D *world) const;
    // Double-click: the orbit pivot glides to a point, keeping the view's angle and distance,
    // as the web viewer does. Any navigation cancels the glide.
    void focusOn(const QVector3D &point);
    bool focusing() const { return focusTimer_.isActive(); }
    // Picking splats by hand for an Erase modifier. With a tool on, a left drag paints with a
    // brush of `radius` pixels or draws a lasso; on release the records of the visible splats
    // inside are sent with what the stroke does: replace the chunk's picks, add (Ctrl) or
    // subtract (Alt). The right button orbits and the middle one pans meanwhile.
    enum class SelectTool { None, Brush, Lasso };
    enum class SelectMode { Replace, Add, Subtract };
    void setSelectTool(SelectTool tool, float radius);
    SelectTool selectTool() const { return selectTool_; }
    // The records of the visible splats a region of the view covers.
    std::vector<uint32_t> recordsInside(const QPainterPath &region) const;
    // The region a brush stroke along `path` covers.
    static QPainterPath brushRegion(const QPainterPath &path, float radius);
    // The scene as it is drawn now, without the grid, gizmo, crop wires, ghost or help text:
    // the centre square of the view, `side` pixels across. An export's thumbnail.
    QImage cleanImage(int side = 256);
    // The current frame from `camera`, for inspection sheets: floor grid but no gizmo, ghost
    // or hints, splats or points as asked; optionally what the modifiers remove drawn in red
    // instead of hidden, and the selected crop's wire volume. The user's view is untouched.
    QImage sheetImage(const Camera &camera, bool splats, bool showRemoved, bool cropOutline);
    // The box of the points on screen, each axis without `trim` of them at either end so a
    // few floaters do not stretch it. False when nothing is visible.
    bool visibleExtent(QVector3D *minimum, QVector3D *maximum, float trim = 0.01f) const;
    // A Loop morph's markers, drawn numbered over the capture where they are in the world;
    // hollow until placed on both frames, ringed while selected, and dimmed with a broken ring
    // where splats in front hide them. A suggested one is a target where the next one
    // probably goes.
    struct ViewMarker {QVector3D world;int number=0;bool paired=false,selected=false,suggested=false;};
    void setViewMarkers(const QVector<ViewMarker> &markers);
    // Placing markers: a left click that does not drag picks the point under it (pointPicked);
    // dragging still orbits. `hint` says what the click places.
    void setPointPicking(bool picking, const QString &hint = {});
    bool pointPicking() const { return pointPicking_; }
    // The visible splat or point nearest a world position, in the world; false with none.
    bool nearestVisiblePoint(const QVector3D &world, QVector3D *found) const;
    // A line over the view while something slow is being worked out; empty for none.
    void setBusyText(const QString &text);
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
    void selectionStroke(std::vector<uint32_t> records, Viewport::SelectMode mode);
    // Ctrl + wheel over the brush asks for a new radius; the owner sets it back.
    void brushRadiusRequested(float radius);
    // Esc in the viewport: the owner leaves its editing modes too.
    void escapePressed();
    void pointPicked(QVector3D world);
    // Enter and Backspace in the viewport, for what is being placed there.
    void enterPressed();
    void backspacePressed();
protected:
    bool event(QEvent *event) override;
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    quint64 frameRevision_ = 0;
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
    void drawCrop(const QMatrix4x4 &viewProjection, bool always = false);
    std::vector<QVector3D> visibleWorldPoints() const;
    void drawGhost(const QMatrix4x4 &viewProjection,const QSize &pixels,float dpr);
    Transform worldTransformed(const Transform &start,const QMatrix4x4 &delta) const;
    QVector<ViewMarker> viewMarkers_;
    // Which markers splats hide, and the view, frame and markers that was worked out for.
    std::vector<bool> markerHidden_;
    QMatrix4x4 markerHiddenView_;
    quint64 markerHiddenFrame_ = 0;
    bool markerHiddenSplats_ = false, markerHiddenValid_ = false;
    void updateMarkerOcclusion(const QMatrix4x4 &model, const QMatrix4x4 &view, const QMatrix4x4 &viewProjection);
    QString busyText_;
    bool pointPicking_ = false;
    QString pickHint_;
    QPointF pickPress_{-1e9,-1e9};
    bool cleanCapture_ = false, sheetCapture_ = false, sheetShowRemoved_ = false, sheetCrop_ = false;
    SelectTool selectTool_ = SelectTool::None;
    float brushRadius_ = 20;
    bool selecting_ = false;
    SelectMode strokeMode_ = SelectMode::Replace;
    QPainterPath stroke_;
    QPointF hover_{-1000,-1000};
    QTimer focusTimer_;
    QElapsedTimer focusClock_;
    QTimer orbitInertiaTimer_;
    QElapsedTimer orbitInertiaClock_, orbitSampleClock_;
    QVector2D orbitVelocity_;
    bool orbitDragging_ = false;
    QVector3D focusFrom_, focusTo_;
    FramePtr frame_;
    Transform transform_;
    Camera camera_;
    ViewCube *viewCube_;
    QLabel *statistics_;
    QString playbackTimeText_;
    QTimer performanceTimer_;
    QElapsedTimer performanceClock_;
    quint64 presentedFrameSerial_ = 0;
    int presentedFrames_ = 0, playbackFrames_ = 0;
    std::array<int,10> presentedSamples_{}, playbackSamples_{};
    std::array<double,10> elapsedSamples_{};
    int performanceSampleIndex_ = 0, performanceSampleCount_ = 0;
    double viewportFps_ = 0, playbackFps_ = 0, targetPlaybackFps_ = 0;
    bool playbackPlaying_ = false, viewportIdle_ = true;
    QWidget *displayControls_ = nullptr;
    CropVolume crop_;
    QVector<Modifier> modifiers_;
    bool modifierStack_ = false, cpuFiltered_ = false;
    GLuint modifierBuffer_ = 0, modifierTexture_ = 0;
    bool cropEditing_ = false;
    QPoint lastMouse_;
    bool walking_ = false;
    double walkDistance_ = 0;
    bool axes_ = false, frontMarker_ = true, viewerMode_ = false;
    GLuint markerVao_ = 0, markerBuffer_ = 0;
    int markerVertices_ = 0;
    bool grid_ = true, frameDirty_ = true, initialized_ = false, lightBackground_ = false;
    QColor overlayText() const;
    void updateStatistics();
    GLuint vao_ = 0, buffer_ = 0, shBuffer_ = 0, shTexture_ = 0;
    GLuint gridVao_ = 0, gridBuffer_ = 0, fineVao_ = 0, fineBuffer_ = 0;
    int fineVertices_ = 0;
    int gridVertices_ = 0, shCoefficients_ = 0;
    std::unique_ptr<QOpenGLShaderProgram> pointShader_, gridShader_, splatShader_;
    bool splats_ = false, splatsDirty_ = true, splatLimitReported_ = false;
    int splatShDegree_ = -1;
    bool splatAntialiasing_ = false;
    GLuint splatVao_ = 0, cornerBuffer_ = 0, orderBuffer_ = 0, splatBuffer_ = 0, splatTexture_ = 0;
    std::vector<uint32_t> splatOrder_;
    std::vector<float> splatDepths_;
    // The order drawn and what it was sorted for; frames are told apart by serial, not pointer.
    QMatrix4x4 sortedView_, requestedView_;
    quint64 frameSerial_ = 0, sortedSerial_ = 0, requestedSerial_ = 0;
    bool orderComplete_ = false; // every splat of its frame, none filtered out
    std::unique_ptr<SplatSorter> sorter_;
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
