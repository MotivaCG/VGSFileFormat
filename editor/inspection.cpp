// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
//
// Inspection images: the view on screen, a survey of nine views of the frame on screen and
// a motion strip of top and front views across Start/End. The assistant gets them through
// MCP; the user gets them composed in one image from Export > Inspection image.
#include "mainwindow.h"
#include "viewport.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
const double tangent=std::tan(qDegreesToRadians(22.5)); // half the 45 degree field of view
constexpr int SurveyCell=360, MotionCell=280;
QJsonArray vec(const QVector3D &v) {return {v.x(),v.y(),v.z()};}
Camera orthographic(ViewPreset preset,const QVector3D &target,double distance) {
    Camera c;c.target=target;c.distance=float(distance);c.preset=preset;c.orthographic=true;c.roll=0;c.pitch=0;
    switch (preset) {
    case ViewPreset::Back: c.yaw=180;break;
    case ViewPreset::Left: c.yaw=-90;break;
    case ViewPreset::Right: c.yaw=90;break;
    case ViewPreset::Top: c.yaw=0;c.pitch=90;break;
    default: c.yaw=0;break;
    }
    return c;
}
Camera perspective(float yaw,float pitch,const QVector3D &target,double distance) {
    Camera c;c.target=target;c.distance=float(distance);c.yaw=yaw;c.pitch=pitch;c.roll=0;c.preset=ViewPreset::Free;c.orthographic=false;
    return c;
}
// A viewport grab in plain pixels: on a scaled screen it carries that screen's ratio, which
// QPainter would otherwise shrink it by.
QImage pixels(QImage image) {image.setDevicePixelRatio(1);return image;}
// The 4:3 middle of a viewport image, where the framed subject is, at `width`.
QImage cell(const QImage &grab,int width) {
    const QImage image=pixels(grab);
    const int cropWidth=std::min(image.width(),std::max(1,int(std::lround(image.height()*4.0/3))));
    return pixels(image.copy((image.width()-cropWidth)/2,0,cropWidth,image.height()).scaled(width,width*3/4,Qt::IgnoreAspectRatio,Qt::SmoothTransformation));
}
void label(QPainter &painter,const QPoint &at,const QString &text) {
    painter.setFont(QFont("Segoe UI",9));const QRect box=painter.fontMetrics().boundingRect(text).adjusted(-5,-3,5,3);
    painter.fillRect(box.translated(at-box.topLeft()),QColor(0,0,0,190));painter.setPen(Qt::white);
    painter.drawText(at-box.topLeft()+QPoint(0,0),text);
}
}

