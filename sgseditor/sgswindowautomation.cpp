// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

// SgsWindow::automation: what the MCP adapter can ask of the editor (sgsautomation.h, mcp/server.py).

#include "sgswindow.h"
#include "colmapfinder.h"
#include "sgsautomation.h"
#include "viewport.h"
#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QProgressBar>
#include <cmath>
#include <stdexcept>

namespace {

[[noreturn]] void refuse(const QString &message) {throw std::runtime_error(message.toStdString());}
QString text(const QJsonObject &a,const char *key) {
    if (!a[key].isString() || a[key].toString().trimmed().isEmpty()) refuse(QStringLiteral("%1 must be a nonempty string.").arg(key));
    return a[key].toString();
}
bool flag(const QJsonObject &a,const char *key,bool fallback) {
    if (!a.contains(key)) return fallback;
    if (!a[key].isBool()) refuse(QStringLiteral("%1 must be a boolean.").arg(key));
    return a[key].toBool();
}
int integer(const QJsonObject &a,const char *key,int lo,int hi,int fallback) {
    if (!a.contains(key)) return fallback;
    const double v=a[key].toDouble(std::nan(""));
    if (!a[key].isDouble() || v!=std::floor(v) || v<lo || v>hi) refuse(QStringLiteral("%1 must be an integer in [%2, %3].").arg(key).arg(lo).arg(hi));
    return int(v);
}
double number(const QJsonObject &a,const char *key,double lo,double hi,double fallback) {
    if (!a.contains(key)) return fallback;
    const double v=a[key].toDouble(std::nan(""));
    if (!a[key].isDouble() || !(v>=lo && v<=hi)) refuse(QStringLiteral("%1 must be a number in [%2, %3].").arg(key).arg(lo).arg(hi));
    return v;
}
QJsonArray array(const QVector3D &v) {return QJsonArray{v.x(),v.y(),v.z()};}
QJsonArray strings(const QStringList &list) {QJsonArray a;for (const auto &s:list) a.append(s);return a;}
bool hasSuffix(const QString &path,const char *suffix) {return QFileInfo(path).suffix().compare(QLatin1String(suffix),Qt::CaseInsensitive)==0;}

}

QJsonObject SgsWindow::automationState() const {
    QJsonObject s;
    s["loaded"]=bool(capture_);
    s["ply"]=capture_ ? capture_->path : QString();
    s["splats"]=capture_ ? double(capture_->visible) : 0.0;
    s["project_path"]=projectPath_;
    s["modified"]=modified_;
    s["normals_estimated"]=project_.estimatedNormals;
    s["fine_normals"]=project_.fineNormals;
    s["photos"]=project_.colmap;
    s["mesh_settings"]=project_.mesh.json();
    s["mesh_files"]=strings(project_.meshFiles);
    s["mesh_levels"]=strings(project_.meshObjs()); // 0: the main mesh (its edit when edited), then the LODs
    s["mesh_edited"]=project_.meshEdited;
    s["mesh_out_of_date"]=project_.meshOutOfDate();
    s["lods_out_of_date"]=project_.lodsOutOfDate();
    const bool running=gs2mesh_->running();
    s["busy"]=meshBusy_ || running;
    s["job"]=running ? (lodsJob_ ? "lods" : "mesh") : "";
    s["stage"]=meshStageName_;
    s["progress"]=meshProgress_->value()/1000.0;
    s["last_error"]=meshError_;
    s["log_lines"]=int(meshLog_.size());
    s["show_mesh"]=showMesh_;
    s["mesh_level"]=meshLevel_;
    s["mesh_gray"]=meshGray_;
    const Camera c=viewport_->camera();
    s["camera"]=QJsonObject{{"target",array(c.target)},{"yaw",c.yaw},{"pitch",c.pitch},{"distance",c.distance},{"orthographic",c.orthographic}};
    if (capture_) s["bounds"]=QJsonObject{{"minimum",array(capture_->minimum)},{"maximum",array(capture_->maximum)}};
    return s;
}

