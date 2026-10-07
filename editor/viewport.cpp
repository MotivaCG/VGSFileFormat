#include "viewport.h"
#include "viewcube.h"
#include "editortheme.h"
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QLabel>
#include <QOpenGLFramebufferObject>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cstddef>
#include <numeric>

static const char *pointVertex = R"GLSL(
#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 color;
layout(location=2) in float sourceId;
layout(location=3) in float modifierVisibility;
uniform bool usePurgeMask;
uniform mat4 model, view, projection;
uniform vec3 eyeLocal;
uniform samplerBuffer shData;
uniform int shCoefficients;
out vec4 rgba;
vec3 shadedColor() {
    vec3 result=color;
    if(shCoefficients==0) return clamp(result,0.0,1.0);
    vec3 d=normalize(position-eyeLocal);
    float x=d.x,y=d.y,z=d.z;
    float basis[15];
    basis[0]=-0.4886025119*y; basis[1]=0.4886025119*z; basis[2]=-0.4886025119*x;
    basis[3]=1.0925484306*x*y; basis[4]=-1.0925484306*y*z;
    basis[5]=0.3153915653*(2*z*z-x*x-y*y); basis[6]=-1.0925484306*x*z;
    basis[7]=0.5462742153*(x*x-y*y);
    basis[8]=-0.5900435899*y*(3*x*x-y*y);
    basis[9]=2.8906114426*x*y*z;
    basis[10]=-0.4570457995*y*(4*z*z-x*x-y*y);
    basis[11]=0.3731763326*z*(2*z*z-3*x*x-3*y*y);
    basis[12]=-0.4570457995*x*(4*z*z-x*x-y*y);
    basis[13]=1.4453057213*z*(x*x-y*y);
    basis[14]=-0.5900435899*x*(x*x-3*y*y);
    for(int k=0;k<shCoefficients;k++) result+=basis[k]*texelFetch(shData,int(sourceId)*shCoefficients+k).rgb;
    return clamp(result,0.0,1.0);
}
uniform float pointSize;
uniform samplerBuffer modifierData;
uniform int cropCount, greenCount;
uniform bool editingCrop;
bool removedByModifiers() {
    if(usePurgeMask && modifierVisibility<0.5) return true;
    vec3 world=(model*vec4(position,1)).xyz;
    if(cropCount>0 && !editingCrop) {
        bool inside=false;
        for(int i=0;i<cropCount;++i) {
            int base=i*6;
            mat4 inverse=mat4(texelFetch(modifierData,base),texelFetch(modifierData,base+1),texelFetch(modifierData,base+2),texelFetch(modifierData,base+3));
            vec4 dimensions=texelFetch(modifierData,base+4);vec4 extent=texelFetch(modifierData,base+5);
            vec3 p=(inverse*vec4(world,1)).xyz;
            bool horizontal=dimensions.x>0.5 ? abs(p.x)<=extent.x && abs(p.z)<=extent.y : dot(p.xz,p.xz)<=dimensions.z*dimensions.z;
            if(horizontal && p.y>=0.0 && p.y<=dimensions.y) {inside=true;break;}
        }
        if(!inside) return true;
    }
    vec3 baseRgb=clamp(color,0.0,1.0);
    for(int i=0;i<greenCount;++i) {
        vec4 filter=texelFetch(modifierData,cropCount*6+i);
        vec3 rgb=filter.w>0.5 ? mix(baseRgb/12.92,pow((baseRgb+0.055)/1.055,vec3(2.4)),greaterThan(baseRgb,vec3(0.04045))) : baseRgb;
        float maximum=max(rgb.r,max(rgb.g,rgb.b)),minimum=min(rgb.r,min(rgb.g,rgb.b)),chroma=maximum-minimum;
        if(chroma<=0.0 || maximum<=0.0) continue;
        float hue=maximum==rgb.r ? (rgb.g-rgb.b)/chroma : maximum==rgb.g ? 2.0+(rgb.b-rgb.r)/chroma : 4.0+(rgb.r-rgb.g)/chroma;
        hue*=60.0;if(hue<0.0) hue+=360.0;float distance=abs(hue-120.0);distance=min(distance,360.0-distance);
        if(chroma/maximum>=filter.x && distance<=filter.y) return true;
    }
    return false;
}
void main() {
    if(removedByModifiers()) {rgba=vec4(0);gl_Position=vec4(2,2,2,1);gl_PointSize=pointSize;return;}
    rgba=vec4(shadedColor(),1);
    gl_Position=projection*view*model*vec4(position,1);
    gl_PointSize=pointSize;
}
)GLSL";
static const char *pointFragment = R"GLSL(
#version 330 core
in vec4 rgba;
out vec4 fragColor;
void main() {
    vec2 p=gl_PointCoord*2.0-1.0;
    if(dot(p,p)>1.0) discard;
    fragColor=rgba;
}
)GLSL";
static const char *gridVertex = R"GLSL(
#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 color;
uniform mat4 mvp;
out vec3 rgb;
void main() { gl_Position=mvp*vec4(position,1); rgb=color; }
)GLSL";
static const char *gridFragment = R"GLSL(
#version 330 core
in vec3 rgb;
out vec4 fragColor;
void main() { fragColor=vec4(rgb,1); }
)GLSL";

