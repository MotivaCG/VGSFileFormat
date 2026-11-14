// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).
//
// The SGS editor's checks. A sphere of flat splats stands in for a capture; SGS_TEST_PLY names
// a real one for the checks that want it, and SGS_TEST_GS2MESH=1 lets gs2mesh run on it
// (minutes, an NVIDIA GPU).

#include "colmapfinder.h"
#include "exportcapture.h"
#include "meshio.h"
#include "sgscapture.h"
#include "sgsexport.h"
#include "sgsproject.h"
#include "sgswindow.h"
#include "viewport.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QQuaternion>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <map>
#include <random>
#include <tuple>

namespace {
// Flat splats on a sphere of `radius` around `centre`, each lying in the surface (its
// shortest axis along the radius), coloured by height; their outward normals in `normals`.
vgs::Frame sphereFrame(int count,float radius,const QVector3D &centre,std::vector<float> *normals=nullptr) {
    vgs::Frame f;f.count=uint64_t(count);f.shCoefficients=3;
    std::mt19937 random(7);std::uniform_real_distribution<float> uniform(-1,1);
    for (int i=0;i<count;++i) {
        // Fibonacci points, evenly spread.
        const float y=1-2*(i+0.5f)/count,r=std::sqrt(1-y*y),phi=float(i)*2.39996323f;
        const QVector3D n(r*std::cos(phi),y,r*std::sin(phi)),p=centre+n*radius;
        const QQuaternion q=QQuaternion::rotationTo(QVector3D(0,0,1),n); // local z, the shortest axis, outwards
        f.position.insert(f.position.end(),{p.x(),p.y(),p.z()});
        f.rotation.insert(f.rotation.end(),{q.x(),q.y(),q.z(),q.scalar()});
        f.scale.insert(f.scale.end(),{0.02f,0.02f,0.002f});
        f.colorDc.insert(f.colorDc.end(),{0.5f+0.4f*y,0.5f,0.5f-0.4f*y});
        f.opacity.push_back(0.9f);f.active.push_back(1);
        for (int k=0;k<9;++k) f.shRest.push_back(0.01f*uniform(random));
        if (normals) normals->insert(normals->end(),{n.x(),n.y(),n.z()});
    }
    return f;
}
QString writeSphere(const QString &dir,const QString &name,int count=6000,bool withNormals=false) {
    std::vector<float> normals;vgs::Frame f=sphereFrame(count,0.5f,QVector3D(0,1,0),&normals);
    if (withNormals) f.normal=normals;
    const QString path=dir+"/"+name;writePly(f,path);return path;
}
double degreesBetween(const float *a,const float *b) {
    const double dot=double(a[0])*b[0]+double(a[1])*b[1]+double(a[2])*b[2];
    const double la=std::sqrt(double(a[0])*a[0]+double(a[1])*a[1]+double(a[2])*a[2]),lb=std::sqrt(double(b[0])*b[0]+double(b[1])*b[1]+double(b[2])*b[2]);
    return qRadiansToDegrees(std::acos(std::clamp(dot/std::max(la*lb,1e-30),-1.0,1.0)));
}
// A tiny OBJ: a quad of two triangles, its texture beside it.
QString writeQuadObj(const QString &dir) {
    QImage albedo(64,32,QImage::Format_RGB888);albedo.fill(QColor(200,100,50));albedo.save(dir+"/quad_albedo.png");
    QImage normal(64,32,QImage::Format_RGB888);normal.fill(QColor(128,128,255));normal.save(dir+"/quad_normal.png");
    QFile mtl(dir+"/quad.mtl");mtl.open(QIODevice::WriteOnly);mtl.write("newmtl m\nmap_Kd quad_albedo.png\nnorm quad_normal.png\n");mtl.close();
    QFile obj(dir+"/quad.obj");obj.open(QIODevice::WriteOnly);
    obj.write("mtllib quad.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n");obj.close();
    return dir+"/quad.obj";
}
// A COLMAP model of `photos` cameras on a ring of `radius` around `centre`, looking at it.
void writeColmap(const QString &dir,int photos,float radius,const QVector3D &centre,bool writeImages=true,int cameraModel=1,const QString &sparse="/sparse/0") {
    QDir().mkpath(dir+sparse);QDir().mkpath(dir+"/images");
    auto put=[](QByteArray &b,auto v) {char bytes[sizeof(v)];qToLittleEndian(v,bytes);b.append(bytes,sizeof(v));};
    QByteArray cameras;put(cameras,quint64(1));put(cameras,quint32(1));put(cameras,qint32(cameraModel));put(cameras,quint64(64));put(cameras,quint64(48));
    // PINHOLE: fx fy cx cy; OPENCV (4): those and k1 k2 p1 p2.
    for (double v:{50.0,50.0,32.0,24.0}) put(cameras,v);
    if (cameraModel==4) for (double v:{0.1,0.01,0.0,0.0}) put(cameras,v);
    QFile c(dir+sparse+"/cameras.bin");c.open(QIODevice::WriteOnly);c.write(cameras);c.close();
    QByteArray images;put(images,quint64(photos));
    for (int i=0;i<photos;++i) {
        const float a=6.2831853f*float(i)/float(photos);const QVector3D eye=centre+QVector3D(std::sin(a),0,std::cos(a))*radius;
        // World to camera: R looks from eye to centre; t = -R eye.
        const QQuaternion look=QQuaternion::fromDirection(-(centre-eye).normalized(),QVector3D(0,1,0)).conjugated();
        const QVector3D t=-look.rotatedVector(eye);
        put(images,quint32(i+1));for (float v:{look.scalar(),look.x(),look.y(),look.z()}) put(images,double(v));
        for (float v:{t.x(),t.y(),t.z()}) put(images,double(v));
        put(images,quint32(1));const QByteArray name=QStringLiteral("photo%1.jpg").arg(i).toUtf8();images.append(name);images.append('\0');put(images,quint64(0));
        if (writeImages) {QImage img(64,48,QImage::Format_RGB888);img.fill(Qt::gray);img.save(dir+"/images/"+QString::fromUtf8(name));}
    }
    QFile im(dir+sparse+"/images.bin");im.open(QIODevice::WriteOnly);im.write(images);im.close();
}
QColor average(const QImage &image,const QRect &area) {
    double r=0,g=0,b=0;int n=0;
    for (int y=area.top();y<=area.bottom();++y) for (int x=area.left();x<=area.right();++x) {const QColor c=image.pixelColor(x,y);r+=c.redF();g+=c.greenF();b+=c.blueF();++n;}
    return QColor::fromRgbF(float(r/n),float(g/n),float(b/n));
}
double luminance(const QColor &c) {return 0.2126*c.redF()+0.7152*c.greenF()+0.0722*c.blueF();}
} // namespace

