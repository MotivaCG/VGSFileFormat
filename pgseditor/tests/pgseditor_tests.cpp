// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).
//
// The PGS editor's checks. A sphere of flat splats stands in for a capture; PGS_TEST_PLY names
// a real one for the checks that want it, and PGS_TEST_GS2MESH=1 lets gs2mesh run on it
// (minutes, an NVIDIA GPU).

#include "colmapfinder.h"
#include "exportcapture.h"
#include "meshio.h"
#include "meshsculpt.h"
#include "pgscapture.h"
#include "pgsexport.h"
#include "pgsproject.h"
#include "pgswindow.h"
#include "pgsqueue.h"
#include <QSettings>
#include "viewport.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QToolBar>
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
#include <array>
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
    QImage ao(64,32,QImage::Format_Grayscale8);ao.fill(QColor(180,180,180));ao.save(dir+"/quad_ao.png");
    QFile mtl(dir+"/quad.mtl");mtl.open(QIODevice::WriteOnly);mtl.write("newmtl m\nmap_Kd quad_albedo.png\nnorm quad_normal.png\nmap_Ka quad_ao.png\n");mtl.close();
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
// A UV sphere as gs2mesh writes meshes (v, vt, vn; faces v/vt/vn), its seam's vertices sharing
// their positions, with its material and albedo beside it; and, asked for, a loose tetrahedron.
QString writeSphereObj(const QString &dir,const QString &name,float radius,const QVector3D &centre,bool loosePiece=false,int rings=24,int segments=32) {
    QImage albedo(64,32,QImage::Format_RGB888);albedo.fill(QColor(200,100,50));albedo.save(dir+"/"+name+"_albedo.png");
    QFile mtl(dir+"/"+name+".mtl");mtl.open(QIODevice::WriteOnly);mtl.write(("newmtl material_0\nmap_Kd "+name+"_albedo.png\n").toUtf8());mtl.close();
    QByteArray v,vt,vn,f;
    auto pos=[&](const QVector3D &p) {
        v+=QStringLiteral("v %1 %2 %3\n").arg(p.x()).arg(p.y()).arg(p.z()).toUtf8();
        const QVector3D n=(p-centre).normalized();vn+=QStringLiteral("vn %1 %2 %3\n").arg(n.x()).arg(n.y()).arg(n.z()).toUtf8();};
    // Positions: the poles, then rings x segments (no seam column); texture coordinates: every
    // ring has segments + 1, the last at u = 1 on the seam.
    pos(centre+QVector3D(0,radius,0));pos(centre-QVector3D(0,radius,0));
    for (int r=1;r<rings;++r) for (int s=0;s<segments;++s) {
        const double th=M_PI*r/rings,ph=2*M_PI*s/segments;
        pos(centre+QVector3D(float(std::sin(th)*std::cos(ph)),float(std::cos(th)),float(std::sin(th)*std::sin(ph)))*radius);
    }
    for (int r=0;r<=rings;++r) for (int s=0;s<=segments;++s) vt+=QStringLiteral("vt %1 %2\n").arg(double(s)/segments).arg(1.0-double(r)/rings).toUtf8();
    auto P=[&](int r,int s) {return r==0 ? 1 : r==rings ? 2 : 3+(r-1)*segments+(s%segments);};
    auto T=[&](int r,int s) {return 1+r*(segments+1)+s;};
    auto face=[&](int r0,int s0,int r1,int s1,int r2,int s2) {
        f+=QStringLiteral("f %1/%2/%1 %3/%4/%3 %5/%6/%5\n").arg(P(r0,s0)).arg(T(r0,s0)).arg(P(r1,s1)).arg(T(r1,s1)).arg(P(r2,s2)).arg(T(r2,s2)).toUtf8();};
    for (int r=0;r<rings;++r) for (int s=0;s<segments;++s) {
        if (r>0) face(r,s,r,s+1,r+1,s);
        if (r<rings-1) face(r,s+1,r+1,s+1,r+1,s);
    }
    if (loosePiece) {
        // Far off the sphere, its own four positions.
        const int at=2+(rings-1)*segments;const QVector3D o=centre+QVector3D(radius*3,0,0);const float e=radius*0.05f;
        for (const QVector3D &d:{QVector3D(0,0,0),QVector3D(e,0,0),QVector3D(0,e,0),QVector3D(0,0,e)}) pos(o+d);
        const int a=at+1,b=at+2,c=at+3,d=at+4;
        for (const auto &t:{std::array<int,3>{a,c,b},{a,b,d},{a,d,c},{b,c,d}}) f+=QStringLiteral("f %1/1/%1 %2/1/%2 %3/1/%3\n").arg(t[0]).arg(t[1]).arg(t[2]).toUtf8();
    }
    QFile obj(dir+"/"+name+".obj");obj.open(QIODevice::WriteOnly);
    obj.write(("mtllib "+name+".mtl\nusemtl material_0\n").toUtf8()+v+vt+vn+f);obj.close();
    return dir+"/"+name+".obj";
}
QVector3D at3(const std::vector<float> &p,size_t i) {return {p[i*3],p[i*3+1],p[i*3+2]};}
// Where a ray meets the mesh, as a brush lands there; false when it misses.
bool landAt(const SculptMesh &s,const QVector3D &origin,const QVector3D &direction,float radius,SculptMesh::Dab *d,float strength=1.f,bool invert=false) {
    int triangle=-1;if (!s.raycast(origin,direction,&d->centre,nullptr,&triangle)) return false;
    d->triangle=triangle;d->radius=radius;d->strength=strength;d->invert=invert;d->hardness=0.3f;return true;
}
} // namespace

class PgsEditorTests : public QObject {
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
    void pgsCarriesSplatsNormalsAndMesh();
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
    void sculptKeepsSeamsAndUndoes();
    void sculptFixesSpikesAndPieces();
    void relaxUnfoldsATangle();
    void texturePaintsTheFrontOnly();
    void noBlackWhereNormalsCancel();
    void doubleClickPaintsWithABrush();
    void editsReachTheDenseSurface();
    void sculptingInTheWindow();
    void sculptRealMesh();
    void lodsFromEditedMesh();
    void meshQueue();
    void meshAgainstSplats();
    void realCapture();
    void visualSheet();
};

