#include "exportcapture.h"
#include "vgssign.h"
#include "vgsdecoder/vgsdecoder.h"
#include "rangeslider.h"
#include "mintfile.h"
#include "mintskinrecovery.h"
#include "nativeexport.h"
#include "captureworker.h"
#include <Eigen/Geometry>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include <QtTest>
#include <cstring>

namespace {
void word32(uint8_t *p,uint32_t v) {for (int i=0;i<4;++i) p[i]=uint8_t(v>>(i*8));}
void word64(uint8_t *p,uint64_t v) {for (int i=0;i<8;++i) p[i]=uint8_t(v>>(i*8));}
uint64_t positionWord(float x,float y,float z,double maximum) {
    const auto q=[&](float v) {return uint64_t(std::llround(v/maximum*2097151));};return (q(x)<<43)|(q(y)<<22)|(q(z)<<1);
}
vgs::DecodedChunk temporalFixture(bool residual) {
    const size_t n=600,T=6,S=T+1;vgs::Frame f;f.count=n;f.shCoefficients=15;
    f.active.assign(n,1);f.opacity.assign(n,.7f);f.shRest.resize(n*45);
    for (size_t i=0;i<n;++i) {
        f.position.insert(f.position.end(),{i<300 ? 0.f : 3.f,1,0});f.rotation.insert(f.rotation.end(),{0,0,0,1});f.scale.insert(f.scale.end(),{.1f,.2f,.3f});f.colorDc.insert(f.colorDc.end(),{.8f,.9f,.3f});
        for (int k=0;k<45;++k) f.shRest[i*45+k]=float(k)*.001f;
    }
    auto chunk=packExportFrame(f,3);
    auto repeat=[&](vgs::Bytes &bytes,size_t stride) {
        const size_t rows=bytes.size()/(2*stride);vgs::Bytes result(rows*S*stride);
        for (size_t row=0;row<rows;++row) for (size_t sample=0;sample<S;++sample) std::memcpy(result.data()+(row*S+sample)*stride,bytes.data()+row*2*stride,stride);bytes=std::move(result);
    };
    for (auto &p:chunk.pages) switch (p.descriptor.attribute) {
    case vgs::PositionSamples:repeat(p.bytes,8);break;
    case vgs::RotationSamples:repeat(p.bytes,4);break;
    case vgs::Sh0Trajectories:repeat(p.bytes,6);break;
    case vgs::OpacityTrajectories:repeat(p.bytes,2);break;
    case vgs::ShTemporalBook: {
        const size_t stride=p.bytes.size()/2;vgs::Bytes expanded(S*stride);
        for (size_t sample=0;sample<S;++sample) std::memcpy(expanded.data()+sample*stride,p.bytes.data(),stride);p.bytes=std::move(expanded);break;
    }
    case vgs::Lifetimes:for (size_t row=0;row<n;++row) p.bytes[2*row+1]=uint8_t(T);break;
    default:break;
    }
    for (auto &g:chunk.groups) g.intervals=T;
    auto add=[&](uint32_t attribute,uint32_t group,vgs::Bytes bytes) {vgs::DecodedPage page;page.descriptor.attribute=attribute;page.descriptor.group=group;page.bytes=std::move(bytes);chunk.pages.push_back(std::move(page));};
    if (residual) {
        vgs::Bytes bases(n*8);
        for (const auto &p:chunk.pages) if (p.descriptor.attribute==vgs::PositionSamples) for (size_t row=0;row<n;++row) std::memcpy(bases.data()+row*8,p.bytes.data()+row*S*8,8);
        chunk.pages.erase(std::remove_if(chunk.pages.begin(),chunk.pages.end(),[](const auto &p) {return p.descriptor.attribute==vgs::PositionSamples || p.descriptor.attribute==vgs::RotationSamples;}),chunk.pages.end());
        chunk.groups[1].flags=0;chunk.groups[0].counts[5]=2;chunk.groups[0].counts[4]=1;chunk.groups[0].trajectoryMin=0;chunk.groups[0].trajectoryMax=2;
        vgs::Bytes trajectory(2*S*8);word64(trajectory.data()+(S+2)*8,positionWord(2,0,0,2));
        add(vgs::PositionTrajectories,0,std::move(trajectory));add(vgs::PositionBase,1,std::move(bases));
        vgs::Bytes terms(n*4);for (size_t row=0;row<n;++row) word32(terms.data()+row*4,(0x3c00u<<16)|(row==0 ? 1u : 0u));add(vgs::PositionTerms,1,std::move(terms));
        vgs::Bytes boundary(16);for (int i=0;i<4;++i) word32(boundary.data()+i*4,uint32_t(n));add(vgs::PositionRanks,1,std::move(boundary));
        vgs::Bytes rotations(n*4);const uint32_t identity=(512u<<20)|(512u<<10)|(256u<<1);
        for (size_t i=0;i<n;++i) word32(rotations.data()+i*4,identity);add(vgs::RotationBase,1,std::move(rotations));add(vgs::RotationTerms,1,vgs::Bytes(n*2));
        vgs::Bytes rotationBoundary(20);for (int i=0;i<5;++i) word32(rotationBoundary.data()+i*4,uint32_t(n));add(vgs::RotationRanks,1,std::move(rotationBoundary));
        add(vgs::RotationInitial,0,{0,0x3c,0,0,0,0,0,0});add(vgs::RotationDeltas,0,vgs::Bytes(T*4));add(vgs::RotationDeltaLut,0,vgs::Bytes(1024));
    } else {
        for (auto &p:chunk.pages) if (p.descriptor.attribute==vgs::PositionSamples) word64(p.bytes.data()+2*8,positionWord(2,1,0,3));
    }
    // Shape the independently authored fixture as complete logical arrays.
    for (auto &p:chunk.pages) {
        auto &d=p.descriptor;d.firstRow=0;d.spec={};d.spec.rows=p.bytes.size();d.totalRows=d.spec.rows;
        if (d.attribute==vgs::ShStaticIndices || d.attribute==vgs::ShTemporalIndices) {d.spec.kind=3;d.spec.width=5;d.spec.fields={10,10,10,2};d.spec.rows=n;d.totalRows=n;}
    }
    return assembleNativeChunk(std::move(chunk));
}
vgs::Frame sample() {
    vgs::Frame f;f.count=3;f.position={0,1,0, 3,1,0, 0,1,0};
    f.rotation={0,0,0,1, 0,0,0,1, 0,0,0,1};f.scale={.1f,.2f,.3f,.1f,.2f,.3f,.1f,.2f,.3f};
    f.opacity={.7f,0,1};f.colorDc={.8f,.9f,.3f, .1f,.2f,.3f, .1f,.2f,.3f};f.active={1,1,0};
    f.shCoefficients=15;f.shRest.resize(135);
    for (size_t i=0;i<f.shRest.size();++i) f.shRest[i]=float(int(i%11)-5)*.02f;return f;
}
void sourceFile(const QString &path,int frames=3,bool allEmpty=false) {
    vgs::Header h;h.frameCount=h.durationTicks=frames;h.timeDenominator=25;h.chunks.resize(frames);
    for (int i=0;i<frames;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
    vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.compression=vgs::Compression::None;
    options.metadata.title="Source title";options.playbackMode=vgs::PlaybackMode::PingPong;
    QFile file(path);if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create fixture.");
    vgs::encodeSequence(h,[&](size_t i) {auto f=sample();if (allEmpty || i==1) f.active={0,0,0};return packExportFrame(f,3);},
        [&](uint64_t offset,const uint8_t *bytes,size_t count) {if (!file.seek(qint64(offset)) || file.write(reinterpret_cast<const char *>(bytes),qint64(count))!=qint64(count)) throw std::runtime_error("Cannot write fixture.");},options);
}
Eigen::Matrix3d covariance(const float *q,const float *s) {
    Eigen::Quaterniond rotation(q[3],q[0],q[1],q[2]);rotation.normalize();auto r=rotation.toRotationMatrix();
    return r*Eigen::Vector3d(s[0]*s[0],s[1]*s[1],s[2]*s[2]).asDiagonal()*r.transpose();
}
Eigen::Matrix<double,16,1> basis(const Eigen::Vector3d &d) {
    double x=d.x(),y=d.y(),z=d.z();Eigen::Matrix<double,16,1> b;
    b << .28209479177387814,-.4886025119029199*y,.4886025119029199*z,-.4886025119029199*x,
        1.0925484305920792*x*y,-1.0925484305920792*y*z,.31539156525252005*(2*z*z-x*x-y*y),
        -1.0925484305920792*x*z,.5462742152960396*(x*x-y*y),
        -.5900435899266435*y*(3*x*x-y*y),2.890611442640554*x*y*z,
        -.4570457994644658*y*(4*z*z-x*x-y*y),.3731763325901154*z*(2*z*z-3*x*x-3*y*y),
        -.4570457994644658*x*(4*z*z-x*x-y*y),1.445305721320277*z*(x*x-y*y),
        -.5900435899266435*x*(x*x-3*y*y);return b;
}
double colour(const vgs::Frame &f,const Eigen::Vector3d &direction,int c) {
    auto b=basis(direction);double out=f.colorDc[c];for (int i=0;i<f.shCoefficients;++i) out+=b[i+1]*f.shRest[3*i+c];return out;
}
}
class ExportTests : public QObject {
    Q_OBJECT
private slots:
    void animatedOffsetsBakeReferenceTrimCovarianceAndSh() {
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,5);Project project;project.asset=source;project.in=.08;project.out=.16;project.transform.position={1,2,3};project.modifiers.clear();
        Modifier animation;animation.id=Project::newId();animation.name="Motion";animation.type=ModifierType::AnimateTransform;Transform end;end.position={2,0,0};end.rotation={0,90,0};end.scale={2,1,1};animation.animation.setKey(0,{});animation.animation.setKey(4,end);project.modifiers.append(animation);
        auto original=vgsdec::Capture::openFile(source.toStdString());
        const auto guardedOutput=dir.filePath("existing.vgs");QFile guardedFile(guardedOutput);QVERIFY(guardedFile.open(QIODevice::WriteOnly));guardedFile.write("existing");guardedFile.close();
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(project,guardedOutput),std::runtime_error);QVERIFY(guardedFile.open(QIODevice::ReadOnly));QCOMPARE(guardedFile.readAll(),QByteArray("existing"));guardedFile.close();
        // Non-uniform animated scale cannot be stored as motion, so the export above is
        // refused; the reference bake it would be compared with is still checked here.
        for (int frame=2;frame<5;++frame) {const auto input=copy(original.setTime(frame/25.,true));const auto expected=bakeExportFrame(input,project,3,{},25);QVERIFY(expected.count>0);}
        animation.animation.keys={animation.animation.keys.back()};project.modifiers[0]=animation;
        for (const auto &extension:{QString("vgs"),QString("pgs"),QString("mint")}) {
            project.captureSettings.plain=extension=="pgs";const auto path=dir.filePath("animated."+extension);const auto result=exportCaptureFile(project,path);QCOMPARE(result.frames,3);
            std::unique_ptr<vgsdec::Capture> capture;MintFile mint;QString error;if (extension=="mint") QVERIFY(mint.open(path,&error));else capture=std::make_unique<vgsdec::Capture>(vgsdec::Capture::openFile(path.toStdString()));
            for (int sample=0;sample<3;++sample) {
                const auto input=copy(original.setTime((sample+2)/25.,true));const auto expected=bakeExportFrame(input,project,3,{},25);vgs::Frame actual;
                if (capture) actual=copy(capture->setTime((sample+.25)/25.,true));else {MintFrame frame;QVERIFY(mint.decode((sample+.25)/25.,&frame,true,&error));actual.count=frame.count;actual.position={frame.position.begin(),frame.position.end()};actual.rotation={frame.rotation.begin(),frame.rotation.end()};actual.scale={frame.scale.begin(),frame.scale.end()};actual.colorDc={frame.colorDc.begin(),frame.colorDc.end()};actual.shRest={frame.shRest.begin(),frame.shRest.end()};actual.shCoefficients=15;}
                QCOMPARE(actual.count,expected.count);for (size_t row=0;row<actual.count;++row) {
                    for (int c=0;c<3;++c) QVERIFY(std::abs(actual.position[row*3+c]-expected.position[row*3+c])<1e-4f);
                    QVERIFY((covariance(actual.rotation.data()+row*4,actual.scale.data()+row*3)-covariance(expected.rotation.data()+row*4,expected.scale.data()+row*3)).norm()<.002);
                    for (int c=0;c<3;++c) QVERIFY(std::abs(actual.colorDc[row*3+c]-expected.colorDc[row*3+c])<.003);
                    for (int c=0;c<45;++c) QVERIFY(std::abs(actual.shRest[row*45+c]-expected.shRest[row*45+c])<.003);
                }
            }
        }
    }
    void realMintAtLowerShDegrees() {
        // Reading a MINT at a lower degree once handed over five SH index planes against
        // codebooks rebuilt for fewer, so exporting at degree 2 or 1 failed or read past them.
        const QString asset=qEnvironmentVariable("EDITOR_TEST_CAPTURE");if (asset.isEmpty()) QSKIP("Set EDITOR_TEST_CAPTURE to exercise a real MINT capture.");
        QTemporaryDir dir;std::map<int,QString> outputs;
        for (int degree:{3,2,1,0}) {
            Project project;project.asset=asset;project.in=0;project.out=.5;project.captureSettings.shDegree=degree;
            outputs[degree]=dir.filePath(QString("sh%1.vgs").arg(degree));exportCaptureFile(project,outputs[degree]);
            exportCaptureFile(project,dir.filePath(QString("sh%1.mint").arg(degree)));
        }
        auto full=vgsdec::Capture::openFile(outputs[3].toStdString());const auto reference=copy(full.setTime(.25,true));
        for (int degree:{2,1}) {
            auto lower=vgsdec::Capture::openFile(outputs[degree].toStdString());const auto frame=copy(lower.setTime(.25,true));
            const int kept=degree==2 ? 8 : 3;QCOMPARE(frame.shCoefficients,kept);QCOMPARE(frame.count,reference.count);
            for (size_t i=0;i<frame.count;++i) for (int k=0;k<kept*3;++k) QCOMPARE(frame.shRest[i*kept*3+k],reference.shRest[i*45+k]);
        }
    }
    void animatedMotionIsStoredAsSamples() {
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,5);
        Project project;project.asset=source;project.in=.08;project.out=.16;project.transform.position={1,2,3};project.transform.rotation={0,20,0};project.modifiers.clear();
        Modifier animation;animation.id=Project::newId();animation.name="Walk";animation.type=ModifierType::AnimateTransform;
        Transform end;end.position={2,0,1};end.rotation={10,90,30};end.scale={1.5f,1.5f,1.5f};animation.animation.setKey(0,{});animation.animation.setKey(4,end);project.modifiers.append(animation);
        QVERIFY(project.hasAnimatedMotion());
        auto original=vgsdec::Capture::openFile(source.toStdString());
        Project still=project;still.modifiers.clear();const auto staticPath=dir.filePath("static.vgs");exportCaptureFile(still,staticPath);
        auto compare=[&](vgsdec::Capture &capture,const Project &reference,double tolerance) {
            for (int sample=0;sample<3;++sample) {
                const auto input=copy(original.setTime((sample+2)/25.,true));const auto expected=bakeExportFrame(input,reference,3,{},25);
                const auto actual=copy(capture.setTime(sample/25.,true));QCOMPARE(actual.count,expected.count);
                for (size_t row=0;row<actual.count;++row) {
                    for (int c=0;c<3;++c) QVERIFY2(std::abs(actual.position[row*3+c]-expected.position[row*3+c])<tolerance,qPrintable(QString("sample %1 row %2").arg(sample).arg(row)));
                    QVERIFY((covariance(actual.rotation.data()+row*4,actual.scale.data()+row*3)-covariance(expected.rotation.data()+row*4,expected.scale.data()+row*3)).norm()<.002);
                    for (int c=0;c<3;++c) QVERIFY(std::abs(actual.colorDc[row*3+c]-expected.colorDc[row*3+c])<.003);
                    for (int c=0;c<45;++c) QVERIFY(std::abs(actual.shRest[row*45+c]-expected.shRest[row*45+c])<.003);
                }
                // The box a player culls with holds every splat that is alive.
                const auto &box=capture.chunk(capture.chunkAt(sample/25.)).bounds;
                for (size_t row=0;row<actual.count;++row) if (actual.active[row]) for (int c=0;c<3;++c)
                    QVERIFY(actual.position[row*3+c]>=box[c]-1e-5f && actual.position[row*3+c]<=box[c+3]+1e-5f);
            }
        };
        for (const auto &extension:{QString("vgs"),QString("pgs")}) {
            project.captureSettings.plain=extension=="pgs";const auto path=dir.filePath("moving."+extension);const auto result=exportCaptureFile(project,path);QCOMPARE(result.frames,3);
            auto capture=vgsdec::Capture::openFile(path.toStdString());QVERIFY(capture.hasMotion());
            compare(capture,project,1e-4);
            // The first exported frame is what is baked, so the motion starts at the identity.
            capture.setTime(0,false);const auto start=capture.motionAt(0);
            QVERIFY(std::abs(start.translation[0])+std::abs(start.translation[1])+std::abs(start.translation[2])<1e-5);
            QVERIFY(std::abs(std::abs(start.rotation[3])-1)<1e-6 && std::abs(start.scale-1)<1e-6);
            // Positions alone are moved the same way setTime moves them.
            uint64_t count=0;const float *positions=capture.positionsAt(1/25.,&count);const auto frame=copy(capture.setTime(1/25.,false));
            QCOMPARE(count,frame.count);for (size_t i=0;i<count*3;++i) QCOMPARE(positions[i],frame.position[i]);
            if (extension=="vgs") QVERIFY(QFileInfo(path).size()<QFileInfo(staticPath).size()+4096);
        }
        // A moving .vgs edited again keeps its motion, composed with the new transform once.
        Project again;again.asset=dir.filePath("moving.vgs");again.in=0;again.out=2./25;again.modifiers.clear();again.transform.position={0,.5f,0};
        const auto twice=dir.filePath("twice.vgs");exportCaptureFile(again,twice);
        auto first=vgsdec::Capture::openFile(again.asset.toStdString()),second=vgsdec::Capture::openFile(twice.toStdString());QVERIFY(second.hasMotion());
        for (int sample=0;sample<3;++sample) {
            const auto a=copy(first.setTime(sample/25.,true)),b=copy(second.setTime(sample/25.,true));QCOMPARE(b.count,a.count);
            for (size_t row=0;row<a.count;++row) for (int c=0;c<3;++c) QVERIFY(std::abs(b.position[row*3+c]-(a.position[row*3+c]+(c==1 ? .5f : 0.f)))<1e-4f);
        }
        // MINT has nowhere to put motion: refused, and the destination is left alone.
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(project,dir.filePath("moving.mint")),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(again,dir.filePath("again.mint")),std::runtime_error);
        QVERIFY(!QFileInfo::exists(dir.filePath("moving.mint")));
    }
    void walkMarksTheHeaderAndLeavesTheDataInPlace() {
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,5);
        Project project;project.asset=source;project.in=.08;project.out=.16;project.modifiers.clear();
        Modifier walk;walk.id=Project::newId();walk.name="Walk";walk.type=ModifierType::Walk;walk.walkSpeed=1.5;walk.walkKmh=true;project.modifiers.append(walk);project.selectedModifier=walk.id;project.time=project.in;
        QCOMPARE(project.walkSpeed(),1.5);QCOMPARE(project.walkDistance(.08+2),3.);
        Project restored;QString error;QVERIFY2(Project::fromJson(project.json(dir.filePath("scene.vgsproj")),dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers.size(),1);QCOMPARE(restored.modifiers[0].type,ModifierType::Walk);QCOMPARE(restored.modifiers[0].walkSpeed,1.5);QVERIFY(restored.modifiers[0].walkKmh);
        Project still=project;still.modifiers.clear();
        const auto walking=dir.filePath("walking.vgs"),inPlace=dir.filePath("in-place.vgs");exportCaptureFile(project,walking);exportCaptureFile(still,inPlace);
        auto a=vgsdec::Capture::openFile(walking.toStdString()),b=vgsdec::Capture::openFile(inPlace.toStdString());
        QCOMPARE(a.motionType(),vgsdec::MotionType::Walking);QCOMPARE(a.movingSpeed(),1.5f);QVERIFY(!a.hasMotion());
        QCOMPARE(b.motionType(),vgsdec::MotionType::InPlace);QCOMPARE(b.movingSpeed(),0.f);
        for (int sample=0;sample<3;++sample) {
            const auto x=copy(a.setTime(sample/25.,true)),y=copy(b.setTime(sample/25.,true));QCOMPARE(x.count,y.count);
            for (size_t i=0;i<x.position.size();++i) QCOMPARE(x.position[i],y.position[i]);
        }
        // MINT has nowhere to say it: exported, and the loss is reported.
        const auto mint=exportCaptureFile(project,dir.filePath("walking.mint"));
        QVERIFY(std::any_of(mint.notes.begin(),mint.notes.end(),[](const QString &n) {return n.contains("Walk");}));
    }
    void isolationMatchesNativeSampledAndWorkerPreview() {
        QTemporaryDir dir;const auto source=dir.filePath("cloud.pgs");vgs::Frame cloud;cloud.count=26;cloud.active.assign(26,1);cloud.opacity.assign(26,1);
        for (int i=0;i<26;++i) {cloud.position.insert(cloud.position.end(),{i==25 ? 5.f : (i%5)*.01f,i==25 ? 5.f : 1.f,i==25 ? 5.f : (i/5)*.01f});cloud.rotation.insert(cloud.rotation.end(),{0,0,0,1});cloud.scale.insert(cloud.scale.end(),{.01f,.02f,.03f});cloud.colorDc.insert(cloud.colorDc.end(),{.8f,.4f,.2f});}
        vgs::Header h;h.shDegree=0;h.frameCount=h.durationTicks=2;h.chunks.resize(2);for (int i=0;i<2;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.shDegree=0;QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));
        vgs::encodeSequence(h,[&](size_t frame) {auto sample=cloud;if (frame) {sample.position[75]=.025f;sample.position[76]=1;sample.position[77]=.025f;}return packExportFrame(sample,0);},[&](uint64_t at,const uint8_t *data,size_t n) {if (!file.seek(at) || file.write(reinterpret_cast<const char *>(data),n)!=qint64(n)) throw std::runtime_error("Fixture write failed.");},options);file.close();
        Project project;project.asset=source;project.out=1./30;project.modifiers.clear();Modifier purge;purge.id=Project::newId();purge.name="Purge";purge.type=ModifierType::PurgeIsolated;purge.isolation={4,300};project.modifiers.append(purge);
        for (bool sampled:{false,true}) {project.transform.scale=sampled ? QVector3D(1,.8f,1) : QVector3D(1,1,1);const auto output=dir.filePath(sampled ? "sampled.vgs" : "native.vgs");exportCaptureFile(project,output);auto capture=vgsdec::Capture::openFile(output.toStdString());QCOMPARE(capture.setTime(0,true).activeCount,25ull);QCOMPARE(capture.setTime(1./30,true).activeCount,26ull);}
        CaptureWorker worker;QSignalSpy decoded(&worker,&CaptureWorker::decoded);worker.open(source,1,true);worker.decode(0,1,true,project);QCOMPARE(decoded.size(),1);const auto preview=qvariant_cast<FramePtr>(decoded[0][0]);QCOMPARE(preview->points.size(),size_t(26));QCOMPARE(preview->records.size(),size_t(26));QCOMPARE(std::count_if(preview->points.begin(),preview->points.end(),[](const auto &point) {return point.modifierVisibility>.5;}),25);
        project.modifiers[0].enabled=false;worker.decode(0,1,true,project);const auto disabled=qvariant_cast<FramePtr>(decoded[1][0]);QCOMPARE(std::count_if(disabled->points.begin(),disabled->points.end(),[](const auto &point) {return point.modifierVisibility>.5;}),26);
    }
    void mintExportPreservesEditedAttributesAndEmptyFrames() {
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source);
        Project project;project.asset=source;project.out=.08;project.crop().enabled=true;project.transform.position={.2f,0,0};project.captureSettings.shDegree=1;QString error;
        for (bool sampled:{false,true}) {
            project.transform.scale=sampled ? QVector3D(1,.8f,1) : QVector3D(1,1,1);
            const auto reference=dir.filePath(sampled ? "sampled.vgs" : "native.vgs"),output=dir.filePath(sampled ? "sampled.mint" : "native.mint");
            exportCaptureFile(project,reference);const auto result=exportCaptureFile(project,output);QCOMPARE(result.frames,3);QCOMPARE(result.kept,2ull);
            MintFile mint;QVERIFY2(mint.open(output,&error),qPrintable(error));QCOMPARE(mint.frameRate(),25.);QCOMPARE(mint.frameCount(),3);
            auto expected=vgsdec::Capture::openFile(reference.toStdString());
            for (int frame=0;frame<3;++frame) {
                // The MINT runtime converts time to float32. Sample inside each
                // frame to avoid choosing different sides of a chunk boundary.
                const double seconds=(frame+.25)/25.;MintFrame actual;QVERIFY2(mint.decode(seconds,&actual,true,&error),qPrintable(error));const auto &before=expected.setTime(seconds,true);
                QCOMPARE(actual.count,before.splatCount);for (size_t row=0;row<actual.count;++row) {QCOMPARE(actual.active[int(row)],before.active[row]);
                    for (int c=0;c<3;++c) {QVERIFY(std::abs(actual.position[int(row*3+c)]-before.positions[row*3+c])<1e-5f);QVERIFY(std::abs(actual.scale[int(row*3+c)]-before.scales[row*3+c])<1e-5f);QVERIFY(std::abs(actual.colorDc[int(row*3+c)]-before.colors[row*3+c])<1e-5f);}
                    for (int c=0;c<9;++c) QVERIFY(std::abs(actual.shRest[int(row*45+c)]-before.sphericalHarmonics[row*9+c])<1e-5f);
                    for (int c=0;c<4;++c) QVERIFY(std::abs(actual.rotation[int(row*4+c)]-before.rotations[row*4+c])<1e-5f);
                    QVERIFY(std::abs(actual.opacity[int(row)]-before.opacities[row])<1e-5f);
                    for (int c=9;c<45;++c) QCOMPARE(actual.shRest[int(row*45+c)],0.f);
                }
            }
            const auto bytes=mint.bytes();vgs::MintLogicalSource logical(reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()));QCOMPARE(logical.header().frameCount,3ull);
        }
        const auto destination=dir.filePath("cancelled.mint");QFile existing(destination);QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("existing");existing.close();
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(project,destination,[](int value,const QString &) {return value<95;}),std::runtime_error);
        QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("existing"));
    }
    void modifiersMatchAcrossNativeAndSampledExportBeforeDespill() {
        vgs::Frame frame;frame.count=4;frame.position={-1.5f,1,0,1.5f,1,0,1.5f,1,0,0,1,0};frame.active={1,1,1,1};frame.opacity={1,1,1,1};
        frame.colorDc={.9f,.1f,.1f,.1f,.1f,.9f,0,1,0,.8f,.8f,.8f};
        for (int i=0;i<4;++i) {frame.rotation.insert(frame.rotation.end(),{0,0,0,1});frame.scale.insert(frame.scale.end(),{.1f,.2f,.3f});}
        Project project;project.modifiers.clear();
        Modifier first;first.id=Project::newId();first.name="First";first.crop.enabled=true;first.crop.radius=.5f;first.crop.height=2;first.crop.transform.position={-1.5f,0,0};
        Modifier second=first;second.id=Project::newId();second.name="Second";second.crop.transform.position={1.5f,0,0};
        Modifier green;green.id=Project::newId();green.name="Green";green.type=ModifierType::RemoveGreen;project.modifiers={first,second,green};project.selectedModifier=green.id;
        project.captureSettings.despill=true;project.captureSettings.recoverSkin=false;
        // Enabled future modifiers leave both export paths unchanged.
        Modifier animate;animate.id=Project::newId();animate.name="Animate";animate.type=ModifierType::AnimateTransform;
        Modifier purge=animate;purge.id=Project::newId();purge.name="Purge";purge.type=ModifierType::PurgeIsolated;
        project.modifiers.insert(0,animate);project.modifiers.insert(0,purge);
        QCOMPARE(bakeExportFrame(frame,project,0).count,2ull);
        auto source=packExportFrame(frame,0);auto processed=source;MintDespillOptions options;options.recoverSkinColour=false;QString error;
        QVERIFY(MintFile::despillLogical(&processed,1./30,options,{},&error));
        ExportResult result;auto edited=editNativeChunk(processed,{0,0,1},project,&result,{},&source);vgs::FrameDecoder decoded(edited);
        auto output=decoded.evaluate(0,false);QCOMPARE(std::count(output.active.begin(),output.active.end(),uint8_t(1)),2);QCOMPARE(result.kept,2ull);QCOMPARE(result.removed,2ull);
        project.modifiers.back().enabled=false;QCOMPARE(bakeExportFrame(frame,project,0).count,3ull);
        project.modifiers[2].enabled=false;QCOMPARE(bakeExportFrame(frame,project,0).count,2ull);
        project.modifiers[3].enabled=false;QCOMPARE(bakeExportFrame(frame,project,0).count,4ull);
    }
    void skinRecoveryProtectsNeutralClothWithSkinLikeDc() {
        const int n=402;MintFrame original,corrected;original.count=corrected.count=n;
        original.active.fill(1,n);original.opacity.fill(1,n);original.position.resize(n*3);original.colorDc.resize(n*3);original.shRest.fill(0,n*45);corrected.colorDc.resize(n*3);
        for (int i=0;i<n;++i) {
            original.position[i*3]=i<400 ? float(i%20)*.01f : .09f;
            original.position[i*3+1]=i<400 ? 1+float(i/20)*.01f : 1.09f;original.position[i*3+2]=i<400 ? 0 : .01f;
            const float colour[3]={.65f,.42f,.39f};for (int c=0;c<3;++c) original.colorDc[i*3+c]=corrected.colorDc[i*3+c]=colour[c];
        }
        for (int i=400;i<n;++i) {corrected.colorDc[i*3]=.55f;corrected.colorDc[i*3+1]=.50f;corrected.colorDc[i*3+2]=.47f;}
        // Both targets have identical skin-like DC; only the cloth has a neutral
        // view-dependent response. Its angular signal must participate in detection.
        const float dc[3]={.65f,.42f,.39f},white[3]={.8f,.8f,.8f};
        for (int c=0;c<3;++c) original.shRest[401*45+5*3+c]=2.3f*(white[c]-dc[c])/.6307831305f;
        QVector<float> deltas;QVERIFY(mintSkinRecovery(original,corrected,&deltas,{}));
        QVERIFY(deltas[400*3]>.05f);
        for (int c=0;c<3;++c) QCOMPARE(deltas[401*3+c],0.f);
        original.shRest.clear();QVERIFY(mintSkinRecovery(original,corrected,&deltas,{}));QVERIFY(deltas[401*3]>.05f);
    }
    void colourProcessingDiagnostics() {
        const QString path=qEnvironmentVariable("EDITOR_DIAGNOSE_PROJECT");if (path.isEmpty()) QSKIP("Set EDITOR_DIAGNOSE_PROJECT for colour processing diagnostics.");
        Project p;QString error;QVERIFY2(Project::read(path,&p,&error),qPrintable(error));
        MintFile raw,full,noSkin;QVERIFY(raw.open(p.asset,&error));QVERIFY(full.open(p.asset,&error));QVERIFY(noSkin.open(p.asset,&error));
        MintDespillOptions options;options.strength=p.captureSettings.despillStrength;options.greenGain=p.captureSettings.greenGain;options.viewChromaScale=p.captureSettings.viewChromaScale;
        options.recoverSkinColour=true;QVERIFY2(full.despill(options,{},&error),qPrintable(error));options.recoverSkinColour=false;QVERIFY2(noSkin.despill(options,{},&error),qPrintable(error));
        const auto eye=p.transform.matrix().inverted().map(p.camera.viewMatrix().inverted().map({0,0,0}));
        auto rgb=[&](const MintFrame &frame,size_t row) {
            const auto d=(QVector3D(frame.position[row*3],frame.position[row*3+1],frame.position[row*3+2])-eye).normalized();auto b=basis({d.x(),d.y(),d.z()});std::array<float,3> result;
            for (int channel=0;channel<3;++channel) {double value=frame.colorDc[row*3+channel];for (int k=0;k<15;++k) value+=b[k+1]*frame.shRest[row*45+k*3+channel];result[channel]=float(std::clamp(value,0.,1.));}return result;
        };
        for (double time:{0.,.5}) {
            MintFrame before,after,withoutSkin;QVERIFY(raw.decode(time,&before,true,&error));QVERIFY(full.decode(time,&after,true,&error));QVERIFY(noSkin.decode(time,&withoutSkin,true,&error));
            size_t white=0,redFull=0,redNoSkin=0,skinChanges=0;double fullMaximum=0,noSkinMaximum=0;std::vector<std::pair<float,size_t>> largest;
            for (size_t row=0;row<before.count;++row) {
                if (!before.active[row] || before.opacity[row]<.1f) continue;
                const auto position=p.transform.matrix().map({before.position[row*3],before.position[row*3+1],before.position[row*3+2]});
                if (position.y()<.3 || position.y()>1.6 || !p.crop().contains(position)) continue;
                const auto original=rgb(before,row),processed=rgb(after,row),plain=rgb(withoutSkin,row);
                const float low=std::min({original[0],original[1],original[2]}),high=std::max({original[0],original[1],original[2]});
                if (low<.35f || high-low>.12f) continue;++white;
                const float change=(processed[0]-.5f*(processed[1]+processed[2]))-(original[0]-.5f*(original[1]+original[2]));
                const float changeNoSkin=(plain[0]-.5f*(plain[1]+plain[2]))-(original[0]-.5f*(original[1]+original[2]));
                if (change>.04f) ++redFull;if (changeNoSkin>.04f) ++redNoSkin;
                if (std::abs(processed[0]-plain[0])+std::abs(processed[1]-plain[1])+std::abs(processed[2]-plain[2])>.01f) ++skinChanges;
                fullMaximum=std::max(fullMaximum,double(change));noSkinMaximum=std::max(noSkinMaximum,double(changeNoSkin));largest.push_back({change,row});
            }
            if (QFileInfo(p.asset).fileName()=="THE4DSCANNER_CAPT1_Oussama_Sonia_hablando__0_30.mint" && before.count>144954) {
                const auto protectedColour=rgb(after,144954),withoutRecovery=rgb(withoutSkin,144954);
                for (int c=0;c<3;++c) QVERIFY(std::abs(protectedColour[c]-withoutRecovery[c])<1e-6f);
                QVERIFY(std::max({protectedColour[0],protectedColour[1],protectedColour[2]})-std::min({protectedColour[0],protectedColour[1],protectedColour[2]})<.025f);
                qInfo("Reported crease row 144954: protected RGB %.4f %.4f %.4f",protectedColour[0],protectedColour[1],protectedColour[2]);
            }
            std::sort(largest.begin(),largest.end(),std::greater<std::pair<float,size_t>>());
            qInfo("t=%.2f white=%llu red(full/no skin)=%llu/%llu max=%.5f/%.5f skinChanges=%llu",time,qulonglong(white),qulonglong(redFull),qulonglong(redNoSkin),fullMaximum,noSkinMaximum,qulonglong(skinChanges));
            for (size_t i=0;i<std::min(size_t(5),largest.size());++i) {
                const auto row=largest[i].second;const auto a=rgb(before,row),b=rgb(after,row),c=rgb(withoutSkin,row);
                qInfo("row=%llu xyz=%.5f %.5f %.5f original=%.4f %.4f %.4f despill=%.4f %.4f %.4f noSkin=%.4f %.4f %.4f",qulonglong(row),before.position[row*3],before.position[row*3+1],before.position[row*3+2],a[0],a[1],a[2],b[0],b[1],b[2],c[0],c[1],c[2]);
            }
        }
    }
    void nativeRotationKeepsCovarianceAndSh_data() {
        QTest::addColumn<bool>("reflection");QTest::newRow("rotation")<<false;QTest::newRow("reflection")<<true;
    }
    void nativeRotationKeepsCovarianceAndSh() {
        QFETCH(bool,reflection);auto source=temporalFixture(true);vgs::FrameDecoder original(source);Project p;
        p.transform.position={.2f,.3f,.4f};p.transform.rotation={18,35,-12};p.transform.scale=reflection ? QVector3D(-1.5f,-1.5f,-1.5f) : QVector3D(1.5f,1.5f,1.5f);
        QVERIFY(supportsNativeTransform(p));ExportResult result;
        const auto edited=editNativeChunk(source,{0,0,6},p,&result,{});vgs::FrameDecoder output(edited);
        Eigen::Matrix3d a;auto m=p.transform.matrix();for (int i=0;i<3;++i) for (int j=0;j<3;++j) a(i,j)=m(i,j);
        for (int tick=0;tick<6;++tick) {
            auto before=original.evaluate(tick/6.,true),after=output.evaluate(tick/6.,true);QCOMPARE(after.count,before.count);
            for (size_t row=0;row<before.count;row+=100) {
                auto world=m.map({before.position[row*3],before.position[row*3+1],before.position[row*3+2]});
                QVERIFY((QVector3D(after.position[row*3],after.position[row*3+1],after.position[row*3+2])-world).length()<3e-5f);
                const Eigen::Matrix3d expected=a*covariance(before.rotation.data()+row*4,before.scale.data()+row*3)*a.transpose();
                QVERIFY((covariance(after.rotation.data()+row*4,after.scale.data()+row*3)-expected).norm()<.001);
            }
            for (int direction=0;direction<20;++direction) {
                QVector3D world=QVector3D(std::sin(direction*.7f),std::cos(direction*.3f),std::sin(direction*1.3f)).normalized();auto local=m.inverted().mapVector(world).normalized();
                for (int channel=0;channel<3;++channel) QVERIFY(std::abs(colour(after,{world.x(),world.y(),world.z()},channel)-colour(before,{local.x(),local.y(),local.z()},channel))<1e-4);
            }
        }
        QVERIFY(!result.notes.isEmpty());
    }
    void nativeTemporalCropTrimAndRepeatedVisibility_data() {QTest::addColumn<bool>("residual");QTest::newRow("residual")<<true;QTest::newRow("direct")<<false;}
    void nativeTemporalCropTrimAndRepeatedVisibility() {
        QFETCH(bool,residual);auto source=temporalFixture(residual);vgs::FrameDecoder reference(source);Project project;
        project.transform.position={.25f,0,0};project.crop().enabled=true;project.crop().radius=1;project.crop().height=2;
        for (auto plan:{NativeChunkPlan{0,0,6},NativeChunkPlan{0,1,4}}) {
            ExportResult result;auto edited=editNativeChunk(source,plan,project,&result,{});vgs::FrameDecoder output(edited);
            quint64 kept=0,removed=0;
            for (int tick=0;tick<plan.intervals;++tick) {
                auto original=reference.evaluate((plan.first+tick)/6.,true),actual=output.evaluate(tick/double(plan.intervals),true);
                std::vector<size_t> expectedRows,actualRows;
                for (size_t row=0;row<original.count;++row) if (original.active[row]) {
                    auto world=project.transform.matrix().map({original.position[row*3],original.position[row*3+1],original.position[row*3+2]});
                    if (project.crop().contains(world)) {expectedRows.push_back(row);++kept;} else ++removed;
                }
                for (size_t row=0;row<actual.count;++row) if (actual.active[row]) actualRows.push_back(row);
                QCOMPARE(actualRows.size(),expectedRows.size());
                for (size_t j=0;j<actualRows.size();++j) {
                    const size_t row=actualRows[j],expected=expectedRows[j];const auto world=project.transform.matrix().map({original.position[expected*3],original.position[expected*3+1],original.position[expected*3+2]});
                    QVERIFY((QVector3D(actual.position[row*3],actual.position[row*3+1],actual.position[row*3+2])-world).length()<3e-5f);
                    for (int k=0;k<45;++k) QCOMPARE(actual.shRest[row*45+k],original.shRest[expected*45+k]);
                    for (int k=0;k<3;++k) {QCOMPARE(actual.colorDc[row*3+k],original.colorDc[expected*3+k]);QCOMPARE(actual.scale[row*3+k],original.scale[expected*3+k]);}
                }
            }
            QCOMPARE(result.kept,kept);QCOMPARE(result.removed,removed);
            QVERIFY(edited.groups[1].splats<source.groups[1].splats);QCOMPARE(edited.groups[1].intervals,uint64_t(plan.intervals));
        }
    }
    void reportedProjectStaysSmallerThanMint() {
        const QString path=qEnvironmentVariable("EDITOR_TEST_PROJECT");if (path.isEmpty()) QSKIP("Set EDITOR_TEST_PROJECT to verify a reported export-size regression.");
        Project project;QString error;QVERIFY2(Project::read(path,&project,&error),qPrintable(error));QVERIFY(supportsNativeTransform(project));
        QTemporaryDir dir;const QString output=dir.filePath("fixed.vgs");ExportResult result;
        try {result=exportCaptureFile(project,output);} catch (const std::exception &e) {QFAIL(e.what());}
        QVERIFY(QFileInfo(output).size()<QFileInfo(project.asset).size());auto verified=vgsdec::Capture::openFile(output.toStdString());
        QCOMPARE(verified.frameCount(),uint64_t(result.frames));QVERIFY(verified.chunkCount()<verified.frameCount());
        MintFile original;QVERIFY2(original.open(project.asset,&error),qPrintable(error));quint64 samples=0;
        if (project.captureSettings.despill) {
            MintDespillOptions options;options.strength=project.captureSettings.despillStrength;options.greenGain=project.captureSettings.greenGain;
            options.viewChromaScale=project.captureSettings.viewChromaScale;options.recoverSkinColour=project.captureSettings.recoverSkin;
            QVERIFY2(original.despill(options,{},&error),qPrintable(error));
        }
        const double fps=original.frameRate();const int first=int(std::round(project.in*fps));
        for (int frame=0;frame<result.frames;++frame) {
            MintFrame reference;QVERIFY2(original.decode((first+frame)/fps,&reference,true,&error),qPrintable(error));std::vector<size_t> expected;
            for (size_t row=0;row<reference.count;++row) if (reference.active[row] && project.crop().contains(project.transform.matrix().map({reference.position[row*3],reference.position[row*3+1],reference.position[row*3+2]}))) expected.push_back(row);
            const auto &actual=verified.setTime(frame/fps,true);QCOMPARE(actual.activeCount,uint64_t(expected.size()));samples+=actual.activeCount;size_t next=0;
            for (size_t row=0;row<actual.splatCount;++row) if (actual.active[row]) {
                const size_t sourceRow=expected[next++];
                const auto position=project.transform.matrix().map({reference.position[sourceRow*3],reference.position[sourceRow*3+1],reference.position[sourceRow*3+2]});
                QVERIFY((QVector3D(actual.positions[row*3],actual.positions[row*3+1],actual.positions[row*3+2])-position).length()<3e-5f);
                if (project.transform.rotation==QVector3D() && project.transform.scale==QVector3D(1,1,1)) {
                    for (int channel=0;channel<3;++channel) QVERIFY(std::abs(actual.colors[row*3+channel]-reference.colorDc[sourceRow*3+channel])<2e-6f);
                    for (int coefficient=0;coefficient<actual.shCoefficients;++coefficient) for (int channel=0;channel<3;++channel)
                        QCOMPARE(actual.sphericalHarmonics[(row*actual.shCoefficients+coefficient)*3+channel],reference.shRest[sourceRow*45+coefficient*3+channel]);
                }
                auto crop=project.crop();crop.radius+=1e-5f;crop.height+=1e-5f;crop.transform.position.setY(crop.transform.position.y()-1e-5f);
                QVERIFY(crop.contains({actual.positions[row*3],actual.positions[row*3+1],actual.positions[row*3+2]}));
            }
        }
        QCOMPARE(samples,result.kept);
    }
    void cropUsesWorldAndKeepsZeroOpacity() {
        auto f=sample();Project p;p.transform.position={-2,0,0};p.crop().enabled=true;p.crop().radius=1.1f;p.crop().height=2;
        auto result=bakeExportFrame(f,p,3);QCOMPARE(result.count,1ull);QCOMPARE(result.opacity.front(),0.f);QCOMPARE(result.position.front(),1.f);
        QCOMPARE(p.crop().transform.position,QVector3D());QCOMPARE(f.count,3ull);QCOMPARE(f.position[0],0.f);
        p.crop().shape=CropShape::Box;p.crop().width=3;p.crop().depth=2;p.crop().transform.rotation={0,45,0};
        result=bakeExportFrame(f,p,3);QCOMPARE(result.count,1ull);
        p.transform.position={0,0,0};result=bakeExportFrame(f,p,3);QCOMPARE(result.count,1ull);QCOMPARE(result.opacity[0],.7f);
    }
    void affineCovarianceAndReflections() {
        auto f=sample();f.active={1,0,0};Project p;p.transform.rotation={35,70,15};p.transform.scale={-2,.5f,3};p.transform.shear={.2f,.3f,-.4f};
        auto baked=bakeExportFrame(f,p,3);auto matrix=p.transform.matrix();Eigen::Matrix3d a;
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) a(i,j)=matrix(i,j);
        Eigen::Matrix3d expected=a*covariance(f.rotation.data(),f.scale.data())*a.transpose();
        QVERIFY((covariance(baked.rotation.data(),baked.scale.data())-expected).norm()<1e-6);
        p.transform.scale={0,1,1};QVERIFY_EXCEPTION_THROWN(bakeExportFrame(f,p,3),std::runtime_error);
    }
    void shRotatesWithCapture() {
        auto f=sample();f.active={1,0,0};Project p;p.transform.rotation={27,-39,84};p.transform.scale={2,2,2};
        auto baked=bakeExportFrame(f,p,3);auto inverse=p.transform.matrix().inverted();
        for (int i=0;i<40;++i) {
            QVector3D world=QVector3D(std::sin(i*.7f),std::cos(i*.3f),std::sin(i*1.3f)).normalized();auto local=inverse.mapVector(world).normalized();
            for (int c=0;c<3;++c) QVERIFY(std::abs(colour(baked,{world.x(),world.y(),world.z()},c)-colour(f,{local.x(),local.y(),local.z()},c))<2e-6);
        }
    }
    void despillActuallyChangesExportColours() {
        auto f=sample();Project p;p.captureSettings.despill=true;p.captureSettings.recoverSkin=false;
        auto processed=bakeExportFrame(f,p,3);QVERIFY(processed.colorDc[1]<f.colorDc[1]);QVERIFY(processed.colorDc[1]<=(processed.colorDc[0]+processed.colorDc[2])*.5f+1e-6f);
        for (int k=0;k<15;++k) QVERIFY(std::abs(processed.shRest[3*k+1]-(processed.shRest[3*k]+processed.shRest[3*k+2])*.5f)<1e-6f);
        p.captureSettings.recoverSkin=true;QVERIFY(bakeExportFrame(f,p,3).count>0);
    }
    void compatibleSignedExportTrimMetadataCropAndEmptyFrame() {
        QTemporaryDir dir;QString source=dir.filePath("source.pgs");sourceFile(source);
        Project p;p.asset=source;p.in=1./25;p.out=2./25;p.crop().enabled=true;p.crop().radius=1;p.crop().height=2;
        p.transform.position={.25f,0,0};p.transform.rotation={0,25,0};p.transform.scale={2,1,.5f};
        p.captureSettings.title="Edited metadata";p.captureSettings.extraJson="[1,2,3]";p.captureSettings.plain=true;
        auto destination=dir.filePath("edited.pgs");auto result=exportCaptureFile(p,destination);QCOMPARE(result.frames,2);QCOMPARE(result.kept,1ull);QCOMPARE(result.removed,1ull);
        auto decoded=vgsdec::Capture::openFile(destination.toStdString());QCOMPARE(decoded.frameCount(),2ull);QCOMPARE(decoded.frameRate(),25.);QCOMPARE(decoded.metadata().title,std::string("Edited metadata"));QVERIFY(decoded.hasMetadataJson2());
        QVERIFY(decoded.metadataJson2().find("[1,2,3]")!=std::string::npos);QCOMPARE(decoded.playbackMode(),vgsdec::PlaybackMode::PingPong);
        QCOMPARE(decoded.setTime(0,true).activeCount,0ull);const auto &frame=decoded.setTime(.04,true);QCOMPARE(frame.activeCount,1ull);
        QVERIFY(std::abs(frame.positions[0]-.25f)<2e-5f);QVERIFY(std::abs(frame.positions[1]-1)<2e-5f);
        auto original=vgsdec::Capture::openFile(source.toStdString());auto expected=bakeExportFrame(copy(original.setTime(.08,true)),p,3);
        QVERIFY((covariance(frame.rotations,frame.scales)-covariance(expected.rotation.data(),expected.scale.data())).norm()<.001);
        for (int c=0;c<3;++c) QVERIFY(std::abs(frame.colors[c]-expected.colorDc[c])<.0001f);
        for (int k=0;k<45;++k) QVERIFY(std::abs(frame.sphericalHarmonics[k]-expected.shRest[k])<.0001f);
        QFile file(destination);QVERIFY(file.open(QIODevice::ReadOnly));auto bytes=file.readAll();auto header=vgs::readHeader(reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()));
        QCOMPARE(header.encoding,0u);for (const auto &policy : header.policies) QCOMPARE(policy.codec,uint32_t(vgs::Raw));file.close();
        p.captureSettings.plain=false;p.captureSettings.shDegree=1;auto compressed=dir.filePath("edited.vgs");exportCaptureFile(p,compressed);
        auto compressedCapture=vgsdec::Capture::openFile(compressed.toStdString());QCOMPARE(compressedCapture.shDegree(),1u);QCOMPARE(compressedCapture.setTime(.04,true).shCoefficients,3);
        QVERIFY(QFileInfo(compressed).size()<QFileInfo(destination).size());
    }
    void realMintExportWithCrop() {
        const QString asset=qEnvironmentVariable("EDITOR_TEST_CAPTURE");if (asset.isEmpty()) QSKIP("Set EDITOR_TEST_CAPTURE to exercise a real MINT capture.");
        MintFile mint;QString error;QVERIFY2(mint.open(asset,&error),qPrintable(error));MintFrame source;QVERIFY2(mint.decode(0,&source,true,&error),qPrintable(error));
        size_t seed=0;while (seed<source.count && (!source.active[seed] || source.opacity[seed]<.5f)) ++seed;QVERIFY(seed<source.count);
        Project p;p.asset=asset;p.out=1/mint.frameRate();p.transform.position={2,3,4};p.transform.rotation={10,20,30};p.transform.scale={1.1f,.8f,1.2f};
        p.crop().enabled=true;p.crop().shape=CropShape::Box;p.crop().width=p.crop().depth=p.crop().height=.08f;
        auto position=p.transform.matrix().map(QVector3D(source.position[3*seed],source.position[3*seed+1],source.position[3*seed+2]));
        p.crop().transform.position=position-QVector3D(0,.04f,0);p.captureSettings.title="Real MINT edited";
        QTemporaryDir dir;auto output=dir.filePath("mint-edited.vgs");ExportResult result;try {result=exportCaptureFile(p,output);} catch (const std::exception &e) {QFAIL(e.what());}QCOMPARE(result.frames,2);QVERIFY(result.kept>0);QVERIFY(result.removed>0);
        auto verified=vgsdec::Capture::openFile(output.toStdString());QCOMPARE(verified.frameRate(),mint.frameRate());QCOMPARE(verified.metadata().title,std::string("Real MINT edited"));
        for (int frame=0;frame<2;++frame) {const auto &f=verified.setTime(frame/mint.frameRate(),true);for (size_t i=0;i<f.splatCount;++i) if (f.active[i]) QVERIFY(p.crop().contains({f.positions[3*i],f.positions[3*i+1],f.positions[3*i+2]}));}
    }
    void largeDictionariesKeepScaleOpacityAndShWithinQuantizationTolerance() {
        auto f=sample();f.count=5000;f.position.resize(15000);f.rotation.resize(20000);f.scale.resize(15000);f.opacity.resize(5000);f.active.assign(5000,1);f.colorDc.resize(15000);f.shRest.resize(5000*45);
        for (size_t i=0;i<f.count;++i) {
            for (int c=0;c<3;++c) {f.position[3*i+c]=float(i)*.0001f;f.scale[3*i+c]=.001f*std::exp(float((i*3+c)%5000)/5000*4);f.colorDc[3*i+c]=.5f+.4f*std::sin(float(i*.12+c));}
            f.rotation[4*i]=f.rotation[4*i+1]=f.rotation[4*i+2]=0;f.rotation[4*i+3]=1;f.opacity[i]=float(i)/5000;
            for (int k=0;k<45;++k) f.shRest[i*45+k]=std::sin(float(i*.4+k))*.3f;
        }
        vgs::FrameDecoder decoder(packExportFrame(f,3));auto decoded=decoder.evaluate(.3,true);QCOMPARE(decoded.count,f.count);
        for (size_t i=0;i<f.count;++i) {
            QVERIFY(std::abs(decoded.opacity[i]-f.opacity[i])<.0006f);
            for (int c=0;c<3;++c) {QVERIFY(std::abs(decoded.scale[3*i+c]/f.scale[3*i+c]-1)<.01f);QVERIFY(std::abs(decoded.colorDc[3*i+c]-f.colorDc[3*i+c])<.002f);}
            for (int k=0;k<45;++k) QVERIFY(std::abs(decoded.shRest[i*45+k]-f.shRest[i*45+k])<.012f);
        }
    }
    void cancellationAndEmptyCropLeaveDestinationUntouched() {
        QTemporaryDir dir;QString source=dir.filePath("source.pgs");sourceFile(source);Project p;p.asset=source;p.out=.08;
        QString output=dir.filePath("output.vgs");QFile file(output);QVERIFY(file.open(QIODevice::WriteOnly));file.write("existing");file.close();
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(p,output,[](int value,const QString &) {return value<95;}),std::runtime_error);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("existing"));file.close();
        p.crop().enabled=true;p.crop().transform.position={100,100,100};
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(p,output),std::runtime_error);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("existing"));
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(p,source),std::runtime_error);
    }
    void rangeMarkersDragIndependentlyAtPlaybackBoundaries() {
        RangeSlider slider;slider.resize(660,38);slider.show();slider.setFrameRange(0,100);slider.setRangeValues(0,100);slider.setPlayheadValue(0);
        QSignalSpy range(&slider,&RangeSlider::rangeChanged),seek(&slider,&RangeSlider::playheadChanged);
        // In overlaps the playhead at the beginning; the triangle must win.
        QTest::mousePress(&slider,Qt::LeftButton,Qt::NoModifier,{10,3});QTest::mouseMove(&slider,{138,3});QTest::mouseRelease(&slider,Qt::LeftButton,Qt::NoModifier,{138,3});
        QCOMPARE(slider.startValue(),20);QCOMPARE(slider.endValue(),100);QVERIFY(range.count()>0);QCOMPARE(seek.count(),0);
        QTest::mousePress(&slider,Qt::LeftButton,Qt::NoModifier,{650,34});QTest::mouseMove(&slider,{522,34});QTest::mouseRelease(&slider,Qt::LeftButton,Qt::NoModifier,{522,34});
        QCOMPARE(slider.endValue(),80);slider.setPlayheadValue(50);
        QTest::mousePress(&slider,Qt::LeftButton,Qt::NoModifier,{330,19});QTest::mouseMove(&slider,{394,19});QTest::mouseRelease(&slider,Qt::LeftButton,Qt::NoModifier,{394,19});
        QCOMPARE(slider.playheadValue(),60);QCOMPARE(slider.startValue(),20);QCOMPARE(slider.endValue(),80);QVERIFY(seek.count()>0);
    }
private:
    static vgs::Frame copy(const vgsdec::Frame &f) {
        vgs::Frame out;out.count=f.splatCount;out.seconds=f.seconds;out.shCoefficients=f.shCoefficients;size_t n=f.splatCount;
        out.position.assign(f.positions,f.positions+n*3);out.rotation.assign(f.rotations,f.rotations+n*4);out.scale.assign(f.scales,f.scales+n*3);
        out.opacity.assign(f.opacities,f.opacities+n);out.colorDc.assign(f.colors,f.colors+n*3);out.active.assign(f.active,f.active+n);
        out.shRest.assign(f.sphericalHarmonics,f.sphericalHarmonics+n*f.shCoefficients*3);return out;
    }
};
QTEST_MAIN(ExportTests)
#include "export_tests.moc"