class SgsEditorTests : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void readsPlyAsTheEditorDoes();
    void zeroNormalsAreNoNormals();
    void normalsPointOutwards();
    void projectRoundTripsWithRelativePaths();
    void lightAnglesRoundTrip();
    void findsAndChecksColmap();
    void objToGlb();
    void sgsCarriesSplatsNormalsAndMesh();
    void plyExportKeepsNormals();
    void lightsShadeTheSplats();
    void lightColoursAreSrgb();
    void presetsLight();
    void gizmoMovesTheSelectedLight();
    void lightsTurn();
    void spaceTogglesTurning();
    void lightIconsHideBehindSplats();
    void modelsHaveDefaults();
    void meshIsDrawnLit();
    void realCapture();
    void visualSheet();
};

void SgsEditorTests::readsPlyAsTheEditorDoes() {
    std::vector<float> normals;const vgs::Frame source=sphereFrame(500,0.5f,{0,1,0},&normals);
    const QString path=dir_.filePath("read.ply");writePly(source,path);
    const SgsPly ply=readSgsPly(path);
    QCOMPARE(ply.frame.count,source.count);QCOMPARE(ply.shDegree,1);QCOMPARE(ply.frame.shCoefficients,3);
    for (size_t i=0;i<source.position.size();++i) QVERIFY(std::abs(ply.frame.position[i]-source.position[i])<1e-6f);
    for (size_t i=0;i<source.scale.size();++i) QVERIFY(std::abs(ply.frame.scale[i]-source.scale[i])<1e-6f*std::max(1.f,source.scale[i]*1e3f));
    for (size_t i=0;i<source.colorDc.size();++i) QVERIFY(std::abs(ply.frame.colorDc[i]-source.colorDc[i])<1e-5f);
    for (size_t i=0;i<source.opacity.size();++i) QVERIFY(std::abs(ply.frame.opacity[i]-source.opacity[i])<1e-5f);
    for (size_t i=0;i<source.shRest.size();++i) QVERIFY(std::abs(ply.frame.shRest[i]-source.shRest[i])<1e-6f);
    // xyzw, the same turn (or its negative).
    for (size_t i=0;i<size_t(source.count);++i) {
        double dot=0;for (int c=0;c<4;++c) dot+=double(ply.frame.rotation[i*4+c])*source.rotation[i*4+c];
        QVERIFY(std::abs(std::abs(dot)-1)<1e-5);
    }
}
void SgsEditorTests::zeroNormalsAreNoNormals() {
    // writePly writes zeros without normals, as 3DGS training does.
    const QString path=writeSphere(dir_.path(),"zeros.ply",300,false);
    const auto c=SgsCapture::load(path);
    QVERIFY(c.frame.normal.empty());QCOMPARE(c.normals,NormalsSource::None);
    const auto withNormals=SgsCapture::load(writeSphere(dir_.path(),"normals.ply",300,true));
    QCOMPARE(withNormals.normals,NormalsSource::File);QCOMPARE(withNormals.frame.normal.size(),size_t(900));
}
void SgsEditorTests::normalsPointOutwards() {
    std::vector<float> truth;vgs::Frame f=sphereFrame(6000,0.5f,{0,1,0},&truth);
    const QString path=dir_.filePath("estimate.ply");writePly(f,path);
    auto c=SgsCapture::load(path);QString note;c.estimateNormals(&note);
    // No rig named: cameras all around.
    QCOMPARE(c.normals,NormalsSource::Surrounding);QVERIFY(note.contains("all around"));
    int within=0;for (size_t i=0;i<size_t(c.frame.count);++i) within+=degreesBetween(&c.frame.normal[i*3],&truth[i*3])<15;
    QVERIFY2(within>int(c.frame.count)*95/100,qPrintable(QStringLiteral("%1 of %2 within 15 degrees").arg(within).arg(c.frame.count)));
}
void SgsEditorTests::projectRoundTripsWithRelativePaths() {
    SgsProject p;p.ply=dir_.filePath("sub/capture.ply");p.colmap=dir_.filePath("photos");p.selfIllumination=0.25f;p.estimatedNormals=true;
    p.mesh.triangles=100000;p.mesh.lods={20000};p.mesh.textureSize=2048;
    p.meshFiles={dir_.filePath("sub/capture_mesh/capture.obj"),dir_.filePath("sub/capture_mesh/capture_lod1.obj")};p.meshInputs="abc";
    SgsLight sun=SgsLight::sun({0,1,0},1.8f);sun.colour=QColor(255,200,150);p.lights={sun,SgsLight::bulb({0,1,0},1.8f)};p.lights[1].enabled=false;
    p.sgsMesh=0;p.sgsTextureSize=1024;
    const QString projectPath=dir_.filePath("project/one.sgsproj");
    const QJsonObject json=p.json(projectPath);
    QCOMPARE(json.value("ply").toString(),QStringLiteral("../sub/capture.ply"));
    // Moved with its files: the paths follow.
    const QString moved=QDir::cleanPath(dir_.filePath("elsewhere/project/one.sgsproj"));
    const SgsProject q=SgsProject::fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object(),moved);
    QCOMPARE(q.ply,QDir::cleanPath(dir_.filePath("elsewhere/sub/capture.ply")));
    QCOMPARE(q.selfIllumination,0.25f);QVERIFY(q.estimatedNormals);QCOMPARE(q.mesh.triangles,100000);QCOMPARE(q.mesh.lods,QVector<int>{20000});
    QCOMPARE(q.lights.size(),2);QVERIFY(q.lights[0].directional);QCOMPARE(q.lights[0].colour,QColor(255,200,150));QVERIFY(!q.lights[1].enabled);
    QVERIFY((q.lights[0].direction()-p.lights[0].direction()).length()<1e-4f);
    QCOMPARE(q.meshFiles.size(),2);QCOMPARE(q.sgsMesh,0);QCOMPARE(q.sgsTextureSize,1024);
    QCOMPARE(q.sgsMeshFile(),QDir::cleanPath(dir_.filePath("elsewhere/sub/capture_mesh/capture.obj")));
    SgsProject r=q;r.sgsMesh=-1;QVERIFY(r.sgsMeshFile().endsWith("capture_lod1.obj"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,SgsProject::fromJson(QJsonObject{{"format","other"}},moved));
}
void SgsEditorTests::lightAnglesRoundTrip() {
    for (float azimuth:{-170.f,-90.f,0.f,45.f,120.f}) for (float elevation:{-30.f,0.f,35.f,80.f}) {
        SgsLight l;l.directional=true;l.setAngles(azimuth,elevation);
        float a=0,e=0;l.angles(&a,&e);
        QVERIFY2(std::abs(e-elevation)<0.05f,qPrintable(QStringLiteral("%1/%2 -> %3/%4").arg(azimuth).arg(elevation).arg(a).arg(e)));
        float da=std::fmod(std::abs(a-azimuth),360.f);da=std::min(da,360-da);QVERIFY(da<0.05f);
        // The light travels away from where it comes from.
        const QVector3D from(std::cos(qDegreesToRadians(elevation))*std::sin(qDegreesToRadians(azimuth)),std::sin(qDegreesToRadians(elevation)),std::cos(qDegreesToRadians(elevation))*std::cos(qDegreesToRadians(azimuth)));
        QVERIFY((l.direction()+from).length()<1e-3f);
    }
    // The default sun lights the front (+Z) from above.
    const SgsLight sun=SgsLight::sun({0,1,0},1.8f);QVERIFY(sun.direction().y()<-0.3f);QVERIFY(sun.direction().z()<0);
}
void SgsEditorTests::findsAndChecksColmap() {
    const QString good=dir_.filePath("colmap_good");writeColmap(good,12,3.f,{0,1,0});
    auto m=findColmap(good);QVERIFY2(m.usable(),qPrintable(m.problem));
    QCOMPARE(m.photos,12);QCOMPARE(m.cameras,1);QCOMPARE(QDir::cleanPath(m.images),QDir::cleanPath(good+"/images"));
    for (const auto &c:m.centres) QVERIFY(std::abs((c-QVector3D(0,1,0)).length()-3.f)<1e-3f);
    QVERIFY(colmapMatches(m,{-0.5f,0.5f,-0.5f},{0.5f,1.5f,0.5f}).isEmpty());
    // Another capture's, a hundred times the size.
    const QString far=dir_.filePath("colmap_far");writeColmap(far,12,300.f,{0,1,0});
    QVERIFY(!colmapMatches(findColmap(far),{-0.5f,0.5f,-0.5f},{0.5f,1.5f,0.5f}).isEmpty());
    // Photos missing.
    const QString missing=dir_.filePath("colmap_missing");writeColmap(missing,5,3.f,{0,1,0},false);
    m=findColmap(missing);QVERIFY(!m.usable());QCOMPARE(m.missing,5);
    QVERIFY(!findColmap(dir_.filePath("nothing_here")).usable());
    // Beside a PLY: suggested.
    const QString beside=dir_.filePath("beside");QDir().mkpath(beside);writeSphere(beside,"cap.ply",100);writeColmap(beside+"/colmap",8,3.f,{0,1,0});
    QCOMPARE(QDir::cleanPath(suggestColmap(beside+"/cap.ply")),QDir::cleanPath(beside+"/colmap"));
    // COLMAP's own layout: the original sparse/ keeps its distortion, undistorted/sparse is
    // what gs2mesh takes, beside the PLY.
    const QString colmapLayout=dir_.filePath("layout");QDir().mkpath(colmapLayout);writeSphere(colmapLayout,"scan.ply",100);
    writeColmap(colmapLayout,8,3.f,{0,1,0},true,4,"/sparse/0");
    writeColmap(colmapLayout+"/undistorted",8,3.f,{0,1,0},true,1,"/sparse");
    m=findColmap(colmapLayout);QVERIFY2(m.usable(),qPrintable(m.problem));
    QCOMPARE(QDir::cleanPath(m.sparse),QDir::cleanPath(colmapLayout+"/undistorted/sparse"));
    QCOMPARE(QDir::cleanPath(m.images),QDir::cleanPath(colmapLayout+"/undistorted/images"));
    QVERIFY(!suggestColmap(colmapLayout+"/scan.ply").isEmpty());
    QVERIFY(findColmap(suggestColmap(colmapLayout+"/scan.ply")).usable());
    // Only the distorted one: found, but not usable, and said why.
    const QString distorted=dir_.filePath("distorted");writeColmap(distorted,8,3.f,{0,1,0},true,4,"/sparse/0");
    m=findColmap(distorted);QVERIFY(!m.usable());QVERIFY(m.problem.contains("undistorted"));
    // A real capture with its photos beside it (SGS_TEST_COLMAP_PLY): found on import, and its
    // cameras around the splats.
    if (const QString real=qEnvironmentVariable("SGS_TEST_COLMAP_PLY");!real.isEmpty()) {
        SgsWindow window;
        QVERIFY(window.openPly(real));
        const QString found=window.project().colmap;QVERIFY2(!found.isEmpty(),"no COLMAP found beside the PLY");
        const auto model=findColmap(found);QVERIFY2(model.usable(),qPrintable(model.problem));
        const QString fits=colmapMatches(model,window.capture()->minimum,window.capture()->maximum);
        qInfo("%s: %s, %d photos in %s; %s",qPrintable(QFileInfo(real).fileName()),qPrintable(model.sparse),model.photos,qPrintable(model.images),
              fits.isEmpty() ? "the cameras stand around the splats" : qPrintable(fits));
        QVERIFY2(fits.isEmpty(),qPrintable(fits));
    }
}
void SgsEditorTests::objToGlb() {
    const MeshData mesh=readObj(writeQuadObj(dir_.path()));
    QCOMPARE(mesh.triangles(),size_t(2));QCOMPARE(mesh.vertices(),size_t(4));QVERIFY(mesh.albedo.endsWith("quad_albedo.png"));QVERIFY(mesh.normalMap.endsWith("quad_normal.png"));
    const QByteArray glb=writeGlb(mesh,QImage(mesh.albedo),QImage(mesh.normalMap));
    const GlbInfo info=inspectGlb(glb);
    QVERIFY2(info.error.isEmpty(),qPrintable(info.error));
    QCOMPARE(info.triangles,size_t(2));QCOMPARE(info.vertices,size_t(4));QCOMPARE(info.images,2);QCOMPARE(info.materials,1);
    const auto material=info.json.value("materials").toArray()[0].toObject();
    QVERIFY(material.contains("normalTexture"));QVERIFY(material.value("pbrMetallicRoughness").toObject().contains("baseColorTexture"));
    // OBJ's v counts up from the bottom, glTF's down from the top.
    const auto accessors=info.json.value("accessors").toArray();const auto views=info.json.value("bufferViews").toArray();
    const int uvAccessor=info.json.value("meshes").toArray()[0].toObject().value("primitives").toArray()[0].toObject().value("attributes").toObject().value("TEXCOORD_0").toInt();
    const auto view=views[accessors[uvAccessor].toObject().value("bufferView").toInt()].toObject();
    const float *uv=reinterpret_cast<const float *>(info.binary.constData()+view.value("byteOffset").toInt());
    QCOMPARE(uv[1],1.0f);QCOMPARE(uv[5],0.0f); // vertex 0 at v=0, vertex 2 at v=1
    QVERIFY(!inspectGlb(QByteArray("not a glb at all....")).error.isEmpty());
}
void SgsEditorTests::sgsCarriesSplatsNormalsAndMesh() {
    auto capture=SgsCapture::load(writeSphere(dir_.path(),"export.ply",4000,true));
    // Some invisible: left out.
    capture.frame.active[0]=capture.frame.active[1]=0;
    const MeshData mesh=readObj(writeQuadObj(dir_.path()));
    SgsExportOptions options;options.glb=writeGlb(mesh,QImage(mesh.albedo),QImage(mesh.normalMap));options.title="sphere";
    const QString path=dir_.filePath("sphere.sgs");
    const auto result=exportSgs(capture,options,path);
    QVERIFY(result.bytes>0);
    auto read=vgsdec::Capture::openFile(path.toStdString());
    QCOMPARE(read.frameCount(),uint64_t(1));
    const auto &frame=read.setTime(0,true);
    QCOMPARE(frame.splatCount,uint64_t(capture.frame.count-2));
    QVERIFY(frame.normals!=nullptr);
    // Each normal as it went in, within the code's precision, with its own splat: the encoder
    // orders splats its own way, so they are matched by position.
    std::map<std::tuple<int,int,int>,size_t> sources;
    auto key=[](const float *p) {return std::make_tuple(int(std::lround(p[0]*2000)),int(std::lround(p[1]*2000)),int(std::lround(p[2]*2000)));};
    for (size_t s=2;s<size_t(capture.frame.count);++s) sources[key(&capture.frame.position[s*3])]=s;
    double worst=0;size_t matched=0;
    for (size_t i=0;i<frame.splatCount;++i) {
        const auto found=sources.find(key(frame.positions+i*3));if (found==sources.end()) continue;
        ++matched;worst=std::max(worst,degreesBetween(frame.normals+i*3,&capture.frame.normal[found->second*3]));
    }
    QVERIFY2(matched>frame.splatCount*95/100,qPrintable(QStringLiteral("%1 of %2 matched").arg(matched).arg(frame.splatCount)));
    QVERIFY2(worst<2.0,qPrintable(QStringLiteral("worst normal %1 degrees").arg(worst)));
    QVERIFY(read.hasProxyMesh());
    const auto glb=read.proxyMesh();QCOMPARE(QByteArray(reinterpret_cast<const char *>(glb.data()),qsizetype(glb.size())),options.glb);
    QVERIFY(read.metadataJson2().find("sgsEditorVersion")!=std::string::npos);
    // Without a mesh there is none.
    SgsExportOptions plain;exportSgs(capture,plain,dir_.filePath("plain.sgs"));
    QVERIFY(!vgsdec::Capture::openFile(dir_.filePath("plain.sgs").toStdString()).hasProxyMesh());
    // Never over the PLY open.
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,exportSgs(capture,plain,capture.path));
}
void SgsEditorTests::plyExportKeepsNormals() {
    auto capture=SgsCapture::load(writeSphere(dir_.path(),"plyexport.ply",800,false));
    capture.estimateNormals();
    const QString out=dir_.filePath("plyexport_normals.ply");exportSgsPly(capture,out);
    const auto again=SgsCapture::load(out);
    QCOMPARE(again.normals,NormalsSource::File);
    for (size_t i=0;i<size_t(again.frame.count);++i) QVERIFY(degreesBetween(&again.frame.normal[i*3],&capture.frame.normal[i*3])<0.01);
}