static const char *ghostVertex=R"GLSL(
#version 330 core
layout(location=0) in vec3 position;
uniform mat4 viewProjection;
uniform float pointSize;
out vec4 rgba;
void main() {gl_Position=viewProjection*vec4(position,1);gl_PointSize=pointSize;rgba=vec4(1);}
)GLSL";
static const char *ghostCompositeVertex=R"GLSL(
#version 330 core
out vec2 uv;
void main() {
    vec2 p=gl_VertexID==0 ? vec2(-1,-1) : gl_VertexID==1 ? vec2(3,-1) : vec2(-1,3);
    uv=(p+1)*0.5;gl_Position=vec4(p,0,1);
}
)GLSL";
static const char *ghostCompositeFragment=R"GLSL(
#version 330 core
in vec2 uv;
uniform sampler2D ghostMask;
uniform float outlineWidth;
uniform float ghostOpacity;
out vec4 fragColor;
void main() {
    vec2 texel=outlineWidth/vec2(textureSize(ghostMask,0));
    float centre=texture(ghostMask,uv).a;
    float coverage=centre*4.0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        if(x==0 && y==0) continue;
        coverage+=texture(ghostMask,uv+vec2(x,y)*texel).a*((x==0 || y==0) ? 2.0 : 1.0);
    }
    coverage/=16.0;
    float opacity=min(1.0,ghostOpacity*(centre+max(0.0,centre-coverage)*0.55+(1.0-centre)*coverage*0.4));
    if(opacity<0.005) discard;
    fragColor=vec4(1,1,1,opacity);
}
)GLSL";
Viewport::Viewport(QWidget *parent) : QOpenGLWidget(parent) {
    setMinimumSize(400, 300); setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewCube_ = new ViewCube(this); viewCube_->move(width()-viewCube_->width()-12,12);
    statistics_=new QLabel(this);statistics_->setObjectName("viewportStatistics");statistics_->move(18,12);
    statistics_->setAttribute(Qt::WA_TransparentForMouseEvents);statistics_->setStyleSheet(QString("color: %1; font-family: 'Segoe UI'; font-size: 9pt; background: transparent;").arg(EditorTheme::mutedText().name()));
    connect(viewCube_,&ViewCube::viewSelected,this,[this](ViewPreset preset) { setViewPreset(preset); setFocus(); });
}
Viewport::~Viewport() { cleanup(); }
bool Viewport::event(QEvent *event) {
    if (event->type()==QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() & Qt::KeypadModifier) { key->accept(); return true; }
    }
    return QOpenGLWidget::event(event);
}
void Viewport::cleanup() {
    if (!initialized_) return;
    makeCurrent();
    pointShader_.reset(); gridShader_.reset();
    ghostFramebuffer_.reset();ghostShader_.reset();ghostCompositeShader_.reset();
    glDeleteBuffers(1,&ghostBuffer_);glDeleteVertexArrays(1,&ghostVao_);glDeleteVertexArrays(1,&ghostCompositeVao_);
    glDeleteBuffers(1, &buffer_); glDeleteBuffers(1, &shBuffer_);
    glDeleteBuffers(1,&modifierBuffer_);glDeleteTextures(1,&modifierTexture_);
    glDeleteTextures(1, &shTexture_); glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &gridBuffer_); glDeleteVertexArrays(1, &gridVao_);
    glDeleteBuffers(1, &gizmoBuffer_); glDeleteVertexArrays(1, &gizmoVao_);
    initialized_ = false; doneCurrent();
}
void Viewport::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        error_ = tr("The viewport requires OpenGL 3.3."); emit renderFailed(error_); return;
    }
    initialized_ = true;
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &Viewport::cleanup, Qt::DirectConnection);
    auto build = [&](std::unique_ptr<QOpenGLShaderProgram> &program, const char *vertex, const char *fragment) {
        program = std::make_unique<QOpenGLShaderProgram>();
        if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
            !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program->link()) {
            error_ = program->log(); emit renderFailed(error_); return false;
        }
        return true;
    };
    if (!build(pointShader_, pointVertex, pointFragment) || !build(gridShader_, gridVertex, gridFragment)) return;
    if (!build(ghostShader_,ghostVertex,pointFragment) || !build(ghostCompositeShader_,ghostCompositeVertex,ghostCompositeFragment)) return;
    glGenVertexArrays(1,&ghostVao_);glGenBuffers(1,&ghostBuffer_);glGenVertexArrays(1,&ghostCompositeVao_);
    glBindVertexArray(ghostVao_);glBindBuffer(GL_ARRAY_BUFFER,ghostBuffer_);glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(GhostPoint),nullptr);ghostDirty_=true;
    glGenVertexArrays(1, &vao_); glGenBuffers(1, &buffer_);
    glGenBuffers(1, &shBuffer_); glGenTextures(1, &shTexture_);
    glGenBuffers(1,&modifierBuffer_);glGenTextures(1,&modifierTexture_);
    glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    const int widths[] = {3, 3, 1};
    const size_t offsets[] = {offsetof(PointVertex, position), offsetof(PointVertex, color), offsetof(PointVertex, id)};
    for (GLuint i = 0; i < 3; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, widths[i], GL_FLOAT, GL_FALSE, sizeof(PointVertex), reinterpret_cast<void *>(offsets[i]));
        glVertexAttribDivisor(i, 0);
    }
    struct LineVertex { float p[3], c[3]; };
    glEnableVertexAttribArray(3);glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,sizeof(PointVertex),reinterpret_cast<void *>(offsetof(PointVertex,modifierVisibility)));
    std::vector<LineVertex> lines;
    auto line = [&](QVector3D a, QVector3D b, QVector3D c) {
        lines.push_back({{a.x(),a.y(),a.z()}, {c.x(),c.y(),c.z()}});
        lines.push_back({{b.x(),b.y(),b.z()}, {c.x(),c.y(),c.z()}});
    };
    for (int i = -20; i <= 20; ++i) {
        const float shade = i % 5 == 0 ? 0.22f : 0.14f;
        if (i == 0) {
            // Reserve the positive X/Z segments for the coloured axes.
            line({0,0,-20}, {0,0,0}, {shade,shade,shade});
            line({0,0,2}, {0,0,20}, {shade,shade,shade});
            line({-20,0,0}, {0,0,0}, {shade,shade,shade});
            line({2,0,0}, {20,0,0}, {shade,shade,shade});
        } else {
            line({float(i),0,-20}, {float(i),0,20}, {shade,shade,shade});
            line({-20,0,float(i)}, {20,0,float(i)}, {shade,shade,shade});
        }
    }
    // Restore these grid segments only when coloured axes are hidden.
    line({0,0,0}, {2,0,0}, {0.22f,0.22f,0.22f});
    line({0,0,0}, {0,0,2}, {0.22f,0.22f,0.22f});
    line({0,0,0}, {2,0,0}, {0.9f,0.25f,0.28f});
    line({0,0,0}, {0,2,0}, {0.3f,0.8f,0.45f});
    line({0,0,0}, {0,0,2}, {0.25f,0.5f,0.95f});
    gridVertices_ = int(lines.size());
    glGenVertexArrays(1, &gridVao_); glGenBuffers(1, &gridBuffer_);
    glBindVertexArray(gridVao_); glBindBuffer(GL_ARRAY_BUFFER, gridBuffer_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(lines.size() * sizeof(LineVertex)), lines.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<void *>(3*sizeof(float)));
    glGenVertexArrays(1,&gizmoVao_); glGenBuffers(1,&gizmoBuffer_);
    glBindVertexArray(gizmoVao_); glBindBuffer(GL_ARRAY_BUFFER,gizmoBuffer_);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(LineVertex),nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(LineVertex),reinterpret_cast<void *>(3*sizeof(float)));
    glBindVertexArray(0);
    frameDirty_ = true;
}
QMatrix4x4 Viewport::viewMatrix() const {
    return camera_.viewMatrix();
}
QMatrix4x4 Viewport::projectionMatrix() const {
    QMatrix4x4 projection;
    const float aspect = float(width())/std::max(1,height()), far = std::max(1000.0f,camera_.distance*100);
    if (camera_.orthographic && camera_.preset!=ViewPreset::Free) {
        const float halfHeight = camera_.distance*std::tan(qDegreesToRadians(22.5f));
        projection.ortho(-halfHeight*aspect,halfHeight*aspect,-halfHeight,halfHeight,0.0001f,far);
    } else projection.perspective(45,aspect,std::max(0.0001f,camera_.distance/1000),far);
    return projection;
}
void Viewport::paintGL() {
    if (!initialized_) return;
    QPainter painter(this);
    painter.beginNativePainting();
    const auto background=EditorTheme::viewportBackground();glClearColor(float(background.redF()),float(background.greenF()),float(background.blueF()),1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!error_.isEmpty()) {
        painter.endNativePainting(); painter.setPen(Qt::white);
        painter.drawText(rect().adjusted(20,20,-20,-20), Qt::AlignCenter | Qt::TextWordWrap, error_);
        return;
    }
    const float dpr = float(devicePixelRatioF());
    const QVector2D size(float(width())*dpr, float(height())*dpr);
    glViewport(0, 0, int(size.x()), int(size.y()));
    const auto projection = projectionMatrix();
    const auto view = viewMatrix(), model = transform_.matrix();
    glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    if (grid_) {
        gridShader_->bind(); gridShader_->setUniformValue("mvp", projection * view);
        glBindVertexArray(gridVao_); glDrawArrays(GL_LINES, 0, gridVertices_-10);
        if (camera_.orthographic) {
            glDrawArrays(GL_LINES,gridVertices_-10,4);
        } else {
            // No grid segment runs beneath X/Z; equal depth also covers crossings.
            glDepthFunc(GL_LEQUAL);glDrawArrays(GL_LINES,gridVertices_-6,6);glDepthFunc(GL_LESS);
        }
        gridShader_->release();
    }
    if (frame_ && !frame_->points.empty()) {
        QElapsedTimer timer;
        timer.start();
        if (frameDirty_) {
            glBindBuffer(GL_ARRAY_BUFFER, buffer_);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(frame_->points.size()*sizeof(PointVertex)), frame_->points.data(), GL_STREAM_DRAW);
        }
        if (frameDirty_) {
            shCoefficients_ = frame_->coefficients;
            GLint maxTexels = 0; glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
            if (frame_->sh.size()/3 > size_t(maxTexels)) {
                shCoefficients_ = 0;
                emit renderFailed(tr("This GPU cannot hold all SH coefficients for this capture; showing base colour."));
            }
            glBindBuffer(GL_TEXTURE_BUFFER, shBuffer_);
            const float dummy[3] = {};
            glBufferData(GL_TEXTURE_BUFFER, shCoefficients_ ? GLsizeiptr(frame_->sh.size()*sizeof(float)) : GLsizeiptr(sizeof(dummy)), shCoefficients_ ? frame_->sh.data() : dummy, GL_STREAM_DRAW);
            glBindTexture(GL_TEXTURE_BUFFER, shTexture_); glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB32F, shBuffer_);
            frameDirty_ = false;
            uploadMs_ = timer.nsecsElapsed()/1e6;
        }
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        glDisable(GL_BLEND); glEnable(GL_PROGRAM_POINT_SIZE);
        pointShader_->bind();
        pointShader_->setUniformValue("model", model); pointShader_->setUniformValue("view", view);
        pointShader_->setUniformValue("projection", projection);
        pointShader_->setUniformValue("pointSize", pointSize_*dpr);
        auto activeModifiers=modifiers_;
        if (!modifierStack_) {Modifier modifier;modifier.crop=crop_;activeModifiers={modifier};}
        CompiledModifiers modifiers(activeModifiers);std::vector<QVector4D> values;
        pointShader_->setUniformValue("usePurgeMask",!modifiers.isolations.isEmpty());
        for (const auto &crop:modifiers.crops) {
            for (int col=0;col<4;++col) values.push_back(crop.inverse.column(col));
            values.push_back({float(int(crop.volume.shape)),crop.volume.height,crop.volume.radius,0});
            values.push_back({crop.volume.width*.5f,crop.volume.depth*.5f,0,0});
        }
        for (const auto &green:modifiers.greens) values.push_back({green.minimumSaturation,green.hueTolerance,120,green.linearRgb ? 1.f : 0.f});
        GLint maximum=0;glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE,&maximum);size_t drawCount=frame_->points.size();
        const bool cpuFiltering=values.size()>size_t(maximum);
        if (cpuFiltering) {
            std::vector<PointVertex> points;points.reserve(frame_->points.size());
            for (const auto &p:frame_->points) if ((cropEditing_ || modifiers.keepsPosition(model.map({p.position[0],p.position[1],p.position[2]}))) && !modifiers.removesColour({p.color[0],p.color[1],p.color[2]})) points.push_back(p);
            glBindBuffer(GL_ARRAY_BUFFER,buffer_);glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(points.size()*sizeof(PointVertex)),points.data(),GL_STREAM_DRAW);drawCount=points.size();values.clear();
        }
        cpuFiltered_=cpuFiltering;
        if (values.empty()) values.push_back({0,0,0,0});
        glBindBuffer(GL_TEXTURE_BUFFER,modifierBuffer_);glBufferData(GL_TEXTURE_BUFFER,GLsizeiptr(values.size()*sizeof(QVector4D)),values.data(),GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_BUFFER,modifierTexture_);glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32F,modifierBuffer_);
        pointShader_->setUniformValue("modifierData",1);pointShader_->setUniformValue("cropCount",cpuFiltering ? 0 : int(modifiers.crops.size()));pointShader_->setUniformValue("greenCount",cpuFiltering ? 0 : int(modifiers.greens.size()));pointShader_->setUniformValue("editingCrop",cropEditing_);
        const auto eye = view.inverted().map(QVector3D(0,0,0));
        pointShader_->setUniformValue("eyeLocal", model.inverted().map(eye));
        pointShader_->setUniformValue("shCoefficients", shCoefficients_);
        pointShader_->setUniformValue("shData", 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_BUFFER, shTexture_);
        glBindVertexArray(vao_); glDrawArrays(GL_POINTS, 0, GLsizei(drawCount));
        pointShader_->release(); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    }
    drawGhost(projection*view,QSize(int(size.x()),int(size.y())),dpr);
    drawCrop(projection*view); drawGizmo(projection*view);
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_PROGRAM_POINT_SIZE);
    painter.endNativePainting();
    painter.setPen(EditorTheme::mutedText()); painter.setFont(QFont("Segoe UI", 9));
    painter.drawText(18, height()-18, mode_ == TransformMode::None
        ? tr("Drag: orbit   ·   Right drag: pan   ·   Wheel: zoom   ·   G/R/S: transform")
        : tr("Drag a gizmo handle to transform   ·   Repeat G/R/S: Global/Local   ·   Esc: exit mode"));
    if (frame_) {
        QString text=tr("%1 source points\nDecode %2 ms   ·   upload %3 ms   ·   %4")
            .arg(qulonglong(frame_->points.size())).arg(frame_->decodeMs,0,'f',1).arg(uploadMs_,0,'f',1).arg(shCoefficients_ ? "SH" : "base colour");
        if (!playbackTimeText_.isEmpty()) text+='\n'+playbackTimeText_;
        statistics_->setText(text);
        statistics_->adjustSize();
    } else {
        statistics_->clear();
        painter.setPen(QColor("#ddd")); painter.setFont(QFont("Segoe UI", 18));
        painter.drawText(rect().adjusted(30,30,-30,-30), Qt::AlignCenter, tr("Open a .vgs or .mint capture\n\nCtrl+O"));
    }
}
void Viewport::setFrame(FramePtr frame) {
    if (!frame) setGhost(false);
    if (!frame) setTransformMode(TransformMode::None);
    frame_ = std::move(frame); frameDirty_ = true; update();
}
void Viewport::setPlaybackTime(double seconds,double duration) {
    playbackTimeText_=tr("%1 s / %2 s").arg(seconds,0,'f',3).arg(duration,0,'f',3);update();
}
void Viewport::setTransform(const Transform &transform) { transform_ = transform; update(); }
void Viewport::setCamera(const Camera &camera) {
    camera_ = camera;
    if (camera_.preset==ViewPreset::Free) { camera_.orthographic = false; camera_.pitch = std::clamp(camera_.pitch,-89.0f,89.0f); }
    viewCube_->setCamera(camera_); update();
}
void Viewport::setPointSize(float size) { if (pointSize_ != size) { pointSize_ = size; update(); } }
void Viewport::setDisplayControls(QWidget *controls) {
    displayControls_=controls;viewCube_->setDisplayControls(controls);
}
void Viewport::setGrid(bool enabled) { if (grid_ != enabled) { grid_ = enabled; update(); } }
void Viewport::fit(const QVector3D &minimum, const QVector3D &maximum) {
    const auto m = transform_.matrix();
    camera_.target = m.map((minimum+maximum)*0.5f);
    float radius = 0;
    for (int i = 0; i < 8; ++i) {
        QVector3D p((i&1)?maximum.x():minimum.x(), (i&2)?maximum.y():minimum.y(), (i&4)?maximum.z():minimum.z());
        radius = std::max(radius, (m.map(p)-camera_.target).length());
    }
    camera_.distance = std::max(0.1f, radius / std::sin(qDegreesToRadians(22.5f)) * 1.15f);
    update(); emit cameraChanged();
}
std::vector<QVector3D> Viewport::visibleWorldPoints() const {
    std::vector<QVector3D> points;if (!frame_) return points;points.reserve(frame_->points.size());const auto model=transform_.matrix();
    auto stack=modifiers_;if (!modifierStack_) {Modifier m;m.crop=crop_;stack={m};}CompiledModifiers modifiers(stack);
    for (const auto &point:frame_->points) {
        if (!modifiers.isolations.isEmpty() && point.modifierVisibility<.5f) continue;
        const auto world=model.map({point.position[0],point.position[1],point.position[2]});
        if (!std::isfinite(world.x()) || !std::isfinite(world.y()) || !std::isfinite(world.z())) continue;
        if ((cropEditing_ || modifiers.keepsPosition(world)) && !modifiers.removesColour({point.color[0],point.color[1],point.color[2]})) points.push_back(world);
    }return points;
}
bool Viewport::focusVisible() {
    const auto points=visibleWorldPoints();bool any=false;QVector3D minimum,maximum;
    auto include=[&](QVector3D p) {if (!any) {minimum=maximum=p;any=true;}else for (int axis=0;axis<3;++axis) {minimum[axis]=std::min(minimum[axis],p[axis]);maximum[axis]=std::max(maximum[axis],p[axis]);}};
    for (const auto &point:points) include(point);
    if (ghostEnabled_ && ghostOpacity_>0) for (const auto &point:ghostPoints_) include({point.position[0],point.position[1],point.position[2]});
    if (!any) return false;camera_.target=(minimum+maximum)*.5f;
    const float radius=(maximum-minimum).length()*.5f,aspect=float(width())/std::max(1,height());
    const float halfAngle=std::atan(std::tan(qDegreesToRadians(22.5f))*std::min(1.f,aspect));camera_.distance=std::max(.1f,radius/std::sin(halfAngle)*1.15f);
    update();emit cameraChanged();return true;
}
bool Viewport::setGhost(bool enabled) {
    if (enabled==ghostEnabled_) return ghostEnabled_;
    ghostPoints_.clear();if (enabled) {for (const auto &p:visibleWorldPoints()) ghostPoints_.push_back({{p.x(),p.y(),p.z()}});ghostTime_=frame_ ? frame_->seconds : 0;}
    ghostEnabled_=enabled && !ghostPoints_.empty();ghostDirty_=true;
    if (!ghostEnabled_ && initialized_) {makeCurrent();ghostFramebuffer_.reset();glBindBuffer(GL_ARRAY_BUFFER,ghostBuffer_);glBufferData(GL_ARRAY_BUFFER,0,nullptr,GL_STATIC_DRAW);doneCurrent();}
    update();emit ghostChanged(ghostEnabled_);return ghostEnabled_;
}
void Viewport::setGhostOpacity(float opacity) {
    if (!std::isfinite(opacity)) return;
    ghostOpacity_=std::clamp(opacity,0.0f,1.0f);update();
}
void Viewport::drawGhost(const QMatrix4x4 &viewProjection,const QSize &pixels,float dpr) {
    if (!ghostEnabled_ || ghostPoints_.empty() || ghostOpacity_<=0) return;
    if (!ghostFramebuffer_ || ghostFramebuffer_->size()!=pixels) {
        QOpenGLFramebufferObjectFormat format;format.setInternalTextureFormat(GL_RGBA8);ghostFramebuffer_=std::make_unique<QOpenGLFramebufferObject>(pixels,format);
        if (!ghostFramebuffer_->isValid()) {emit renderFailed(tr("Could not create the ghost overlay framebuffer."));return;}
    }
    // Draw an opaque white mask first, then blend once per pixel. Dense or
    // overlapping points must not accumulate into an opaque reference layer.
    ghostFramebuffer_->bind();glViewport(0,0,pixels.width(),pixels.height());glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDepthMask(GL_FALSE);glEnable(GL_PROGRAM_POINT_SIZE);
    glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);glBindVertexArray(ghostVao_);
    if (ghostDirty_) {glBindBuffer(GL_ARRAY_BUFFER,ghostBuffer_);glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(ghostPoints_.size()*sizeof(GhostPoint)),ghostPoints_.data(),GL_STATIC_DRAW);ghostDirty_=false;}
    ghostShader_->bind();ghostShader_->setUniformValue("viewProjection",viewProjection);ghostShader_->setUniformValue("pointSize",pointSize_*dpr);glDrawArrays(GL_POINTS,0,GLsizei(ghostPoints_.size()));ghostShader_->release();
    glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());glViewport(0,0,pixels.width(),pixels.height());
    glDisable(GL_PROGRAM_POINT_SIZE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,ghostFramebuffer_->texture());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    ghostCompositeShader_->bind();ghostCompositeShader_->setUniformValue("ghostMask",0);ghostCompositeShader_->setUniformValue("outlineWidth",dpr);ghostCompositeShader_->setUniformValue("ghostOpacity",ghostOpacity_);glBindVertexArray(ghostCompositeVao_);glDrawArrays(GL_TRIANGLES,0,3);ghostCompositeShader_->release();glDisable(GL_BLEND);glDepthMask(GL_TRUE);
}
void Viewport::mousePressEvent(QMouseEvent *event) {
    lastMouse_ = event->position().toPoint(); setFocus();
    if (event->button()==Qt::LeftButton && !(event->modifiers() & Qt::ShiftModifier)) {
        const int handle = pickHandle(event->position());
        if (handle >= 0) { beginManipulation(handle,event->position()); event->accept(); return; }
    }
    QOpenGLWidget::mousePressEvent(event);
}
void Viewport::mouseMoveEvent(QMouseEvent *event) {
    const QPoint delta = event->position().toPoint() - lastMouse_; lastMouse_ = event->position().toPoint();
    if (event->buttons() & Qt::RightButton || event->buttons() & Qt::MiddleButton ||
        (event->buttons() & Qt::LeftButton && event->modifiers() & Qt::ShiftModifier)) {
        if (dragging_) { dragging_ = false; ignoreLeftUntilRelease_ = true; }
        const auto inv = viewMatrix().inverted();
        camera_.target += inv.mapVector({-float(delta.x()),float(delta.y()),0}) * (camera_.distance*0.0015f);
    } else if (event->buttons() & Qt::LeftButton) {
        if (ignoreLeftUntilRelease_) return;
        if (dragging_) { updateManipulation(event->position()); event->accept(); return; }
        orbit(-delta.x()*0.3f,delta.y()*0.3f);
    } else {
        const int handle = pickHandle(event->position());
        if (hoverHandle_ != handle) { hoverHandle_ = handle; update(); }
        if (handle >= 0) setCursor(Qt::CrossCursor); else unsetCursor();
        return;
    }
    viewCube_->setCamera(camera_); update(); emit cameraChanged();
}
void Viewport::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button()==Qt::LeftButton) {
        if (dragging_) updateManipulation(event->position());
        dragging_ = false; ignoreLeftUntilRelease_ = false; hoverHandle_ = pickHandle(event->position()); update();
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}
void Viewport::keyPressEvent(QKeyEvent *event) {
    if (handleViewKey(event->key(),event->modifiers())) { event->accept(); return; }
    if (event->key()==Qt::Key_Escape) { setTransformMode(TransformMode::None); event->accept(); return; }
    if (!event->modifiers()) {
        if (event->isAutoRepeat() && (event->key()==Qt::Key_G || event->key()==Qt::Key_R || event->key()==Qt::Key_S)) { event->accept(); return; }
        if (event->key()==Qt::Key_G) { activateTransformShortcut(TransformMode::Move); event->accept(); return; }
        if (event->key()==Qt::Key_R) { activateTransformShortcut(TransformMode::Rotate); event->accept(); return; }
        if (event->key()==Qt::Key_S) { activateTransformShortcut(TransformMode::Scale); event->accept(); return; }
    }
    QOpenGLWidget::keyPressEvent(event);
}
void Viewport::wheelEvent(QWheelEvent *event) {
    if (dragging_) { dragging_ = false; ignoreLeftUntilRelease_ = true; }
    camera_.distance = std::clamp(camera_.distance * std::pow(0.85f, event->angleDelta().y()/120.0f), 0.001f, 1e7f);
    update(); emit cameraChanged();
}

