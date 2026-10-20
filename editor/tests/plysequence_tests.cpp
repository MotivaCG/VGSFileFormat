// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#include "plysequence.h"
#include "plyexport.h"
#include "project.h"
#include "captureworker.h"
#include "exportcapture.h"
#include "nativeexport.h"
#include "mintfile.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cstring>
#include <map>

namespace {
vgs::Frame sample(float x=0,bool alive=true,float scale=.03f) {
    vgs::Frame f;f.count=3;f.active={1,1,1};f.position={x,1,0, 100,100,100, x+5,1,0};f.rotation={0,0,0,1,0,0,0,1,0,0,0,1};
    f.scale={.02f,.03f,.04f, .01f,.01f,.01f, scale,scale,scale};f.opacity={.8f,0,alive ? .7f : 0};f.colorDc={.3f,.4f,.5f,.5f,.5f,.5f,.7f,.6f,.4f};f.shCoefficients=3;f.shRest.resize(27);
    for (size_t i=0;i<f.shRest.size();++i) f.shRest[i]=float(i)*.005f;return f;
}
QString ply(const QString &directory,int number,const vgs::Frame &f) {const auto path=QDir(directory).filePath(QString::number(number).rightJustified(8,'0')+".ply");writePly(f,path);return path;}
void text(const QString &path,const QByteArray &bytes) {QFile f(path);if (!f.open(QIODevice::WriteOnly) || f.write(bytes)!=bytes.size()) throw std::runtime_error("Cannot create fixture.");}
int alive(const vgs::Frame &f) {return int(std::count(f.active.begin(),f.active.end(),uint8_t(1)));}
}
class PlySequenceTests : public QObject {
    Q_OBJECT
private slots:
    void numberedDetectionStopsAtFirstGapAndKeepsWidth() {
        QTemporaryDir dir;const auto first=ply(dir.path(),48,sample());ply(dir.path(),49,sample(1));ply(dir.path(),50,sample(2));ply(dir.path(),52,sample(4));
        text(dir.filePath("0000051.ply"),"bad mixed-width file");
        PlySequence source(first);QCOMPARE(source.frames(),3);QCOMPARE(source.spec().numbers,QVector<qint64>({48,49,50}));QCOMPARE(source.degree(),1);QCOMPARE(source.chunks().size(),1);
        PlySequence middle(dir.filePath("00000049.ply"));QCOMPARE(middle.frames(),2);QCOMPARE(middle.spec().numbers.first(),qint64(49));
        const auto single=dir.filePath("static.ply");QVERIFY(QFile::copy(first,single));PlySequence still(single);QCOMPARE(still.frames(),1);
        QCOMPARE(alive(still.frame(0)),2);QVERIFY(source.bounds()[3]<10); // Dead room/floater never affects bounds.
    }
    void missingSidecarsStillSplitReappearanceAndScaleChanges() {
        QTemporaryDir dir;const auto first=ply(dir.path(),45,sample());ply(dir.path(),46,sample(1,false));ply(dir.path(),47,sample(2,true,.06f));ply(dir.path(),48,sample(3,true,.07f));
        PlySequence source(first);QVERIFY(source.spec().births.isEmpty());QCOMPARE(source.frames(),4);QCOMPARE(source.chunks().size(),1);
        const auto records=source.records(0);QCOMPARE(records.size(),size_t(4));for (const auto &r:records) QVERIFY(r.row!=1);
        QCOMPARE(alive(source.frame(0)),2);QCOMPARE(alive(source.frame(1/30.)),1);QCOMPARE(alive(source.frame(2/30.)),2);
        int previous=-1;for (int frame:{0,2,3}) {const auto f=source.frame(frame/30.);int current=-1;for (size_t i=0;i<f.count;++i) if (f.active[i] && f.position[i*3]>4) current=int(i);QVERIFY(current>=0);QVERIFY(current!=previous);previous=current;}
        std::vector<uint32_t> order;const auto native=packPlySequenceChunk(source,0,1,{},&order);vgs::FrameDecoder decoder(native,1/30.);
        for (int frame=0;frame<4;++frame) {const auto actual=decoder.evaluate(frame/4.);QCOMPARE(alive(actual),frame==1 ? 1 : 2);
            const auto expected=source.frame(frame/30.);
            for (size_t i=0;i<actual.count;++i) if (actual.active[i]) {const auto at=order[i];for (int c=0;c<3;++c) {QVERIFY(std::abs(actual.position[i*3+c]-expected.position[at*3+c])<.002f);QVERIFY(std::abs(actual.colorDc[i*3+c]-expected.colorDc[at*3+c])<1e-4f);}
                for (int k=0;k<9;++k) QVERIFY(std::abs(actual.shRest[i*9+k]-expected.shRest[at*9+k])<1e-4f);}}
    }
    void birthEventsAreLocalAndIncludeEventsBeforeTheSelectedFile() {
        QTemporaryDir dir;ply(dir.path(),45,sample());const auto first=ply(dir.path(),46,sample(1));ply(dir.path(),47,sample(2));
        text(dir.filePath("birth_events.tsv"),"1\t0\t1\n2\t0\t2\n");
        PlySequence source(first);QCOMPARE(source.spec().birthOrigin,qint64(45));const auto records=source.records(0);
        std::vector<quint32> generations;for (const auto &r:records) if (!r.row) generations.push_back(r.generation);
        QCOMPARE(generations,(std::vector<quint32>{1,2}));QCOMPARE(alive(source.frame(0)),2);QCOMPARE(alive(source.frame(1/30.)),2);
    }
    void residualRotationsKeepAlternatingQuaternionSignsContinuous() {
        QTemporaryDir dir;QString first;
        for (int frame=0;frame<4;++frame) {auto f=sample(float(frame));const float angle=float(frame)*.1745329252f,sign=frame%2 ? -1.f : 1.f;
            f.rotation[0]=0;f.rotation[1]=sign*std::sin(angle);f.rotation[2]=0;f.rotation[3]=sign*std::cos(angle);
            const auto path=ply(dir.path(),frame,f);if (!frame) first=path;}
        PlySequence source(first);std::vector<uint32_t> order;vgs::FrameDecoder decoder(packPlySequenceChunk(source,0,1,{},&order));
        for (int frame=0;frame<4;++frame) {const auto expected=source.frame(frame/30.),actual=decoder.evaluate(frame/4.);
            for (size_t i=0;i<actual.count;++i) if (actual.active[i]) {const auto at=order[i];double dot=0;for (int c=0;c<4;++c) dot+=actual.rotation[i*4+c]*expected.rotation[at*4+c];
                const double degrees=2*std::acos(std::clamp(std::abs(dot),0.,1.))*180/3.141592653589793;QVERIFY2(degrees<3,qPrintable(QString::number(degrees)));}}
    }
    void completeGenerationResetStartsAnotherChunk() {
        QTemporaryDir dir;const auto first=ply(dir.path(),45,sample());for (int i=1;i<4;++i) ply(dir.path(),45+i,sample(float(i)));
        text(dir.filePath("birth_events.tsv"),"2\t0\t1\n2\t1\t1\n2\t2\t1\n");PlySequence source(first);
        QCOMPARE(source.spec().chunks,QVector<int>({0,2}));QCOMPARE(source.chunks()[0].records,size_t(2));QCOMPARE(source.chunks()[1].records,size_t(2));
        Project project;project.asset=first;project.plySequence=source.spec();project.modifiers.clear();project.out=3/30.;
        const auto output=dir.filePath("out.pgs");exportCaptureFile(project,output);auto capture=vgsdec::Capture::openFile(output.toStdString());
        QCOMPARE(capture.chunkCount(),size_t(2));QCOMPARE(capture.chunk(0).intervals,quint64(2));QCOMPARE(capture.chunk(1).intervals,quint64(2));
        for (int frame=0;frame<4;++frame) QCOMPARE(capture.setTime(frame/30.,true).activeCount,quint64(2));
    }
    void forgeMetadataIsOptionalAndCanSplitEqualPopulations() {
        QTemporaryDir dir;const auto first=ply(dir.path(),45,sample());for (int i=1;i<4;++i) ply(dir.path(),45+i,sample(float(i)));
        text(dir.filePath("sequence.json"),R"({"frames":[{"frame":45,"chunk":"00000045"},{"frame":46,"chunk":"00000045"},{"frame":47,"chunk":"00000047"},{"frame":48,"chunk":"00000047"}]})");
        PlySequence source(first);QCOMPARE(source.spec().chunks,QVector<int>({0,2}));QVERIFY(source.spec().births.isEmpty());
    }
    void recordsRunIntoTheClosingSampleAndTheFitIsChecked() {
        QTemporaryDir dir;const auto first=ply(dir.path(),45,sample());for (int i=1;i<4;++i) ply(dir.path(),45+i,sample(float(i)));
        text(dir.filePath("sequence.json"),R"({"frames":[{"frame":45,"chunk":"00000045"},{"frame":46,"chunk":"00000045"},{"frame":47,"chunk":"00000047"},{"frame":48,"chunk":"00000047"}]})");
        PlySequence source(first);QCOMPARE(source.spec().chunks,QVector<int>({0,2}));
        for (const auto &r:source.records(0)) QCOMPARE(r.last,2);for (const auto &r:source.records(1)) QCOMPARE(r.last,3);
        PlyFitCheck check;std::vector<uint32_t> order;vgs::FrameDecoder decoder(packPlySequenceChunk(source,0,1,{},&order,&check),1/30.);
        QCOMPARE(check.shown,quint64(4));QCOMPARE(check.missing,quint64(0));QCOMPARE(check.stray,quint64(0));QVERIFY(check.worst<.002f);
        // Halfway through the last interval, the splat is halfway to the next chunk's first frame.
        const auto halfway=decoder.evaluate(.75);for (size_t i=0;i<halfway.count;++i) if (halfway.active[i] && halfway.position[i*3+1]>.5f && halfway.position[i*3]<4) QVERIFY(std::abs(halfway.position[i*3]-1.5f)<.01f);
    }
    void chunksRunWhileFramesStayCompatible() {
        QTemporaryDir plain,cut;QString first,cutFirst;QByteArray reset;
        for (int i=0;i<40;++i) {const auto a=ply(plain.path(),i,sample(float(i)*.01f)),b=ply(cut.path(),i,sample(float(i)*.01f));if (!i) {first=a;cutFirst=b;}}
        for (int row=0;row<3;++row) reset+="36 "+QByteArray::number(row)+" 1\n";text(QDir(cut.path()).filePath("birth_events.tsv"),reset);
        QCOMPARE(PlySequence(first).spec().chunks,QVector<int>({0}));QCOMPARE(PlySequence(cutFirst).spec().chunks,QVector<int>({0,36}));
        QTemporaryDir mixed;QString mixedFirst;
        for (int i=0;i<3;++i) {auto f=sample();f.count=3-i;f.active.resize(f.count);f.position.resize(f.count*3);f.rotation.resize(f.count*4);f.scale.resize(f.count*3);
            f.opacity.resize(f.count);f.colorDc.resize(f.count*3);f.shRest.resize(f.count*9);const auto path=ply(mixed.path(),i,f);if (!i) mixedFirst=path;}
        QCOMPARE(PlySequence(mixedFirst).spec().chunks,QVector<int>({0,1,2}));
    }
    void publishedForgeFramesUseTheRunsAlignedFrames() {
        QTemporaryDir dir;const auto first=ply(dir.path(),45,sample());ply(dir.path(),46,sample(1));ply(dir.path(),47,sample(2));
        text(dir.filePath("sequence.json"),R"({"vgs":["00000045_00000047.vgs"],"frames":[{"frame":45,"chunk":"00000045"},{"frame":46,"chunk":"00000045"},{"frame":47,"chunk":"00000045"}]})");
        text(dir.filePath("00000045_00000047.vgs"),"encoded");QVERIFY_EXCEPTION_THROWN(PlySequence published(first),std::runtime_error);
        const auto aligned=dir.filePath(".work/vgs_00000045");QVERIFY(QDir().mkpath(aligned));for (int i=0;i<3;++i) ply(aligned,45+i,sample(float(i)));
        text(QDir(aligned).filePath("birth_events.tsv"),"1 0 1\n");
        PlySequence source(first);QCOMPARE(source.frames(),3);QCOMPARE(source.spec().births,QDir(aligned).filePath("birth_events.tsv"));QCOMPARE(source.spec().birthOrigin,qint64(45));
        for (const auto &file:source.spec().files) QVERIFY(file.startsWith(aligned));
    }
    void spirulaFrameDirectoriesAndExplicitRoomOffset() {
        QTemporaryDir dir;for (int i=0;i<3;++i) {const auto path=dir.filePath(QString::number(i).rightJustified(3,'0'));QVERIFY(QDir().mkpath(path));writePly(sample(float(i)),path+"/splat.ply");}
        text(dir.filePath("birth_events.tsv"),"1\t0\t1\n");PlySequence plain(dir.filePath("001/splat.ply"));QCOMPARE(plain.frames(),2);QCOMPARE(plain.spec().birthOrigin,qint64(0));
        auto spec=plain.spec();spec.rowOffset=2;PlySequence withRoom(spec.files.first(),spec);QCOMPARE(withRoom.spec().rowOffset,2);
        for (const auto &r:withRoom.records(0)) if (r.row==2) QCOMPARE(r.generation,quint32(1));
    }
    void savedProjectFreezesFilesTimingAndChunks() {
        QTemporaryDir dir;const auto first=ply(dir.path(),48,sample());ply(dir.path(),49,sample(1));PlySequence source(first);
        Project project;project.asset=first;project.plySequence=source.spec();project.captureSettings.plyFps=24;project.out=1/24.;
        const auto saved=dir.filePath("scene.vgsproj");QString error;QVERIFY2(project.write(saved,&error),qPrintable(error));ply(dir.path(),50,sample(2));
        Project restored;QVERIFY2(Project::read(saved,&restored,&error),qPrintable(error));QCOMPARE(restored.captureSettings.plyFps,24.);
        PlySequence fixed(restored.asset,restored.plySequence,restored.captureSettings.plyFps);QCOMPARE(fixed.frames(),2);QCOMPARE(fixed.fps(),24.);QCOMPARE(fixed.spec().chunks,source.spec().chunks);
        QFile file(saved);QVERIFY(file.open(QIODevice::ReadOnly));const auto paths=QJsonDocument::fromJson(file.readAll()).object()["plySequence"].toObject()["files"].toArray();QCOMPARE(paths[0].toString(),QString("00000048.ply"));
    }
    void corruptExistingFilesFailInsteadOfEndingTheSequence() {
        QTemporaryDir dir;const auto first=ply(dir.path(),48,sample());text(dir.filePath("00000049.ply"),"not a PLY");
        QVERIFY_EXCEPTION_THROWN(PlySequence sequence(first),std::runtime_error);QFile::remove(dir.filePath("00000049.ply"));
        text(dir.filePath("birth_events.tsv"),"1 0 nope\n");QVERIFY_EXCEPTION_THROWN(PlySequence sequence(first),std::runtime_error);
    }
    void asciiPointCloudWorksWithoutGaussianFields() {
        QTemporaryDir dir;const auto path=dir.filePath("points.ply");text(path,"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n0 1 0 255 0 0\n1 2 3 0 128 255\n");
        PlySequence source(path);const auto frame=source.frame(0);QCOMPARE(frame.count,quint64(2));QCOMPARE(frame.colorDc[0],1.f);QVERIFY(std::abs(frame.colorDc[4]-128/255.f)<1e-6f);QCOMPARE(source.degree(),0);
    }
    void emptyChunksAndDifferentShSchemasRemainValid() {
        QTemporaryDir dir;auto dead=sample();dead.opacity={0,0,0};const auto first=ply(dir.path(),0,dead);ply(dir.path(),1,dead);auto noSh=sample(2);noSh.shCoefficients=0;noSh.shRest.clear();ply(dir.path(),2,noSh);
        PlySequence source(first);QCOMPARE(source.chunks().size(),2);QCOMPARE(source.chunks()[0].records,size_t(0));
        vgs::FrameDecoder empty(packPlySequenceChunk(source,0,1));QCOMPARE(alive(empty.evaluate(.5)),0);
        vgs::FrameDecoder padded(packPlySequenceChunk(source,1,1));const auto shown=padded.evaluate(0);QCOMPARE(alive(shown),2);QCOMPARE(shown.shCoefficients,3);for (float v:shown.shRest) QCOMPARE(v,0.f);
    }
    void binaryBigEndianPointCloud() {
        QTemporaryDir dir;QByteArray bytes="ply\nformat binary_big_endian 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n";
        for (float v:{1.5f,2.f,-3.f}) {quint32 bits;std::memcpy(&bits,&v,4);for (int b=3;b>=0;--b) bytes.append(char(bits>>(8*b)));}const auto path=dir.filePath("point.ply");text(path,bytes);
        PlySequence source(path);const auto f=source.frame(0);QCOMPARE(f.position,(std::vector<float>{1.5f,2,-3}));
    }
    void plyExportsUseTheSameRecordIdsAsPreview() {
        QTemporaryDir dir;const auto first=ply(dir.path(),48,sample());ply(dir.path(),49,sample(1,false));ply(dir.path(),50,sample(2,true,.06f));PlySequence source(first);
        Project project;project.asset=first;project.plySequence=source.spec();project.modifiers.clear();project.out=2/30.;
        const auto path=dir.filePath("plain.pgs");exportCaptureFile(project,path);auto capture=vgsdec::Capture::openFile(path.toStdString());QCOMPARE(capture.chunkCount(),size_t(1));QCOMPARE(capture.shDegree(),1u);
        for (int frame=0;frame<3;++frame) QCOMPARE(capture.setTime(frame/30.,true).activeCount,quint64(frame==1 ? 1 : 2));
        int old=-1;const auto records=source.records(0);for (size_t i=0;i<records.size();++i) if (records[i].row==2 && records[i].first==0) old=int(i);QVERIFY(old>=0);
        Modifier erase;erase.id=Project::newId();erase.type=ModifierType::Erase;erase.erased[0].push_back(uint32_t(old));project.modifiers={erase};
        const auto edited=dir.filePath("edited.pgs");exportCaptureFile(project,edited);auto output=vgsdec::Capture::openFile(edited.toStdString());QCOMPARE(output.setTime(0,false).activeCount,quint64(1));QCOMPARE(output.setTime(2/30.,false).activeCount,quint64(2));
        const auto asPly=dir.filePath("snapshot.ply");QCOMPARE(exportFramePly(project,2/30.,asPly).kept,quint64(2));PlySequence snapshot(asPly);QCOMPARE(alive(snapshot.frame(0)),2);
        const auto originalSize=QFileInfo(dir.filePath("00000049.ply")).size();QVERIFY_EXCEPTION_THROWN(exportFramePly(project,0,dir.filePath("00000049.ply")),std::runtime_error);QCOMPARE(QFileInfo(dir.filePath("00000049.ply")).size(),originalSize);
        const auto estimate=estimateExportSize(project);QVERIFY(estimate.exact);QVERIFY(estimate.bytes>0);
    }
    void plyExportsToMintWithAndWithoutHigherSh() {
        QTemporaryDir dir;const auto first=ply(dir.path(),48,sample());ply(dir.path(),49,sample(1));PlySequence source(first);
        Project project;project.asset=first;project.plySequence=source.spec();project.modifiers.clear();project.out=1/30.;
        for (int degree:{0,1}) {
            project.captureSettings.shDegree=degree;const auto path=dir.filePath(QString("degree%1.mint").arg(degree));exportCaptureFile(project,path);
            MintFile mint;QString error;QVERIFY2(mint.openReadOnly(path,&error),qPrintable(error));QCOMPARE(mint.shDegree(),degree ? 3 : 0);QCOMPARE(mint.frameCount(),2);
            for (int frame=0;frame<2;++frame) {MintFrame output;QVERIFY2(mint.decode(frame/30.,&output,true,&error),qPrintable(error));
                QCOMPARE(std::count(output.active.begin(),output.active.end(),uint8_t(1)),2);}
        }
    }
    void realDn1ImportFrom48() {
        const auto path=qEnvironmentVariable("PLY_SEQUENCE_REFERENCE");if (path.isEmpty()) QSKIP("Set PLY_SEQUENCE_REFERENCE to exercise the DN1 sequence.");
        PlySequence source(path);QCOMPARE(source.frames(),7);QCOMPARE(source.spec().numbers.first(),qint64(48));QCOMPARE(source.spec().numbers.last(),qint64(54));QCOMPARE(source.spec().birthOrigin,qint64(45));
        QVERIFY(source.chunks().size()>=1);QVERIFY(source.degree()==1);for (int frame:{0,3,6}) {const auto f=source.frame(frame/30.);QVERIFY(alive(f)>1000);}
        double positionError=0,colourError=0,shError=0,shSquared=0,positionSquared=0,colourSquared=0;size_t shSamples=0,baseSamples=0;
        for (size_t ci=0;ci<size_t(source.chunks().size());++ci) {const auto &c=source.chunks()[qsizetype(ci)];std::vector<uint32_t> order;vgs::FrameDecoder decoder(packPlySequenceChunk(source,ci,1,{},&order));
            for (int frame:{0,3,6}) {if (source.chunkAt(frame/30.)!=ci) continue;
                const auto expected=source.frame(frame/30.);const auto actual=decoder.evaluate(double(frame-c.first)/c.frames);QCOMPARE(actual.count,expected.count);
                for (size_t i=0;i<actual.count;i+=17) {const auto at=order[i];if (!expected.active[at]) continue;
                    for (int k=0;k<3;++k) {const double p=actual.position[i*3+k]-expected.position[at*3+k],c=actual.colorDc[i*3+k]-expected.colorDc[at*3+k];positionError=std::max(positionError,std::abs(p));colourError=std::max(colourError,std::abs(c));positionSquared+=p*p;colourSquared+=c*c;++baseSamples;}
                    for (int k=0;k<9;++k) {const double e=actual.shRest[i*9+k]-expected.shRest[at*9+k];shError=std::max(shError,std::abs(e));shSquared+=e*e;++shSamples;}}}}
        const double shRms=std::sqrt(shSquared/shSamples);
        // vgsplyseq uses a static median SH codebook per identity record.
        qInfo("DN1 sampled maximum errors: position=%g m, DC=%g, SH=%g (SH RMS=%g)",positionError,colourError,shError,shRms);QVERIFY(std::sqrt(positionSquared/baseSamples)<.0005);QVERIFY(std::sqrt(colourSquared/baseSamples)<.003);QVERIFY(shRms<.02);
        Project project;project.asset=path;project.plySequence=source.spec();project.modifiers.clear();project.out=6/30.;QTemporaryDir dir;
        exportCaptureFile(project,dir.filePath("dn1.pgs"));auto capture=vgsdec::Capture::openFile(dir.filePath("dn1.pgs").toStdString());QCOMPARE(capture.frameRate(),30.);
        for (int frame:{0,3,6}) QCOMPARE(capture.setTime(frame/30.,false).activeCount,quint64(alive(source.frame(frame/30.,false))));
    }
    void realForgeTakeFromPublishedFrames() {
        const auto path=qEnvironmentVariable("PLY_FORGE_TAKE");if (path.isEmpty()) QSKIP("Set PLY_FORGE_TAKE to a published frame of a Forge take kept with -graciakeepwork.");
        PlySequence source(path);QVERIFY(source.spec().files.first().contains("/.work/vgs_"));QVERIFY(!source.spec().births.isEmpty());
        qInfo("Forge take: %d frames, %d chunks",source.frames(),int(source.chunks().size()));
        Project project;project.asset=path;project.plySequence=source.spec();project.modifiers.clear();project.out=(source.frames()-1)/source.fps();QTemporaryDir dir;
        const auto requested=qEnvironmentVariable("PLY_FORGE_ROUNDTRIP");const auto output=requested.isEmpty() ? dir.filePath("take.vgs") : requested;
        const auto result=exportCaptureFile(project,output);for (const auto &note:result.notes) qInfo("%s",qPrintable(note));
        qInfo("Forge take bytes: %lld",QFileInfo(output).size());
        const auto fit=result.notes.filter("PLY fit");QCOMPARE(fit.size(),1);QVERIFY(fit.first().contains(" 0 visible PLY splats not shown, 0 shown where"));
    }
    void realRd2ComparedWithNativeReferenceAndWithoutSidecars() {
        const auto folder=qEnvironmentVariable("PLY_RD2_REFERENCE");if (folder.isEmpty()) QSKIP("Set PLY_RD2_REFERENCE to the RD2 directory beside RD2.vgs.");
        PlySequence source(QDir(folder).filePath("00000045.ply"));QCOMPARE(source.frames(),10);QCOMPARE(source.degree(),1);QCOMPARE(source.chunks().size(),1);
        auto reference=vgsdec::Capture::openFile((folder+".vgs").toStdString());QCOMPARE(reference.frameCount(),uint64_t(10));QCOMPARE(reference.frameRate(),30.);
        qInfo("RD2 PLY bounds: [%g,%g,%g]..[%g,%g,%g]; native: [%g,%g,%g]..[%g,%g,%g]",
            source.bounds()[0],source.bounds()[1],source.bounds()[2],source.bounds()[3],source.bounds()[4],source.bounds()[5],
            reference.bounds()[0],reference.bounds()[1],reference.bounds()[2],reference.bounds()[3],reference.bounds()[4],reference.bounds()[5]);
        auto independent=source.spec();independent.births.clear();independent.chunks.clear();PlySequence withoutSidecars(independent.files.first(),independent);
        qInfo("RD2: %d source chunks, %zu native chunks, %d chunks without sidecars",source.chunks().size(),reference.chunkCount(),withoutSidecars.chunks().size());
        for (int frame:{0,4,9}) {
            const auto raw=source.sourceFrame(frame,false),imported=source.frame(frame/30.,false),fallback=withoutSidecars.frame(frame/30.,false);
            const auto &native=reference.setTime(frame/30.,false);QCOMPARE(alive(imported),alive(raw));QCOMPARE(alive(fallback),alive(raw));
            qInfo("RD2 frame %d: PLY/imported/no-sidecar active=%d, native active=%llu",frame,alive(raw),static_cast<unsigned long long>(native.activeCount));
            QVERIFY(std::abs(double(native.activeCount)-alive(raw))<std::max(10.,alive(raw)*.005));
            constexpr double cell=.02;std::map<std::array<int,3>,std::vector<size_t>> grid;
            auto key=[](const float *p) {return std::array<int,3>{int(std::floor(p[0]/cell)),int(std::floor(p[1]/cell)),int(std::floor(p[2]/cell))};};
            for (size_t i=0;i<native.splatCount;++i) if (native.active[i]) grid[key(native.positions+i*3)].push_back(i);
            std::vector<double> distances,scales;
            for (size_t i=0;i<raw.count;i+=127) if (raw.active[i]) {
                const auto at=key(raw.position.data()+i*3);double best=1;size_t match=0;
                for (int x=-1;x<=1;++x) for (int y=-1;y<=1;++y) for (int z=-1;z<=1;++z) {
                    const auto it=grid.find({at[0]+x,at[1]+y,at[2]+z});if (it==grid.end()) continue;
                    for (size_t j:it->second) {double d=0;for (int c=0;c<3;++c) d+=std::pow(raw.position[i*3+c]-native.positions[j*3+c],2);if (d<best) {best=d;match=j;}}
                }
                distances.push_back(std::sqrt(best));double scale=0;for (int c=0;c<3;++c) scale=std::max(scale,double(std::abs(raw.scale[i*3+c]-native.scales[match*3+c])));scales.push_back(scale);
            }
            QVERIFY(distances.size()>100);std::sort(distances.begin(),distances.end());std::sort(scales.begin(),scales.end());
            const auto p95=distances[distances.size()*95/100],medianScale=scales[scales.size()/2];
            qInfo("RD2 frame %d: nearest-reference position p95=%g m, scale median=%g m",frame,p95,medianScale);QVERIFY(p95<.02);QVERIFY(medianScale<.005);
        }
        Project project;project.asset=source.spec().files.first();project.plySequence=source.spec();project.modifiers.clear();project.out=9/30.;QTemporaryDir dir;
        const auto requested=qEnvironmentVariable("PLY_RD2_ROUNDTRIP");const auto path=requested.isEmpty() ? dir.filePath("rd2.vgs") : requested;exportCaptureFile(project,path);
        const auto encodedBytes=QFileInfo(path).size(),referenceBytes=QFileInfo(folder+".vgs").size();
        qInfo("RD2 bytes: exported=%lld, reference=%lld, ratio=%.3f",encodedBytes,referenceBytes,double(encodedBytes)/referenceBytes);
        QVERIFY(encodedBytes<referenceBytes*1.15);auto roundtrip=vgsdec::Capture::openFile(path.toStdString());
        QCOMPARE(roundtrip.frameCount(),uint64_t(10));QCOMPARE(roundtrip.shDegree(),1u);QCOMPARE(roundtrip.chunkCount(),size_t(source.chunks().size()));
        double positionError=0,dcError=0,shSquared=0,scaleSquared=0;size_t shCount=0,scaleCount=0;
        for (int frame:{0,4,9}) {
            const auto expected=source.frame(frame/30.,true);const auto &actual=roundtrip.setTime(frame/30.,true);
            QCOMPARE(actual.activeCount,uint64_t(alive(expected)));QCOMPARE(actual.splatCount,expected.count);
            // Container encoding stores records in spatial order. Match samples
            // locally by position, colour and scale rather than assuming row order.
            constexpr double cell=.005;std::map<std::array<int,3>,std::vector<size_t>> grid;
            auto key=[](const float *p) {return std::array<int,3>{int(std::floor(p[0]/cell)),int(std::floor(p[1]/cell)),int(std::floor(p[2]/cell))};};
            for (size_t i=0;i<actual.splatCount;++i) if (actual.active[i]) grid[key(actual.positions+i*3)].push_back(i);
            for (size_t i=0;i<expected.count;i+=127) if (expected.active[i]) {
                const auto at=key(expected.position.data()+i*3);double best=1e300;size_t match=0;
                for (int x=-1;x<=1;++x) for (int y=-1;y<=1;++y) for (int z=-1;z<=1;++z) {
                    const auto it=grid.find({at[0]+x,at[1]+y,at[2]+z});if (it==grid.end()) continue;
                    for (size_t j:it->second) {double d=0;for (int c=0;c<3;++c) d+=std::pow((expected.position[i*3+c]-actual.positions[j*3+c])/.001,2)+
                            std::pow((expected.colorDc[i*3+c]-actual.colors[j*3+c])/.03,2)+std::pow((expected.scale[i*3+c]-actual.scales[j*3+c])/.0005,2);
                        if (d<best) {best=d;match=j;}}
                }
                QVERIFY(best<1e300);
                for (int c=0;c<3;++c) {positionError=std::max(positionError,double(std::abs(expected.position[i*3+c]-actual.positions[match*3+c])));dcError=std::max(dcError,double(std::abs(expected.colorDc[i*3+c]-actual.colors[match*3+c])));
                    const double e=expected.scale[i*3+c]-actual.scales[match*3+c];scaleSquared+=e*e;++scaleCount;}
                for (int c=0;c<9;++c) {const double e=expected.shRest[i*9+c]-actual.sphericalHarmonics[match*9+c];shSquared+=e*e;++shCount;}
            }
        }
        const double shRms=std::sqrt(shSquared/shCount),scaleRms=std::sqrt(scaleSquared/scaleCount);
        qInfo("RD2 VGS export: sampled max position=%g m, max DC=%g, SH RMS=%g, scale RMS=%g m",positionError,dcError,shRms,scaleRms);
        // The reference vgsplyseq log reports higher-SH RMS 0.0237: its compact
        // representation holds the per-record median rather than temporal SH.
        QVERIFY(positionError<.002);QVERIFY(dcError<.03);QVERIFY(shRms<.03);QVERIFY(scaleRms<.0005);
    }
};
QTEST_MAIN(PlySequenceTests)
#include "plysequence_tests.moc"