void SgsEditorTests::lightsShadeTheSplats() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"lit.ply",20000,true)));
    auto *viewport=window.viewport();
    // A new capture comes with a magenta and a blue rim light, both off: drawn as captured.
    QCOMPARE(window.project().lights.size(),2);
    for (const auto &l:window.project().lights) {QVERIFY(!l.enabled);QVERIFY(!l.directional);QVERIFY(l.position.z()<1.f);}
    QVERIFY(window.project().lights[0].colour.red()>200 && window.project().lights[0].colour.blue()>150);
    QVERIFY(window.project().lights[1].colour.blue()>200 && window.project().lights[1].colour.red()<100);
    QVERIFY(!viewport->lit());
    window.project().lights.clear();window.selectLight(-1);
    // Looking at the front of the sphere (+Z).
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=2.2f;viewport->setCamera(camera);
    QTest::qWait(100);
    const QRect centre(viewport->width()/2-20,viewport->height()/2-20,40,40);
    auto shot=[&] {viewport->update();QTest::qWait(50);return viewport->grabFramebuffer();};
    const QImage unlit=shot();
    // A light that is off changes nothing.
    window.addLight(true);window.project().lights[0].enabled=false;window.selectLight(-1);
    QVERIFY(!viewport->lit());
    const QImage off=shot();QVERIFY(std::abs(luminance(average(off,centre))-luminance(average(unlit,centre)))<0.01);
    // Full self-illumination and no light: as captured.
    window.project().lights[0].enabled=true;window.project().lights[0].intensity=0;window.setSelfIllumination(1);window.selectLight(-1);
    QVERIFY(viewport->lit());
    const QImage same=shot();
    QVERIFY2(std::abs(luminance(average(same,centre))-luminance(average(unlit,centre)))<0.02,"self-illumination 1 with a dark light is the capture as captured");
    // A sun from the front lights the front; from behind it does not.
    window.setSelfIllumination(0.1f);auto &sun=window.project().lights[0];sun.intensity=1.5f;sun.setAngles(0,0);window.selectLight(-1);
    const double front=luminance(average(shot(),centre));
    sun.setAngles(180,0);window.selectLight(-1);
    const double back=luminance(average(shot(),centre));
    QVERIFY2(front>back*3,qPrintable(QStringLiteral("front %1, back %2").arg(front).arg(back)));
    QVERIFY(back<luminance(average(unlit,centre))*0.4);
    // A point light near the front, then far: the inverse square.
    window.project().lights[0].enabled=false;window.addLight(false);
    auto &bulb=window.project().lights[1];bulb.position={0,1,1.0f};bulb.intensity=1;bulb.range=0;window.selectLight(-1);
    const double near=luminance(average(shot(),centre));
    bulb.position={0,1,3.0f};window.selectLight(-1);
    const double farther=luminance(average(shot(),centre));
    QVERIFY2(near>farther*1.5,qPrintable(QStringLiteral("near %1, far %2").arg(near).arg(farther)));
}
void SgsEditorTests::lightColoursAreSrgb() {
    // A deep blue picked in a colour dialog lights deep blue, not sky blue: the light takes the
    // colour's linear value, as Unreal does.
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    // A grey sphere, so what colour there is comes from the light.
    std::vector<float> normals;vgs::Frame f=sphereFrame(20000,0.5f,{0,1,0},&normals);f.normal=normals;
    for (size_t i=0;i<f.colorDc.size();++i) f.colorDc[i]=0.7f;std::fill(f.shRest.begin(),f.shRest.end(),0.f);
    const QString path=dir_.filePath("grey.ply");writePly(f,path);
    QVERIFY(window.openPly(path));window.project().lights.clear();window.setSelfIllumination(0);
    auto *viewport=window.viewport();
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=2.2f;viewport->setCamera(camera);
    window.addLight(true);auto &l=window.project().lights[0];l.setAngles(0,0);l.colour=QColor(40,110,255);l.intensity=1;window.selectLight(-1);
    viewport->update();QTest::qWait(80);
    const QColor c=average(viewport->grabFramebuffer(),QRect(viewport->width()/2-20,viewport->height()/2-20,40,40));
    QVERIFY2(c.blueF()>2.0*c.greenF() && c.blueF()>4.0*c.redF(),qPrintable(c.name()));
}
void SgsEditorTests::presetsLight() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"presets.ply",2000,true)));
    const QStringList names=SgsLight::presetNames();QVERIFY(names.size()>=6);
    for (int i=0;i<names.size();++i) {
        window.applyPreset(i);
        QVERIFY2(!window.project().lights.isEmpty(),qPrintable(names[i]));
        for (const auto &l:window.project().lights) {
            QVERIFY(l.enabled);QVERIFY(l.intensity>0);
            QVERIFY(std::isfinite(l.position.x()) && std::isfinite(l.position.y()) && std::isfinite(l.position.z()));
            QVERIFY((l.position-QVector3D(0,1,0)).length()<5);
        }
        QVERIFY(window.viewport()->lit());QCOMPARE(window.viewport()->lights().size(),window.project().lights.size());
    }
    // The sun ones have a sun.
    window.applyPreset(int(names.indexOf("Daylight sun")));QVERIFY(window.project().lights[0].directional);
    QVERIFY(window.project().lights[0].direction().y()<-0.5f);
}