void PgsEditorTests::readsPlyAsTheEditorDoes() {
    std::vector<float> normals;const vgs::Frame source=sphereFrame(500,0.5f,{0,1,0},&normals);
    const QString path=dir_.filePath("read.ply");writePly(source,path);
    const PgsPly ply=readPgsPly(path);
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
void PgsEditorTests::zeroNormalsAreNoNormals() {
    // writePly writes zeros without normals, as 3DGS training does.
    const QString path=writeSphere(dir_.path(),"zeros.ply",300,false);
    const auto c=PgsCapture::load(path);
    QVERIFY(c.frame.normal.empty());QCOMPARE(c.normals,NormalsSource::None);
    const auto withNormals=PgsCapture::load(writeSphere(dir_.path(),"normals.ply",300,true));
    QCOMPARE(withNormals.normals,NormalsSource::File);QCOMPARE(withNormals.frame.normal.size(),size_t(900));
}
void PgsEditorTests::normalsPointOutwards() {
    std::vector<float> truth;vgs::Frame f=sphereFrame(6000,0.5f,{0,1,0},&truth);
    const QString path=dir_.filePath("estimate.ply");writePly(f,path);
    auto c=PgsCapture::load(path);QString note;c.estimateNormals(&note);
    // No rig named: cameras all around.
    QCOMPARE(c.normals,NormalsSource::Surrounding);QVERIFY(note.contains("all around"));
    int within=0;for (size_t i=0;i<size_t(c.frame.count);++i) within+=degreesBetween(&c.frame.normal[i*3],&truth[i*3])<15;
    QVERIFY2(within>int(c.frame.count)*95/100,qPrintable(QStringLiteral("%1 of %2 within 15 degrees").arg(within).arg(c.frame.count)));
    // Estimate normals: the fine estimate, on another thread while the window waits; the project
    // remembers it was the fine one.
    PgsWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(path));QVERIFY(!window.project().fineNormals);
    window.findChild<QPushButton *>("estimateNormals")->click();
    QVERIFY(window.project().estimatedNormals && window.project().fineNormals);
    const PgsCapture *fine=window.capture();QCOMPARE(fine->normals,NormalsSource::Surrounding);
    within=0;for (size_t i=0;i<size_t(fine->frame.count);++i) within+=degreesBetween(&fine->frame.normal[i*3],&truth[i*3])<15;
    QVERIFY2(within>int(fine->frame.count)*95/100,qPrintable(QStringLiteral("fine: %1 of %2 within 15 degrees").arg(within).arg(fine->frame.count)));
}
void PgsEditorTests::projectRoundTripsWithRelativePaths() {
    PgsProject p;p.ply=dir_.filePath("sub/capture.ply");p.colmap=dir_.filePath("photos");p.selfIllumination=0.25f;p.estimatedNormals=true;p.fineNormals=true;
    p.mesh.triangles=100000;p.mesh.lods={20000};p.mesh.textureSize=2048;
    p.meshFiles={dir_.filePath("sub/capture_mesh/capture.obj"),dir_.filePath("sub/capture_mesh/capture_lod1.obj")};p.meshInputs="abc";
    PgsLight sun=PgsLight::sun({0,1,0},1.8f);sun.colour=QColor(255,200,150);p.lights={sun,PgsLight::bulb({0,1,0},1.8f)};p.lights[1].enabled=false;
    p.pgsMesh=0;p.pgsTextureSize=1024;
    const QString projectPath=dir_.filePath("project/one.pgsproj");
    const QJsonObject json=p.json(projectPath);
    QCOMPARE(json.value("ply").toString(),QStringLiteral("../sub/capture.ply"));
    // Moved with its files: the paths follow.
    const QString moved=QDir::cleanPath(dir_.filePath("elsewhere/project/one.pgsproj"));
    const PgsProject q=PgsProject::fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object(),moved);
    QCOMPARE(q.ply,QDir::cleanPath(dir_.filePath("elsewhere/sub/capture.ply")));
    QCOMPARE(q.selfIllumination,0.25f);QVERIFY(q.estimatedNormals);QVERIFY(q.fineNormals);QCOMPARE(q.mesh.triangles,100000);QCOMPARE(q.mesh.lods,QVector<int>{20000});
    QCOMPARE(q.lights.size(),2);QVERIFY(q.lights[0].directional);QCOMPARE(q.lights[0].colour,QColor(255,200,150));QVERIFY(!q.lights[1].enabled);
    QVERIFY((q.lights[0].direction()-p.lights[0].direction()).length()<1e-4f);
    QCOMPARE(q.meshFiles.size(),2);QCOMPARE(q.pgsMesh,0);QCOMPARE(q.pgsTextureSize,1024);
    QCOMPARE(q.pgsMeshFile(),QDir::cleanPath(dir_.filePath("elsewhere/sub/capture_mesh/capture.obj")));
    PgsProject r=q;r.pgsMesh=-1;QVERIFY(r.pgsMeshFile().endsWith("capture_lod1.obj"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,PgsProject::fromJson(QJsonObject{{"format","other"}},moved));
}
void PgsEditorTests::lightAnglesRoundTrip() {
    for (float azimuth:{-170.f,-90.f,0.f,45.f,120.f}) for (float elevation:{-30.f,0.f,35.f,80.f}) {
        PgsLight l;l.directional=true;l.setAngles(azimuth,elevation);
        float a=0,e=0;l.angles(&a,&e);
        QVERIFY2(std::abs(e-elevation)<0.05f,qPrintable(QStringLiteral("%1/%2 -> %3/%4").arg(azimuth).arg(elevation).arg(a).arg(e)));
        float da=std::fmod(std::abs(a-azimuth),360.f);da=std::min(da,360-da);QVERIFY(da<0.05f);
        // The light travels away from where it comes from.
        const QVector3D from(std::cos(qDegreesToRadians(elevation))*std::sin(qDegreesToRadians(azimuth)),std::sin(qDegreesToRadians(elevation)),std::cos(qDegreesToRadians(elevation))*std::cos(qDegreesToRadians(azimuth)));
        QVERIFY((l.direction()+from).length()<1e-3f);
    }
    // The default sun lights the front (+Z) from above.
    const PgsLight sun=PgsLight::sun({0,1,0},1.8f);QVERIFY(sun.direction().y()<-0.3f);QVERIFY(sun.direction().z()<0);
}
void PgsEditorTests::findsAndChecksColmap() {
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
    QCOMPARE(QDir::cleanPath(suggestColmap(beside+"/cap.ply")),QDir::cleanPath(beside+"/colmap/sparse/0"));
    // COLMAP's own layout: the original sparse/ keeps its distortion, undistorted/sparse is
    // what gs2mesh takes, beside the PLY.
    const QString colmapLayout=dir_.filePath("layout");QDir().mkpath(colmapLayout);writeSphere(colmapLayout,"scan.ply",100);
    writeColmap(colmapLayout,8,3.f,{0,1,0},true,4,"/sparse/0");
    writeColmap(colmapLayout+"/undistorted",8,3.f,{0,1,0},true,1,"/sparse");
    m=findColmap(colmapLayout);QVERIFY2(m.usable(),qPrintable(m.problem));
    QCOMPARE(QDir::cleanPath(m.sparse),QDir::cleanPath(colmapLayout+"/undistorted/sparse"));
    QCOMPARE(QDir::cleanPath(m.images),QDir::cleanPath(colmapLayout+"/undistorted/images"));
    QCOMPARE(QDir::cleanPath(suggestColmap(colmapLayout+"/scan.ply")),QDir::cleanPath(colmapLayout+"/undistorted/sparse"));
    QVERIFY(findColmap(suggestColmap(colmapLayout+"/scan.ply")).usable());
    // Only the distorted one: found, but not usable, and said why.
    const QString distorted=dir_.filePath("distorted");writeColmap(distorted,8,3.f,{0,1,0},true,4,"/sparse/0");
    m=findColmap(distorted);QVERIFY(!m.usable());QVERIFY(m.problem.contains("undistorted"));
    // A real capture with its photos beside it (PGS_TEST_COLMAP_PLY): found on import, and its
    // cameras around the splats.
    if (const QString real=qEnvironmentVariable("PGS_TEST_COLMAP_PLY");!real.isEmpty()) {
        PgsWindow window;
        QVERIFY(window.openPly(real));
        const QString found=window.project().colmap;QVERIFY2(!found.isEmpty(),"no COLMAP found beside the PLY");
        const auto model=findColmap(found);QVERIFY2(model.usable(),qPrintable(model.problem));
        const QString fits=colmapMatches(model,window.capture()->minimum,window.capture()->maximum);
        qInfo("%s: %s, %d photos in %s; %s",qPrintable(QFileInfo(real).fileName()),qPrintable(model.sparse),model.photos,qPrintable(model.images),
              fits.isEmpty() ? "the cameras stand around the splats" : qPrintable(fits));
        QVERIFY2(fits.isEmpty(),qPrintable(fits));
    }
}
void PgsEditorTests::objToGlb() {
    const MeshData mesh=readObj(writeQuadObj(dir_.path()));
    QCOMPARE(mesh.triangles(),size_t(2));QCOMPARE(mesh.vertices(),size_t(4));QVERIFY(mesh.albedo.endsWith("quad_albedo.png"));QVERIFY(mesh.normalMap.endsWith("quad_normal.png"));
    QVERIFY(mesh.occlusion.endsWith("quad_ao.png"));
    const QByteArray glb=writeGlb(mesh,QImage(mesh.albedo),QImage(mesh.normalMap),{},QImage(mesh.occlusion));
    const GlbInfo info=inspectGlb(glb);
    QVERIFY2(info.error.isEmpty(),qPrintable(info.error));
    QCOMPARE(info.triangles,size_t(2));QCOMPARE(info.vertices,size_t(4));QCOMPARE(info.images,3);QCOMPARE(info.materials,1);
    const auto material=info.json.value("materials").toArray()[0].toObject();
    QVERIFY(material.contains("occlusionTexture"));
    QVERIFY(material.contains("normalTexture"));QVERIFY(material.value("pbrMetallicRoughness").toObject().contains("baseColorTexture"));
    // OBJ's v counts up from the bottom, glTF's down from the top.
    const auto accessors=info.json.value("accessors").toArray();const auto views=info.json.value("bufferViews").toArray();
    const int uvAccessor=info.json.value("meshes").toArray()[0].toObject().value("primitives").toArray()[0].toObject().value("attributes").toObject().value("TEXCOORD_0").toInt();
    const auto view=views[accessors[uvAccessor].toObject().value("bufferView").toInt()].toObject();
    const float *uv=reinterpret_cast<const float *>(info.binary.constData()+view.value("byteOffset").toInt());
    QCOMPARE(uv[1],1.0f);QCOMPARE(uv[5],0.0f); // vertex 0 at v=0, vertex 2 at v=1
    QVERIFY(!inspectGlb(QByteArray("not a glb at all....")).error.isEmpty());
}
void PgsEditorTests::pgsCarriesSplatsNormalsAndMesh() {
    auto capture=PgsCapture::load(writeSphere(dir_.path(),"export.ply",4000,true));
    // Some invisible: left out.
    capture.frame.active[0]=capture.frame.active[1]=0;
    const MeshData mesh=readObj(writeQuadObj(dir_.path()));
    PgsExportOptions options;options.glb=writeGlb(mesh,QImage(mesh.albedo),QImage(mesh.normalMap));options.title="sphere";
    const QString path=dir_.filePath("sphere.pgs");
    const auto result=exportPgs(capture,options,path);
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
    QVERIFY(read.metadataJson2().find("pgsEditorVersion")!=std::string::npos);
    // Without a mesh there is none.
    PgsExportOptions plain;exportPgs(capture,plain,dir_.filePath("plain.pgs"));
    QVERIFY(!vgsdec::Capture::openFile(dir_.filePath("plain.pgs").toStdString()).hasProxyMesh());
    // Never over the PLY open.
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,exportPgs(capture,plain,capture.path));
}
void PgsEditorTests::plyExportKeepsNormals() {
    auto capture=PgsCapture::load(writeSphere(dir_.path(),"plyexport.ply",800,false));
    capture.estimateNormals();
    const QString out=dir_.filePath("plyexport_normals.ply");exportPgsPly(capture,out);
    const auto again=PgsCapture::load(out);
    QCOMPARE(again.normals,NormalsSource::File);
    for (size_t i=0;i<size_t(again.frame.count);++i) QVERIFY(degreesBetween(&again.frame.normal[i*3],&capture.frame.normal[i*3])<0.01);
}