void Viewport::setTransformMode(TransformMode mode) {
    if (mode_==mode) return;
    cancelManipulation(); mode_ = mode; hoverHandle_ = -1; unsetCursor(); update();
    emit transformModeChanged(mode);
}
void Viewport::toggleTransformMode(TransformMode mode) { setTransformMode(mode_==mode ? TransformMode::None : mode); }
void Viewport::activateTransformShortcut(TransformMode mode) {
    if (mode==TransformMode::None) return;
    if (mode_!=mode) setTransformMode(mode);
    else setCoordinateSpace(mode,coordinateSpace(mode)==CoordinateSpace::Global ? CoordinateSpace::Local : CoordinateSpace::Global);
}
Transform Viewport::editableTransform() const { return cropEditing_ ? crop_.transform : transform_; }
CoordinateSpace Viewport::coordinateSpace(TransformMode mode) const {
    return mode==TransformMode::None ? CoordinateSpace::Global : spaces_[int(mode)-1];
}
void Viewport::setCoordinateSpace(TransformMode mode,CoordinateSpace space) {
    if (mode==TransformMode::None || coordinateSpace(mode)==space) return;
    cancelManipulation(); spaces_[int(mode)-1] = space; hoverHandle_ = -1; update();
}
Transform Viewport::displayedTransform() const {
    Transform result = editableTransform(); const auto world = editableParent()*result.matrix();
    if (spaces_[0]==CoordinateSpace::Global) result.position = world.column(3).toVector3D();
    if (spaces_[1]==CoordinateSpace::Global) result.rotation = Transform::fromMatrix(world).rotation;
    if (spaces_[2]==CoordinateSpace::Global) for (int i=0; i<3; ++i)
        result.scale[i] = QVector3D(world(i,0),world(i,1),world(i,2)).length();
    return result;
}
Transform Viewport::worldTransformed(const Transform &start,const QMatrix4x4 &delta) const {
    const auto parent = editableParent(); const auto pivot = parent.map(start.position);
    QMatrix4x4 around; around.translate(pivot); around *= delta; around.translate(-pivot);
    Transform result = Transform::fromMatrix(parent.inverted()*around*parent*start.matrix());
    result.position = start.position; // Rotation/scale keep the capture origin or cylinder base fixed.
    return result;
}
void Viewport::setDisplayedComponent(int group,int axis,float value) {
    cancelManipulation(); Transform target = editableTransform();
    if (spaces_[group]==CoordinateSpace::Local) {
        (group==0 ? target.position : group==1 ? target.rotation : target.scale)[axis] = value;
    } else if (group==0) {
        auto position = displayedTransform().position; position[axis] = value;
        target.position = editableParent().inverted().map(position);
    } else if (group==1) {
        Transform worldRotation; worldRotation.rotation = displayedTransform().rotation;
        const auto previous = worldRotation.rotationMatrix(); worldRotation.rotation[axis] = value;
        target = worldTransformed(target,worldRotation.rotationMatrix()*previous.inverted());
    } else {
        const float current = displayedTransform().scale[axis];
        QVector3D factor(1,1,1); factor[axis] = value/std::max(current,1e-8f);
        QMatrix4x4 delta; delta.scale(factor); target = worldTransformed(target,delta);
    }
    applyEditableTransform(target);
}
QMatrix4x4 Viewport::editableParent() const { return QMatrix4x4(); }
QVector3D Viewport::editablePivot() const { return editableParent().map(editableTransform().position); }
void Viewport::applyEditableTransform(const Transform &transform) {
    if (cropEditing_) { crop_.transform = transform; emit cropEdited(crop_); }
    else { transform_ = transform; emit transformEdited(transform); }
    update();
}
void Viewport::setCrop(const CropVolume &crop) { crop_ = crop; if (cpuFiltered_) frameDirty_=true; update(); }
void Viewport::setModifiers(const QVector<Modifier> &modifiers) {modifiers_=modifiers;modifierStack_=true;if (cpuFiltered_) frameDirty_=true;update();}
void Viewport::setCropEditing(bool enabled) {
    if (cropEditing_==enabled) return;
    cancelManipulation(); cropEditing_ = enabled; hoverHandle_ = -1; update();
}
QVector3D Viewport::axisDirection(int axis) const {
    QVector3D direction; direction[axis] = 1;
    if (coordinateSpace(mode_)==CoordinateSpace::Global) return direction;
    return editableParent().mapVector(editableTransform().rotationMatrix().mapVector(direction)).normalized();
}
float Viewport::gizmoLength() const {
    const float depth = camera_.orthographic ? camera_.distance : -viewMatrix().map(editablePivot()).z();
    return depth <= 0 ? 0 : 2*depth*std::tan(qDegreesToRadians(22.5f))*92/std::max(1,height());
}
bool Viewport::projectPoint(const QVector3D &world, QPointF *screen) const {
    const QVector4D clip = projectionMatrix()*viewMatrix()*QVector4D(world,1);
    if (clip.w() <= 0.00001f || clip.z() < -clip.w() || clip.z() > clip.w()) return false;
    *screen = QPointF((clip.x()/clip.w()+1)*width()*0.5,(1-clip.y()/clip.w())*height()*0.5);
    return true;
}
void Viewport::mouseRay(const QPointF &screen, QVector3D *origin, QVector3D *direction) const {
    const auto inverse = (projectionMatrix()*viewMatrix()).inverted();
    const float x = float(screen.x()*2/std::max(1,width())-1), y = float(1-screen.y()*2/std::max(1,height()));
    const QVector3D near = (inverse*QVector4D(x,y,-1,1)).toVector3DAffine();
    const QVector3D far = (inverse*QVector4D(x,y,1,1)).toVector3DAffine();
    *origin = near; *direction = (far-near).normalized();
}
bool Viewport::axisParameter(const QPointF &screen, const QVector3D &pivot, const QVector3D &axis, float *parameter) const {
    QVector3D origin,direction; mouseRay(screen,&origin,&direction);
    const float dot = QVector3D::dotProduct(axis,direction), denominator = 1-dot*dot;
    if (denominator < 0.01f) return false;
    const QVector3D offset = origin-pivot;
    *parameter = (QVector3D::dotProduct(axis,offset)-dot*QVector3D::dotProduct(direction,offset))/denominator;
    return std::isfinite(*parameter);
}
bool Viewport::planePoint(const QPointF &screen, const QVector3D &pivot, const QVector3D &normal, QVector3D *point) const {
    QVector3D origin,direction; mouseRay(screen,&origin,&direction);
    const float denominator = QVector3D::dotProduct(direction,normal);
    if (std::abs(denominator)<0.0001f) return false;
    const float t = QVector3D::dotProduct(pivot-origin,normal)/denominator;
    if (t<0 || !std::isfinite(t)) return false;
    *point = origin+direction*t; return true;
}
static void planeBasis(const QVector3D &axis, QVector3D *u, QVector3D *v) {
    const QVector3D reference = std::abs(axis.y())<0.9f ? QVector3D(0,1,0) : QVector3D(1,0,0);
    *u = QVector3D::crossProduct(axis,reference).normalized(); *v = QVector3D::crossProduct(axis,*u).normalized();
}
static double segmentDistance(const QPointF &point, const QPointF &a, const QPointF &b) {
    const QPointF delta = b-a; const double lengthSquared = QPointF::dotProduct(delta,delta);
    const double t = lengthSquared>1e-8 ? std::clamp(QPointF::dotProduct(point-a,delta)/lengthSquared,0.0,1.0) : 0;
    return QLineF(point,a+delta*t).length();
}
int Viewport::pickHandle(const QPointF &screen) const {
    if (mode_==TransformMode::None || !frame_ || gizmoLength()<=0) return -1;
    const float length = gizmoLength(); const QVector3D pivot = editablePivot();
    QPointF center;
    if (!projectPoint(pivot,&center)) return -1;
    if (mode_!=TransformMode::Rotate && QLineF(screen,center).length()<9) return 3;
    double bestDistance = 9; int best = -1;
    for (int axis=0; axis<3; ++axis) {
        const auto direction = axisDirection(axis);
        if (mode_==TransformMode::Rotate) {
            QVector3D u,v; planeBasis(direction,&u,&v);
            for (int i=0; i<96; ++i) {
                const float a = float(i)*float(2*M_PI/96), b = float(i+1)*float(2*M_PI/96);
                QPointF p0,p1;
                if (!projectPoint(pivot+(u*std::cos(a)+v*std::sin(a))*length,&p0) ||
                    !projectPoint(pivot+(u*std::cos(b)+v*std::sin(b))*length,&p1)) continue;
                const double distance = segmentDistance(screen,p0,p1);
                if (distance < bestDistance) { bestDistance = distance; best = axis; }
            }
        } else {
            QPointF start,end;
            if (!projectPoint(pivot+direction*length*0.18f,&start) || !projectPoint(pivot+direction*length,&end)) continue;
            const double distance = segmentDistance(screen,start,end);
            if (distance<bestDistance) { bestDistance = distance; best = axis; }
        }
    }
    return best;
}
void Viewport::drawGizmo(const QMatrix4x4 &viewProjection) {
    if (mode_==TransformMode::None || !frame_ || gizmoLength()<=0) return;
    struct Vertex { float p[3],c[3]; };
    std::vector<Vertex> lines,triangles;
    auto vertex = [](QVector3D p,QVector3D c) { return Vertex{{p.x(),p.y(),p.z()},{c.x(),c.y(),c.z()}}; };
    auto line = [&](QVector3D a,QVector3D b,QVector3D c) { lines.push_back(vertex(a,c)); lines.push_back(vertex(b,c)); };
    auto triangle = [&](QVector3D a,QVector3D b,QVector3D c,QVector3D colour) {
        triangles.push_back(vertex(a,colour)); triangles.push_back(vertex(b,colour)); triangles.push_back(vertex(c,colour));
    };
    const float length = gizmoLength(); const auto pivot = editablePivot();
    const QVector3D colours[] = {{0.95f,0.22f,0.22f},{0.3f,0.9f,0.3f},{0.25f,0.55f,1}};
    const int selected = dragging_ ? dragHandle_ : hoverHandle_;
    auto box = [&](QVector3D center,float radius,QVector3D colour) {
        QVector3D corners[8];
        for (int i=0; i<8; ++i) corners[i] = center+QVector3D((i&1)?radius:-radius,(i&2)?radius:-radius,(i&4)?radius:-radius);
        const int faces[][4] = {{0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5}};
        for (const auto &f : faces) { triangle(corners[f[0]],corners[f[1]],corners[f[2]],colour); triangle(corners[f[0]],corners[f[2]],corners[f[3]],colour); }
    };
    for (int axis=0; axis<3; ++axis) {
        const auto direction = axisDirection(axis);
        const QVector3D colour = selected==axis ? QVector3D(1,0.85f,0.15f) : colours[axis];
        QVector3D u,v; planeBasis(direction,&u,&v);
        if (mode_==TransformMode::Rotate) {
            for (int i=0; i<96; ++i) {
                const float a = float(i)*float(2*M_PI/96), b = float(i+1)*float(2*M_PI/96);
                line(pivot+(u*std::cos(a)+v*std::sin(a))*length,pivot+(u*std::cos(b)+v*std::sin(b))*length,colour);
            }
        } else {
            line(pivot,pivot+direction*length,colour);
            if (mode_==TransformMode::Scale) box(pivot+direction*length,length*0.055f,colour);
            else for (int i=0; i<16; ++i) {
                const float a = float(i)*float(2*M_PI/16), b = float(i+1)*float(2*M_PI/16);
                const auto base = pivot+direction*length*0.8f;
                triangle(pivot+direction*length,base+(u*std::cos(a)+v*std::sin(a))*length*0.055f,
                    base+(u*std::cos(b)+v*std::sin(b))*length*0.055f,colour);
            }
        }
    }
    if (mode_!=TransformMode::Rotate) box(pivot,length*0.055f,selected==3 ? QVector3D(1,0.85f,0.15f) : QVector3D(0.9f,0.9f,0.9f));
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    gridShader_->bind(); gridShader_->setUniformValue("mvp",viewProjection);
    glBindVertexArray(gizmoVao_); glBindBuffer(GL_ARRAY_BUFFER,gizmoBuffer_);
    const size_t lineCount = lines.size(); lines.insert(lines.end(),triangles.begin(),triangles.end());
    glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(lines.size()*sizeof(Vertex)),lines.data(),GL_STREAM_DRAW);
    glDrawArrays(GL_LINES,0,GLsizei(lineCount)); glDrawArrays(GL_TRIANGLES,GLint(lineCount),GLsizei(triangles.size()));
    gridShader_->release();
}
void Viewport::beginManipulation(int handle,const QPointF &screen) {
    dragging_ = true; ignoreLeftUntilRelease_ = false; dragHandle_ = handle; dragStart_ = editableTransform();
    dragScreenStart_ = screen; dragPivot_ = editablePivot(); dragLength_ = gizmoLength(); dragRotationAngle_ = 0;
    if (handle<3) {
        dragAxis_ = axisDirection(handle);
        QPointF center,end; projectPoint(dragPivot_,&center); projectPoint(dragPivot_+dragAxis_*dragLength_,&end); dragScreenAxis_ = end-center;
        if (mode_==TransformMode::Rotate) {
            QVector3D point; dragConstraintValid_ = planePoint(screen,dragPivot_,dragAxis_,&point) && (point-dragPivot_).lengthSquared()>1e-12f;
            if (dragConstraintValid_) dragRotationVector_ = (point-dragPivot_).normalized();
        } else dragConstraintValid_ = axisParameter(screen,dragPivot_,dragAxis_,&dragParameter_);
    } else {
        dragPlaneNormal_ = viewMatrix().inverted().mapVector({0,0,1}).normalized();
        dragConstraintValid_ = planePoint(screen,dragPivot_,dragPlaneNormal_,&dragPlaneStart_);
    }
    setCursor(Qt::CrossCursor); update();
}
void Viewport::updateManipulation(const QPointF &screen) {
    Transform result = dragStart_;
    if (mode_==TransformMode::Rotate) {
        if (dragConstraintValid_) {
            QVector3D point;
            if (!planePoint(screen,dragPivot_,dragAxis_,&point) || (point-dragPivot_).lengthSquared()<1e-12f) return;
            const auto vector = (point-dragPivot_).normalized();
            dragRotationAngle_ += qRadiansToDegrees(std::atan2(QVector3D::dotProduct(dragAxis_,QVector3D::crossProduct(dragRotationVector_,vector)),QVector3D::dotProduct(dragRotationVector_,vector)));
            dragRotationVector_ = vector;
        } else dragRotationAngle_ = float(screen.x()-dragScreenStart_.x()-screen.y()+dragScreenStart_.y())*0.5f;
        if (coordinateSpace(mode_)==CoordinateSpace::Local) result = dragStart_.rotatedLocal(dragHandle_,dragRotationAngle_);
        else { QMatrix4x4 delta; delta.rotate(dragRotationAngle_,dragAxis_); result = worldTransformed(dragStart_,delta); }
    } else if (dragHandle_==3) {
        if (mode_==TransformMode::Move) {
            QVector3D point; if (!dragConstraintValid_ || !planePoint(screen,dragPivot_,dragPlaneNormal_,&point)) return;
            result.position += editableParent().inverted().mapVector(point-dragPlaneStart_);
        } else {
            const float factor = std::exp(std::clamp(float(screen.x()-dragScreenStart_.x()-screen.y()+dragScreenStart_.y())*0.01f,-12.0f,12.0f));
            result.scale *= factor;
        }
    } else {
        float distance = 0;
        if (dragConstraintValid_) {
            float parameter;
            if (!axisParameter(screen,dragPivot_,dragAxis_,&parameter)) return;
            distance = parameter-dragParameter_;
        } else {
            const double lengthSquared = QPointF::dotProduct(dragScreenAxis_,dragScreenAxis_);
            if (lengthSquared<1e-5) return;
            distance = float(QPointF::dotProduct(screen-dragScreenStart_,dragScreenAxis_)/lengthSquared)*dragLength_;
        }
        if (mode_==TransformMode::Move) result.position += editableParent().inverted().mapVector(dragAxis_*distance);
        else {
            const float factor = std::exp(std::clamp(distance/dragLength_*2,-12.0f,12.0f));
            if (coordinateSpace(mode_)==CoordinateSpace::Local) result.scale[dragHandle_] *= factor;
            else { QVector3D scale(1,1,1); scale[dragHandle_] = factor; QMatrix4x4 delta; delta.scale(scale); result = worldTransformed(dragStart_,delta); }
        }
    }
    for (int axis=0; axis<3; ++axis) {
        result.position[axis] = std::clamp(result.position[axis],-1e6f,1e6f);
        result.scale[axis] = std::clamp(result.scale[axis],0.0001f,10000.0f);
    }
    applyEditableTransform(result);
}
void Viewport::cancelManipulation() {
    if (!dragging_) return;
    dragging_ = false; ignoreLeftUntilRelease_ = true; applyEditableTransform(dragStart_);
}
void Viewport::resizeEvent(QResizeEvent *event) {
    QOpenGLWidget::resizeEvent(event); viewCube_->move(width()-viewCube_->width()-12,12); viewCube_->raise();
}
void Viewport::orbit(float yawDelta,float pitchDelta) {
    camera_.preset = ViewPreset::Free; camera_.orthographic = false;
    camera_.yaw = std::remainder(camera_.yaw+yawDelta,360.0f);
    camera_.pitch = std::clamp(camera_.pitch+pitchDelta,-89.0f,89.0f);
    viewCube_->setCamera(camera_); update();
}
void Viewport::setViewPreset(ViewPreset preset,bool orthographic) {
    cancelManipulation(); camera_.preset = preset; camera_.orthographic = preset!=ViewPreset::Free && orthographic; camera_.roll = 0;
    camera_.pitch = 0;
    switch (preset) {
    case ViewPreset::Front: camera_.yaw = 0; break;
    case ViewPreset::Back: camera_.yaw = 180; break;
    case ViewPreset::Left: camera_.yaw = -90; break;
    case ViewPreset::Right: camera_.yaw = 90; break;
    case ViewPreset::Top: camera_.yaw = 0; camera_.pitch = 90; break;
    case ViewPreset::Bottom: camera_.yaw = 0; camera_.pitch = -90; break;
    case ViewPreset::Free: camera_.yaw = 25; camera_.pitch = 12; break;
    }
    viewCube_->setCamera(camera_); update(); emit cameraChanged();
}
bool Viewport::handleViewKey(int key,Qt::KeyboardModifiers modifiers) {
    if (!(modifiers & Qt::KeypadModifier) || (modifiers & (Qt::AltModifier | Qt::MetaModifier))) return false;
    switch (key) { // Preserve the physical numpad layout with Num Lock off.
    case Qt::Key_End: key = Qt::Key_1; break;
    case Qt::Key_Down: key = Qt::Key_2; break;
    case Qt::Key_PageDown: key = Qt::Key_3; break;
    case Qt::Key_Left: key = Qt::Key_4; break;
    case Qt::Key_Clear: key = Qt::Key_5; break;
    case Qt::Key_Right: key = Qt::Key_6; break;
    case Qt::Key_Home: key = Qt::Key_7; break;
    case Qt::Key_Up: key = Qt::Key_8; break;
    case Qt::Key_PageUp: key = Qt::Key_9; break;
    case Qt::Key_Insert: key = Qt::Key_0; break;
    default: break;
    }
    if (modifiers & Qt::ShiftModifier) {
        if ((key!=Qt::Key_4 && key!=Qt::Key_6) || (modifiers & Qt::ControlModifier)) return false;
        camera_.preset = ViewPreset::Free; camera_.orthographic = false; camera_.pitch = std::clamp(camera_.pitch,-89.0f,89.0f);
        camera_.roll = std::remainder(camera_.roll+(key==Qt::Key_4 ? -15 : 15),360.0f);
        viewCube_->setCamera(camera_); update(); emit cameraChanged(); return true;
    }
    const bool opposite = modifiers & Qt::ControlModifier;
    if (key==Qt::Key_1) { setViewPreset(opposite ? ViewPreset::Back : ViewPreset::Front); return true; }
    if (key==Qt::Key_3) { setViewPreset(opposite ? ViewPreset::Left : ViewPreset::Right); return true; }
    if (key==Qt::Key_7) { setViewPreset(opposite ? ViewPreset::Bottom : ViewPreset::Top); return true; }
    if (key==Qt::Key_5) {
        if (camera_.preset!=ViewPreset::Free) { cancelManipulation(); camera_.orthographic = !camera_.orthographic; viewCube_->setCamera(camera_); update(); emit cameraChanged(); }
        return true;
    }
    if (key==Qt::Key_9) {
        switch (camera_.preset) {
        case ViewPreset::Front: setViewPreset(ViewPreset::Back,camera_.orthographic); break;
        case ViewPreset::Back: setViewPreset(ViewPreset::Front,camera_.orthographic); break;
        case ViewPreset::Left: setViewPreset(ViewPreset::Right,camera_.orthographic); break;
        case ViewPreset::Right: setViewPreset(ViewPreset::Left,camera_.orthographic); break;
        case ViewPreset::Top: setViewPreset(ViewPreset::Bottom,camera_.orthographic); break;
        case ViewPreset::Bottom: setViewPreset(ViewPreset::Top,camera_.orthographic); break;
        case ViewPreset::Free: orbit(180,-2*camera_.pitch); emit cameraChanged(); break;
        }
        return true;
    }
    if (key==Qt::Key_2 || key==Qt::Key_4 || key==Qt::Key_6 || key==Qt::Key_8) {
        if (opposite) {
            const float x = key==Qt::Key_4 ? -1 : key==Qt::Key_6 ? 1 : 0;
            const float y = key==Qt::Key_2 ? -1 : key==Qt::Key_8 ? 1 : 0;
            camera_.target += viewMatrix().inverted().mapVector({x,y,0})*(camera_.distance*0.05f);
        } else orbit(key==Qt::Key_4 ? -15 : key==Qt::Key_6 ? 15 : 0,key==Qt::Key_2 ? -15 : key==Qt::Key_8 ? 15 : 0);
        update(); emit cameraChanged(); return true;
    }
    if (key==Qt::Key_Plus || key==Qt::Key_Minus) {
        camera_.distance = std::clamp(camera_.distance*(key==Qt::Key_Plus ? 0.85f : 1.0f/0.85f),0.001f,1e7f);
        update(); emit cameraChanged(); return true;
    }
    if (key==Qt::Key_Period || key==Qt::Key_Comma || key==Qt::Key_Delete) { emit frameRequested(); return true; }
    if (key==Qt::Key_0) { setViewPreset(ViewPreset::Free); return true; }
    return false;
}
void Viewport::drawCrop(const QMatrix4x4 &viewProjection) {
    if (!cropEditing_) return;
    struct Vertex { float p[3],c[3]; }; std::vector<Vertex> lines;
    const auto m = crop_.transform.matrix();
    auto line = [&](QVector3D a,QVector3D b) {
        for (const auto &point : {a,b}) { const auto p = m.map(point); lines.push_back({{p.x(),p.y(),p.z()},{240.f/255,60.f/255,90.f/255}}); }
    };
    if (crop_.shape==CropShape::Box) {
        QVector3D corners[8];
        for (int i=0; i<8; ++i) corners[i] = QVector3D((i&1)?crop_.width*0.5f:-crop_.width*0.5f,(i&2)?crop_.height:0.0f,(i&4)?crop_.depth*0.5f:-crop_.depth*0.5f);
        for (int i=0; i<8; ++i) for (int bit : {1,2,4}) if (!(i&bit)) line(corners[i],corners[i|bit]);
    } else for (int i=0; i<96; ++i) {
        const float a = float(i)*float(2*M_PI/96), b = float(i+1)*float(2*M_PI/96);
        const float x = crop_.radius*std::cos(a), z = crop_.radius*std::sin(a);
        for (float y : {0.0f,crop_.height}) line({x,y,z},{crop_.radius*std::cos(b),y,crop_.radius*std::sin(b)});
        if (i%8==0) line({x,0,z},{x,crop_.height,z});
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); gridShader_->bind(); gridShader_->setUniformValue("mvp",viewProjection);
    glBindVertexArray(gizmoVao_); glBindBuffer(GL_ARRAY_BUFFER,gizmoBuffer_);
    glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(lines.size()*sizeof(Vertex)),lines.data(),GL_STREAM_DRAW);
    glDrawArrays(GL_LINES,0,GLsizei(lines.size())); gridShader_->release();
}
