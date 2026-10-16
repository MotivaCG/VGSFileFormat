// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#include "mainwindow.h"
#include "viewport.h"
#include "exporttask.h"
#include "rangeslider.h"
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainterPath>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <QScopedValueRollback>
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString text(const QJsonObject &a,const char *key) {
    if (!a[key].isString() || a[key].toString().trimmed().isEmpty()) fail(QString("%1 must be a nonempty string.").arg(key));
    return a[key].toString();
}
double number(const QJsonObject &a,const char *key,double lo,double hi) {
    const auto v=a[key];const double n=v.toDouble();
    if (!v.isDouble() || !std::isfinite(n) || n<lo || n>hi) fail(QString("%1 must be between %2 and %3.").arg(key).arg(lo).arg(hi));
    return n;
}
int integer(const QJsonObject &a,const char *key,int lo,int hi) {
    const double n=number(a,key,lo,hi);if (n!=std::floor(n)) fail(QString("%1 must be an integer.").arg(key));return int(n);
}
bool flag(const QJsonObject &a,const char *key,bool fallback=false) {
    if (!a.contains(key)) return fallback;
    if (!a[key].isBool()) fail(QString("%1 must be a boolean.").arg(key));return a[key].toBool();
}
QJsonArray vec(const QVector3D &v) {return {v.x(),v.y(),v.z()};}
// Unknown keys are errors, so a misspelled parameter cannot silently succeed.
QJsonObject merge(QJsonObject base,const QJsonObject &patch) {
    for (auto it=patch.begin();it!=patch.end();++it) {
        if (!base.contains(it.key())) fail("Unknown parameter: "+it.key());
        if (it.value().isObject()) {
            if (!base[it.key()].isObject()) fail("Expected a scalar parameter: "+it.key());
            base[it.key()]=merge(base[it.key()].toObject(),it.value().toObject());
        } else base[it.key()]=it.value();
    }
    return base;
}
}