void SgsEditorTests::lightsTurn() {
    // About +Y: a quarter turn counter-clockwise seen from above takes +X to -Z.
    SgsLight l;l.position={1,2,0};
    QVERIFY((l.turned(90).position-QVector3D(0,2,-1)).length()<1e-5f);
    QVERIFY((l.turned(-90).position-QVector3D(0,2,1)).length()<1e-5f);
    l.turnSeconds=8;QCOMPARE(l.turnRate(),45.f);l.clockwise=true;QCOMPARE(l.turnRate(),-45.f);
    SgsLight sun;sun.directional=true;sun.setAngles(0,30);
    float a=0,e=0;sun.turned(90).angles(&a,&e);QVERIFY(std::abs(e-30)<0.05f);QVERIFY(std::abs(std::abs(a)-90)<0.05f);
    // Kept in the project.
    SgsProject p;p.ply=dir_.filePath("t.ply");p.lights={l};p.rigTurnSeconds=20;p.rigClockwise=true;
    const SgsProject q=SgsProject::fromJson(p.json(dir_.filePath("t.sgsproj")),dir_.filePath("t.sgsproj"));
    QCOMPARE(q.lights[0].turnSeconds,8.f);QVERIFY(q.lights[0].clockwise);QCOMPARE(q.rigTurnSeconds,20.f);QVERIFY(q.rigClockwise);
    QVERIFY(std::abs(q.lightAngle(0,2)-(-45*2-18*2))<1e-3f); // its own -45 and the set's -18 degrees a second

    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"turn.ply",2000,true)));
    window.project().lights.clear();window.addLight(false);window.project().lights[0].position={1,1,0};window.selectLight(-1);
    // Nothing turns yet: Play does nothing.
    window.setTurning(true);QVERIFY(!window.turning());
    window.setLightTurn(0,4,false); // a turn every 4 s
    window.setLightTime(1);
    auto near=[](const QVector3D &a,const QVector3D &b) {return (a-b).length()<1e-4f;};
    QVERIFY(near(window.shownLight(0).position,{0,1,-1}));
    QVERIFY(near(window.viewport()->lights()[0].position,{0,1,-1}));
    QVERIFY(near(window.project().lights[0].position,{1,1,0})); // its pose at time 0 is kept
    // It plays, and pauses.
    window.setTurning(true);QVERIFY(window.turning());QTest::qWait(250);
    QVERIFY(window.lightTime()>1.15);
    // Selecting pauses; editing it moves it where it is seen, and playing goes on from there.
    window.selectLight(0);QVERIFY(!window.turning());
    window.setLightTime(1);
    window.viewport()->setDisplayedComponent(0,0,0.5f); // the gizmo's X, as a drag sets it
    QVERIFY(std::abs(window.shownLight(0).position.x()-0.5f)<1e-4f);
    QVERIFY(std::abs(window.shownLight(0).position.z()+1)<1e-4f);
    window.setLightTime(0);QVERIFY(near(window.shownLight(0).position,window.project().lights[0].position));
    QVERIFY(near(window.project().lights[0].position,{1,1,0.5f})); // (0.5, -1) turned back a quarter
    // A new speed does not move it: it goes on from where it is seen.
    window.setLightTime(1.5);const QVector3D before=window.shownLight(0).position;
    window.setLightTurn(0,10,true);QVERIFY(near(window.shownLight(0).position,before));
    // Every light together, on top: the same.
    window.setRigTurn(6,false);QVERIFY(near(window.shownLight(0).position,before));
    QVERIFY(std::abs(window.project().lightAngle(0,1)-(-36.f+60.f))<1e-3f);
    window.selectLight(-1);
}

