// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.

#include "captureworker.h"
#include "viewport.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Sequential preview with full SH, including chunk boundaries. Each pass plays
// the same frames so cache misses and hits can be measured without frame skipping.
int main(int argc, char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setSamples(4);format.setSwapInterval(0);QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    if (argc<2) {std::fprintf(stderr,"Usage: playback_bench capture-or-project [frames=120] [start=project-in|morph] [passes=1]\n");return 2;}
    CaptureWorker worker;CaptureInfo info;FramePtr frame;QString error;
    QObject::connect(&worker,&CaptureWorker::opened,[&](CaptureInfo i,FramePtr f,quint64) {info=i;frame=std::move(f);});
    QObject::connect(&worker,&CaptureWorker::decoded,[&](FramePtr f,quint64) {frame=std::move(f);});
    QObject::connect(&worker,&CaptureWorker::failed,[&](QString e,quint64,bool) {error=e;});
    const QString path=QString::fromLocal8Bit(argv[1]);const bool isProject=QFileInfo(path).suffix()=="vgsproj";
    Project project;project.modifiers.clear();
    if (isProject && !Project::read(path,&project,&error)) {std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
    if (qEnvironmentVariableIsSet("PLAYBACK_DESPILL_ALWAYS")) {
        Modifier colour;colour.type=ModifierType::Colour;colour.colourDespill=Modifier::DespillAlways;
        bool changed=false;
        for (auto &m:project.modifiers) if (m.type==ModifierType::Colour) {m.colourDespill=Modifier::DespillAlways;m.colourRecoverSkin=true;changed=true;}
        if (!changed) project.modifiers.append(colour);
    }
    if (qEnvironmentVariableIsSet("PLAYBACK_COLOUR_CACHE_BYTES")) worker.setColourCacheBudget(size_t(qEnvironmentVariable("PLAYBACK_COLOUR_CACHE_BYTES").toULongLong()));
    worker.open(isProject ? project.asset : path,1,true);
    if (!error.isEmpty()) {std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
    Viewport viewport;viewport.resize(1280,720);viewport.setGrid(false);viewport.setFrontMarker(false);viewport.setSplatRendering(true);viewport.setSplatAntialiasing(info.antialiased);if (isProject) viewport.setCamera(project.camera);else viewport.fit(info.minimum,info.maximum);viewport.setFrame(frame);viewport.show();app.processEvents();viewport.repaint();
    if (!isProject) project.out=(info.frames-1)/info.fps;
    project.previewPlaying=true;
    if (qEnvironmentVariableIsSet("PLAYBACK_CPU_SH")) project.previewPlaying=false;
#ifdef EDITOR_PLAYBACK_TIMINGS
    project.previewRevision=1;
#endif
    const int count=argc>2 ? std::max(1,std::atoi(argv[2])) : 120;
    const double start=argc>3 ? (QString::fromLocal8Bit(argv[3])=="morph" ? project.morphStart(info.fps) : std::atof(argv[3])) : project.in;
    const int passes=argc>4 ? std::max(1,std::atoi(argv[4])) : 1;
    const bool verify=qEnvironmentVariableIsSet("PLAYBACK_VERIFY");
    for (int pass=0;pass<passes;++pass) {
    std::vector<double> decode,render,total,source,assembly,upload,pack,sort,gpuPrepare,gpuDraw;
    int cacheHits=0,shapeHits=0,colourHits=0;
#ifdef EDITOR_PLAYBACK_TIMINGS
    const size_t isolationBefore=worker.isolationEvaluations();
    const size_t despillBefore=worker.despillEvaluations();
#endif
    QCryptographicHash recordsHash(QCryptographicHash::Sha256),imageHash(QCryptographicHash::Sha256);
    for (int i=0;i<count;++i) {
        const double time=start+i/info.fps;if (time>project.timelineEnd(info.fps)+1e-6) break;
        const bool morph=project.inMorph(time,info.fps);const int pose=int(std::lround((morph ? project.out : time)*info.fps));
        viewport.setTransform(project.transformAtFrame(pose));
        viewport.setModifiers(morph ? project.modifiersBelowMorph(pose) : project.modifiersAtFrame(pose));
        QElapsedTimer clock;clock.start();worker.decode(time,1,true,project);
        if (!error.isEmpty()) {std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
        const double decoded=clock.nsecsElapsed()/1e6;
        // grabFramebuffer forces a paint even if Qt would coalesce update/repaint
        // requests, and waits for the GPU. It also includes a small readback cost.
        viewport.setFrame(frame);const auto rendered=viewport.grabFramebuffer();
        if (rendered.isNull()) {std::fprintf(stderr,"Viewport did not render\n");return 1;}
        const double elapsed=clock.nsecsElapsed()/1e6;
        decode.push_back(decoded);render.push_back(elapsed-decoded);total.push_back(elapsed);
#ifdef EDITOR_PLAYBACK_TIMINGS
        cacheHits+=frame->visibilityFromCache;shapeHits+=frame->morphShapeFromCache;colourHits+=frame->colourFromCache;source.push_back(frame->sourceMs);assembly.push_back(frame->assemblyMs);
        const auto timings=viewport.frameTimings();upload.push_back(timings.upload);pack.push_back(timings.pack);sort.push_back(timings.sort);
        gpuPrepare.push_back(timings.gpuPrepare);gpuDraw.push_back(timings.gpuDraw);
#endif
        if (verify) {
            recordsHash.addData(QByteArrayView(reinterpret_cast<const char *>(frame->records.data()),qsizetype(frame->records.size()*sizeof(Splat))));
            recordsHash.addData(QByteArrayView(reinterpret_cast<const char *>(frame->active.data()),qsizetype(frame->active.size())));
            recordsHash.addData(QByteArrayView(reinterpret_cast<const char *>(frame->sh.data()),qsizetype(frame->sh.size()*sizeof(float))));
            imageHash.addData(QByteArrayView(reinterpret_cast<const char *>(rendered.constBits()),rendered.sizeInBytes()));
            if (i==count-1 && qEnvironmentVariableIsSet("PLAYBACK_IMAGE")) rendered.save(qEnvironmentVariable("PLAYBACK_IMAGE"));
        }
    }
    if (total.empty()) {std::fprintf(stderr,"No frames in the requested range\n");return 2;}
    auto report=[](const char *label,std::vector<double> samples) {
        const double mean=std::accumulate(samples.begin(),samples.end(),0.0)/samples.size();std::sort(samples.begin(),samples.end());
        std::printf("%s mean=%.3f ms p50=%.3f p95=%.3f max=%.3f\n",label,mean,samples[samples.size()/2],samples[size_t((samples.size()-1)*.95)],samples.back());return mean;
    };
    std::printf("pass=%d %s frames=%zu records=%llu points=%zu SH=%d viewport=%dx%d pixels\n",pass+1,qPrintable(info.format),total.size(),static_cast<unsigned long long>(frame->total),frame->points.size(),frame->coefficients,int(viewport.width()*viewport.devicePixelRatioF()),int(viewport.height()*viewport.devicePixelRatioF()));
#ifdef EDITOR_PLAYBACK_TIMINGS
    std::printf("visibility_hits=%d shape_hits=%d isolation_evaluations=%zu visibility_entries=%zu cache_bytes=%zu\n",cacheHits,shapeHits,worker.isolationEvaluations()-isolationBefore,worker.visibilityFrames(),worker.visibilityBytes());
    std::printf("colour_hits=%d despill_evaluations=%zu colour_entries=%zu\n",colourHits,worker.despillEvaluations()-despillBefore,worker.colourCacheFrames());
    std::printf("retained_MiB visibility=%.2f morph_shapes=%.2f reusable_buffers=%.2f despill_skin=%.2f\n",worker.visibilityBytes()/1048576.,worker.morphCacheBytes()/1048576.,worker.reusableFrameBytes()/1048576.,worker.colourCacheBytes()/1048576.);
    report("source",source);report("assembly",assembly);report("upload",upload);report("pack",pack);report("sort",sort);
    if (qEnvironmentVariableIsSet("PLAYBACK_GPU_TIMING")) {report("GPU prepare",gpuPrepare);report("GPU draw",gpuDraw);}
#endif
    report("worker",decode);report("render+upload+sort+GPU+readback",render);
    const double mean=report("total",total);std::printf("throughput=%.2f fps\n",1000/mean);
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX memory={};memory.cb=sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)))
        std::printf("process_MiB private=%.2f working_set=%.2f peak_working_set=%.2f\n",memory.PrivateUsage/1048576.,memory.WorkingSetSize/1048576.,memory.PeakWorkingSetSize/1048576.);
#endif
    if (verify) std::printf("records_sha256=%s\nimage_sha256=%s\n",recordsHash.result().toHex().constData(),imageHash.result().toHex().constData());
    std::fflush(stdout);
    }
}