void PgsEditorTests::lightsShadeTheSplats() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
void PgsEditorTests::lightColoursAreSrgb() {
    // A deep blue picked in a colour dialog lights deep blue, not sky blue: the light takes the
    // colour's linear value, as Unreal does.
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
void PgsEditorTests::presetsLight() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"presets.ply",2000,true)));
    const QStringList names=PgsLight::presetNames();QVERIFY(names.size()>=6);
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

void PgsEditorTests::lightsTurn() {
    // About +Y: a quarter turn counter-clockwise seen from above takes +X to -Z.
    PgsLight l;l.position={1,2,0};
    QVERIFY((l.turned(90).position-QVector3D(0,2,-1)).length()<1e-5f);
    QVERIFY((l.turned(-90).position-QVector3D(0,2,1)).length()<1e-5f);
    l.turnSeconds=8;QCOMPARE(l.turnRate(),45.f);l.clockwise=true;QCOMPARE(l.turnRate(),-45.f);
    PgsLight sun;sun.directional=true;sun.setAngles(0,30);
    float a=0,e=0;sun.turned(90).angles(&a,&e);QVERIFY(std::abs(e-30)<0.05f);QVERIFY(std::abs(std::abs(a)-90)<0.05f);
    // Kept in the project.
    PgsProject p;p.ply=dir_.filePath("t.ply");p.lights={l};p.rigTurnSeconds=20;p.rigClockwise=true;
    const PgsProject q=PgsProject::fromJson(p.json(dir_.filePath("t.pgsproj")),dir_.filePath("t.pgsproj"));
    QCOMPARE(q.lights[0].turnSeconds,8.f);QVERIFY(q.lights[0].clockwise);QCOMPARE(q.rigTurnSeconds,20.f);QVERIFY(q.rigClockwise);
    QVERIFY(std::abs(q.lightAngle(0,2)-(-45*2-18*2))<1e-3f); // its own -45 and the set's -18 degrees a second
    // A project saved without the set's turn turns as a new capture: once every 5 s.
    QJsonObject old=p.json(dir_.filePath("t.pgsproj"));old.remove("lightsTurn");
    QCOMPARE(PgsProject::fromJson(old,dir_.filePath("t.pgsproj")).rigTurnSeconds,5.f);

    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"turn.ply",2000,true)));
    window.project().lights.clear();window.addLight(false);window.project().lights[0].position={1,1,0};window.selectLight(-1);
    // A new capture's set turns once every 5 s; with it still and no light turning, Play does nothing.
    QCOMPARE(window.project().rigTurnSeconds,5.f);
    window.setRigTurn(0,false);
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

void PgsEditorTests::spaceTogglesTurning() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowActive(&window));
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
void PgsEditorTests::lightIconsHideBehindSplats() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
void PgsEditorTests::modelsHaveDefaults() {
    // This build knows where the models are (PGS_STEREO_MODEL, PGS_MOGE_MODEL), or a deployed
    // editor has them beside gs2mesh: either way, existing files or none.
    for (auto model:{Gs2Mesh::Model::Stereo,Gs2Mesh::Model::MoGe}) {
        const QString path=Gs2Mesh::defaultModel(model);
        if (!path.isEmpty()) QVERIFY2(QFileInfo::exists(path),qPrintable(path));
        qInfo("%s: %s",model==Gs2Mesh::Model::Stereo ? "stereo" : "MoGe",path.isEmpty() ? "none" : qPrintable(path));
    }
}