void SgsEditorTests::spaceTogglesTurning() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowActive(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"space.ply",1000,true)));
    window.project().lights.clear();window.addLight(false);window.selectLight(-1);window.setLightTurn(0,4,false);
    window.viewport()->setFocus();
    QTest::keyClick(window.viewport(),Qt::Key_Space);QVERIFY(window.turning());
    QTest::qWait(100);
    QTest::keyClick(window.viewport(),Qt::Key_Space);QVERIFY(!window.turning());
    QVERIFY(window.lightTime()>0.05);
    QTest::keyClick(window.viewport(),Qt::Key_Home);QCOMPARE(window.lightTime(),0.0);
    // While it plays, the speed of them all can be typed: the clock does not rewrite it.
    window.setTurning(true);QVERIFY(window.turning());
    auto *rig=window.findChild<QDoubleSpinBox *>("rigTurn");QVERIFY(rig && rig->isEnabled());
    rig->setFocus();rig->selectAll();QTest::keyClicks(rig,"12");QTest::qWait(120);
    QCOMPARE(rig->text().left(2),QStringLiteral("12"));
    QTest::keyClick(rig,Qt::Key_Return);QTest::qWait(30);
    QCOMPARE(window.project().rigTurnSeconds,12.f);QVERIFY(window.turning());
    window.setTurning(false);
}
void SgsEditorTests::lightIconsHideBehindSplats() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"icons.ply",20000,true)));
    auto *viewport=window.viewport();
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=2.5f;viewport->setCamera(camera);
    // One light in front of the sphere, one behind it, both on the view's axis but apart on screen.
    window.project().lights.clear();window.addLight(false);window.addLight(false);
    window.project().lights[0].position={0.25f,1.1f,0.8f};window.project().lights[1].position={-0.15f,1.0f,-0.8f};window.selectLight(-1);
    viewport->update();QTest::qWait(80);
    QSignalSpy clicked(viewport,&Viewport::lightClicked);
    auto click=[&](const QVector3D &world) {QPointF at;QVERIFY(viewport->projectToView(world,&at));QTest::mouseClick(viewport,Qt::LeftButton,{},at.toPoint());QTest::qWait(20);};
    click(window.project().lights[0].position);QCOMPARE(clicked.last().at(0).toInt(),0);
    // Behind the splats: not drawn, not clicked.
    click(window.project().lights[1].position);QCOMPARE(clicked.last().at(0).toInt(),-1);
    // Icons off: none.
    viewport->setLightIcons(false);
    click(window.project().lights[0].position);QCOMPARE(clicked.last().at(0).toInt(),-1);
    viewport->setLightIcons(true);
}
void SgsEditorTests::modelsHaveDefaults() {
    // This build knows where the models are (SGS_STEREO_MODEL, SGS_MOGE_MODEL), or a deployed
    // editor has them beside gs2mesh: either way, existing files or none.
    for (auto model:{Gs2Mesh::Model::Stereo,Gs2Mesh::Model::MoGe}) {
        const QString path=Gs2Mesh::defaultModel(model);
        if (!path.isEmpty()) QVERIFY2(QFileInfo::exists(path),qPrintable(path));
        qInfo("%s: %s",model==Gs2Mesh::Model::Stereo ? "stereo" : "MoGe",path.isEmpty() ? "none" : qPrintable(path));
    }
}