QString MainWindow::automationViewId(const QString &connectionId) const {
    auto root=project_.json(projectPath_);
    const auto camera=viewport_->camera();
    root["actualCamera"]=QJsonObject{{"target",vec(camera.target)},{"yaw",camera.yaw},{"pitch",camera.pitch},{"roll",camera.roll},
        {"distance",camera.distance},{"preset",int(camera.preset)},{"orthographic",camera.orthographic}};
    root["viewport"]=QJsonArray{viewport_->width(),viewport_->height(),viewport_->devicePixelRatioF()};
    root["render"]=renderStyle_->currentIndex();root["sh"]=viewport_->splatShDegree();
    root["pointSize"]=project_.pointSize;
    root["background"]=viewport_->lightBackground();root["cropEditing"]=viewport_->cropEditing();
    root["ghost"]=QJsonArray{viewport_->ghostEnabled(),viewport_->ghostOpacity(),viewport_->ghostTime()};
    root["eraseEditing"]=eraseEditing_;root["frameRevision"]=QString::number(viewport_->frameRevision());
    root["connection"]=connectionId;
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(root).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
void MainWindow::requireAutomationReady() const {
    if (!loaded_) fail("Import a capture or open a project first.");
    if (loading_ || decoding_ || pendingDecode_) fail("Editor is busy. Wait for ready=true.");
    if (playback_.isActive()) fail("Pause playback before editing or taking a snapshot.");
}
QJsonObject MainWindow::automationState(const QString &connectionId) const {
    const auto &w=*this;const auto frame_=viewport_->frame();const auto camera=w.viewport_->camera();QJsonArray boundaries;
    for (double s:w.info_.chunkBoundaries) boundaries.append(QJsonObject{{"seconds",s},{"frame",s*w.info_.fps}});
    QJsonObject result{{"session_id",connectionId.section(':',0,0)},{"loaded",w.loaded_},{"loading",w.loading_},{"processing",w.decoding_ || w.pendingDecode_},
        {"ready",w.loaded_ && !w.loading_ && !w.decoding_ && !w.pendingDecode_ && !w.playback_.isActive()},
        {"playing",w.playback_.isActive()},{"error",w.automationError_},{"modified",w.isWindowModified()},
        {"asset",w.project_.asset},{"project_path",w.projectPath_},{"frame",w.currentFrame()},{"seconds",w.project_.time},
        {"start_frame",int(std::round(w.project_.in*w.info_.fps))},{"end_frame",int(std::round(w.project_.out*w.info_.fps))},
        {"chunk",w.screenChunk_},{"frames",w.info_.frames},{"fps",w.info_.fps},{"duration",w.info_.duration},
        {"chunk_count",w.loaded_ ? w.info_.chunkBoundaries.size()+1 : 0},
        {"bounds",QJsonObject{{"minimum",vec(w.info_.minimum)},{"maximum",vec(w.info_.maximum)}}},
        {"chunk_boundaries",boundaries},{"selected_modifier",w.project_.selectedModifier},{"modifiers",w.project_.modifierJson()},
        {"render_mode",w.viewport_->splatRendering() ? "splats" : "points"},{"point_size",w.project_.pointSize},
        {"erase_editing",w.eraseEditing_},{"crop_editing",w.viewport_->cropEditing()},
        {"display",QJsonObject{{"sh_degree",w.viewport_->splatShDegree()<0 ? QJsonValue("all") : QJsonValue(w.viewport_->splatShDegree())},
            {"background",w.viewport_->lightBackground() ? "light" : "dark"}}},{"view_id",automationViewId(connectionId)},{"can_undo",!w.undo_.isEmpty()},{"can_redo",!w.redo_.isEmpty()},
        {"camera",QJsonObject{{"target",vec(camera.target)},{"yaw",camera.yaw},{"pitch",camera.pitch},{"roll",camera.roll},
            {"distance",camera.distance},{"preset",int(camera.preset)},{"orthographic",camera.orthographic}}}};
    if (frame_ && w.loaded_) result["decoded"]=QJsonObject{{"records",double(frame_->total)},{"active_points",double(frame_->points.size())},{"seconds",frame_->seconds}};
    return result;
}
void MainWindow::automationChanged() {
    auto &w=*this;w.automationError_.clear();
    w.undoOpen_=false;w.viewport_->setTransformMode(TransformMode::None);w.viewport_->setCropEditing(false);
    w.project_.showCropsAtFrame(w.currentFrame());w.viewport_->setTransform(w.project_.transformAtFrame(w.currentFrame()));
    w.viewport_->setCrop(w.project_.crop());w.syncUi();w.dirty();w.undoOpen_=false;
    w.requestFrame();
}
void MainWindow::validateAutomationProject(const QJsonObject &root,Project *result) const {
    QString error;
    const QString base=projectPath_.isEmpty() ? QDir::currentPath() : QFileInfo(projectPath_).absolutePath();
    if (!Project::fromJson(root,base,result,&error)) fail(error);
}
QJsonObject MainWindow::automationCommand(const QString &method,const QJsonObject &a,const QString &connectionId) {
    if (closePending_) fail("The editor is closing.");
    auto &w=*this;
    const QScopedValueRollback<bool> origin(remoteCommand_,true);
    auto state=[this,&connectionId] {return automationState(connectionId);};
    if (method=="state") return state();
    if (method=="pause") {w.play(false);return state();}
    if (method=="open") {
        QString error;
        if (!w.openPath(text(a,"path"),Interaction::Automation,flag(a,"discard_unsaved"),&error)) fail(error);
        return state();
    }
    if (method=="recent_files") {
        // File > Open recent, most recent first; a missing file stays listed, as in the menu.
        QJsonArray files;for (const auto &path:w.history_.recent()) files.append(QJsonObject{{"path",path},{"exists",QFileInfo(path).isFile()}});
        return {{"files",files}};
    }
    if (method=="presets") {
        QJsonArray entries;for (const auto &p:w.presetStore_.list()) entries.append(QJsonObject{{"name",p.name},{"path",p.path},{"system",p.system}});
        return {{"presets",entries}};
    }
    requireAutomationReady();
    if (method!="project" && method!="snapshot" && method!="sample_points") {
        w.automationError_.clear();
    }
    if (method=="project") {auto copy=w.project_;copy.camera=w.viewport_->camera();return copy.json(w.projectPath_);}
    if (method=="seek") {
        const int frame=integer(a,"frame",0,std::max(0,w.info_.frames-1));
        if (frame<w.project_.in*w.info_.fps-1e-6 || frame>w.project_.out*w.info_.fps+1e-6) fail("Frame is outside Start/End. Change the range first.");
        w.automationError_.clear();w.setTime(frame/w.info_.fps,false);return state();
    }
    if (method=="range") {
        const int first=integer(a,"start",0,w.info_.frames-1),last=integer(a,"end",first,w.info_.frames-1);
        w.project_.in=first/w.info_.fps;w.project_.out=last/w.info_.fps;w.project_.time=std::clamp(w.project_.time,w.project_.in,w.project_.out);automationChanged();return state();
    }
    if (method=="display") {
        const QString mode=text(a,"mode");if (mode!="points" && mode!="splats") fail("mode must be points or splats.");
        const double size=a.contains("point_size") ? number(a,"point_size",1,12) : w.project_.pointSize;
        w.renderStyle_->setCurrentIndex(mode=="splats" ? 1 : 0);w.pointSize_->setValue(size);return state();
    }
    if (method=="view") {
        const QString preset=text(a,"preset");const QStringList names={"perspective","front","back","left","right","top","bottom"};
        const int index=names.indexOf(preset);if (index<0) fail("Unknown view preset.");
        w.viewport_->setViewPreset(ViewPreset(index));if (flag(a,"focus",true)) w.viewport_->focusVisible();return state();
    }
    if (method=="camera") {
        auto root=w.project_.json(w.projectPath_);root["camera"]=merge(state()["camera"].toObject(),a);
        Project parsed;validateAutomationProject(root,&parsed);w.viewport_->setCamera(parsed.camera);w.project_.camera=parsed.camera;return state();
    }
    if (method=="snapshot") {
        const int maxWidth=a.contains("max_width") ? integer(a,"max_width",256,2048) : 1440;
        if (!w.viewport_->isValid()) fail("Viewport has no OpenGL context. Open the editor on a desktop with OpenGL support.");
        QImage image=w.viewport_->grabFramebuffer();if (image.isNull()) fail("Could not capture the viewport.");
        if (image.width()>maxWidth) image=image.scaledToWidth(maxWidth,Qt::SmoothTransformation);
        QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);if (!image.save(&buffer,"PNG")) fail("Cannot encode preview.");
        QJsonObject result{{"png",QString::fromLatin1(png.toBase64())},{"width",image.width()},{"height",image.height()},
            {"view_id",automationViewId(connectionId)},{"frame",w.currentFrame()},{"chunk",w.screenChunk_},{"render_mode",w.viewport_->splatRendering() ? "splats" : "points"},
            {"coordinates","Normalised viewport coordinates: x,y in [0,1], origin at the image's top left. Brush radius is a fraction of the shorter side."}};
        // Orthographic named views measure: the image centre is the camera target and every pixel
        // is the same size, so distances and heights can be read off the image.
        const auto camera=w.viewport_->camera();
        if (camera.orthographic && camera.preset!=ViewPreset::Free) {
            const auto view=camera.viewMatrix();
            result["scale"]=QJsonObject{{"pixels_per_metre",image.height()/(2*camera.distance*std::tan(qDegreesToRadians(22.5)))},
                {"image_centre_world",vec(camera.target)},{"image_right_world",vec(view.row(0).toVector3D())},{"image_up_world",vec(view.row(1).toVector3D())}};
        }
        return result;
    }
    if (method=="sample_points") {
        auto result=w.viewport_->samplePoints(a.contains("limit") ? integer(a,"limit",1,2000) : 200,flag(a,"include_removed"));
        result["frame"]=w.currentFrame();return result;
    }
    if (method=="add_modifier") {
        const QString type=text(a,"type");
        const std::map<QString,ModifierType> types={{"crop",ModifierType::Crop},{"remove-green",ModifierType::RemoveGreen},{"purge-isolated",ModifierType::PurgeIsolated},
            {"erase",ModifierType::Erase},{"colour",ModifierType::Colour},{"animate-transform",ModifierType::AnimateTransform},{"walk",ModifierType::Walk},
            {"audio",ModifierType::Audio},{"prune-low-contribution",ModifierType::PruneLowContribution},{"bake-antialiasing",ModifierType::BakeAntialiasing},{"loop-morph",ModifierType::LoopMorph}};
        const auto it=types.find(type);if (it==types.end()) fail("Unknown modifier type.");
        Modifier m;m.id=Project::newId();m.name=a.contains("name") ? text(a,"name") : type;
        if (m.name.size()>120) fail("Modifier name is too long.");
        m.type=it->second;m.crop.enabled=true;
        w.project_.modifiers.append(m);w.project_.selectedModifier=m.id;
        w.undoOpen_=false;
        if (m.type==ModifierType::Crop) {w.fitCrop();}
        automationChanged();auto result=state();result["modifier_id"]=m.id;return result;
    }
    if (method=="edit_modifier" || method=="remove_modifier" || method=="select_modifier" || method=="move_modifier") {
        const QString id=text(a,"modifier_id");int index=-1;
        for (int i=0;i<w.project_.modifiers.size();++i) if (w.project_.modifiers[i].id==id) {index=i;break;}
        if (index<0) fail("Unknown modifier id.");
        if (method=="edit_modifier") {
            if (!a["patch"].isObject()) fail("patch must be an object.");const auto patch=a["patch"].toObject();
            for (const char *key:{"id","type","timeline","erase"}) if (patch.contains(key)) fail(QString("Cannot patch %1; use the dedicated tools.").arg(key));
            auto root=w.project_.json(w.projectPath_);auto modifiers=root["modifiers"].toArray();
            modifiers[index]=merge(modifiers[index].toObject(),patch);root["modifiers"]=modifiers;
            Project parsed;validateAutomationProject(root,&parsed);w.project_.modifiers=parsed.modifiers;
        } else if (method=="remove_modifier") {
            w.project_.modifiers.removeAt(index);
            if (w.project_.selectedModifier==id) w.project_.selectedModifier=w.project_.modifiers.isEmpty() ? QString() : w.project_.modifiers[std::min(index,int(w.project_.modifiers.size()-1))].id;
        } else if (method=="move_modifier") {
            const int position=integer(a,"position",0,w.project_.modifiers.size()-1);w.project_.modifiers.move(index,position);
        } else {w.project_.selectedModifier=id;w.viewport_->setCropEditing(false);w.syncUi();return state();}
        automationChanged();return state();
    }
    if (method=="display_options") {
        // Display only, like the SH selector and the view cube's background: nothing is exported.
        if (a.contains("sh_degree")) {
            const QJsonValue v=a["sh_degree"];
            if (v.isString() && v.toString()=="all") w.splatSh_->setCurrentIndex(w.splatSh_->count()-1);
            else w.splatSh_->setCurrentIndex(w.splatSh_->findData(integer(a,"sh_degree",0,3)));
        }
        if (a.contains("background")) {
            const QString background=text(a,"background");if (background!="dark" && background!="light") fail("background must be dark or light.");
            w.viewport_->setLightBackground(background=="light");
        }
        return state();
    }
    if (method=="crop_editing") {
        // The selected crop's wire volume, with what it removes shown as the editor's preview
        // chooses (red by default); no transform gizmo, which would hide the image.
        const bool enabled=flag(a,"enabled");
        if (enabled && (!w.project_.modifier() || w.project_.modifier()->type!=ModifierType::Crop)) fail("Select a Crop modifier first.");
        w.editCrop(enabled);w.viewport_->setTransformMode(TransformMode::None);return state();
    }
    if (method=="erase_editing") {w.setEraseEditing(flag(a,"enabled"));return state();}
    if (method=="stroke") {
        if (text(a,"view_id")!=automationViewId(connectionId)) fail("Stale view: camera, frame, chunk or scene changed. Take a new snapshot before painting.");
        const auto *m=w.project_.modifier();if (!m || m->type!=ModifierType::Erase || !m->enabled || !w.eraseEditing_) fail("Select an enabled Erase modifier and turn Erase editing on before taking the snapshot.");
        const QString tool=text(a,"tool"),mode=text(a,"mode");
        if (tool!="brush" && tool!="lasso") fail("tool must be brush or lasso.");
        if (mode!="add" && mode!="subtract" && mode!="replace") fail("mode must be add, subtract or replace.");
        const auto points=a["points"].toArray();
        if (!a["points"].isArray() || points.isEmpty() || points.size()>4096 || (tool=="lasso" && points.size()<3)) fail("Give 1..4096 brush points, or at least 3 lasso points.");
        QPainterPath path;
        for (int i=0;i<points.size();++i) {
            const auto p=points[i].toArray();if (p.size()!=2) fail("Each point must be [x,y].");
            const QJsonObject xy{{"x",p[0]},{"y",p[1]}};
            const QPointF at(number(xy,"x",0,1)*w.viewport_->width(),number(xy,"y",0,1)*w.viewport_->height());
            if (i==0) path.moveTo(at);else path.lineTo(at);
        }
        QPainterPath region;
        if (tool=="lasso") {path.closeSubpath();region=path;}
        else region=Viewport::brushRegion(path,float(number(a,"radius",0.001,0.25)*std::min(w.viewport_->width(),w.viewport_->height())));
        const auto records=w.viewport_->recordsInside(region);
        const int operation=int(mode=="replace" ? Viewport::SelectMode::Replace : mode=="subtract" ? Viewport::SelectMode::Subtract : Viewport::SelectMode::Add);
        w.undoOpen_=false;w.automationError_.clear();w.applyStroke(records,operation);w.undoOpen_=false;
        auto result=state();result["picked_records"]=double(records.size());return result;
    }
    if (method=="clear_erased") {
        auto *m=w.project_.modifier();if (!m || m->type!=ModifierType::Erase) fail("Select an Erase modifier.");
        if (flag(a,"all_chunks")) m->erased.clear();else m->erased.erase(w.screenChunk_);automationChanged();return state();
    }
    if (method=="undo" || method=="redo") {
        w.stepUndo(method=="redo");return state();
    }
    if (method=="apply_preset") {
        const QString name=text(a,"name");QString path;
        for (const auto &entry:w.presetStore_.list()) if (entry.name==name) {path=entry.path;break;}
        if (path.isEmpty()) fail("Unknown preset name.");
        EditorPreset preset;QString error;if (!w.presetStore_.read(path,PresetScope::Editor,&preset,&error)) fail(error);
        w.undoOpen_=false;w.applyPresetSettings(preset.settings);w.undoOpen_=false;return state();
    }
    if (method=="transform") {
        auto root=w.project_.json(w.projectPath_);root["transform"]=merge(root["transform"].toObject(),a);
        Project parsed;validateAutomationProject(root,&parsed);w.project_.transform=parsed.transform;automationChanged();return state();
    }
    if (method=="export_settings") {
        auto root=w.project_.json(w.projectPath_);root["captureSettings"]=merge(root["captureSettings"].toObject(),a);
        Project parsed;validateAutomationProject(root,&parsed);w.project_.captureSettings=parsed.captureSettings;automationChanged();return state();
    }
    if (method=="save") {
        const QString path=QFileInfo(text(a,"path")).absoluteFilePath();
        if (QFileInfo(path).suffix().toLower()!="vgsproj") fail("Save to a .vgsproj file.");
        if (QFileInfo(path).exists() && !flag(a,"overwrite") && QFileInfo(path).absoluteFilePath()!=w.projectPath_) fail("Destination exists. Set overwrite=true explicitly.");
        QString error;if (!w.saveTo(path,&error)) fail(error);
        return state();
    }
    fail("Unknown editor operation: "+method);
}