void PgsEditorTests::gizmoMovesTheSelectedLight() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
void PgsEditorTests::meshIsDrawnLit() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
void PgsEditorTests::sculptKeepsSeamsAndUndoes() {
    const QString path=writeSphereObj(dir_.path(),"ball",0.5f,{0,1,0});
    const MeshData data=readObj(path);
    // The seam's vertices share positions: more vertices than positions.
    QCOMPARE(data.positionCount,size_t(2+23*32));QVERIFY(data.vertices()>data.positionCount);
    SculptMesh s(data,QImage());
    // The ray from the front meets the sphere's front.
    QVector3D hit,normal;QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit,&normal));
    QVERIFY2(std::abs(hit.z()-0.5f)<0.01f && normal.z()>0.9f,qPrintable(QStringLiteral("%1 %2").arg(hit.z()).arg(normal.z())));
    // Inflate pushes out, Ctrl (inverted) in; each stroke one step.
    SculptMesh::Dab d;QVERIFY(landAt(s,{0,1,5},{0,0,-1},0.2f,&d));
    s.beginStroke(SculptMesh::Brush::Inflate,d);s.dab(d);QVERIFY(s.endStroke());
    QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));QVERIFY2(hit.z()>0.5f+0.002f,qPrintable(QString::number(hit.z())));
    const float inflated=hit.z();
    QVERIFY(landAt(s,{0,1,5},{0,0,-1},0.2f,&d,1.f,true));
    s.beginStroke(SculptMesh::Brush::Inflate,d);s.dab(d);QVERIFY(s.endStroke());
    QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));QVERIFY(hit.z()<inflated);
    // The seam stays shut: every vertex of a position where the position is, in the view too.
    {
        // A grab across the seam (x > 0, z = 0).
        QVERIFY(s.raycast({5,1,0},{-1,0,0},&hit));
        QVERIFY(landAt(s,{5,1,0},{-1,0,0},0.15f,&d));
        s.beginStroke(SculptMesh::Brush::Move,d);s.moveBy({0.1f,0,0});QVERIFY(s.endStroke());
        const auto view=s.view();const MeshData now=s.mesh();
        for (size_t v=0;v<now.vertices();++v) {
            const QVector3D p=at3(s.points(),now.positionIds[v]);
            QVERIFY((QVector3D(view->vertices[v*8],view->vertices[v*8+1],view->vertices[v*8+2])-p).length()<1e-6f);
        }
        QVERIFY(s.points()[size_t(now.positionIds[0])*3]==now.positions[0]);
    }
    // Undone to the start: exactly as built, and back.
    const std::vector<float> after=s.points();
    QCOMPARE(s.state()!=0,true);
    while (s.canUndo()) s.undo();
    QCOMPARE(s.points(),s.originalPoints());QCOMPARE(s.state(),quint64(0));
    while (s.canRedo()) s.redo();
    QCOMPARE(s.points(),after);
    // Written as gs2mesh writes meshes and read back: the same positions, in the same order.
    const QString edited=dir_.filePath("ball_edited.obj");
    writeObj(edited,s.mesh(),"ball.mtl","material_0");
    const MeshData back=readObj(edited);
    QCOMPARE(back.positionCount,data.positionCount);QCOMPARE(back.vertices(),data.vertices());QCOMPARE(back.triangles(),data.triangles());
    QVERIFY(!back.albedo.isEmpty());
    for (size_t v=0;v<back.vertices();++v) {
        QCOMPARE(back.positionIds[v],data.positionIds[v]);
        QVERIFY((at3(back.positions,v)-at3(s.points(),back.positionIds[v])).length()<1e-5f);
        QVERIFY(std::abs(back.uvs[v*2]-data.uvs[v*2])<1e-5f);
    }
}
void PgsEditorTests::sculptFixesSpikesAndPieces() {
    const MeshData data=readObj(writeSphereObj(dir_.path(),"spiky",0.5f,{0,1,0},true));
    SculptMesh s(data,QImage());
    const size_t triangles=s.triangles();
    // A spike: one point pulled out to twice the radius.
    QVector3D hit;QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));
    SculptMesh::Dab d;QVERIFY(landAt(s,{0,1,5},{0,0,-1},0.02f,&d));d.hardness=0.9f;
    s.beginStroke(SculptMesh::Brush::Move,d);s.moveBy({0,0,0.5f});QVERIFY(s.endStroke());
    float far=0;for (size_t p=0;p<s.points().size()/3;++p) far=std::max(far,(at3(s.points(),p)-QVector3D(0,1,0)).length());
    QVERIFY2(far>0.8f,qPrintable(QString::number(far)));
    const int found=s.fixSpikes();QVERIFY(found>0);
    // Gone: nothing far off the sphere any more, and the rest where it was.
    far=0;size_t moved=0;
    for (size_t p=0;p<2+23*32;++p) {
        far=std::max(far,(at3(s.points(),p)-QVector3D(0,1,0)).length());
        moved+=(at3(s.points(),p)-at3(s.originalPoints(),p)).length()>1e-3f;
    }
    QVERIFY2(far<0.58f,qPrintable(QStringLiteral("%1 still out, %2 points found").arg(far).arg(found)));
    QVERIFY2(moved<60,qPrintable(QStringLiteral("%1 points moved").arg(moved)));
    s.undo();
    far=0;for (size_t p=0;p<s.points().size()/3;++p) far=std::max(far,(at3(s.points(),p)-QVector3D(0,1,0)).length());
    QVERIFY(far>0.8f);
    s.redo();
    // The loose tetrahedron: its 4 triangles go, the sphere stays; undone, it is back.
    QCOMPARE(s.removeLoosePieces(),4);QCOMPARE(s.triangles(),triangles-4);
    QVERIFY(!s.raycast({1.5f+0.01f,1.01f,5},{0,0,-1},&hit));
    s.undo();QCOMPARE(s.triangles(),triangles);
    QCOMPARE(s.removeLoosePieces(),4);QCOMPARE(s.removeLoosePieces(),0);
}
// A tangle as fusion leaves in poorly seen slits: the points of a patch shaken until faces turn
// over. Relax under the brush, and Fix spikes over the whole mesh, each leave none turned over.
void PgsEditorTests::relaxUnfoldsATangle() {
    const MeshData data=readObj(writeSphereObj(dir_.path(),"tangled",0.5f,{0,1,0},false,48,64));
    const QVector3D centre(0,1,0);
    auto turned=[&](const SculptMesh &s) {
        // Faces facing in (the sphere's outside is away from its centre), near the front.
        int count=0;const MeshData m=s.mesh();
        for (size_t t=0;t<m.triangles();++t) {
            const QVector3D a=at3(m.positions,m.indices[t*3]),b=at3(m.positions,m.indices[t*3+1]),c=at3(m.positions,m.indices[t*3+2]);
            const QVector3D mid=(a+b+c)/3.f;
            if (mid.z()>0.3f && QVector3D::dotProduct(QVector3D::crossProduct(b-a,c-a),mid-centre)<0) ++count;
        }
        return count;};
    // Every position of the front patch moved at random by about its edges (3 to 5 cm); a seam's
    // vertices alike.
    MeshData shaken=data;
    {
        std::mt19937 random(3);std::uniform_real_distribution<float> shake(-0.045f,0.045f);
        std::vector<QVector3D> offset(data.positionCount);for (auto &o:offset) o=QVector3D(shake(random),shake(random),shake(random));
        for (size_t v=0;v<shaken.vertices();++v)
            if (shaken.positions[v*3+2]>0.38f) for (int a=0;a<3;++a) shaken.positions[v*3+a]+=offset[shaken.positionIds[v]][a];
    }
    {
        SculptMesh s(shaken,QImage());
        const int before=turned(s);QVERIFY2(before>10,qPrintable(QString::number(before)));
        QVector3D hit;QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));
        // The whole patch under the brush.
        SculptMesh::Dab d;QVERIFY(landAt(s,{0,1,5},{0,0,-1},0.5f,&d));d.hardness=0.6f;
        s.beginStroke(SculptMesh::Brush::Relax,d);
        for (int i=0;i<12;++i) s.dab(d);
        QVERIFY(s.endStroke());
        QVERIFY2(turned(s)==0,qPrintable(QStringLiteral("relax: %1 of %2 still turned over").arg(turned(s)).arg(before)));
        // Its shape kept: the front still where the sphere is.
        QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));QVERIFY2(std::abs(hit.z()-0.5f)<0.02f,qPrintable(QString::number(hit.z())));
    }
    {
        SculptMesh s(shaken,QImage());
        const int before=turned(s);QVERIFY(s.fixSpikes()>0);
        QVERIFY2(turned(s)==0,qPrintable(QStringLiteral("fix spikes: %1 of %2 still turned over").arg(turned(s)).arg(before)));
    }
}
// The texture brushes paint the albedo where the brush lands, on the faces turned to the eye;
// undone exactly; Erase paint brings back what gs2mesh made; in the window, written beside the
// edited mesh and named by its material.
void PgsEditorTests::texturePaintsTheFrontOnly() {
    const QString obj=writeSphereObj(dir_.path(),"painted",0.5f,{0,1,0});
    const MeshData data=readObj(obj);
    SculptMesh s(data,QImage(data.albedo));QVERIFY(s.hasAlbedo());
    const QImage before=s.albedo().copy();
    // The front (+z) is u = 0.25, v = 0.5: texel (16, 16) of 64 x 32; the back u = 0.75.
    auto texel=[&](int x,int y) {return s.albedo().pixelColor(x,y);};
    QCOMPARE(texel(16,16),QColor(200,100,50));
    SculptMesh::TextureDab dab;QVERIFY(s.raycast({0,1,5},{0,0,-1},&dab.centre));
    dab.eye={0,1,5};dab.radius=0.2f;dab.hardness=0.5f;dab.opacity=1.f;dab.colour=QColor(0,0,255);
    s.beginTextureStroke(SculptMesh::TextureBrush::Paint);QVERIFY(!s.textureDab(dab).isNull());QVERIFY(s.endStroke());
    QVERIFY2(texel(16,16).blue()>200 && texel(16,16).red()<40,qPrintable(texel(16,16).name()));
    QCOMPARE(texel(48,16),QColor(200,100,50));QVERIFY(s.albedoPainted());
    // Seen from behind, the same brush on the front paints nothing: those faces are turned away.
    s.beginTextureStroke(SculptMesh::TextureBrush::Paint);dab.eye={0,1,-5};dab.colour=QColor(0,255,0);
    QVERIFY(s.textureDab(dab).isNull());QVERIFY(!s.endStroke());dab.eye={0,1,5};
    // Undone: exactly as before; redone: blue again.
    QVERIFY(!s.undo().isNull());QVERIFY(s.albedo()==before);
    QVERIFY(!s.redo().isNull());QVERIFY(texel(16,16).blue()>200);
    // Erase paint: back to the albedo it was built with.
    s.beginTextureStroke(SculptMesh::TextureBrush::Unpaint);s.textureDab(dab);s.textureDab(dab);s.textureDab(dab);QVERIFY(s.endStroke());
    QVERIFY2(std::abs(texel(16,16).red()-200)<12,qPrintable(texel(16,16).name()));
    // Lighten and Darken move the texels' brightness.
    s.beginTextureStroke(SculptMesh::TextureBrush::Darken);s.textureDab(dab);QVERIFY(s.endStroke());
    QVERIFY(texel(16,16).red()<190);

    // In the window: painted, written as <name>_edited_albedo.png, its material naming it.
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"topaint.ply",3000,true)));
    QDir().mkpath(dir_.filePath("topaint_mesh"));
    const QString main=writeSphereObj(dir_.filePath("topaint_mesh"),"topaint",0.5f,{0,1,0});
    window.project().meshFiles={main};window.setShowMesh(true);
    window.setSculptTool(PgsWindow::Paint);QCOMPARE(window.sculptTool(),int(PgsWindow::Paint));
    // Painting shows the texture, lit as usual.
    QVERIFY(window.viewport()->meshTextured());QVERIFY(!window.viewport()->meshShapeLight());
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=2.5f;window.viewport()->setCamera(camera);
    const QPointF centre(window.viewport()->width()/2.0,window.viewport()->height()/2.0);
    window.sculptStroke(centre,0,Qt::LeftButton,{});window.sculptStroke(centre+QPointF(6,0),1,Qt::LeftButton,{});window.sculptStroke(centre+QPointF(6,0),2,Qt::LeftButton,{});
    QVERIFY(window.sculptDirty());QVERIFY(window.saveSculpt());
    const QString albedo=dir_.filePath("topaint_mesh/topaint_edited_albedo.png");
    QVERIFY(QFileInfo::exists(albedo));
    const MeshData back=readObj(dir_.filePath("topaint_mesh/topaint_edited.obj"));
    QCOMPARE(QFileInfo(back.albedo).fileName(),QStringLiteral("topaint_edited_albedo.png"));
    QVERIFY(QImage(albedo)!=QImage(dir_.filePath("topaint_mesh/topaint_albedo.png")));
    // The generated albedo stays as it was.
    QCOMPARE(QImage(dir_.filePath("topaint_mesh/topaint_albedo.png")).pixelColor(16,16),QColor(200,100,50));
    window.revertSculpt(false);QVERIFY(!QFileInfo::exists(albedo));
}
// Across a fold the vertex normals can cancel out, and a surface can face the view exactly:
// neither may draw black (normalize(0), pow of a negative: NaN). A quad whose two sides' normals
// point opposite ways, seen head-on in the sculpting light.
void PgsEditorTests::noBlackWhereNormalsCancel() {
    QImage albedo(16,16,QImage::Format_RGB888);albedo.fill(QColor(200,200,200));albedo.save(dir_.filePath("cancel_albedo.png"));
    {QFile mtl(dir_.filePath("cancel.mtl"));mtl.open(QIODevice::WriteOnly);mtl.write("newmtl m\nmap_Kd cancel_albedo.png\n");}
    {QFile obj(dir_.filePath("cancel.obj"));obj.open(QIODevice::WriteOnly);
     obj.write("mtllib cancel.mtl\nv -1 0 0\nv 1 0 0\nv 1 2 0\nv -1 2 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nvn 0 0 -1\n"
               "f 1/1/1 2/2/1 3/3/2\nf 1/1/1 3/3/2 4/4/2\n");}
    PgsWindow window;window.resize(700,600);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"cancel.ply",500,true)));
    window.project().meshFiles={dir_.filePath("cancel.obj")};window.setShowMesh(true);QVERIFY(window.showsMesh());
    auto *viewport=window.viewport();viewport->setMeshTextured(false);viewport->setMeshShapeLight(true);
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=3.f;viewport->setCamera(camera);
    viewport->update();QTest::qWait(80);const QImage shot=viewport->grabFramebuffer();
    // The middle of the quad, where its normals cancel and where it faces the view: never black.
    int black=0,seen=0;
    for (int y=shot.height()/2-60;y<=shot.height()/2+60;y+=2)
        for (int x=shot.width()/2-60;x<=shot.width()/2+60;x+=2) {++seen;black+=luminance(shot.pixelColor(x,y))<0.05;}
    QVERIFY2(black==0,qPrintable(QStringLiteral("%1 of %2 pixels black").arg(black).arg(seen)));
}
// With a brush a double-click paints twice and leaves the view where it is (no focus jump).
void PgsEditorTests::doubleClickPaintsWithABrush() {
    PgsWindow window;window.resize(800,600);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"dbl.ply",2000,true)));
    QDir().mkpath(dir_.filePath("dbl_mesh"));
    window.project().meshFiles={writeSphereObj(dir_.filePath("dbl_mesh"),"dbl",0.5f,{0,1,0})};window.setShowMesh(true);
    window.setSculptTool(PgsWindow::Draw);QCOMPARE(window.sculptTool(),int(PgsWindow::Draw));
    // A brush larger than this sphere's triangles (5 to 9 cm).
    const double radiusBefore=window.findChild<QDoubleSpinBox *>("brushRadius")->value();
    window.findChild<QDoubleSpinBox *>("brushRadius")->setValue(15);
    auto *viewport=window.viewport();
    Camera camera;camera.target={0.2f,1.1f,0};camera.yaw=0;camera.pitch=0;camera.distance=2.5f;viewport->setCamera(camera);
    QSignalSpy strokes(viewport,&Viewport::paintStroke);
    const QPoint at(viewport->width()/2,viewport->height()/2);
    // As the system sends a double-click: press, release, double-click (in place of the second
    // press), release.
    auto send=[&](QEvent::Type type) {
        QMouseEvent e(type,QPointF(at),viewport->mapToGlobal(QPointF(at)),Qt::LeftButton,type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&e);};
    send(QEvent::MouseButtonPress);send(QEvent::MouseButtonRelease);send(QEvent::MouseButtonDblClick);send(QEvent::MouseButtonRelease);
    QVERIFY((viewport->camera().target-camera.target).length()<1e-6f);
    // Two strokes, each pressed and released.
    QVERIFY2(strokes.size()==4,qPrintable(QString::number(strokes.size())));
    QVERIFY(window.sculpt()->canUndo());
    window.findChild<QDoubleSpinBox *>("brushRadius")->setValue(radiusBefore); // the brush is remembered on this machine
}
void PgsEditorTests::editsReachTheDenseSurface() {
    // The dense surface: a finer cloud on the same sphere (here its positions, a little inside).
    const MeshData data=readObj(writeSphereObj(dir_.path(),"densed",0.5f,{0,1,0}));
    SculptMesh s(data,QImage());
    QVector3D hit;QVERIFY(s.raycast({0,1,5},{0,0,-1},&hit));
    SculptMesh::Dab d;QVERIFY(landAt(s,{0,1,5},{0,0,-1},0.15f,&d));
    s.beginStroke(SculptMesh::Brush::Move,d);s.moveBy({0,0,0.1f});QVERIFY(s.endStroke());
    std::vector<QVector3D> dense;
    for (size_t p=0;p<s.originalPoints().size()/3;++p) dense.push_back(QVector3D(0,1,0)+(at3(s.originalPoints(),p)-QVector3D(0,1,0))*0.995f);
    QByteArray ply=QStringLiteral("ply\nformat binary_little_endian 1.0\nelement vertex %1\nproperty float x\nproperty float y\nproperty float z\nelement face 0\nproperty list uchar int vertex_indices\nend_header\n").arg(dense.size()).toUtf8();
    for (const auto &d:dense) for (float c:{d.x(),d.y(),d.z()}) ply.append(reinterpret_cast<const char *>(&c),4);
    const QString in=dir_.filePath("ball_dense.ply"),out=dir_.filePath("ball_edited_dense.ply");
    {QFile f(in);f.open(QIODevice::WriteOnly);f.write(ply);}
    transferEdits(s.originalPoints(),s.points(),in,out);
    QFile f(out);QVERIFY(f.open(QIODevice::ReadOnly));const QByteArray moved=f.readAll();
    QCOMPARE(moved.size(),ply.size());
    const char *body=moved.constData()+moved.indexOf("end_header\n")+11;
    float front=0,back=0;
    for (size_t i=0;i<dense.size();++i) {
        const float *q=reinterpret_cast<const float *>(body)+i*3;const QVector3D d=QVector3D(q[0],q[1],q[2])-dense[i];
        if (dense[i].z()>0.49f) front=std::max(front,d.z());
        if (dense[i].z()<0) back=std::max(back,d.length());
    }
    // Where the mesh was pulled the dense surface follows; behind it nothing moved.
    QVERIFY2(front>0.08f,qPrintable(QString::number(front)));QVERIFY2(back<1e-6f,qPrintable(QString::number(back)));
}
void PgsEditorTests::sculptingInTheWindow() {
    PgsWindow window;window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();QVERIFY(QTest::qWaitForWindowActive(&window));
    QVERIFY(window.openPly(writeSphere(dir_.path(),"tosculpt.ply",3000,true)));
    QDir().mkpath(dir_.filePath("tosculpt_mesh"));
    const QString obj=writeSphereObj(dir_.filePath("tosculpt_mesh"),"tosculpt",0.5f,{0,1,0});
    const QString lod=writeSphereObj(dir_.filePath("tosculpt_mesh"),"tosculpt_lod1",0.5f,{0,1,0},false,8,10);
    window.project().meshFiles={obj,lod};window.project().lodsSource=PgsProject::fileSignature(obj);
    window.loadMesh(0);
    auto *viewport=window.viewport();viewport->setFocus();
    // Tab: the mesh, and the tools for it; Tab again: the splats, the tools gone.
    QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(window.showsMesh());
    auto *section=window.findChild<QWidget *>("sculptSection");QVERIFY(section->isVisible());
    QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(!window.showsMesh());QVERIFY(!section->isVisible());
    window.setShowMesh(true);QVERIFY(section->isVisible());
    // A brush: the view paints, the mesh grey and lit to read it; Navigate gives the look back.
    QTest::keyClick(&window,Qt::Key_F3,Qt::ControlModifier);QCOMPARE(window.sculptTool(),int(PgsWindow::Inflate));
    QVERIFY(window.findChild<QToolBar *>("sculptToolbar")->isVisible());
    QCOMPARE(viewport->selectTool(),Viewport::SelectTool::Paint);QVERIFY(!viewport->meshTextured());QVERIFY(viewport->meshShapeLight());
    Camera camera;camera.target={0,1,0};camera.yaw=0;camera.pitch=0;camera.distance=2.5f;viewport->setCamera(camera);
    const QPointF centre(viewport->width()/2.0,viewport->height()/2.0);
    QVector3D o,d,before;viewport->captureRay(centre,&o,&d);QVERIFY(window.sculpt()->raycast(o,d,&before));
    window.sculptStroke(centre,0,Qt::LeftButton,{});window.sculptStroke(centre+QPointF(12,0),1,Qt::LeftButton,{});window.sculptStroke(centre+QPointF(12,0),2,Qt::LeftButton,{});
    QVector3D after;QVERIFY(window.sculpt()->raycast(o,d,&after));
    QVERIFY2((after-QVector3D(0,1,0)).length()>(before-QVector3D(0,1,0)).length()+1e-3f,qPrintable(QStringLiteral("%1 -> %2").arg(before.z()).arg(after.z())));
    QVERIFY(window.sculptDirty());
    // The LODs are now of another mesh: said so.
    QVERIFY(window.findChildren<QLabel *>().size()>0);
    bool told=false;for (auto *l:window.findChildren<QLabel *>()) told=told || l->text().contains("LODs are of the mesh before");
    QVERIFY(told);
    // Undone back to as it was: nothing to write; redone and written: <main>_edited.obj, the main mesh now.
    window.undoSculpt();QVERIFY(!window.sculptDirty());
    window.redoSculpt();QVERIFY(window.sculptDirty());
    QVERIFY(window.saveSculpt());QVERIFY(!window.sculptDirty());
    const QString edited=dir_.filePath("tosculpt_mesh/tosculpt_edited.obj");
    QCOMPARE(window.project().meshEdited,edited);QVERIFY(QFileInfo::exists(edited));QVERIFY(QFileInfo::exists(dir_.filePath("tosculpt_mesh/tosculpt_edited.mtl")));
    QCOMPARE(window.project().meshObjs().value(0),edited);QCOMPARE(window.project().meshObjs().value(1),lod);QCOMPARE(window.project().generatedObj(),obj);
    QVERIFY(window.project().lodsOutOfDate());
    // Kept in the project, relative.
    const PgsProject reread=PgsProject::fromJson(window.project().json(dir_.filePath("p.pgsproj")),dir_.filePath("p.pgsproj"));
    QCOMPARE(reread.meshEdited,edited);
    // Only on the main mesh: the LOD shown, the tools go (and the brush with them).
    window.findChild<QPushButton *>("displayMeshLevel")->click();QVERIFY(!section->isVisible());QCOMPARE(window.sculptTool(),int(PgsWindow::Navigate));
    QVERIFY(!window.findChild<QToolBar *>("sculptToolbar")->isVisible());
    QVERIFY(viewport->meshTextured());QVERIFY(!viewport->meshShapeLight());
    window.findChild<QPushButton *>("displayMeshLevel")->click();QVERIFY(section->isVisible());
    // Reverted: the generated mesh again, the edit's files gone.
    window.revertSculpt(false);
    QVERIFY(window.project().meshEdited.isEmpty());QVERIFY(!QFileInfo::exists(edited));QCOMPARE(window.project().meshObjs().value(0),obj);
}
// PGS_TEST_MESH=<main.obj> (with PGS_TEST_SHOTS=<folder> for pictures): sculpting a real mesh,
// how long its parts take and what the fixes find.
void PgsEditorTests::sculptRealMesh() {
    const QString obj=qEnvironmentVariable("PGS_TEST_MESH");
    if (obj.isEmpty()) QSKIP("PGS_TEST_MESH names no mesh.");
    QElapsedTimer timer;timer.start();
    const MeshData data=readObj(obj);const double readMs=timer.restart();
    SculptMesh s(data,data.albedo.isEmpty() ? QImage() : QImage(data.albedo));const double buildMs=timer.restart();
    const int spikes=s.fixSpikes();const double spikesMs=timer.restart();
    const int pieces=s.removeLoosePieces();const double piecesMs=timer.restart();
    const QVector3D lo=data.minimum(),hi=data.maximum(),mid=(lo+hi)*0.5f;
    QVector3D hit;const bool front=s.raycast(QVector3D(mid.x(),mid.y()+0.25f*(hi.y()-lo.y()),hi.z()+1),{0,0,-1},&hit);QVERIFY(front);
    SculptMesh::Dab d;QVERIFY(landAt(s,QVector3D(mid.x(),mid.y()+0.25f*(hi.y()-lo.y()),hi.z()+1),{0,0,-1},0.03f,&d,0.5f));
    s.beginStroke(SculptMesh::Brush::Smooth,d);
    for (int i=0;i<60;++i) {d.centre=hit+QVector3D(0.001f*i,0,0);d.triangle=-1;s.dab(d);}
    s.endStroke();const double strokeMs=timer.restart();
    // Painting its albedo: a stroke of 30 dabs (the stroke's start copies the albedo, for its undo).
    double paintMs=0;
    if (s.hasAlbedo()) {
        timer.restart();
        s.beginTextureStroke(SculptMesh::TextureBrush::Paint);
        SculptMesh::TextureDab t;t.radius=0.03f;t.opacity=0.6f;t.colour=QColor(30,90,220);t.eye=QVector3D(mid.x(),mid.y(),hi.z()+2);
        for (int i=0;i<30;++i) {t.centre=hit+QVector3D(0.002f*i,0,0);s.textureDab(t);}
        s.endStroke();s.padAlbedo();paintMs=timer.restart();
    }
    qInfo("%s: %zu triangles, %zu positions; read %.0f ms, made editable %.0f ms; fix spikes %d points %.0f ms; loose pieces %d triangles %.0f ms; 60 dabs %.0f ms; a painted stroke of 30 dabs on its %dx%d albedo, padded, %.0f ms",
          qPrintable(QFileInfo(obj).fileName()),data.triangles(),data.positionCount,readMs,buildMs,spikes,spikesMs,pieces,piecesMs,strokeMs,
          s.albedo().width(),s.albedo().height(),paintMs);
    if (const QString shots=qEnvironmentVariable("PGS_TEST_SHOTS");!shots.isEmpty()) {
        QDir().mkpath(shots);
        PgsWindow window;window.resize(1400,1000);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openPly(writeSphere(dir_.path(),"forshots.ply",500,true)));
        window.project().meshFiles={obj};window.setShowMesh(true);QVERIFY(window.showsMesh());
        auto *viewport=window.viewport();
        Camera camera;camera.target=QVector3D(mid.x(),mid.y()+0.3f*(hi.y()-lo.y()),mid.z());camera.yaw=0;camera.pitch=0;camera.distance=0.9f;viewport->setCamera(camera);
        window.setSculptTool(PgsWindow::Smooth);
        auto save=[&](const QString &name) {viewport->update();QTest::qWait(200);viewport->grabFramebuffer().save(shots+"/"+name);};
        save("sculpt_before.png");
        window.fixSpikes();save("sculpt_fixspikes.png");
        window.removeLoosePieces();
        QTest::mouseMove(viewport,QPoint(viewport->width()/2,viewport->height()/2));save("sculpt_brush.png");
        // A painted stroke across the chest, blue, and the texture brushes' look.
        window.setSculptTool(PgsWindow::Paint);
        const QPointF from(viewport->width()*0.42,viewport->height()*0.62),to(viewport->width()*0.58,viewport->height()*0.62);
        window.sculptStroke(from,0,Qt::LeftButton,{});
        for (int i=1;i<=20;++i) window.sculptStroke(from+(to-from)*(i/20.0),1,Qt::LeftButton,{});
        window.sculptStroke(to,2,Qt::LeftButton,{});
        QTest::mouseMove(viewport,to.toPoint());save("sculpt_paint.png");
        window.setSculptTool(PgsWindow::Navigate);save("sculpt_textured.png");
        window.grab().save(shots+"/sculpt_window.png");
    }
}
// PGS_TEST_LODS=<main.obj gs2mesh made> with PGS_TEST_PLY=<its capture>: a copy of that mesh
// edited, then its LODs made by gs2mesh from the edit (minutes, an NVIDIA GPU).
void PgsEditorTests::lodsFromEditedMesh() {
    const QString source=qEnvironmentVariable("PGS_TEST_LODS"),ply=qEnvironmentVariable("PGS_TEST_PLY");
    if (source.isEmpty() || ply.isEmpty()) QSKIP("PGS_TEST_LODS and PGS_TEST_PLY name no mesh and capture.");
    if (!Gs2Mesh::rebakesMain()) QSKIP("This gs2mesh cannot bake an edited main mesh again.");
    // The mesh's files, copied: the original stays as it is.
    const QFileInfo main(source);const QString stem=main.completeBaseName(),folder=dir_.filePath("lods_mesh");QDir().mkpath(folder);
    QStringList files;
    for (const QString &suffix:{".obj",".mtl","_albedo.png","_normal.png","_dense.ply"}) {
        const QString from=main.absolutePath()+"/"+stem+suffix,to=folder+"/"+stem+suffix;
        if (QFileInfo::exists(from)) {QVERIFY(QFile::copy(from,to));files << to;}
    }
    PgsWindow window;window.resize(1200,900);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));
    window.project().meshFiles=files;window.project().mesh.lods={20000};
    window.setShowMesh(true);QVERIFY(window.ensureSculpt());
    // An edit worth seeing: the top of the mesh pulled up 2 cm, and the spikes fixed.
    const MeshData data=readObj(files.first());const QVector3D lo=data.minimum(),hi=data.maximum();
    QVector3D hit;QVERIFY(window.sculpt()->raycast(QVector3D((lo.x()+hi.x())/2,hi.y()+1,(lo.z()+hi.z())/2),{0,-1,0},&hit));
    SculptMesh::Dab d;QVERIFY(landAt(*window.sculpt(),QVector3D((lo.x()+hi.x())/2,hi.y()+1,(lo.z()+hi.z())/2),{0,-1,0},0.08f,&d));
    window.sculpt()->beginStroke(SculptMesh::Brush::Move,d);window.sculpt()->moveBy({0,0.02f,0});QVERIFY(window.sculpt()->endStroke());
    window.fixSpikes();QVERIFY(window.sculptDirty());
    const std::vector<float> edited=window.sculpt()->points();
    QSignalSpy finished(&window,&PgsWindow::meshFinished);
    QVERIFY(window.startLods());
    QVERIFY(finished.wait(30*60*1000));
    QVERIFY2(finished.first().at(0).toBool(),qPrintable(finished.first().at(1).toString()));
    const QString editedObj=folder+"/"+stem+"_edited.obj";
    QCOMPARE(window.project().meshEdited,editedObj);QVERIFY(!window.sculptDirty());
    // Its own normal map, baked again, named by its material; the dense surface moved alike.
    QVERIFY(QFileInfo::exists(folder+"/"+stem+"_edited_normal.png"));QVERIFY(QFileInfo::exists(folder+"/"+stem+"_edited_dense.ply"));
    const MeshData back=readObj(editedObj);
    QCOMPARE(QFileInfo(back.normalMap).fileName(),stem+"_edited_normal.png");QCOMPARE(QFileInfo(back.albedo).fileName(),stem+"_albedo.png");
    // gs2mesh wrote the edit as it was: the same positions in the same order.
    QCOMPARE(back.positionCount,data.positionCount);
    double worst=0;for (size_t v=0;v<back.vertices();++v) worst=std::max(worst,double((at3(back.positions,v)-at3(edited,back.positionIds[v])).length()));
    QVERIFY2(worst<1e-4,qPrintable(QString::number(worst)));
    // The LOD: of the edit, up to date, the edit itself not taken for one.
    const QStringList objs=window.project().meshObjs();
    QCOMPARE(objs.size(),2);QCOMPARE(objs.first(),editedObj);QCOMPARE(QFileInfo(objs.last()).completeBaseName(),stem+"_edited_lod1");
    QVERIFY(!window.project().lodsOutOfDate());
    const MeshData lod=readObj(objs.last());
    QVERIFY2(lod.triangles()<=20000 && lod.triangles()>15000,qPrintable(QString::number(lod.triangles())));
    // It follows the edit: as high as the pulled top.
    QVERIFY2(lod.maximum().y()>hi.y()+0.01f,qPrintable(QStringLiteral("%1 vs %2").arg(lod.maximum().y()).arg(hi.y())));
    qInfo("LODs of the edited mesh: %s, %zu triangles, top %.3f (was %.3f)",qPrintable(QFileInfo(objs.last()).fileName()),lod.triangles(),lod.maximum().y(),hi.y());
}
// PGS_TEST_PLY, PGS_TEST_MESH (one or more .obj, ';'-separated), PGS_TEST_SHOTS and PGS_TEST_VIEW
// ("target x,y,z,yaw,pitch,distance", several separated by ';'): the splats and each mesh, textured,
// from the same cameras, side by side, to compare what the mesh's texture and shape keep.
void PgsEditorTests::meshAgainstSplats() {
    const QString ply=qEnvironmentVariable("PGS_TEST_PLY"),meshes=qEnvironmentVariable("PGS_TEST_MESH"),out=qEnvironmentVariable("PGS_TEST_SHOTS"),views=qEnvironmentVariable("PGS_TEST_VIEW");
    if (ply.isEmpty() || meshes.isEmpty() || out.isEmpty() || views.isEmpty()) QSKIP("PGS_TEST_PLY, PGS_TEST_MESH, PGS_TEST_SHOTS and PGS_TEST_VIEW name no capture, mesh, folder and view.");
    QDir().mkpath(out);
    PgsWindow window;window.resize(1400,1000);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));
    window.project().lights.clear();window.selectLight(-1);
    auto *viewport=window.viewport();
    // A light background, as one judges a texture, and the same size for every shot.
    viewport->setLightBackground(true);viewport->setFixedSize(1000,900);
    int v=0;
    for (const QString &view:views.split(';',Qt::SkipEmptyParts)) {
        const QStringList n=view.split(',');QCOMPARE(n.size(),6);
        Camera camera;camera.target={n[0].toFloat(),n[1].toFloat(),n[2].toFloat()};camera.yaw=n[3].toFloat();camera.pitch=n[4].toFloat();camera.distance=n[5].toFloat();
        QList<QImage> shots;
        window.setShowMesh(false);viewport->setCamera(camera);viewport->update();QTest::qWait(300);shots << viewport->grabFramebuffer();
        for (const QString &obj:meshes.split(';',Qt::SkipEmptyParts)) {
            window.project().meshFiles={obj};window.loadMesh(0);window.setShowMesh(true);
            viewport->setMeshTextured(qEnvironmentVariable("PGS_TEST_GRAY")!="1"); // PGS_TEST_GRAY=1: the shape alone
            viewport->setCamera(camera);viewport->update();QTest::qWait(300);shots << viewport->grabFramebuffer();
        }
        // side by side: the splats, then each mesh in the order given
        const int w=shots.first().width(),h=shots.first().height();
        QImage sheet(w*int(shots.size()),h,QImage::Format_RGB32);sheet.fill(Qt::black);
        QPainter painter(&sheet);
        for (int i=0;i<shots.size();++i) painter.drawImage(i*w,0,shots[i]);
        painter.end();
        sheet.save(QStringLiteral("%1/compare_%2.png").arg(out).arg(v++));
    }
}
void PgsEditorTests::meshQueue() {
    // Captures from folders of their own, one with its photos beside it.
    const QString a=dir_.filePath("queue_a"),b=dir_.filePath("queue_b");QDir().mkpath(a);QDir().mkpath(b);
    const QString plyA=writeSphere(a,"alpha.ply",2000,true),plyB=writeSphere(b,"beta.ply",2000,true);
    writeColmap(a+"/colmap",8,3.f,{0,1,0});
    QSettings().remove("MeshQueue/Items");
    {
        PgsMeshQueue queue(nullptr,{});
        QVERIFY(queue.addPly(plyA));QVERIFY(queue.addPly(plyB));
        QVERIFY(!queue.addPly(plyA));QVERIFY(!queue.addPly(dir_.filePath("none.ply")));QVERIFY(!queue.addPly(a+"/colmap"));
        QCOMPARE(queue.count(),2);QCOMPARE(queue.item(0).triangles,200000);QCOMPARE(queue.item(1).triangles,200000);
        QVERIFY(QDir::cleanPath(queue.item(0).colmap).startsWith(QDir::cleanPath(a+"/colmap")));QVERIFY(queue.item(1).colmap.isEmpty());
        queue.setTriangles(1,100000);queue.setColmap(1,a+"/colmap");QVERIFY(!queue.item(1).colmap.isEmpty());queue.setColmap(1,"");
        QCOMPARE(PgsMeshQueue::projectPath(plyA),a+"/alpha.pgsproj");QCOMPARE(PgsMeshQueue::meshFolder(plyB),b+"/beta_mesh");
        if (const QString shots=qEnvironmentVariable("PGS_TEST_SHOTS");!shots.isEmpty()) {
            queue.show();QVERIFY(QTest::qWaitForWindowExposed(&queue));QDir().mkpath(shots);queue.grab().save(shots+"/mesh_queue.png");
        }
    }
    // Kept between sessions.
    {
        PgsMeshQueue again(nullptr,{});QCOMPARE(again.count(),2);QCOMPARE(again.item(1).triangles,100000);QVERIFY(again.item(1).colmap.isEmpty());
        QCOMPARE(again.item(0).state,PgsMeshQueue::State::Waiting);
        again.removeRow(0);again.removeRow(0);QCOMPARE(again.count(),0);
    }
    // The whole way on a real capture (PGS_TEST_GS2MESH=1, PGS_TEST_PLY), copied so that its own project is not touched; the
    // one open in the editor waits: it is not made behind the editor's back.
    const QString real=qEnvironmentVariable("PGS_TEST_PLY");
    if (qEnvironmentVariable("PGS_TEST_GS2MESH")!="1" || real.isEmpty()) return;
    const QString c=dir_.filePath("queue_real");QDir().mkpath(c);const QString copy=c+"/"+QFileInfo(real).fileName();
    QVERIFY(QFile::copy(real,copy));
    PgsWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(plyB));
    PgsMeshQueue *queue=window.meshQueue();
    QVERIFY(queue->addPly(copy));QVERIFY(queue->addPly(plyB));
    QSignalSpy done(queue,&PgsMeshQueue::finishedAll);
    queue->start();QVERIFY(queue->running());
    // While it runs the editor's own gs2mesh waits for it (after saying so).
    QTimer::singleShot(500,[] {if (QWidget *w=QApplication::activeModalWidget()) w->close();});
    QVERIFY(!window.startMesh());
    QVERIFY(done.wait(15*60*1000));
    QCOMPARE(queue->item(0).state,PgsMeshQueue::State::Done);
    QCOMPARE(queue->item(1).state,PgsMeshQueue::State::Failed);QVERIFY(queue->item(1).status.contains("Open in the editor"));
    qInfo("queue: %s",qPrintable(queue->item(0).status));
    // The project opens with its mesh, fine normals and two rim lights, and makes its LODs.
    QVERIFY(window.openProject(PgsMeshQueue::projectPath(copy)));
    QVERIFY(window.project().fineNormals);QVERIFY(!window.project().meshObjs().isEmpty());QCOMPARE(window.project().lights.size(),2);
    QVERIFY(window.project().colmapDeclined || !window.project().colmap.isEmpty());
    QVERIFY(QFileInfo(window.project().meshObjs().first()).absolutePath().endsWith("_mesh"));
    QSettings().remove("MeshQueue/Items");
}