MainWindow::InspectionFraming MainWindow::inspectionFraming(const QVector3D &minimum,const QVector3D &maximum) {
    // Every orthographic view shows the box with a margin: the image height holds the height
    // (side views) and the depth (top view), the 4:3 width holds the width and the depth.
    // Perspective views see its bounding sphere.
    const QVector3D size=maximum-minimum;InspectionFraming f;f.centre=(minimum+maximum)/2;
    const double needed=std::max({double(size.y()),double(size.z()),size.x()*0.75,size.z()*0.75})*1.15+0.1;
    f.orthographic=needed/(2*tangent);
    f.perspective=std::max(0.5,0.5*double(size.length())*1.1/std::sin(qDegreesToRadians(22.5)));
    return f;
}
QImage MainWindow::renderSurvey(const InspectionFraming &f,bool splats,bool showRemoved,QJsonArray *cells) {
    // A cube unfolded around the top view, image up being the back as in the top view; each
    // corner a perspective from that diagonal, high on the left, low on the right.
    struct View {const char *name;int row,column;bool flat;ViewPreset preset;float yaw,pitch;};
    static const View views[]={
        {"back-left high",0,0,false,ViewPreset::Free,-135,40},{"back",0,1,true,ViewPreset::Back,0,0},{"back-right low",0,2,false,ViewPreset::Free,135,-25},
        {"left",1,0,true,ViewPreset::Left,0,0},{"top",1,1,true,ViewPreset::Top,0,0},{"right",1,2,true,ViewPreset::Right,0,0},
        {"front-left high",2,0,false,ViewPreset::Free,-45,40},{"front",2,1,true,ViewPreset::Front,0,0},{"front-right low",2,2,false,ViewPreset::Free,45,-20}};
    const int height=SurveyCell*3/4;QImage sheet(SurveyCell*3,height*3,QImage::Format_RGB32);sheet.fill(QColor(20,20,20));
    QPainter painter(&sheet);
    for (const auto &v:views) {
        const Camera camera=v.flat ? orthographic(v.preset,f.centre,f.orthographic) : perspective(v.yaw,v.pitch,f.centre,f.perspective);
        const QPoint at(v.column*SurveyCell,v.row*height);
        painter.drawImage(at,cell(viewport_->sheetImage(camera,splats,showRemoved,false),SurveyCell));
        label(painter,at+QPoint(6,6),QString::fromLatin1(v.name));
        QJsonObject entry{{"view",v.name},{"row",v.row},{"column",v.column}};
        if (v.flat) {entry["pixels_per_metre"]=height/(2*f.orthographic*tangent);entry["image_centre_world"]=vec(f.centre);}
        cells->append(entry);
    }
    painter.setPen(QColor(60,60,60));
    for (int i=1;i<3;++i) {painter.drawLine(i*SurveyCell,0,i*SurveyCell,sheet.height());painter.drawLine(0,i*height,sheet.width(),i*height);}
    return sheet;
}
void MainWindow::inspect(const InspectionOptions &options,std::function<void(const Inspection &,const QString &)> done,int waited) {
    auto fail=[&](const QString &error) {done({},error);};
    if (inspection_) return fail(tr("An inspection is already running."));
    if (!loaded_ || loading_) return fail(tr("Import a capture or open a project first."));
    if (!viewport_->isValid()) return fail(tr("The viewport has no OpenGL context."));
    // Playing or just moved: pause, and start once the frame on screen is decoded.
    play(false);
    if (decoding_ || pendingDecode_) {
        if (waited>=10000) return fail(tr("The frame on screen is still being decoded. Try again."));
        QTimer::singleShot(20,this,[this,options,done,waited] {inspect(options,done,waited+20);});
        return;
    }
    QVector3D minimum,maximum;
    if (!viewport_->visibleExtent(&minimum,&maximum)) return fail(tr("Nothing is visible to inspect."));
    auto framing=inspectionFraming(minimum,maximum);
    if (options.target) framing.centre=*options.target;
    if (options.distance>0) framing.orthographic=framing.perspective=options.distance;
    const bool splats=options.render<0 ? viewport_->splatRendering() : options.render==1;
    Inspection result;
    const QImage current=pixels(viewport_->grabFramebuffer());
    result.current=current.width()>1024 ? pixels(current.scaledToWidth(1024,Qt::SmoothTransformation)) : current;
    QJsonArray cells;result.survey=renderSurvey(framing,splats,options.showRemoved,&cells);
    const auto counts=viewport_->samplePoints(1,false);
    result.meta={{"frame",currentFrame()},{"chunk",screenChunk_},{"render",splats ? "splats" : "points"},{"show_removed",options.showRemoved},
        {"active_points",counts["active_points"]},{"visible_points",counts["visible_points"]},
        {"framing",QJsonObject{{"centre",vec(framing.centre)},{"orthographic_distance",framing.orthographic},{"perspective_distance",framing.perspective},
            {"visible_minimum",vec(minimum)},{"visible_maximum",vec(maximum)}}},
        {"survey",QJsonObject{{"layout","3x3: back-left high, back, back-right low / left, top, right / front-left high, front, front-right low; top view: image up is -Z"},
            {"cell_width",SurveyCell},{"cell_height",SurveyCell*3/4},{"cells",cells}}}};
    if (!options.motion) return done(result,{});
    // Motion visits frames, so it continues as each one is decoded: a first pass measures where
    // the subject goes over the take, a second renders every frame with that one framing.
    auto run=std::make_unique<InspectionRun>();
    run->options=options;run->splats=splats;run->result=std::move(result);run->done=std::move(done);
    run->original=project_.time;run->generation=generation_;run->remote=remoteCommand_;
    run->framing=framing;run->measuring=!options.target && options.distance<=0;
    const int first=int(std::lround(project_.in*info_.fps)),last=int(std::lround(project_.out*info_.fps)),count=std::clamp(options.motionFrames,2,8);
    for (int i=0;i<count;++i) {const int frame=int(std::lround(first+(last-first)*(i+0.5)/count));if (run->frames.isEmpty() || run->frames.last()!=frame) run->frames.append(frame);}
    inspection_=std::move(run);inspection_->waited.start();
    {const QScopedValueRollback<bool> origin(remoteCommand_,inspection_->remote);setTime(inspection_->frames.first()/info_.fps,false);}
    QTimer::singleShot(0,this,&MainWindow::advanceInspection);
}
void MainWindow::advanceInspection() {
    auto &run=*inspection_;
    auto finish=[this](const QString &error) {
        auto ended=std::move(inspection_);
        if (loaded_ && generation_==ended->generation) {const QScopedValueRollback<bool> origin(remoteCommand_,ended->remote);setTime(ended->original,false);}
        ended->done(error.isEmpty() ? ended->result : Inspection{},error);
    };
    if (!loaded_ || loading_ || generation_!=run.generation || closePending_) return finish(tr("The capture changed or the editor is closing."));
    const int frame=run.frames[run.index];
    if (decoding_ || pendingDecode_ || currentFrame()!=frame) {
        if (run.waited.elapsed()>20000) return finish(tr("Timed out waiting for frame %1.").arg(frame));
        QTimer::singleShot(15,this,&MainWindow::advanceInspection);return;
    }
    if (run.measuring) {
        QVector3D minimum,maximum;
        if (viewport_->visibleExtent(&minimum,&maximum)) {
            if (!run.measured) {run.minimum=minimum;run.maximum=maximum;run.measured=true;}
            else for (int a=0;a<3;++a) {run.minimum[a]=std::min(run.minimum[a],minimum[a]);run.maximum[a]=std::max(run.maximum[a],maximum[a]);}
        }
    } else {
        // The first active crop's wire, whichever modifier is selected.
        const CropVolume shownCrop=project_.crop();
        for (const auto &m:project_.modifiers) if (m.active() && m.type==ModifierType::Crop) {viewport_->setCrop(m.crop);break;}
        const auto top=cell(viewport_->sheetImage(orthographic(ViewPreset::Top,run.framing.centre,run.framing.orthographic),run.splats,run.options.showRemoved,true),MotionCell);
        const auto front=cell(viewport_->sheetImage(orthographic(ViewPreset::Front,run.framing.centre,run.framing.orthographic),run.splats,run.options.showRemoved,true),MotionCell);
        viewport_->setCrop(shownCrop);
        run.tops.append(top);run.fronts.append(front);
        run.columns.append(QJsonObject{{"column",run.index},{"frame",frame},{"chunk",screenChunk_}});
    }
    if (++run.index==run.frames.size()) {
        if (run.measuring) {
            run.measuring=false;run.index=0;
            if (run.measured) run.framing=inspectionFraming(run.minimum,run.maximum);
        } else {
            const int height=MotionCell*3/4;QImage strip(MotionCell*run.tops.size(),height*2,QImage::Format_RGB32);strip.fill(QColor(20,20,20));
            QPainter painter(&strip);
            for (int i=0;i<run.tops.size();++i) {
                const QPoint top(i*MotionCell,0),front(i*MotionCell,height);const int f=run.frames[i];
                painter.drawImage(top,run.tops[i]);painter.drawImage(front,run.fronts[i]);
                label(painter,top+QPoint(6,6),tr("top  f%1").arg(f));label(painter,front+QPoint(6,6),tr("front  f%1").arg(f));
            }
            painter.setPen(QColor(60,60,60));
            for (int i=1;i<run.tops.size();++i) painter.drawLine(i*MotionCell,0,i*MotionCell,strip.height());
            painter.drawLine(0,height,strip.width(),height);painter.end();
            run.result.motion=strip;
            run.result.meta["motion"]=QJsonObject{{"layout","row 0: top, row 1: front; one column per frame, same framing for all; the first active crop's wire is drawn"},
                {"cell_width",MotionCell},{"cell_height",height},{"pixels_per_metre",height/(2*run.framing.orthographic*tangent)},
                {"image_centre_world",vec(run.framing.centre)},{"columns",run.columns}};
            return finish({});
        }
    }
    run.waited.restart();
    {const QScopedValueRollback<bool> origin(remoteCommand_,run.remote);setTime(run.frames[run.index]/info_.fps,false);}
    QTimer::singleShot(15,this,&MainWindow::advanceInspection);
}
QImage MainWindow::composeInspection(const Inspection &inspection) {
    // One image for people: each part under its title, centred on the widest.
    const QList<QPair<QString,QImage>> parts={{tr("View on screen"),inspection.current},{tr("Survey: nine views of frame %1").arg(inspection.meta["frame"].toInt()),inspection.survey},
        {tr("Motion: top and front across Start/End"),inspection.motion}};
    int width=0,height=0;const int title=28;
    for (const auto &[name,image]:parts) if (!image.isNull()) {width=std::max(width,image.width());height+=title+image.height();}
    QImage composed(width,height,QImage::Format_RGB32);composed.fill(QColor(20,20,20));
    QPainter painter(&composed);painter.setFont(QFont("Segoe UI",11,QFont::DemiBold));painter.setPen(Qt::white);int y=0;
    for (const auto &[name,image]:parts) {
        if (image.isNull()) continue;
        painter.drawText(QRect(10,y,width-20,title),Qt::AlignVCenter|Qt::AlignLeft,name);y+=title;
        painter.drawImage((width-image.width())/2,y,pixels(image));y+=image.height();
    }
    return composed;
}
void MainWindow::exportInspection() {
    if (!loaded_ || loading_) return;
    QString path=QFileDialog::getSaveFileName(this,tr("Export inspection image"),
        history_.savePath("Image",QFileInfo(project_.asset).completeBaseName()+"_inspection.png"),tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".png",Qt::CaseInsensitive)) path+=".png";
    exportInspectionTo(path);
}
void MainWindow::exportInspectionTo(const QString &path) {
    auto *progress=new QProgressDialog(tr("Rendering inspection views…"),QString(),0,0,this);
    progress->setObjectName("inspectionProgress");progress->setWindowTitle(tr("Export inspection image"));
    progress->setWindowModality(Qt::WindowModal);progress->setCancelButton(nullptr);progress->setMinimumDuration(0);progress->show();
    InspectionOptions options;options.motion=true;
    inspect(options,[this,path,progress](const Inspection &inspection,const QString &error) {
        progress->close();progress->deleteLater();
        if (!error.isEmpty()) {showError(error);return;}
        if (!composeInspection(inspection).save(path)) {showError(tr("Could not save the image."));return;}
        history_.rememberImage(path);statusBar()->showMessage(tr("Inspection image saved."),5000);
    });
}
