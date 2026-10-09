#include "exportcapture.h"
#include "vgsframe.h"
#include "exporttask.h"
#include <QDir>
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
// The encoder stores splats in spatial order, so an exported capture holds the frame's
// splats in another order: this puts them back in the order of `expected`, pairing each
// expected splat with the closest unused one by position, colour, opacity and life.
vgs::Frame matchedTo(const vgs::Frame &actual,const vgs::Frame &expected) {
    vgs::Frame out=actual;std::vector<bool> used(actual.count,false);
    const size_t sh=size_t(actual.shCoefficients)*3;
    auto move=[](auto &to,const auto &from,size_t width,size_t at,size_t row) {if (!from.empty()) std::copy_n(from.begin()+row*width,width,to.begin()+at*width);};
    for (size_t at=0;at<std::min(actual.count,expected.count);++at) {
        size_t best=0;double closest=1e300;
        for (size_t i=0;i<actual.count;++i) if (!used[i]) {
            double d=0;for (int c=0;c<3;++c) d+=std::abs(actual.position[i*3+c]-expected.position[at*3+c])+std::abs(actual.colorDc[i*3+c]-expected.colorDc[at*3+c]);
            if (!actual.opacity.empty() && !expected.opacity.empty()) d+=std::abs(actual.opacity[i]-expected.opacity[at]);
            if (!actual.active.empty() && !expected.active.empty()) d+=actual.active[i]!=expected.active[at];
            if (d<closest) {closest=d;best=i;}
        }
        used[best]=true;
        move(out.position,actual.position,3,at,best);move(out.rotation,actual.rotation,4,at,best);move(out.scale,actual.scale,3,at,best);
        move(out.opacity,actual.opacity,1,at,best);move(out.colorDc,actual.colorDc,3,at,best);move(out.active,actual.active,1,at,best);move(out.shRest,actual.shRest,sh,at,best);
    }
    return out;
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
                QCOMPARE(actual.count,expected.count);actual=matchedTo(actual,expected);for (size_t row=0;row<actual.count;++row) {
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
                const auto actual=matchedTo(copy(capture.setTime(sample/25.,true)),expected);QCOMPARE(actual.count,expected.count);
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
            const auto a=copy(first.setTime(sample/25.,true)),b=matchedTo(copy(second.setTime(sample/25.,true)),a);QCOMPARE(b.count,a.count);
            for (size_t row=0;row<a.count;++row) for (int c=0;c<3;++c) QVERIFY(std::abs(b.position[row*3+c]-(a.position[row*3+c]+(c==1 ? .5f : 0.f)))<1e-4f);
        }
        // MINT has nowhere to put motion: refused, and the destination is left alone.
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(project,dir.filePath("moving.mint")),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(again,dir.filePath("again.mint")),std::runtime_error);
        QVERIFY(!QFileInfo::exists(dir.filePath("moving.mint")));
    }
    void pruneRemovesWhatAddsLeastAndNothingElse() {
        // Forty splats that make a picture and eight faint specks: 15% of 48 is seven, all specks.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");
        vgs::Frame f;f.count=0;f.shCoefficients=0;
        auto add=[&](float x,float y,float scale,float opacity) {f.position.insert(f.position.end(),{x,y,0});f.rotation.insert(f.rotation.end(),{0,0,0,1});
            f.scale.insert(f.scale.end(),{scale,scale,scale});f.colorDc.insert(f.colorDc.end(),{.5f,.5f,.5f});f.opacity.push_back(opacity);f.active.push_back(1);++f.count;};
        for (int i=0;i<40;++i) add((i%8)*.1f,1+(i/8)*.1f,.04f,.9f);
        for (int i=0;i<8;++i) add(.05f+i*.1f,1.05f,.0004f,.02f);
        {vgs::Header h;h.shDegree=0;h.frameCount=h.durationTicks=3;h.timeDenominator=25;h.chunks.resize(3);for (int i=0;i<3;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
         vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.shDegree=0;options.compression=vgs::Compression::None;
         QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));
         vgs::encodeSequence(h,[&](size_t) {return packExportFrame(f,0);},[&](uint64_t offset,const uint8_t *data,size_t count) {file.seek(qint64(offset));file.write(reinterpret_cast<const char *>(data),qint64(count));},options);}
        Project project;project.asset=source;project.in=0;project.out=2./25;project.modifiers.clear();project.captureSettings.shDegree=0;
        Modifier prune;prune.id=Project::newId();prune.name="Prune";prune.type=ModifierType::PruneLowContribution;project.modifiers={prune};
        QStringList notes;
        auto exported=[&](const Project &p,const QString &name) {
            const auto path=dir.filePath(name);notes=exportCaptureFile(p,path).notes;auto capture=vgsdec::Capture::openFile(path.toStdString());
            const auto frame=copy(capture.setTime(1/25.,false));size_t live=0,specks=0;
            for (size_t i=0;i<frame.count;++i) if (frame.active[i]) {++live;specks+=frame.opacity[i]<.1f;}
            return qMakePair(live,specks);};
        QCOMPARE(exported(project,"pruned.vgs"),qMakePair(size_t(41),size_t(1)));
        // The summary gives the share that really went: seven of 48 in each of the three chunks.
        QVERIFY2(notes.contains("Prune low contribution removed 14.6% of the splats, as asked."),qPrintable(notes.join('\n')));
        // The protection wins over the share: with nothing protected below zero, nothing goes.
        project.modifiers[0].prune.protectAbove=0;QCOMPARE(exported(project,"protected.vgs"),qMakePair(size_t(48),size_t(8)));
        QVERIFY2(notes.contains("Prune low contribution removed 0.0% of the splats, not the 14.6% asked: Protect above kept the rest in 3 of 3 chunks."),qPrintable(notes.join('\n')));
        project.modifiers[0].prune.protectAbove=.25;project.modifiers[0].enabled=false;QCOMPARE(exported(project,"off.vgs"),qMakePair(size_t(48),size_t(8)));
        // The frame exported as a .ply is pruned the same way.
        project.modifiers[0].enabled=true;const auto ply=dir.filePath("frame.ply");const auto result=exportFramePly(project,1/25.,ply);QCOMPARE(result.kept,quint64(41));
        QVERIFY(result.notes.contains("Prune low contribution removed 14.6% of the splats, as asked."));
    }
    void exportKeepsAudioInSyncAndTakesTheThumbnail() {
        // A source with a track and a thumbnail: a range from frame 2 keeps the whole track,
        // starts 2 frames into it, and takes the thumbnail it is given.
        QTemporaryDir dir;const auto source=dir.filePath("source.vgs");
        {vgs::Header h;h.frameCount=h.durationTicks=5;h.timeDenominator=25;h.chunks.resize(5);for (int i=0;i<5;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
         vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.compression=vgs::Compression::None;
         options.extras.push_back({vgs::AudioExtra,vgs::Mp3,vgs::Bytes{'I','D','3',4,0,0}});options.extras.push_back({vgs::ThumbnailExtra,vgs::Png,vgs::Bytes{0x89,'P','N','G'}});
         QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));
         vgs::encodeSequence(h,[&](size_t) {return packExportFrame(sample(),3);},[&](uint64_t offset,const uint8_t *data,size_t count) {file.seek(qint64(offset));file.write(reinterpret_cast<const char *>(data),qint64(count));},options);}
        Project project;project.asset=source;project.in=2./25;project.out=4./25;project.time=project.in;project.modifiers.clear();
        const auto kept=dir.filePath("kept.vgs");const auto result=exportCaptureFile(project,kept);
        {auto capture=vgsdec::Capture::openFile(kept.toStdString());QVERIFY(capture.hasAudio());QCOMPARE(capture.audio(),(std::vector<uint8_t>{'I','D','3',4,0,0}));
         QVERIFY(std::abs(capture.startSeconds()-2./25)<1e-9);QVERIFY(capture.hasThumbnail());QCOMPARE(capture.thumbnail(),(std::vector<uint8_t>{0x89,'P','N','G'}));}
        QVERIFY(result.notes.contains("The source audio is kept whole; players start it where the exported range begins."));
        // The editor's view travels with it, for players without a camera of their own.
        {project.camera.target={0,1,0};project.camera.yaw=90;project.camera.pitch=0;project.camera.distance=3;const auto viewed=dir.filePath("viewed.vgs");exportCaptureFile(project,viewed);
         const auto json=QJsonDocument::fromJson(QByteArray::fromStdString(vgsdec::Capture::openFile(viewed.toStdString()).metadataJson2())).object()["view"].toObject();
         QCOMPARE(json["target"].toArray(),(QJsonArray{0,1,0}));QCOMPARE(json["verticalFov"].toInt(),45);
         const auto eye=json["position"].toArray();QVERIFY(std::abs(eye[0].toDouble()-3)<1e-5 && std::abs(eye[1].toDouble()-1)<1e-5 && std::abs(eye[2].toDouble())<1e-5);}
        const QByteArray jpeg("\xff\xd8\xff\xe0 viewport",14);const auto shot=dir.filePath("shot.vgs");exportCaptureFile(project,shot,{},jpeg);
        {auto capture=vgsdec::Capture::openFile(shot.toStdString());QVERIFY(capture.hasThumbnail());const auto thumb=capture.thumbnail();QCOMPARE(QByteArray(reinterpret_cast<const char *>(thumb.data()),qsizetype(thumb.size())),jpeg);}
        // A task carries the thumbnail it was made with.
        ExportTask task;task.project=project;task.output=dir.filePath("task.vgs");task.path=dir.filePath("task.vgs.vgstask");task.frames=3;task.thumbnail=jpeg;QString error;
        QVERIFY2(writeExportTask(task,&error),qPrintable(error));ExportTask read;QVERIFY2(readExportTask(task.path,&read,&error),qPrintable(error));QCOMPARE(read.thumbnail,jpeg);
    }
    void audioModifierBringsItsFileInSync() {
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,5);
        QFile song(dir.filePath("song.mp3"));QVERIFY(song.open(QIODevice::WriteOnly));song.write("ID3 song");song.close();
        Project project;project.asset=source;project.in=2./25;project.out=4./25;project.time=project.in;project.modifiers.clear();
        Modifier audio;audio.id=Project::newId();audio.name="Song";audio.type=ModifierType::Audio;audio.audioFile=song.fileName();audio.audioOffset=1./25;project.modifiers={audio};
        // The song starts one frame into the capture; the range starts at frame 2, one frame into the song.
        const auto path=dir.filePath("song.vgs");auto result=exportCaptureFile(project,path);
        {auto capture=vgsdec::Capture::openFile(path.toStdString());QVERIFY(capture.hasAudio());QCOMPARE(capture.audioFormat(),vgsdec::Capture::AudioFormat::Mp3);
         const auto track=capture.audio();QCOMPARE(QByteArray(reinterpret_cast<const char *>(track.data()),qsizetype(track.size())),QByteArray("ID3 song"));
         QVERIFY(std::abs(capture.startSeconds()-1./25)<1e-9);}
        QVERIFY(result.notes.contains("The audio is song.mp3, as delivered."));
        // A song that starts after the range does: it plays from the range's start, and says so.
        project.modifiers[0].audioOffset=5./25;result=exportCaptureFile(project,dir.filePath("late.vgs"));
        QVERIFY2(result.notes.contains("The audio starts 0.12 s after the exported range begins; players start it with the range."),qPrintable(result.notes.join('\n')));
        QCOMPARE(vgsdec::Capture::openFile(dir.filePath("late.vgs").toStdString()).startSeconds(),0.);
        // MINT cannot hold it.
        project.modifiers[0].audioOffset=0;result=exportCaptureFile(project,dir.filePath("song.mint"));QVERIFY(result.notes.contains("MINT cannot hold audio: the sound track is not exported."));
        // Not a sound file the container knows.
        QFile other(dir.filePath("song.flac"));QVERIFY(other.open(QIODevice::WriteOnly));other.write("fLaC");other.close();project.modifiers[0].audioFile=other.fileName();
        QVERIFY_EXCEPTION_THROWN(exportCaptureFile(project,dir.filePath("flac.vgs")),std::runtime_error);
    }
    void estimateMatchesTheExport() {
        // A short range is exported whole: the estimate is the file. A long one is sampled.
        QTemporaryDir dir;const auto shortSource=dir.filePath("short.pgs");sourceFile(shortSource,5);
        Project project;project.asset=shortSource;project.in=0;project.out=4./25;project.modifiers.clear();
        const auto estimate=estimateExportSize(project);QVERIFY(estimate.exact);QCOMPARE(estimate.windows,1);
        const auto real=dir.filePath("short.vgs");exportCaptureFile(project,real);
        QVERIFY2(std::abs(estimate.bytes-double(QFileInfo(real).size()))<64,qPrintable(QString("%1 vs %2").arg(estimate.bytes).arg(QFileInfo(real).size())));
        QVERIFY(std::abs(estimate.seconds-5./25)<1e-9);QVERIFY(estimate.averageMbps>0 && estimate.peakMbps>=estimate.averageMbps*0.99);
        const auto longSource=dir.filePath("long.pgs");sourceFile(longSource,250);project.asset=longSource;project.out=249./25;
        const auto sampled=estimateExportSize(project);QVERIFY(!sampled.exact);QCOMPARE(sampled.windows,4);
        const auto full=dir.filePath("long.vgs");exportCaptureFile(project,full);const double size=double(QFileInfo(full).size());
        QVERIFY2(std::abs(sampled.bytes-size)<0.15*size,qPrintable(QString("%1 vs %2").arg(sampled.bytes).arg(size)));
    }
    void erasedSplatsAreNotExported() {
        // The fixture's chunks each hold three records (live at x=0 and x=3, and a dead one).
        // Erasing the one at x=3 in the last chunk removes it there and nowhere else.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,3);
        uint32_t atThree=0;{auto original=vgsdec::Capture::openFile(source.toStdString());const auto f=copy(original.setTime(2./25,false));
            for (size_t i=0;i<f.count;++i) if (f.active[i] && std::abs(f.position[i*3]-3)<1e-3f) atThree=uint32_t(i);}
        Project project;project.asset=source;project.in=0;project.out=2./25;project.modifiers.clear();
        Modifier erase;erase.id=Project::newId();erase.name="Erase";erase.type=ModifierType::Erase;erase.erased[2]={atThree};project.modifiers={erase};
        auto live=[&](const QString &path,double seconds) {auto capture=vgsdec::Capture::openFile(path.toStdString());const auto f=copy(capture.setTime(seconds,false));
            std::vector<float> xs;for (size_t i=0;i<f.count;++i) if (f.active[i]) xs.push_back(f.position[i*3]);std::sort(xs.begin(),xs.end());return xs;};
        const auto path=dir.filePath("erased.vgs");exportCaptureFile(project,path);
        QCOMPARE(live(path,0).size(),size_t(2));QCOMPARE(live(path,2./25).size(),size_t(1));QVERIFY(std::abs(live(path,2./25)[0])<1e-4f);
        // The frame as a .ply too.
        QCOMPARE(exportFramePly(project,2./25,dir.filePath("frame.ply")).kept,quint64(1));
        QCOMPARE(exportFramePly(project,0,dir.filePath("first.ply")).kept,quint64(2));
    }
    void removeCropDeletesWhatIsInsideIt() {
        // The fixture's live splats sit at x=0 and x=3; a Remove cylinder at the origin takes the first.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,3);
        Project project;project.asset=source;project.in=0;project.out=0;project.modifiers.clear();
        Modifier remove;remove.id=Project::newId();remove.name="Remove";remove.crop.enabled=true;remove.crop.remove=true;remove.crop.radius=1;remove.crop.height=3;
        project.modifiers.append(remove);project.selectedModifier=remove.id;
        const auto path=dir.filePath("removed.vgs");exportCaptureFile(project,path);
        auto capture=vgsdec::Capture::openFile(path.toStdString());const auto frame=copy(capture.setTime(0,false));
        std::vector<float> xs;for (size_t i=0;i<frame.count;++i) if (frame.active[i]) xs.push_back(frame.position[i*3]);
        QCOMPARE(xs.size(),size_t(1));QVERIFY(std::abs(xs[0]-3)<1e-4f);
    }
    void antialiasingHintTravelsWithTheSplats() {
        // A plain source has no hint; a .vgs keeps the one it carries; a Gracia .mint is
        // trained with anti-aliasing, so what is exported from it says so.
        QTemporaryDir dir;const auto plain=dir.filePath("plain.pgs");sourceFile(plain,3);
        Project project;project.asset=plain;project.in=0;project.out=2.0/25;project.modifiers.clear();
        auto hinted=[](const QString &path) {return vgsdec::Capture::openFile(path.toStdString()).antialiased();};
        QVERIFY(!hinted(plain));
        const auto fromPlain=dir.filePath("from_plain.vgs");exportCaptureFile(project,fromPlain);QVERIFY(!hinted(fromPlain));
        const auto mint=dir.filePath("source.mint");exportCaptureFile(project,mint);
        Project fromMint=project;fromMint.asset=mint;const auto viaMint=dir.filePath("via_mint.vgs");exportCaptureFile(fromMint,viaMint);QVERIFY(hinted(viaMint));
        Project again=project;again.asset=viaMint;const auto kept=dir.filePath("kept.pgs");exportCaptureFile(again,kept);QVERIFY(hinted(kept));
        // The editor reads it when it opens a capture: that is what its Gaussian view draws with.
        auto opened=[](const QString &path) {CaptureWorker worker;CaptureInfo info;bool done=false;
            QObject::connect(&worker,&CaptureWorker::opened,[&](CaptureInfo i,FramePtr,quint64) {info=i;done=true;});worker.open(path,1,false);return done ? int(info.antialiased) : -1;};
        QCOMPARE(opened(fromPlain),0);QCOMPARE(opened(viaMint),1);QCOMPARE(opened(kept),1);QCOMPARE(opened(mint),1);
        if (!qEnvironmentVariable("CHECK_VGS").isEmpty()) for (const auto &path:qEnvironmentVariable("CHECK_VGS").split(';')) qInfo("%s: antialiased %d",qPrintable(QFileInfo(path).fileName()),opened(path));
    }
    void bakedAntialiasingWidensSplatsAndFadesThem() {
        // From a Gracia source (anti-aliased), unbaked and baked: every axis grows to
        // sqrt(s^2+b^2) and the opacity falls by s/s' of the two largest, native and sampled.
        QTemporaryDir dir;const auto plain=dir.filePath("plain.pgs");sourceFile(plain,3);
        Project base;base.asset=plain;base.in=0;base.out=2.0/25;base.modifiers.clear();
        const auto mint=dir.filePath("source.mint");exportCaptureFile(base,mint);base.asset=mint;
        Project baked=base;Modifier bake;bake.id=Project::newId();bake.name="Bake";bake.type=ModifierType::BakeAntialiasing;bake.bakeDistance=300;bake.bakeScreenHeight=1080;
        baked.modifiers.append(bake);const double b=baked.antialiasingBake();QVERIFY(b>.1 && b<.2);
        const auto reference=dir.filePath("reference.vgs"),nativeOut=dir.filePath("baked.vgs"),sampled=dir.filePath("baked.mint");
        exportCaptureFile(base,reference);const auto result=exportCaptureFile(baked,nativeOut);exportCaptureFile(baked,sampled);
        auto before=vgsdec::Capture::openFile(reference.toStdString()),after=vgsdec::Capture::openFile(nativeOut.toStdString());
        QVERIFY(before.antialiased());QVERIFY(!after.antialiased());
        // The factor travels as an optional attribute: readers that predate it may skip it.
        QFile file(nativeOut);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.read(1<<20);
        const auto header=vgs::readHeader(reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()));
        bool optional=false;for (const auto &policy:header.policies) if (policy.attribute==vgs::OpacityScales) optional=(policy.flags & vgs::OptionalAttribute)!=0;
        QVERIFY(optional);
        MintFile sampledMint;QString error;QVERIFY2(sampledMint.open(sampled,&error),qPrintable(error));MintFrame mintFrame;QVERIFY(sampledMint.decode(0,&mintFrame,false,&error));
        const auto &f0=before.setTime(0,false);std::vector<float> s0(f0.scales,f0.scales+3*f0.splatCount),o0(f0.opacities,f0.opacities+f0.splatCount);std::vector<uint8_t> a0(f0.active,f0.active+f0.splatCount);
        const auto &f1=after.setTime(0,false);QCOMPARE(f1.splatCount,f0.splatCount);
        int checked=0;
        for (size_t i=0;i<f0.splatCount;++i) if (a0[i]) {
            float ratios[3];
            for (int c=0;c<3;++c) {const float s=s0[i*3+c],grown=std::sqrt(s*s+float(b*b));ratios[c]=s/grown;
                QVERIFY2(std::abs(f1.scales[i*3+c]-grown)<grown*.01f,qPrintable(QString("%1 vs %2").arg(f1.scales[i*3+c]).arg(grown)));}
            // The two largest axes' growth: here the smallest is 0.1 m, so it is left out.
            const float expected=o0[i]*ratios[1]*ratios[2];
            QVERIFY2(std::abs(f1.opacities[i]-expected)<=expected*.03f+1e-4f,qPrintable(QString("%1 vs %2").arg(f1.opacities[i]).arg(expected)));
            ++checked;
        }
        QVERIFY(checked>0);
        // The sampled MINT carries the same splats, baked into its own tables.
        int live=0;for (int i=0;i<int(mintFrame.count);++i) if (mintFrame.active[i]) {++live;QVERIFY(mintFrame.opacity[i]<=o0[0]+1e-3f);}
        QCOMPARE(live,checked);QVERIFY(!result.notes.isEmpty());
    }
    void exportTasksKeepTheProjectAndFollowAMovedFolder() {
        QTemporaryDir root;const QString first=root.filePath("first");QVERIFY(QDir().mkpath(first+"/out"));
        sourceFile(first+"/source.pgs",3);
        Project project;project.asset=first+"/source.pgs";project.in=0;project.out=2.0/25;project.transform.position={.5f,0,0};
        // The same output name with another extension gets a task of its own.
        QCOMPARE(exportTaskPath(first+"/out/result.vgs"),first+"/out/result.vgs.vgstask");QVERIFY(exportTaskPath(first+"/out/result.pgs")!=exportTaskPath(first+"/out/result.vgs"));
        ExportTask task;task.project=project;task.output=first+"/out/result.vgs";task.path=first+"/out/result.vgstask";task.frames=exportFrameCount(project,25);
        QCOMPARE(task.frames,3);QString error;QVERIFY2(writeExportTask(task,&error),qPrintable(error));
        // The project is a copy: editing it afterwards does not change the task.
        project.transform.position={9,9,9};
        ExportTask read;QVERIFY2(readExportTask(task.path,&read,&error),qPrintable(error));
        QCOMPARE(read.project.transform.position,QVector3D(.5f,0,0));QCOMPARE(read.frames,3);QCOMPARE(QFileInfo(read.output).absoluteFilePath(),QFileInfo(task.output).absoluteFilePath());
        QCOMPARE(checkExportTask(read),QString());
        // Run twice: the second run overwrites the first.
        QCOMPARE(runExportTask(read).frames,3);QVERIFY(QFile::exists(read.output));const auto firstWrite=QFileInfo(read.output).lastModified();
        QTest::qWait(1100);runExportTask(read);QVERIFY(QFileInfo(read.output).lastModified()>firstWrite);
        // The whole folder moved: the absolute paths are gone, the relative ones lead home.
        const QString moved=root.filePath("moved");QVERIFY(QDir().rename(first,moved));
        QVERIFY2(readExportTask(moved+"/out/result.vgstask",&read,&error),qPrintable(error));
        QCOMPARE(QFileInfo(read.project.asset).absoluteFilePath(),QFileInfo(moved+"/source.pgs").absoluteFilePath());
        QCOMPARE(QFileInfo(read.output).absoluteFilePath(),QFileInfo(moved+"/out/result.vgs").absoluteFilePath());QCOMPARE(checkExportTask(read),QString());
        // What cannot run says why, before anything runs.
        ExportTask missing=read;missing.project.asset=moved+"/nowhere.pgs";QVERIFY(checkExportTask(missing).contains("missing"));
        ExportTask animated=read;animated.output=moved+"/out/result.mint";Modifier move;move.id=Project::newId();move.type=ModifierType::AnimateTransform;
        Transform a,b;b.position={1,0,0};move.animation.setKey(0,a);move.animation.setKey(2,b);animated.project.modifiers.append(move);
        QVERIFY(checkExportTask(animated).contains("MINT"));
        QFile junk(moved+"/junk.vgstask");QVERIFY(junk.open(QIODevice::WriteOnly));junk.write("{}");junk.close();QVERIFY(!readExportTask(moved+"/junk.vgstask",&read,&error));
    }
    void currentFrameIsWrittenAsAnEditedPly() {
        // Frame 0 has live splats at x=0 and x=3; the capture moves 1 m along X and keeps SH1.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,3);
        Project project;project.asset=source;project.in=0;project.out=2.0/25;project.modifiers.clear();
        project.transform.position={1,0,0};project.captureSettings.shDegree=1;
        const auto path=dir.filePath("frame.ply");const auto result=exportFramePly(project,0,path);
        QCOMPARE(result.kept,quint64(2));QCOMPARE(result.removed,quint64(0));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
        const int end=bytes.indexOf("end_header\n");QVERIFY(end>0);const QString header=QString::fromLatin1(bytes.left(end));
        QVERIFY(header.startsWith("ply\nformat binary_little_endian 1.0\nelement vertex 2\n"));
        QCOMPARE(header.count("f_rest_"),9);QCOMPARE(header.count("property float"),3+3+3+9+1+3+4);
        const auto *v=reinterpret_cast<const float *>(bytes.constData()+end+11);const int stride=26;
        QCOMPARE(qsizetype(end+11+2*stride*4),bytes.size());
        auto near=[](float a,float b,float tolerance) {return std::abs(a-b)<=tolerance;};
        QVERIFY(near(v[0],1,1e-4f) && near(v[1],1,1e-4f) && near(v[stride],4,1e-4f));          // x+1
        QVERIFY(near(v[6],float((.8-.5)/0.28209479177387814),.02f));                             // f_dc from rgb
        QVERIFY(near(v[18],std::log(.7f/.3f),.02f));                                              // logit opacity
        QVERIFY(near(v[19],std::log(.1f),.02f) && near(v[21],std::log(.3f),.02f));               // log scale
        // wxyz: any orientation that rebuilds the same covariance (the axes of diag(.1, .2, .3)).
        const float xyzw[4]={v[23],v[24],v[25],v[22]},scale[3]={std::exp(v[19]),std::exp(v[20]),std::exp(v[21])};
        const auto rebuilt=covariance(xyzw,scale);QVERIFY((rebuilt-Eigen::Vector3d(.01,.04,.09).asDiagonal().toDenseMatrix()).norm()<2e-3);
        // A frame where nothing is alive has nothing to write.
        bool threw=false;try {exportFramePly(project,1.0/25,dir.filePath("empty.ply"));} catch (const std::exception &) {threw=true;}
        QVERIFY(threw);QVERIFY(!QFile::exists(dir.filePath("empty.ply")));
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
    void skinRecoveryNeverTintsPastTheSourceColour() {
        // Red and white cloth beside a face passes for skin once despilled; the skin next to it
        // may only move back towards its own source colour, not take the cloth's pink.
        const int n=401;MintFrame original,corrected;original.count=corrected.count=n;
        original.active.fill(1,n);original.opacity.fill(1,n);original.position.resize(n*3);original.colorDc.resize(n*3);corrected.colorDc.resize(n*3);
        for (int i=0;i<n;++i) {
            original.position[i*3]=i<400 ? float(i%20)*.01f : .09f;
            original.position[i*3+1]=i<400 ? 1+float(i/20)*.01f : 1.09f;original.position[i*3+2]=i<400 ? 0 : .01f;
            const float cloth[3]={.75f,.35f,.33f},skin[3]={.70f,.52f,.42f},despilled[3]={.70f,.50f,.42f};
            for (int c=0;c<3;++c) {original.colorDc[i*3+c]=i<400 ? cloth[c] : skin[c];corrected.colorDc[i*3+c]=i<400 ? cloth[c] : despilled[c];}
        }
        QVector<float> deltas;QVERIFY(mintSkinRecovery(original,corrected,&deltas,{}));
        QCOMPARE(deltas[400*3],0.f);QCOMPARE(deltas[400*3+2],0.f);
        QVERIFY(deltas[400*3+1]>=0 && deltas[400*3+1]<=.02f);
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
    void nativeAnimatedCropKeepsWhatItContainsAtEachSample() {
        auto source=temporalFixture(true);vgs::FrameDecoder reference(source);Project project;
        auto &m=*project.modifier();m.crop.enabled=true;CropVolume a=m.crop,b=m.crop;
        // The fixture's splats sit at x=0 and x=3: the crop travels from one group to the other.
        a.transform.position={0,0,0};a.radius=a.radiusZ=1;a.height=2;b=a;b.transform.position={3,0,0};
        m.cropAnimation.setKey(0,a);m.cropAnimation.setKey(5,b);m.cropAnimation.animated=true;
        for (auto plan:{NativeChunkPlan{0,0,6},NativeChunkPlan{0,1,4}}) {
            ExportResult result;auto edited=editNativeChunk(source,plan,project,&result,{},nullptr,nullptr,plan.first);vgs::FrameDecoder output(edited);
            std::vector<size_t> firstKept,lastKept;
            for (int tick=0;tick<plan.intervals;++tick) {
                auto original=reference.evaluate((plan.first+tick)/6.,true),actual=output.evaluate(tick/double(plan.intervals),true);
                const CompiledModifiers at(project.modifiersAtFrame(plan.first+tick));std::vector<size_t> expected;size_t actualCount=0;
                for (size_t row=0;row<original.count;++row) if (original.active[row] && at.keepsPosition({original.position[row*3],original.position[row*3+1],original.position[row*3+2]})) expected.push_back(row);
                for (size_t row=0;row<actual.count;++row) if (actual.active[row]) ++actualCount;
                QCOMPARE(actualCount,expected.size());
                if (tick==0) firstKept=expected;
                lastKept=expected;
            }
            // The crop really moved: what it keeps at the first and last sample differs.
            QVERIFY(firstKept!=lastKept);
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
        auto f=sample();Project p;p.modifiers.clear();Modifier spill;spill.id=Project::newId();spill.name="Despill";spill.type=ModifierType::Colour;spill.colourDespill=Modifier::DespillOnExport;spill.colourRecoverSkin=false;p.modifiers={spill};
        auto processed=bakeExportFrame(f,p,3);QVERIFY(processed.colorDc[1]<f.colorDc[1]);QVERIFY(processed.colorDc[1]<=(processed.colorDc[0]+processed.colorDc[2])*.5f+1e-6f);
        for (int k=0;k<15;++k) QVERIFY(std::abs(processed.shRest[3*k+1]-(processed.shRest[3*k]+processed.shRest[3*k+2])*.5f)<1e-6f);
        p.modifiers[0].colourRecoverSkin=true;QVERIFY(bakeExportFrame(f,p,3).count>0);
        // The export settings no longer despill: only the modifier does.
        Project settingsOnly;settingsOnly.modifiers.clear();settingsOnly.captureSettings.despill=true;QCOMPARE(bakeExportFrame(f,settingsOnly,3).colorDc,bakeExportFrame(f,Project{},3).colorDc);
    }
    void despillAlwaysShowsInThePreview() {
        // Always: the preview's frame carries the export's despilled colour. Only on export: it
        // carries the source's.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,3);
        CaptureWorker worker;FramePtr shown;connect(&worker,&CaptureWorker::decoded,this,[&](FramePtr f,quint64) {shown=f;});
        connect(&worker,&CaptureWorker::opened,this,[&](CaptureInfo,FramePtr,quint64) {});
        worker.open(source,1,true);
        Project project;project.modifiers.clear();Modifier spill;spill.id=Project::newId();spill.name="Despill";spill.type=ModifierType::Colour;spill.colourRecoverSkin=false;
        auto green=[&](int mode) {project.modifiers={spill};project.modifiers[0].colourDespill=mode;shown.reset();worker.decode(0,1,true,project);
            if (!shown) return -1.f;for (size_t i=0;i<shown->records.size();++i) if (shown->active[i] && std::abs(shown->records[i].position[0])<1e-3f) return shown->records[i].color[1];return -1.f;}; // the greenish splat at x=0
        const float source0=green(Modifier::DespillNone),exportOnly=green(Modifier::DespillOnExport),always=green(Modifier::DespillAlways);
        QVERIFY(source0>=0);QCOMPARE(exportOnly,source0);QVERIFY2(always<source0,qPrintable(QString("%1 vs %2").arg(always).arg(source0)));
        // The same despill the export applies.
        vgs::Frame f=sample();CaptureSettings settings;settings.recoverSkin=false;despillFrame(f,settings);QVERIFY(std::abs(always-f.colorDc[1])<2e-3f);
    }
    void colourModifierExportsTheColourItShows() {
        // Half the exposure halves every colour, base and view-dependent, in the native and the
        // sampled export alike.
        QTemporaryDir dir;const auto source=dir.filePath("source.pgs");sourceFile(source,3);
        Project project;project.asset=source;project.in=0;project.out=2./25;project.modifiers.clear();
        const auto plainPath=dir.filePath("plain.vgs");exportCaptureFile(project,plainPath);
        Modifier colour;colour.id=Project::newId();colour.name="Colour";colour.type=ModifierType::Colour;colour.colourExposure=-1;project.modifiers={colour};
        const auto darkPath=dir.filePath("dark.vgs");exportCaptureFile(project,darkPath);
        auto plain=vgsdec::Capture::openFile(plainPath.toStdString()),dark=vgsdec::Capture::openFile(darkPath.toStdString());
        const auto a=copy(plain.setTime(0,true)),b=copy(dark.setTime(0,true));QCOMPARE(b.count,a.count);
        for (size_t i=0;i<a.count;++i) if (a.active[i]) {
            for (int c=0;c<3;++c) QVERIFY2(std::abs(b.colorDc[i*3+c]-a.colorDc[i*3+c]*.5f)<.01f,qPrintable(QString("%1 vs %2").arg(b.colorDc[i*3+c]).arg(a.colorDc[i*3+c]*.5f)));
            for (int k=0;k<a.shCoefficients*3;++k) QVERIFY(std::abs(b.shRest[i*size_t(a.shCoefficients)*3+k]-a.shRest[i*size_t(a.shCoefficients)*3+k]*.5f)<2e-3f);
        }
        const auto f=sample();Project bare;bare.modifiers.clear();const auto base=bakeExportFrame(f,bare,3);Project shaded=bare;shaded.modifiers={colour};const auto halved=bakeExportFrame(f,shaded,3);
        for (size_t i=0;i<base.colorDc.size();++i) QVERIFY(std::abs(halved.colorDc[i]-base.colorDc[i]*.5f)<1e-5f);
        for (size_t i=0;i<base.shRest.size();++i) QVERIFY(std::abs(halved.shRest[i]-base.shRest[i]*.5f)<1e-5f);
        // Despill is the modifier's, with its own settings: export settings that also ask for it
        // change nothing.
        Modifier spill;spill.id=Project::newId();spill.type=ModifierType::Colour;spill.colourDespill=Modifier::DespillOnExport;spill.colourDespillStrength=.6;spill.colourRecoverSkin=false;
        Project modifierOnly=bare;modifierOnly.modifiers={spill};Project both=modifierOnly;both.captureSettings.despill=true;both.captureSettings.despillStrength=.1;both.captureSettings.greenGain=1.5;
        const auto r1=bakeExportFrame(f,modifierOnly,3),r2=bakeExportFrame(f,both,3);
        QCOMPARE(r2.colorDc,r1.colorDc);QCOMPARE(r2.shRest,r1.shRest);QVERIFY(r1.colorDc!=base.colorDc);
        Project gained=bare;Modifier strong=spill;strong.colourGreenGain=1.5;gained.modifiers={strong};QVERIFY(bakeExportFrame(f,gained,3).colorDc!=r1.colorDc);
        // Opacity scales every splat's, at most 1, in both exports.
        Project boosted=bare;Modifier boost;boost.id=Project::newId();boost.name="Boost";boost.type=ModifierType::Colour;boost.colourOpacity=1.5;boosted.modifiers={boost};
        const auto opaque=bakeExportFrame(f,boosted,3);for (size_t i=0;i<base.opacity.size();++i) QVERIFY(std::abs(opaque.opacity[i]-std::min(1.f,base.opacity[i]*1.5f))<1e-6f);
        project.modifiers={boost};const auto boostPath=dir.filePath("boost.vgs");exportCaptureFile(project,boostPath);
        {auto boostedCapture=vgsdec::Capture::openFile(boostPath.toStdString());const auto c=copy(boostedCapture.setTime(0,false)),a0=copy(plain.setTime(0,false));
         for (size_t i=0;i<c.count;++i) if (c.active[i]) QVERIFY2(std::abs(c.opacity[i]-std::min(1.f,a0.opacity[i]*1.5f))<.01f,qPrintable(QString("%1 vs %2").arg(c.opacity[i]).arg(a0.opacity[i])));}
    }
    void compatibleSignedExportTrimMetadataCropAndEmptyFrame() {
        QTemporaryDir dir;QString source=dir.filePath("source.pgs");sourceFile(source);
        Project p;p.asset=source;p.in=1./25;p.out=2./25;p.crop().enabled=true;p.crop().radius=1;p.crop().height=2;
        p.transform.position={.25f,0,0};p.transform.rotation={0,25,0};p.transform.scale={2,1,.5f};
        p.captureSettings.title="Edited metadata";p.captureSettings.extraJson="[1,2,3]";p.captureSettings.plain=false; // the extension decides, not this
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
        p.captureSettings.plain=true;p.captureSettings.shDegree=1;auto compressed=dir.filePath("edited.vgs");exportCaptureFile(p,compressed);
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