void PgsEditorTests::realCapture() {
    const QString ply=qEnvironmentVariable("PGS_TEST_PLY");
    if (ply.isEmpty()) QSKIP("PGS_TEST_PLY names no capture.");
    QElapsedTimer timer;timer.start();
    auto capture=PgsCapture::load(ply);const double loadMs=timer.restart();
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
        qInfo("42 cameras: %.0f ms; %.2f%% of the splats face the other way with %d, %.2f%% differ by more than 15 degrees",
              fewerMs,100.0*double(flipped)/double(compared),PgsCapture::SurroundingViews,100.0*double(turned)/double(compared));
    }
    timer.restart();
    const QString out=dir_.filePath("real.pgs");
    const auto result=exportPgs(capture,{},out);
    qInfo("exported %.1f MB in %.0f ms: %s",result.bytes/1e6,double(timer.restart()),qPrintable(result.notes.join(' ')));
    QVERIFY(vgsdec::Capture::openFile(out.toStdString()).setTime(0,true).normals);
    if (qEnvironmentVariable("PGS_TEST_GS2MESH")!="1") return;
    // The whole way: gs2mesh from the splats alone, then the .pgs with its lowest LOD.
    PgsWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));
    // PGS_TEST_PHOTOS=1: with the COLMAP found beside the PLY; else from the splats alone.
    if (qEnvironmentVariable("PGS_TEST_PHOTOS")!="1") {window.project().colmapDeclined=true;window.project().colmap.clear();}
    else QVERIFY2(!window.project().colmap.isEmpty(),"no COLMAP beside the PLY");
    QSignalSpy finished(&window,&PgsWindow::meshFinished);
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
    if (const QString shots=qEnvironmentVariable("PGS_TEST_SHOTS");!shots.isEmpty()) {
        QDir().mkpath(shots);QTest::qWait(40000);window.grab().save(shots+"/mesh_busy.png");
    }
    QVERIFY(finished.wait(30*60*1000));
    QVERIFY2(!back,"the mesh's progress went back");
    QVERIFY(window.findChild<QListWidget *>("lights")->isEnabled());
    QVERIFY2(finished.first().at(0).toBool(),qPrintable(finished.first().at(1).toString()));
    QVERIFY(!window.project().meshOutOfDate());
    // The mesh comes alone; its LODs afterwards, from it.
    QCOMPARE(window.project().meshObjs().size(),qsizetype(1));
    QVERIFY(window.findChild<QPushButton *>("generateLods")->isEnabled());
    QSignalSpy lodsDone(&window,&PgsWindow::meshFinished);
    QVERIFY(window.startLods());
    QVERIFY(lodsDone.wait(10*60*1000));
    QVERIFY2(lodsDone.first().at(0).toBool(),qPrintable(lodsDone.first().at(1).toString()));
    QCOMPARE(window.project().meshObjs().size(),qsizetype(1+window.project().mesh.lods.size()));
    // They share the main mesh's albedo, each with its own normal map.
    for (const auto &obj:window.project().meshObjs().mid(1)) {
        const MeshData lod=readObj(obj);QVERIFY(lod.albedo.endsWith("_albedo.png"));QVERIFY(lod.normalMap.contains("_lod"));
    }
    // Beside the view cube, the LOD button goes through them.
    auto *level=window.findChild<QPushButton *>("displayMeshLevel");QVERIFY(level->isVisible());
    const QString shownFirst=level->text();level->click();QCOMPARE(level->text(),QStringLiteral("LOD: Main"));level->click();QVERIFY(level->text()!=shownFirst);
    const QString pgs=dir_.filePath("real_mesh.pgs");const auto withMesh=window.exportPgsTo(pgs);
    auto read=vgsdec::Capture::openFile(pgs.toStdString());QVERIFY(read.hasProxyMesh());
    const auto glb=read.proxyMesh();const GlbInfo info=inspectGlb(QByteArray(reinterpret_cast<const char *>(glb.data()),qsizetype(glb.size())));
    QVERIFY2(info.error.isEmpty(),qPrintable(info.error));
    qInfo("with mesh: %.1f MB, glb %zu triangles, %d images",withMesh.bytes/1e6,info.triangles,info.images);
    if (const QString keep=qEnvironmentVariable("PGS_TEST_KEEP");!keep.isEmpty()) {QFile::remove(keep);QFile::copy(pgs,keep);}
}