void SgsEditorTests::gizmoMovesTheSelectedLight() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"gizmo.ply",2000,true)));
    window.project().lights.clear();window.selectLight(-1);
    window.addLight(false);QCOMPARE(window.selectedLight(),0);
    auto *viewport=window.viewport();QVERIFY(viewport->hasGizmoTarget());QCOMPARE(viewport->transformMode(),TransformMode::Move);
    // The gizmo's fields, as the drag sets them: the light follows, the capture does not.
    viewport->setDisplayedComponent(0,0,0.75f);
    QCOMPARE(window.project().lights[0].position.x(),0.75f);
    QCOMPARE(viewport->transform().position,QVector3D(0,0,0));
    QCOMPARE(viewport->lights()[0].position.x(),0.75f);
    // A directional light turns.
    window.addLight(true);QCOMPARE(viewport->transformMode(),TransformMode::Rotate);
    const QVector3D before=window.project().lights[1].direction();
    viewport->setDisplayedComponent(1,1,40);
    QVERIFY((window.project().lights[1].direction()-before).length()>0.1f);
    // Deselecting hands the gizmo back.
    window.selectLight(-1);QVERIFY(!viewport->hasGizmoTarget());QCOMPARE(viewport->transformMode(),TransformMode::None);
    window.deleteLight();QCOMPARE(window.project().lights.size(),2); // nothing selected: nothing deleted
    window.selectLight(0);window.deleteLight();QCOMPARE(window.project().lights.size(),1);QVERIFY(window.project().lights[0].directional);
}
void SgsEditorTests::meshIsDrawnLit() {
    SgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"withmesh.ply",2000,true)));
    window.project().lights.clear();window.selectLight(-1);
    // The quad, facing +Z, as the mesh; seen from the front.
    window.project().meshFiles={writeQuadObj(dir_.path())};
    QVERIFY(window.loadMesh());
    auto *viewport=window.viewport();viewport->setDrawMesh(true);viewport->setDrawSplats(false);
    Camera camera;camera.target={0.5f,0.5f,0};camera.yaw=0;camera.pitch=0;camera.distance=2.f;viewport->setCamera(camera);
    const QRect centre(viewport->width()/2-10,viewport->height()/2-10,20,20);
    auto shot=[&] {viewport->update();QTest::qWait(50);return viewport->grabFramebuffer();};
    const QColor unlit=average(shot(),centre);
    // Its albedo shows (orange), head-lit.
    QVERIFY2(unlit.redF()>unlit.blueF()+0.2,qPrintable(unlit.name()));
    window.addLight(true);window.project().lights[0].setAngles(0,0);window.selectLight(-1);
    const double front=luminance(average(shot(),centre));
    window.project().lights[0].setAngles(180,0);window.selectLight(-1);
    const double back=luminance(average(shot(),centre));
    QVERIFY2(front>back*2,qPrintable(QStringLiteral("front %1, back %2").arg(front).arg(back)));
}
void SgsEditorTests::realCapture() {
    const QString ply=qEnvironmentVariable("SGS_TEST_PLY");
    if (ply.isEmpty()) QSKIP("SGS_TEST_PLY names no capture.");
    QElapsedTimer timer;timer.start();
    auto capture=SgsCapture::load(ply);const double loadMs=timer.restart();
    QString note;capture.estimateNormals(&note);const double normalsMs=timer.restart();
    qInfo("%s: %llu splats, loaded in %.0f ms, normals in %.0f ms: %s",qPrintable(QFileInfo(ply).fileName()),qulonglong(capture.frame.count),loadMs,normalsMs,qPrintable(note));
    {
        // Against the VGS editor's 42 cameras: how many splats change side or turn.
        auto fewer=capture;fewer.estimateNormals(nullptr,42);const double fewerMs=timer.restart();
        size_t flipped=0,turned=0,compared=0;
        for (size_t i=0;i<size_t(capture.frame.count);++i) {
            if (!capture.frame.active[i]) continue;++compared;
            const double d=degreesBetween(&capture.frame.normal[i*3],&fewer.frame.normal[i*3]);
            flipped+=d>120;turned+=d>15;
        }
        qInfo("42 cameras: %.0f ms; %.2f%% of the splats face the other way with 100, %.2f%% differ by more than 15 degrees",
              fewerMs,100.0*double(flipped)/double(compared),100.0*double(turned)/double(compared));
    }
    timer.restart();
    const QString out=dir_.filePath("real.sgs");
    const auto result=exportSgs(capture,{},out);
    qInfo("exported %.1f MB in %.0f ms: %s",result.bytes/1e6,double(timer.restart()),qPrintable(result.notes.join(' ')));
    QVERIFY(vgsdec::Capture::openFile(out.toStdString()).setTime(0,true).normals);
    if (qEnvironmentVariable("SGS_TEST_GS2MESH")!="1") return;
    // The whole way: gs2mesh from the splats alone, then the .sgs with its lowest LOD.
    SgsWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));window.project().colmapDeclined=true;window.project().colmap.clear();
    QSignalSpy finished(&window,&SgsWindow::meshFinished);
    QVERIFY(window.startMesh());
    // While it works, the rest waits: the lights, the panel, the menus; the mesh's Cancel does not.
    QVERIFY(!window.findChild<QListWidget *>("lights")->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>("estimateNormals")->isEnabled());
    QVERIFY(window.findChild<QProgressBar *>()->isEnabled());
    window.selectLight(0);QCOMPARE(window.selectedLight(),-1);
    QTest::keyClick(window.viewport(),Qt::Key_Space);QVERIFY(!window.turning());
    // Its progress never goes back; and, asked for, a picture of the window halfway.
    QProgressBar *bar=window.findChild<QProgressBar *>();int lowest=0;bool back=false;
    QTimer watch;QObject::connect(&watch,&QTimer::timeout,[&] {if (bar->value()<lowest) back=true;lowest=std::max(lowest,bar->value());});watch.start(50);
    if (const QString shots=qEnvironmentVariable("SGS_TEST_SHOTS");!shots.isEmpty()) {
        QDir().mkpath(shots);QTest::qWait(40000);window.grab().save(shots+"/mesh_busy.png");
    }
    QVERIFY(finished.wait(30*60*1000));
    QVERIFY2(!back,"the mesh's progress went back");
    QVERIFY(window.findChild<QListWidget *>("lights")->isEnabled());
    QVERIFY2(finished.first().at(0).toBool(),qPrintable(finished.first().at(1).toString()));
    QVERIFY(window.project().meshFiles.size()>=3);QVERIFY(!window.project().meshOutOfDate());
    const QString sgs=dir_.filePath("real_mesh.sgs");const auto withMesh=window.exportSgsTo(sgs);
    auto read=vgsdec::Capture::openFile(sgs.toStdString());QVERIFY(read.hasProxyMesh());
    const auto glb=read.proxyMesh();const GlbInfo info=inspectGlb(QByteArray(reinterpret_cast<const char *>(glb.data()),qsizetype(glb.size())));
    QVERIFY2(info.error.isEmpty(),qPrintable(info.error));
    qInfo("with mesh: %.1f MB, glb %zu triangles, %d images",withMesh.bytes/1e6,info.triangles,info.images);
    if (const QString keep=qEnvironmentVariable("SGS_TEST_KEEP");!keep.isEmpty()) {QFile::remove(keep);QFile::copy(sgs,keep);}
}