ExportTask MainWindow::prepareAutomationExport(const QString &path, bool overwrite) {
    if (closePending_) fail("The editor is closing.");
    requireAutomationReady();
    const QString output=QFileInfo(path).absoluteFilePath();const QString suffix=QFileInfo(output).suffix().toLower();
    if (suffix!="vgs" && suffix!="pgs" && suffix!="mint") fail("Export to .vgs, .pgs or .mint.");
    if (QFileInfo(output).exists() && !overwrite) fail("Destination exists. Set overwrite=true explicitly.");
    ExportTask task;task.output=output;task.project=project_;task.project.camera=viewport_->camera();task.project.captureSettings.plain=suffix=="pgs";
    task.thumbnail=viewport_->isValid() ? thumbnailJpeg() : QByteArray();task.frames=exportFrameCount(task.project,info_.fps);
    const QString error=checkExportTask(task);if (!error.isEmpty()) fail(error);
    return task;
}

void MainWindow::automationInspect(const QJsonObject &a,std::function<void(const QJsonObject &)> reply) {
    if (closePending_) fail("The editor is closing.");
    requireAutomationReady();
    InspectionOptions options;options.render=0;
    options.motion=flag(a,"include_motion");options.showRemoved=flag(a,"show_removed");
    if (a.contains("render")) {
        const QString render=text(a,"render");
        if (render=="points") options.render=0;
        else if (render=="splats") options.render=1;
        else if (render=="current") options.render=-1;
        else fail("render must be points, splats or current.");
    }
    if (a.contains("motion_frames")) options.motionFrames=integer(a,"motion_frames",2,8);
    if (a.contains("target")) {
        const auto v=a["target"].toArray();if (!a["target"].isArray() || v.size()!=3) fail("target must be [x,y,z].");
        const QJsonObject xyz{{"x",v[0]},{"y",v[1]},{"z",v[2]}};
        options.target=QVector3D(float(number(xyz,"x",-1e6,1e6)),float(number(xyz,"y",-1e6,1e6)),float(number(xyz,"z",-1e6,1e6)));
    }
    if (a.contains("distance")) options.distance=number(a,"distance",0.05,1e4);
    const QScopedValueRollback<bool> origin(remoteCommand_,true);
    inspect(options,[reply](const Inspection &inspection,const QString &error) {
        if (!error.isEmpty()) {reply({{"ok",false},{"error",error}});return;}
        auto png=[](const QImage &image) {QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return QString::fromLatin1(bytes.toBase64());};
        QJsonObject result{{"meta",inspection.meta},{"current",png(inspection.current)},{"survey",png(inspection.survey)}};
        if (!inspection.motion.isNull()) result["motion"]=png(inspection.motion);
        reply({{"ok",true},{"result",result}});
    });
}