// Pictures to look at (PGS_TEST_SHOTS=folder, with PGS_TEST_PLY and, for the mesh,
// PGS_TEST_MESH=its .obj): the window, and the capture unlit, under a sun, under a bulb,
// and its mesh lit the same way.
void PgsEditorTests::visualSheet() {
    const QString out=qEnvironmentVariable("PGS_TEST_SHOTS"),ply=qEnvironmentVariable("PGS_TEST_PLY");
    if (out.isEmpty() || ply.isEmpty()) QSKIP("PGS_TEST_SHOTS and PGS_TEST_PLY name no folder and capture.");
    QDir().mkpath(out);
    PgsWindow window;window.resize(1600,1000);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openPly(ply));
    auto *viewport=window.viewport();const auto *c=window.capture();
    const auto rims=window.project().lights;window.project().lights.clear();window.selectLight(-1);
    const QVector3D centre=(c->minimum+c->maximum)*0.5f;const float height=c->maximum.y()-c->minimum.y();
    Camera camera;camera.target=centre;camera.yaw=float(qEnvironmentVariable("PGS_TEST_YAW","0").toDouble());camera.pitch=5;camera.distance=height*1.6f;viewport->setCamera(camera);
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
    for (int i=0;i<PgsLight::presetNames().size();++i) {window.applyPreset(i);save(QStringLiteral("p%1.png").arg(i));}
    camera.yaw=float(qEnvironmentVariable("PGS_TEST_YAW","0").toDouble());viewport->setCamera(camera);
    window.project().lights.clear();window.addLight(true);window.project().lights[0].intensity=0.8f;window.addLight(false);window.project().lights[1].colour=QColor(255,170,110);window.selectLight(-1);
    if (const QString obj=qEnvironmentVariable("PGS_TEST_MESH");!obj.isEmpty()) {
        // As a click on Show beside the view cube does: the mesh, then grey.
        window.project().meshFiles={obj};window.findChild<QPushButton *>("displayShow")->click();
        QVERIFY(viewport->drawsMesh());QVERIFY(window.findChild<QPushButton *>("displayMeshLook")->isVisible());
        window.selectLight(-1);save("6_mesh_lit.png");
        for (auto &l:window.project().lights) l.enabled=false;window.selectLight(-1);save("7_mesh_unlit.png");
        window.findChild<QPushButton *>("displayMeshLook")->click();QVERIFY(!viewport->meshTextured());save("7b_mesh_gray.png");
        // S switches it back, and again.
        window.activateWindow();QVERIFY(QTest::qWaitForWindowActive(&window));
        viewport->setFocus();QTest::keyClick(viewport,Qt::Key_S);QVERIFY(viewport->meshTextured());
        QTest::keyClick(viewport,Qt::Key_S);QVERIFY(!viewport->meshTextured());
        QVERIFY(window.grab().save(out+"/0b_window_mesh.png"));
    }
}

int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);app.setOrganizationName("VGS");app.setApplicationName("PGS Editor tests");
    PgsEditorTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "pgseditor_tests.moc"