// Pictures to look at (SGS_TEST_SHOTS=folder, with SGS_TEST_PLY and, for the mesh,
// SGS_TEST_MESH=its .obj): the window, and the capture unlit, under a sun, under a bulb,
// and its mesh lit the same way.
void SgsEditorTests::visualSheet() {
    const QString out=qEnvironmentVariable("SGS_TEST_SHOTS"),ply=qEnvironmentVariable("SGS_TEST_PLY");
    if (out.isEmpty() || ply.isEmpty()) QSKIP("SGS_TEST_SHOTS and SGS_TEST_PLY name no folder and capture.");
    QDir().mkpath(out);
    SgsWindow window;window.resize(1600,1000);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));
    auto *viewport=window.viewport();const auto *c=window.capture();
    const auto rims=window.project().lights;window.project().lights.clear();window.selectLight(-1);
    const QVector3D centre=(c->minimum+c->maximum)*0.5f;const float height=c->maximum.y()-c->minimum.y();
    Camera camera;camera.target=centre;camera.yaw=float(qEnvironmentVariable("SGS_TEST_YAW","0").toDouble());camera.pitch=5;camera.distance=height*1.6f;viewport->setCamera(camera);
    auto save=[&](const QString &name) {viewport->update();QTest::qWait(150);QVERIFY(viewport->grabFramebuffer().save(out+"/"+name));};
    save("1_unlit.png");
    window.addLight(true);save("2_sun.png");
    window.selectLight(-1);save("3_sun_unselected.png");
    window.project().lights[0].enabled=false;window.addLight(false);window.selectLight(-1);save("4_bulb.png");
    window.project().lights[0].enabled=true;window.project().lights[0].intensity=0.8f;window.project().lights[1].colour=QColor(255,170,110);window.selectLight(1);save("5_both.png");
    QVERIFY(window.grab().save(out+"/0_window.png"));
    // The rim lights it came with, switched on, alone.
    window.project().lights=rims;for (auto &l:window.project().lights) l.enabled=true;window.selectLight(-1);
    save("8_rims.png");
    camera.yaw=35;viewport->setCamera(camera);save("9_rims_side.png");
    for (int i=0;i<SgsLight::presetNames().size();++i) {window.applyPreset(i);save(QStringLiteral("p%1.png").arg(i));}
    camera.yaw=float(qEnvironmentVariable("SGS_TEST_YAW","0").toDouble());viewport->setCamera(camera);
    window.project().lights.clear();window.addLight(true);window.project().lights[0].intensity=0.8f;window.addLight(false);window.project().lights[1].colour=QColor(255,170,110);window.selectLight(-1);
    if (const QString obj=qEnvironmentVariable("SGS_TEST_MESH");!obj.isEmpty()) {
        // As a click on Show beside the view cube does: the mesh, then grey.
        window.project().meshFiles={obj};window.findChild<QPushButton *>("displayShow")->click();
        QVERIFY(viewport->drawsMesh());QVERIFY(window.findChild<QPushButton *>("displayMeshLook")->isVisible());
        window.selectLight(-1);save("6_mesh_lit.png");
        for (auto &l:window.project().lights) l.enabled=false;window.selectLight(-1);save("7_mesh_unlit.png");
        window.findChild<QPushButton *>("displayMeshLook")->click();QVERIFY(!viewport->meshTextured());save("7b_mesh_gray.png");
        QVERIFY(window.grab().save(out+"/0b_window_mesh.png"));
    }
}

int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);app.setOrganizationName("VGS");app.setApplicationName("SGS Editor tests");
    SgsEditorTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "sgseditor_tests.moc"