QJsonObject SgsWindow::automation(const QString &method,const QJsonObject &a) {
    const bool running=gs2mesh_->running() || meshBusy_;
    auto idle=[&] {if (running) refuse("gs2mesh is working: wait for it (get_state) or cancel it.");};
    auto loaded=[&] {if (!capture_) refuse("No capture is open: open a .ply or a .sgsproj first.");};
    if (method=="state") return automationState();
    if (method=="log") {
        const int last=integer(a,"last",1,5000,80);
        QJsonArray lines;
        for (qsizetype i=std::max<qsizetype>(0,meshLog_.size()-last);i<meshLog_.size();++i) lines.append(meshLog_[i]);
        return QJsonObject{{"lines",lines},{"total",int(meshLog_.size())}};
    }
    if (method=="open") {
        idle();
        const QString path=text(a,"path");
        if (!QFileInfo::exists(path)) refuse("No such file: "+path);
        if (!hasSuffix(path,"ply") && !hasSuffix(path,"sgsproj")) refuse("Open a 3DGS .ply or an SGS project (.sgsproj).");
        if (modified_ && capture_ && !flag(a,"discard_unsaved",false)) refuse("The project has unsaved changes: save it, or pass discard_unsaved=true.");
        modified_=false;
        if (!openFile(path)) refuse("The editor could not open "+path+" (it said why in a dialog).");
        return automationState();
    }
    if (method=="save") {
        loaded();
        const QString path=a.contains("path") ? text(a,"path") : projectPath_;
        if (path.isEmpty()) refuse("The project has no file yet: pass path (a .sgsproj).");
        if (!hasSuffix(path,"sgsproj")) refuse("A project is saved as .sgsproj.");
        if (!saveProject(path)) refuse("The project could not be saved to "+path+".");
        return automationState();
    }
    if (method=="estimate_normals") {
        loaded();idle();
        estimateNormals();
        return automationState();
    }
    if (method=="set_photos") {
        loaded();idle();
        const QString folder=a["folder"].toString();
        if (folder.isEmpty()) project_.colmap.clear();
        else {
            const ColmapModel model=findColmap(folder);
            if (!model.usable()) refuse("gs2mesh cannot use the photos there: "+model.problem);
            project_.colmap=folder;project_.colmapDeclined=false;
        }
        syncPhotos();syncMesh();modified();
        return automationState();
    }
    if (method=="set_mesh_settings") {
        loaded();idle();
        MeshSettings m=project_.mesh;
        m.triangles=integer(a,"triangles",1000,2000000,m.triangles);
        m.textureSize=integer(a,"texture_size",256,16384,m.textureSize);
        if (m.textureSize & (m.textureSize-1)) refuse("texture_size must be a power of two.");
        m.normalMap=flag(a,"normal_map",m.normalMap);
        m.voxelMm=float(number(a,"voxel_mm",0.5,20,m.voxelMm));
        m.virtualResolution=integer(a,"virtual_resolution",256,4096,m.virtualResolution);
        m.denoise=flag(a,"denoise",m.denoise);
        if (a.contains("lods")) {
            if (!a["lods"].isArray() || a["lods"].toArray().size()>4) refuse("lods must be an array of at most 4 triangle counts.");
            QVector<int> lods;
            for (const auto &v:a["lods"].toArray()) {
                const double t=v.toDouble(-1);
                if (!v.isDouble() || t!=std::floor(t) || t<100 || t>=m.triangles) refuse("Each LOD is an integer from 100 triangles to fewer than the main mesh's.");
                lods.append(int(t));
            }
            m.lods=lods;
        }
        if (!(m==project_.mesh)) {project_.mesh=m;syncMesh();modified();}
        return automationState();
    }
    if (method=="generate_mesh") {
        loaded();idle();
        QString why;
        if (!Gs2Mesh::available(&why)) refuse("gs2mesh is not available: "+why);
        if (!project_.colmap.isEmpty()) {
            const ColmapModel model=findColmap(project_.colmap);
            if (!model.usable()) refuse("The project's photos cannot be used: "+model.problem);
        }
        meshError_.clear();quietMesh_=true;
        if (!startMesh()) {quietMesh_=false;refuse("gs2mesh did not start.");}
        return automationState();
    }
    if (method=="generate_lods") {
        loaded();idle();
        if (project_.meshObjs().isEmpty()) refuse("There is no mesh yet: generate_mesh first.");
        if (a.contains("lods")) automation("set_mesh_settings",QJsonObject{{"lods",a["lods"]}});
        if (project_.mesh.lods.isEmpty()) refuse("No LODs are asked for: pass lods.");
        if (!Gs2Mesh::makesLods()) refuse("This gs2mesh cannot make LODs of a mesh made before.");
        meshError_.clear();quietMesh_=true;
        if (!startLods()) {quietMesh_=false;refuse("gs2mesh did not start (the dense surface or the edit may be missing).");}
        return automationState();
    }
    if (method=="cancel") {
        if (gs2mesh_->running()) gs2mesh_->cancel();
        return automationState();
    }
    if (method=="set_view") {
        loaded();
        if (a.contains("show")) {
            const QString show=text(a,"show");
            if (show!="splats" && show!="mesh") refuse("show is \"splats\" or \"mesh\".");
            if (show=="mesh" && project_.meshObjs().isEmpty()) refuse("There is no mesh to show.");
            if (running && show=="mesh") refuse("gs2mesh is working.");
            setShowMesh(show=="mesh");
        }
        if (a.contains("level")) {
            const int level=integer(a,"level",0,std::max(0,int(project_.meshObjs().size())-1),meshLevel_);
            if (!loadMesh(level)) refuse("The mesh of that level could not be read.");
        }
        if (a.contains("gray")) {meshGray_=flag(a,"gray",meshGray_);viewport_->setMeshTextured(!meshGray_);}
        if (a.contains("camera")) {
            if (!a["camera"].isObject()) refuse("camera must be an object.");
            const QJsonObject c=a["camera"].toObject();
            Camera cam=viewport_->camera();
            if (c.contains("target")) {
                const QJsonArray t=c["target"].toArray();
                if (t.size()!=3) refuse("camera.target is [x, y, z].");
                cam.target=QVector3D(float(t[0].toDouble()),float(t[1].toDouble()),float(t[2].toDouble()));
            }
            cam.yaw=float(number(c,"yaw",-36000,36000,cam.yaw));
            cam.pitch=float(number(c,"pitch",-90,90,cam.pitch));
            cam.distance=float(number(c,"distance",0.001,1e6,cam.distance));
            cam.orthographic=flag(c,"orthographic",cam.orthographic);
            cam.preset=ViewPreset::Free;
            viewport_->setCamera(cam);
        }
        syncDisplay();
        return automationState();
    }
    if (method=="view_mesh_file") {
        // any mesh on disk drawn in place of the project's (a dense surface, another run's main mesh):
        // for inspection; the project's own mesh comes back with set_view(level)
        loaded();
        if (running) refuse("gs2mesh is working.");
        const QString path=text(a,"path");
        if (!QFileInfo::exists(path)) refuse("No such file: "+path);
        const MeshData data=hasSuffix(path,"obj") ? readObj(path) : hasSuffix(path,"ply") ? readPlyMesh(path) : (refuse("Show an .obj or a mesh .ply."),MeshData{});
        auto mesh=std::make_shared<Viewport::ViewMesh>();
        mesh->vertices.reserve(data.vertices()*8);
        for (size_t i=0;i<data.vertices();++i) {
            for (int k=0;k<3;++k) mesh->vertices.push_back(data.positions[i*3+k]);
            for (int k=0;k<3;++k) mesh->vertices.push_back(data.normals[i*3+k]);
            mesh->vertices.push_back(data.uvs[i*2]);mesh->vertices.push_back(data.uvs[i*2+1]);
        }
        mesh->indices=data.indices;
        if (!data.albedo.isEmpty()) mesh->albedo=QImage(data.albedo);
        mesh_=mesh;viewport_->setMesh(mesh);inspectingFile_=true;inspectedName_=QFileInfo(path).fileName();
        showMesh_=true;viewport_->setDrawMesh(true);viewport_->setDrawSplats(false);
        meshGray_=flag(a,"gray",data.albedo.isEmpty());viewport_->setMeshTextured(!meshGray_);
        syncDisplay();
        QJsonObject s=automationState();
        s["viewing"]=QJsonObject{{"path",path},{"vertices",double(data.vertices())},{"triangles",double(data.triangles())}};
        return s;
    }
    if (method=="screenshot") {
        const int width=integer(a,"width",256,2048,1024);
        QImage image=viewport_->grabFramebuffer();
        if (image.isNull()) refuse("The view could not be read.");
        if (image.width()>width) image=image.scaledToWidth(width,Qt::SmoothTransformation);
        QByteArray png;
        QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");
        return QJsonObject{{"png",QString::fromLatin1(png.toBase64())},{"width",image.width()},{"height",image.height()}};
    }
    if (method=="export") {
        loaded();idle();
        const QString path=text(a,"path");
        if (QFileInfo::exists(path) && !flag(a,"overwrite",false)) refuse(path+" exists: pass overwrite=true to replace it.");
        if (hasSuffix(path,"ply")) {exportPlyTo(path);return QJsonObject{{"path",path},{"bytes",double(QFileInfo(path).size())}};}
        if (!hasSuffix(path,"sgs")) refuse("Export to a .sgs or a .ply.");
        project_.sgsIncludesMesh=flag(a,"include_mesh",project_.sgsIncludesMesh);
        if (a.contains("mesh_level")) project_.sgsMesh=integer(a,"mesh_level",-1,16,project_.sgsMesh);
        project_.sgsTextureSize=integer(a,"texture_size",0,16384,project_.sgsTextureSize);
        const SgsExportResult result=exportSgsTo(path);
        return QJsonObject{{"path",path},{"bytes",double(result.bytes)},{"notes",strings(result.notes)},
                           {"mesh",project_.sgsIncludesMesh ? project_.sgsMeshFile() : QString()}};
    }
    refuse("Unknown request: "+method);
}
