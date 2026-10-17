// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See ../LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "captureworker.h"
#include "project.h"
#include "filehistory.h"
#include "presetstore.h"
#include "isolation.h"
#include "pruning.h"
#include "displayscaling.h"
#include "loopmorph.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <random>
#include <set>

namespace {
// A rod of splats along `axis` from `from`, `length` long and `radius` thick, as a body's
// limb would be: points on its surface, small round splats, mid grey.
void addRod(loopmorph::Cloud &cloud,QVector3D from,QVector3D axis,float length,float radius,int count,std::mt19937 &random) {
    std::uniform_real_distribution<float> unit(0,1);axis.normalize();
    QVector3D side=QVector3D::crossProduct(axis,{0,0,1});if (side.length()<.1f) side=QVector3D::crossProduct(axis,{0,1,0});side.normalize();
    const QVector3D up=QVector3D::crossProduct(axis,side);
    for (int i=0;i<count;++i) {
        const float along=unit(random)*length,angle=unit(random)*6.2831853f;const QVector3D p=from+axis*along+(side*std::cos(angle)+up*std::sin(angle))*radius;
        cloud.position.insert(cloud.position.end(),{p.x(),p.y(),p.z()});cloud.rotation.insert(cloud.rotation.end(),{0,0,0,1});
        cloud.scale.insert(cloud.scale.end(),{.01f,.01f,.01f});cloud.colour.insert(cloud.colour.end(),{.5f,.5f,.5f});cloud.opacity.push_back(.9f);
    }
}
QVector3D splatAt(const loopmorph::Cloud &c,size_t i) {return {c.position[3*i],c.position[3*i+1],c.position[3*i+2]};}
size_t nearestSplat(const loopmorph::Cloud &c,size_t count,QVector3D to) {
    size_t best=0;float distance=1e30f;for (size_t i=0;i<count;++i) if (const float d=(splatAt(c,i)-to).lengthSquared();d<distance) {distance=d;best=i;}return best;
}
}

class EditorTests : public QObject {
    Q_OBJECT
private slots:
    void previewCacheDependenciesFollowTheProcessingStages() {
        Project project;project.modifiers.clear();project.in=0;project.out=1;
        Modifier crop;crop.type=ModifierType::Crop;crop.crop.enabled=true;
        Modifier purge;purge.type=ModifierType::PurgeIsolated;
        Modifier morph;morph.type=ModifierType::LoopMorph;morph.morphSeconds=.2;
        Modifier colour;colour.type=ModifierType::Colour;colour.colourDespill=Modifier::DespillAlways;
        project.modifiers={crop,purge,morph,purge,colour};project.updatePreviewCacheKeys();
        const auto first=project.previewVisibilityKeys;const auto firstMorph=project.previewMorphKey,firstColour=project.previewColourKey;
        project.modifiers[2].morphSeconds=.4;project.updatePreviewCacheKeys();
        QCOMPARE(project.previewVisibilityKeys[0],first[0]);QCOMPARE(project.previewVisibilityKeys[1],first[1]);
        QVERIFY(project.previewMorphKey!=firstMorph);QVERIFY(project.previewVisibilityKeys[2]!=first[2]);QCOMPARE(project.previewColourKey,firstColour);
        const auto second=project.previewVisibilityKeys;const auto secondMorph=project.previewMorphKey;
        project.modifiers[3].isolation.medianPercent+=50;project.updatePreviewCacheKeys();
        QVERIFY(project.previewVisibilityKeys[0]!=second[0]);QCOMPARE(project.previewVisibilityKeys[1],second[1]);QCOMPARE(project.previewMorphKey,secondMorph);QVERIFY(project.previewVisibilityKeys[2]!=second[2]);
        const auto third=project.previewVisibilityKeys;project.modifiers[4].colourDespillStrength=.5;project.updatePreviewCacheKeys();
        // Without a Remove green below Color, despill does not affect visibility.
        QCOMPARE(project.previewVisibilityKeys[0],third[0]);QCOMPARE(project.previewVisibilityKeys[1],third[1]);QVERIFY(project.previewColourKey!=firstColour);
        const auto colourMorph=project.previewMorphKey;project.modifiers[0].crop.radius+=.1;project.updatePreviewCacheKeys();
        QVERIFY(project.previewVisibilityKeys[0]!=third[0]);QVERIFY(project.previewVisibilityKeys[1]!=third[1]);QVERIFY(project.previewMorphKey!=colourMorph);
        const auto clean=project.previewVisibilityKeys;const auto cleanMorph=project.previewMorphKey;
        project.modifiers[0].name="Renamed";project.selectedModifier=project.modifiers[4].id;project.previewCacheEnabled=false;project.updatePreviewCacheKeys();
        QVERIFY(project.previewVisibilityKeys==clean);QCOMPARE(project.previewMorphKey,cleanMorph);
    }
    void previewMemoryReservesPhysicalAndCommitHeadroom() {
        using namespace previewmemory;
        Snapshot memory{32*GiB,12*GiB,64*GiB,20*GiB,true,false};
        Guard guard([&] {return memory;});
        QCOMPARE(guard.inspect().growth,12*GiB-32*GiB/10);
        QVERIFY(guard.canRetain(GiB));
        memory.physicalAvailable=3*GiB;
        auto pressure=guard.inspect();QVERIFY(pressure.paused);QVERIFY(pressure.reclaim>0);
        QVERIFY(!guard.canRetain(1));
        memory.physicalAvailable=32*GiB/10+100*MiB; // Still inside hysteresis.
        QVERIFY(guard.inspect().paused);
        memory.physicalAvailable=5*GiB;QVERIFY(!guard.inspect().paused);
        memory.commitAvailable=GiB;QVERIFY(guard.inspect().paused);
        memory.commitAvailable=8*GiB;QVERIFY(!guard.inspect().paused);
        memory.lowMemory=true;QVERIFY(guard.inspect().reclaim>=256*MiB);QVERIFY(!guard.canRetain(0));
        memory.lowMemory=false;QVERIFY(guard.canRetain(MiB));
        memory.valid=false;QVERIFY(!guard.canRetain(1));QCOMPARE(guard.inspect().reclaim,uint64_t(0));
        // Small machines still leave at least 2 GiB physically available.
        memory={8*GiB,3*GiB,16*GiB,4*GiB,true,false};
        Guard small([&] {return memory;});QCOMPARE(small.inspect().growth,GiB);
        memory.physicalAvailable=GiB;QVERIFY(small.inspect().paused);
    }
    void loopMorphGpuShMatchesCpuAndRespectsTheBudget() {
        for (int terms:{3,8,15}) {
            std::mt19937 random(17);loopmorph::Cloud end,start;
            addRod(end,{0,0,0},{0,1,0},1,.1f,256,random);addRod(start,{.02f,0,0},{0,1,0},1,.1f,256,random);
            end.shCoefficients=start.shCoefficients=terms;end.sh.resize(end.size()*size_t(terms)*3);start.sh.resize(start.size()*size_t(terms)*3);
            for (size_t i=0;i<end.sh.size();++i) {end.sh[i]=.1f*std::sin(float(i));start.sh[i]=.12f*std::cos(float(i));}
            loopmorph::Morph morph(end,start,{});morph.setCaching(true,1024*1024);
            const auto cpu=morph.evaluate(.37);loopmorph::Cloud gpu;loopmorph::ShBlend blend;
            QVERIFY(morph.evaluatePreviewInto(.37,&gpu,&blend));QVERIFY(gpu.sh.empty());
            QVERIFY(gpu.position==cpu.position && gpu.rotation==cpu.rotation && gpu.opacity==cpu.opacity && gpu.colour==cpu.colour);
            QCOMPARE(blend.source->size(),cpu.size());QCOMPARE(blend.partner->size(),cpu.size());
            for (size_t i=0;i<cpu.size();++i) {
                const size_t own=(*blend.source)[i];const bool fromEnd=own<blend.sources->endCount;
                const size_t other=(*blend.partner)[i]+(fromEnd ? blend.sources->endCount : 0);
                const float weight=fromEnd ? blend.fraction : 1-blend.fraction;
                for (int k=0;k<terms*3;++k) {
                    const float value=(1-weight)*blend.sources->values[own*size_t(terms)*3+size_t(k)]+weight*blend.sources->values[other*size_t(terms)*3+size_t(k)];
                    QVERIFY(std::abs(value-cpu.sh[i*size_t(terms)*3+size_t(k)])<1e-6f);
                }
            }
            loopmorph::ShBlend next;QVERIFY(morph.evaluatePreviewInto(.61,&gpu,&next));QCOMPARE(next.sources,blend.sources);QVERIFY(morph.cachedBytes()<=1024*1024);
            morph.trimCache(0);QCOMPARE(morph.cachedBytes(),size_t(0));
            morph.setCacheAdmission([](size_t) {return false;});
            QVERIFY(!morph.evaluatePreviewInto(.37,&gpu,&next));
            const auto uncached=morph.evaluate(.37);QVERIFY(uncached.sh==cpu.sh && uncached.position==cpu.position);
            QCOMPARE(morph.cachedBytes(),size_t(0));
            morph.setCacheAdmission({});QVERIFY(morph.evaluatePreviewInto(.37,&gpu,&next));
            morph.setCaching(true,1);QVERIFY(!morph.evaluatePreviewInto(.37,&gpu,&next));QCOMPARE(morph.cachedBytes(),size_t(0));
            // Published frames keep their immutable source/shape arrays across invalidation.
            QCOMPARE(blend.sources->values.front(),end.sh.front());QCOMPARE(blend.source->size(),cpu.size());
        }
    }
    void loopMorphSplitsTheStackAndPersists() {
        Project project;project.modifiers.clear();project.in=0;project.out=2;
        auto add=[&](ModifierType type,const QString &name) {Modifier m;m.id=Project::newId();m.name=name;m.type=type;m.crop.enabled=true;project.modifiers.append(m);return m.id;};
        add(ModifierType::Crop,"Crop");add(ModifierType::Colour,"Colour");add(ModifierType::LoopMorph,"Morph");add(ModifierType::RemoveGreen,"Green");add(ModifierType::Walk,"Walk");
        auto names=[](const QVector<Modifier> &stack) {QStringList n;for (const auto &m:stack) n << m.name;return n.join(',');};
        // Removing kinds above it shape the frames it morphs, those below apply to its frames;
        // every other kind applies both ways, and the morph itself to neither.
        QCOMPARE(names(project.modifiersAboveMorph(0)),QString("Crop,Colour,Walk"));
        QCOMPARE(names(project.modifiersBelowMorph(0)),QString("Colour,Green,Walk"));
        auto &morph=project.modifiers[2];morph.morphSeconds=.4;
        QCOMPARE(project.morphFrames(25),10);QVERIFY(std::abs(project.playbackEnd(25)-2.4)<1e-9);
        double fraction=0;QVERIFY(project.inMorph(2.04,25,&fraction));QVERIFY(std::abs(fraction-1/11.)<1e-9);QVERIFY(!project.inMorph(2,25));QVERIFY(!project.inMorph(2.44,25));
        // Markers pair up only on the current End and Start frames.
        MorphMarker both;both.number=1;both.hasEnd=both.hasStart=true;both.end={1,2,3};both.start={4,5,6};both.endFrame=50;both.startFrame=0;
        MorphMarker endOnly;endOnly.number=2;endOnly.hasEnd=true;endOnly.end={.5f,0,0};endOnly.endFrame=50;
        MorphMarker moved=both;moved.number=3;moved.endFrame=40;
        morph.morphMarkers={both,endOnly,moved};
        QCOMPARE(project.morphPairs(25).size(),1);QCOMPARE(project.morphPairs(25)[0].end,QVector3D(1,2,3));QCOMPARE(project.morphPairs(25)[0].number,1);
        // Saved and read back, with the playhead on one of its frames.
        project.time=2.2;QTemporaryDir dir;project.asset=dir.filePath("source.vgs");const auto json=project.json(dir.filePath("scene.vgsproj"));
        Project restored;QString error;QVERIFY2(Project::fromJson(json,dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifierJson(),project.modifierJson());QCOMPARE(restored.modifiers[2].morphMarkers.size(),3);QVERIFY(!restored.modifiers[2].morphMarkers[1].hasStart);
        // Two markers with one number are refused.
        auto invalid=json;auto mods=invalid["modifiers"].toArray();auto m=mods[2].toObject();auto data=m["morph"].toObject();auto markers=data["markers"].toArray();
        markers.append(markers[0]);data["markers"]=markers;m["morph"]=data;mods[2]=m;invalid["modifiers"]=mods;QVERIFY(!Project::fromJson(invalid,dir.path(),&restored,&error));
        // Disabled, it adds nothing and splits nothing.
        project.modifiers[2].enabled=false;QCOMPARE(project.morphFrames(25),0);QCOMPARE(names(project.modifiersAboveMorph(0)),QString("Crop,Colour,Green,Walk"));
    }
    void loopMorphSuggestsPairsAndFlagsAMispairedOne() {
        // Start turned 30 degrees about Y and moved: from four pairs, where a fifth point's pair
        // is follows the turn, and nearby pairs' own misfit is carried along.
        QMatrix4x4 motion;motion.translate(.3f,0,-.1f);motion.rotate(30,0,1,0);
        std::vector<loopmorph::Pair> pairs;
        for (QVector3D p:{QVector3D(0,0,0),QVector3D(0,1.6f,0),QVector3D(.4f,1.3f,0),QVector3D(-.4f,1.3f,.1f)}) pairs.push_back({motion.map(p),p});
        QVector3D guess;QVERIFY(loopmorph::predict(pairs,{.2f,1,0},true,&guess));QVERIFY((guess-motion.map(QVector3D(.2f,1,0))).length()<1e-4f);
        QVERIFY(loopmorph::predict(pairs,motion.map(QVector3D(.2f,1,0)),false,&guess));QVERIFY((guess-QVector3D(.2f,1,0)).length()<1e-4f);
        // A hand raised 10 cm at End, beyond the rigid motion: a point beside it follows it.
        pairs[2].end+=QVector3D(0,.1f,0);QVERIFY(loopmorph::predict(pairs,{.42f,1.3f,0},true,&guess));
        QVERIFY((guess-motion.map(QVector3D(.42f,1.3f,0))-QVector3D(0,.1f,0)).length()<.02f);
        pairs[2].end-=QVector3D(0,.1f,0);
        // Fewer than three, or three in a line, say nothing.
        QVERIFY(!loopmorph::predict({pairs[0],pairs[1]},{0,0,0},true,&guess));
        QVERIFY(!loopmorph::predict({{{0,0,0},{0,0,0}},{{0,1,0},{0,1,0}},{{0,2,0},{0,2,0}}},{1,0,0},true,&guess));
        // A fifth pair clicked on the wrong point stands out; the others agree to the millimetre.
        pairs.push_back({motion.map(QVector3D(0,1,.1f))+QVector3D(.25f,0,0),{0,1,.1f}});
        const auto off=loopmorph::deviations(pairs);QCOMPARE(off.size(),size_t(5));
        QVERIFY2(off[4]>.15f,qPrintable(QString::number(off[4])));
        for (int i=0;i<4;++i) QVERIFY2(off[size_t(i)]<off[4]/2,qPrintable(QString("%1: %2").arg(i).arg(off[size_t(i)])));
        // With three pairs nothing is left over to judge one by.
        QVERIFY(std::isnan(loopmorph::deviations({pairs[0],pairs[1],pairs[2]})[0]));
    }
    void loopMorphMovesARigidSubjectRigidly() {
        // The start frame is the end frame turned 40 degrees about Y and moved half a metre,
        // its own splats drawn afresh: no splat is the same one in both.
        std::mt19937 random(7);loopmorph::Cloud end,start;
        addRod(end,{0,0,0},{0,1,0},1.6f,.15f,6000,random);addRod(end,{-.4f,1.3f,0},{1,0,0},.8f,.05f,2000,random);
        QMatrix4x4 motion;motion.translate(.5f,0,.2f);motion.rotate(40,0,1,0);
        addRod(start,motion.map({0,0,0}),motion.mapVector({0,1,0}),1.6f,.15f,6000,random);addRod(start,motion.map({-.4f,1.3f,0}),motion.mapVector({1,0,0}),.8f,.05f,2000,random);
        std::vector<loopmorph::Pair> pairs;for (QVector3D p:{QVector3D(0,0,.15f),QVector3D(0,1.6f,.15f),QVector3D(-.4f,1.3f,.05f),QVector3D(.4f,1.3f,.05f)}) pairs.push_back({p,motion.map(p)});
        loopmorph::Morph morph(end,start,pairs);
        QVERIFY2(morph.markerError()<.02,qPrintable(QString::number(morph.markerError())));
        // At the ends each frame is itself, the other faded out entirely.
        const auto first=morph.evaluate(0),last=morph.evaluate(1);
        QCOMPARE(first.size(),end.size());QCOMPARE(last.size(),start.size());
        for (size_t i=0;i<end.size();i+=97) QVERIFY((splatAt(first,i)-splatAt(end,i)).length()<1e-4f);
        for (size_t i=0;i<start.size();i+=97) QVERIFY((splatAt(last,i)-splatAt(start,i)).length()<1e-4f);
        // Halfway the end frame's splats are the subject, rigid: the arm's tips as far apart.
        std::vector<uint32_t> from;const auto middle=morph.evaluate(.5,&from);QCOMPARE(from.size(),middle.size());
        QCOMPARE(middle.size(),end.size()+start.size()); // halfway both frames show, each in part
        morph.setCaching(true);const auto cached=morph.evaluate(.5);const size_t bytes=morph.cachedBytes();
        QVERIFY(bytes>0);QCOMPARE(morph.cachedFrames(),size_t(1));QVERIFY(cached.position==middle.position && cached.sh==middle.sh);
        morph.setCaching(true,bytes);morph.evaluate(.5);morph.evaluate(.6);
        QVERIFY(morph.cachedBytes()<=bytes);QVERIFY(morph.cachedFrames()<=1);
        const auto again=morph.evaluate(.5);QVERIFY(again.position==middle.position && again.rotation==middle.rotation && again.opacity==middle.opacity);
        morph.setCaching(true,1);morph.evaluate(.5);QCOMPARE(morph.cachedBytes(),size_t(0));QCOMPARE(morph.cachedFrames(),size_t(0));
        morph.setCaching(false);
        loopmorph::Cloud reused;morph.evaluateInto(.5,{},&reused);
        QVERIFY(reused.position==middle.position && reused.sh==middle.sh && reused.colour==middle.colour);
        morph.evaluateInto(.6,{},&reused);morph.evaluateInto(.5,{},&reused);
        QVERIFY(reused.position==middle.position && reused.rotation==middle.rotation && reused.opacity==middle.opacity);
        std::vector<QVector3D> markers;for (const auto &pair:pairs) markers.push_back(pair.end);
        const auto carried=morph.carry(markers,.5);
        for (size_t i=0;i<markers.size();++i) QCOMPARE(carried[i],morph.carry(markers[i],.5));
        // Where splat `i` of the End frame is in the morph's frame; the arm's tips show halfway.
        auto in=[&](const std::vector<uint32_t> &f,size_t i) {return size_t(std::find(f.begin(),f.end(),uint32_t(i))-f.begin());};
        const size_t left=nearestSplat(end,end.size(),{-.4f,1.3f,.05f}),right=nearestSplat(end,end.size(),{.4f,1.3f,.05f});
        // Pick tips that still show halfway: the change-over noise may have hidden the nearest.
        auto shown=[&](size_t i) {return in(from,i)<from.size();};
        size_t l=left,r=right;for (size_t i=0;i<end.size() && !shown(l);++i) if (shown(i) && (splatAt(end,i)-splatAt(end,left)).length()<.08f) l=i;
        for (size_t i=0;i<end.size() && !shown(r);++i) if (shown(i) && (splatAt(end,i)-splatAt(end,right)).length()<.08f) r=i;
        QVERIFY(shown(l) && shown(r));
        const float span=(splatAt(end,l)-splatAt(end,r)).length(),now=(splatAt(middle,in(from,l))-splatAt(middle,in(from,r))).length();
        QVERIFY2(std::abs(now-span)<.03f*span,qPrintable(QString("%1 vs %2").arg(now).arg(span)));
        // Turned about halfway: the arm's direction 20 degrees from where it was.
        const QVector3D was=(splatAt(end,r)-splatAt(end,l)).normalized(),is=(splatAt(middle,in(from,r))-splatAt(middle,in(from,l))).normalized();
        const float turned=qRadiansToDegrees(std::acos(std::clamp(QVector3D::dotProduct(was,is),-1.f,1.f)));
        QVERIFY2(std::abs(turned-20)<5,qPrintable(QString::number(turned)));
        // The End frame hands its optical depth to the Start frame between 0.2 and 0.8 of the
        // way: before, only End shows, as it is; after, only Start; and in between, a splat of
        // each lying on one another stops as much light as either alone - not less, which would
        // let what is inside show through and read paler.
        for (float alpha:{.05f,.4f,.9f}) {
            QCOMPARE(loopmorph::endOpacity(0,alpha),alpha);QCOMPARE(loopmorph::startOpacity(0,alpha),0.f);
            QVERIFY(std::abs(loopmorph::endOpacity(1,alpha))<1e-6f);QVERIFY(std::abs(loopmorph::startOpacity(1,alpha)-alpha)<1e-6f);
            for (double s:{.3,.5,.7}) {
                const float together=1-(1-loopmorph::endOpacity(s,alpha))*(1-loopmorph::startOpacity(s,alpha));
                QVERIFY2(std::abs(together-alpha)<1e-5f,qPrintable(QString("alpha %1 at %2: %3").arg(alpha).arg(s).arg(together)));
            }
        }
    }
    void loopMorphSwingsABendingArmRoundTheElbow() {
        // An arm that bends 90 degrees at the elbow between the frames.
        std::mt19937 random(3);loopmorph::Cloud end,start;
        addRod(end,{0,0,0},{1,0,0},.5f,.05f,3000,random);addRod(end,{.5f,0,0},{1,0,0},.5f,.05f,3000,random);
        addRod(start,{0,0,0},{1,0,0},.5f,.05f,3000,random);addRod(start,{.5f,0,0},{0,1,0},.5f,.05f,3000,random);
        const std::vector<loopmorph::Pair> pairs{{{0,0,.05f},{0,0,.05f}},{{.5f,0,.05f},{.5f,0,.05f}},{{1,0,.05f},{.5f,.5f,.05f}},{{.75f,0,.05f},{.5f,.25f,.05f}}};
        const loopmorph::Morph morph(end,start,pairs);
        QVERIFY2(morph.markerError()<.03,qPrintable(QString::number(morph.markerError())));
        // The hand keeps its distance from the elbow all the way: it swings round on an arc
        // rather than cutting the corner, where it would come within 0.35 m.
        // The splats of the hand (the last 3 cm of the arm) and of the elbow in both frames, and
        // where those that show are on average: whichever frame they come from.
        std::set<uint32_t> hand,elbow;
        for (size_t i=0;i<end.size();++i) {if ((splatAt(end,i)-QVector3D(1,0,0)).length()<.06f) hand.insert(uint32_t(i));if ((splatAt(end,i)-QVector3D(.5f,0,0)).length()<.06f) elbow.insert(uint32_t(i));}
        for (size_t j=0;j<start.size();++j) {if ((splatAt(start,j)-QVector3D(.5f,.5f,0)).length()<.06f) hand.insert(uint32_t(end.size()+j));if ((splatAt(start,j)-QVector3D(.5f,0,0)).length()<.06f) elbow.insert(uint32_t(end.size()+j));}
        auto mean=[](const loopmorph::Cloud &c,const std::vector<uint32_t> &from,const std::set<uint32_t> &which) {
            QVector3D sum;int n=0;for (size_t i=0;i<from.size();++i) if (which.count(from[i])) {sum+=splatAt(c,i);++n;}return n ? sum/float(n) : QVector3D(1e9f,0,0);};
        for (double s:{.25,.5,.75}) {
            std::vector<uint32_t> from;const auto frame=morph.evaluate(s,&from);const float reach=(mean(frame,from,hand)-mean(frame,from,elbow)).length();
            QVERIFY2(reach>.42f && reach<.56f,qPrintable(QString("s=%1: %2").arg(s).arg(reach)));
        }
        // And it ends where the start frame's hand is.
        std::vector<uint32_t> from;const auto nearlyThere=morph.evaluate(.98,&from);const QVector3D there=mean(nearlyThere,from,hand);
        QVERIFY2((there-QVector3D(.5f,.5f,0)).length()<.08f,qPrintable(QString::number((there-QVector3D(.5f,.5f,0)).length())));
    }
    void loopMorphOverlapsTheRangesEnd() {
        Project project;project.modifiers.clear();project.in=1;project.out=2;
        Modifier morph;morph.id=Project::newId();morph.name="Morph";morph.type=ModifierType::LoopMorph;morph.morphSeconds=-.2;project.modifiers={morph};
        // Five frames before End (frame 50) are the morph's, 45 to 49: End stands for Start, so
        // playback loops after frame 49, and a seek still reaches End to place markers on it.
        QVERIFY(project.morphOverlaps());QCOMPARE(project.morphFrames(25),5);QCOMPARE(project.morphTail(25),0);
        QVERIFY(std::abs(project.playbackEnd(25)-49/25.)<1e-9);QVERIFY(std::abs(project.timelineEnd(25)-2)<1e-9);QVERIFY(std::abs(project.morphStart(25)-45/25.)<1e-9);
        double fraction=0;int source=0;
        QVERIFY(project.inMorph(45/25.,25,&fraction,&source));QVERIFY(std::abs(fraction-1/6.)<1e-9);QCOMPARE(source,45);
        QVERIFY(project.inMorph(49/25.,25,&fraction,&source));QVERIFY(std::abs(fraction-5/6.)<1e-9);QCOMPARE(source,49);
        QVERIFY(!project.inMorph(44/25.,25));QVERIFY(!project.inMorph(2,25));
        // Leading in needs as many frames before Start (frame 25) as it is long.
        QVERIFY(!project.morphLeadsIn(25));project.modifiers[0].morphLeadIn=true;QVERIFY(project.morphLeadsIn(25));
        project.in=4/25.;QVERIFY(!project.morphLeadsIn(25));project.in=1;
        // At most all but Start's frames; none when the range has no room.
        project.modifiers[0].morphSeconds=-10;QCOMPARE(project.morphFrames(25),24);
        project.out=project.in+1/25.;QCOMPARE(project.morphFrames(25),0);QVERIFY(std::abs(project.playbackEnd(25)-project.out)<1e-9);QVERIFY(!project.inMorph(project.out,25));
        project.out=2;project.modifiers[0].morphSeconds=-.2;
        // Saved and read back with its sign and lead-in; a zero length is refused.
        QTemporaryDir dir;project.asset=dir.filePath("source.vgs");project.time=1.9;const auto json=project.json(dir.filePath("scene.vgsproj"));
        Project restored;QString error;QVERIFY2(Project::fromJson(json,dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers[0].morphSeconds,-.2);QVERIFY(restored.modifiers[0].morphLeadIn);QCOMPARE(restored.modifierJson(),project.modifierJson());
        auto invalid=json;auto mods=invalid["modifiers"].toArray();auto m=mods[0].toObject();auto data=m["morph"].toObject();
        data["seconds"]=0;m["morph"]=data;mods[0]=m;invalid["modifiers"]=mods;QVERIFY(!Project::fromJson(invalid,dir.path(),&restored,&error));
        // A project from before the option reads as not leading in.
        data["seconds"]=.5;data.remove("leadIn");m["morph"]=data;mods[0]=m;invalid["modifiers"]=mods;
        QVERIFY2(Project::fromJson(invalid,dir.path(),&restored,&error),qPrintable(error));QVERIFY(!restored.modifiers[0].morphLeadIn);
    }
    void loopMorphFollowsTheFramesBeforeEnd() {
        // A body and an arm that moves along +Z 1.5 cm a frame. End is that body; Start the same
        // shape turned 12 degrees about Y and set 5 cm back. Followed back through eight frames
        // - frames 0 to 3 before End in End's chunk, the same splats, then a chunk of other ones -
        // its nodes keep up with the body, and each frame takes on the correction as it moves.
        std::mt19937 random(5);
        auto body=[&](const QMatrix4x4 &at) {loopmorph::Cloud c;
            addRod(c,at.map({0,0,0}),at.mapVector({0,1,0}),1.6f,.15f,6000,random);addRod(c,at.map({-.4f,1.3f,0}),at.mapVector({1,0,0}),.8f,.05f,2000,random);return c;};
        const QVector3D velocity(0,0,.015f);QMatrix4x4 turn;turn.translate(0,0,-.05f);turn.rotate(12,0,1,0);
        const loopmorph::Cloud end=body({}),start=body(turn),other=body({});
        std::vector<loopmorph::Pair> pairs;for (QVector3D p:{QVector3D(0,0,.15f),QVector3D(0,1.6f,.15f),QVector3D(-.4f,1.3f,.05f),QVector3D(.4f,1.3f,.05f)}) pairs.push_back({p,turn.map(p)});
        loopmorph::Morph morph(end,start,pairs);
        auto frame=[&](const loopmorph::Cloud &from,int back,long long chunk) {
            loopmorph::Track t;t.chunk=chunk;
            for (size_t i=0;i<from.size();++i) {const QVector3D p=splatAt(from,i)-velocity*float(back);t.position.insert(t.position.end(),{p.x(),p.y(),p.z()});t.colour.insert(t.colour.end(),{.5f,.5f,.5f});t.record.push_back(uint32_t(i));}
            return t;
        };
        auto moved=[&](const loopmorph::Cloud &from,int back) {loopmorph::Cloud c=from;for (size_t i=0;i<c.size();++i) c.position[3*i+2]-=velocity.z()*float(back);return c;};
        std::vector<loopmorph::Track> back;for (int k=0;k<=8;++k) back.push_back(k<=3 ? frame(end,k,7) : frame(other,k,6));
        morph.follow(false,back);QCOMPARE(morph.followed(false),size_t(8));QCOMPARE(morph.followed(true),size_t(0));
        // Carried with no correction, a point of End is where the body was that many frames back,
        // across the chunk change too; with it, the correction rides along with the body.
        const QVector3D probes[]={{0,.3f,.15f},{0,1.5f,-.15f},{-.4f,1.3f,.05f},{.35f,1.3f,-.05f}};
        for (int k:{1,3,4,8}) for (const auto &p:probes) {
            const float still=(morph.carry(p,0,size_t(k))-(p-velocity*float(k))).length();
            QVERIFY2(still<.01f,qPrintable(QString("%1 frames back: %2 m off").arg(k).arg(still)));
            for (double s:{.3,.7}) {
                const float carried=(morph.carry(p,s,size_t(k))-(morph.carry(p,s)-velocity*float(k))).length();
                QVERIFY2(carried<.01f,qPrintable(QString("%1 frames back at %2: %3 m off").arg(k).arg(s).arg(carried)));
            }
        }
        // At 0 the frame shown is the capture's own, untouched.
        const auto live=moved(other,5);loopmorph::Morph::Live at;at.back=5;at.end=&live;
        std::vector<uint32_t> from;const auto still=morph.evaluate(0,at,&from);QCOMPARE(still.size(),live.size());
        for (size_t i=0;i<still.size();i+=53) QVERIFY((splatAt(still,i)-splatAt(live,from[i])).length()<1e-3f);
        // Further on, the frame's own splats move as the End frame's would there: skinned to the
        // nodes where they have been followed rather than where they were, they differ only at
        // the ends of a limb, where the fit is least rigid.
        const auto later=morph.evaluate(.5,at,&from);std::vector<float> off;
        for (size_t i=0;i<later.size();i+=41) if (from[i]<live.size()) off.push_back((splatAt(later,i)-(morph.carry(splatAt(live,from[i])+velocity*5.f,.5)-velocity*5.f)).length());
        std::sort(off.begin(),off.end());const float median=off[off.size()/2],high=off[off.size()*99/100];
        QVERIFY2(median<.003f && high<.015f,qPrintable(QString("median %1, 99th percentile %2").arg(median).arg(high)));
        QVERIFY_EXCEPTION_THROWN(morph.evaluate(.5,loopmorph::Morph::Live{9,&live,nullptr}),std::runtime_error);
        // Leading in, the motion passes from End's frames to Start's: here Start's frames move
        // along -X, so the motion is the blend of the two.
        const QVector3D lead(-.02f,0,0);std::vector<loopmorph::Track> before;
        for (int k=0;k<=8;++k) {loopmorph::Track t;t.chunk=k<=2 ? 3 : 2;
            for (size_t i=0;i<start.size();++i) {const QVector3D p=splatAt(start,i)-lead*float(k);t.position.insert(t.position.end(),{p.x(),p.y(),p.z()});t.colour.insert(t.colour.end(),{.5f,.5f,.5f});t.record.push_back(uint32_t(i));}
            before.push_back(std::move(t));}
        morph.follow(true,before);QCOMPARE(morph.followed(true),size_t(8));
        for (int k:{2,6}) for (double s:{.25,.75}) for (const auto &p:probes) {
            const QVector3D expected=morph.carry(p,s)-float(1-s)*velocity*float(k)-float(s)*lead*float(k);
            const float off=(morph.carry(p,s,size_t(k),true)-expected).length();
            QVERIFY2(off<.012f,qPrintable(QString("lead-in, %1 frames back at %2: %3 m off").arg(k).arg(s).arg(off)));
        }
    }
    void compactDensityMatchesFullHdAndFourK() {
        QCOMPARE(compactDisplayScale({1920,1080}),.875);QCOMPARE(compactDisplayScale({3840,2160}),1.0);
        QCOMPARE(compactDisplayScale({1080,1920}),.875);QCOMPARE(compactDisplayScale({2560,1440}),1.0);
        QCOMPARE(compactDisplayScale({0,0}),1.0);QCOMPARE(compactDisplayScale({1280,720}),.875);
    }
    void animationOffsetsInterpolationEditingAndPersistence() {
        Project project;project.modifiers.clear();project.transform.position={10,20,30};project.transform.rotation={0,0,90};project.transform.scale={2,2,2};const auto reference=project.transform;
        Modifier first;first.id=Project::newId();first.name="Animation";first.type=ModifierType::AnimateTransform;Transform a,b;b.position={4,0,0};b.rotation={0,0,90};b.scale={2,2,2};first.animation.setKey(0,a);first.animation.setKey(10,b);project.modifiers={first};project.selectedModifier=first.id;
        const auto midpoint=first.animation.evaluate(5);QVERIFY((midpoint.position-QVector3D(2,0,0)).length()<1e-5);QVERIFY((midpoint.scale-QVector3D(1.5f,1.5f,1.5f)).length()<1e-5);QVERIFY(std::abs(midpoint.rotation.z()-45)<1e-3);
        QVERIFY((project.transformAtFrame(5).position-QVector3D(10,24,30)).length()<1e-4);QVERIFY(project.hasAnimatedMotion());
        auto desired=project.transformAtFrame(5);desired.position+=QVector3D(1,2,3);project.setAnimatedPose(5,desired);QCOMPARE(project.modifier()->animation.keys.size(),3);QVERIFY((project.transformAtFrame(5).position-desired.position).length()<1e-4);QCOMPARE(project.transform.position,reference.position);QCOMPARE(project.transform.rotation,reference.rotation);QCOMPARE(project.transform.scale,reference.scale);
        Modifier second;second.id=Project::newId();second.name="Second animation";second.type=ModifierType::AnimateTransform;Transform extra;extra.position={0,1,0};second.animation.setKey(0,extra);project.modifiers.append(second);
        desired=project.transformAtFrame(7);desired.position+=QVector3D(-2,0,1);project.setAnimatedPose(7,desired);QVERIFY((project.transformAtFrame(7).position-desired.position).length()<1e-4);
        TransformAnimation wrap;Transform left,right;left.rotation={0,0,170};right.rotation={0,0,-170};wrap.setKey(0,left);wrap.setKey(10,right);QVERIFY(std::abs(std::abs(wrap.evaluate(5).rotation.z())-180)<1e-3);
        QTemporaryDir dir;project.asset=dir.filePath("source.mint");const auto json=project.json(dir.filePath("scene.vgsproj"));Project restored;QString error;QVERIFY(Project::fromJson(json,dir.path(),&restored,&error));QCOMPARE(restored.modifierJson(),project.modifierJson());
        auto invalid=json;auto mods=invalid["modifiers"].toArray();auto m=mods[0].toObject();auto animation=m["animation"].toObject();auto keys=animation["keys"].toArray();keys.append(keys[0]);animation["keys"]=keys;m["animation"]=animation;mods[0]=m;invalid["modifiers"]=mods;QVERIFY(!Project::fromJson(invalid,dir.path(),&restored,&error));
        project.modifiers[0].enabled=false;project.modifiers[1].enabled=false;QCOMPARE(project.transformAtFrame(5).position,reference.position);
    }
    void animatedCropInterpolatesSwitchesAndPersists() {
        Project project;auto &m=*project.modifier();QCOMPARE(m.type,ModifierType::Crop);m.crop.enabled=true;
        m.crop.radius=1;m.crop.remove=true;const auto still=m.crop;
        CropVolume a=m.crop,b=m.crop;a.transform.position={-1,0,0};a.height=2;b.transform.position={1,0,0};b.transform.rotation={0,90,0};b.height=4;b.radiusZ=3;
        m.cropAnimation.setKey(0,a);m.cropAnimation.setKey(10,b);
        // Static ignores the keys without losing them.
        QCOMPARE(project.modifiersAtFrame(5)[0].crop.transform.position,still.transform.position);QVERIFY(!project.hasAnimatedCrop());
        m.cropAnimation.still=m.crop;m.cropAnimation.animated=true;QVERIFY(project.hasAnimatedCrop());
        const auto mid=project.modifiersAtFrame(5)[0].crop;
        QVERIFY((mid.transform.position-QVector3D(0,0,0)).length()<1e-5f);QVERIFY(std::abs(mid.transform.rotation.y()-45)<1e-3f);
        QVERIFY(std::abs(mid.height-3)<1e-5f);QVERIFY(std::abs(mid.radiusZ-2)<1e-5f);
        // Mode and shape are the modifier's own, never animated; outside the keys the ends hold.
        QVERIFY(mid.remove);QCOMPARE(project.modifiersAtFrame(-4)[0].crop.transform.position,a.transform.position);QCOMPARE(project.modifiersAtFrame(40)[0].crop.height,4.f);
        // A Remove crop: what it deletes follows it over time.
        QVERIFY(CompiledModifiers(project.modifiersAtFrame(0)).keepsPosition({1,1,0}));QVERIFY(!CompiledModifiers(project.modifiersAtFrame(10)).keepsPosition({1,1,0}));
        // The shown pose tracks the frame; the static pose is what is saved as the crop.
        project.showCropsAtFrame(10);QCOMPARE(m.crop.transform.position,b.transform.position);QCOMPARE(m.staticCrop().transform.position,still.transform.position);
        QTemporaryDir dir;project.asset=dir.filePath("source.mint");const auto json=project.json(dir.filePath("scene.vgsproj"));Project restored;QString error;
        QVERIFY2(Project::fromJson(json,dir.path(),&restored,&error),qPrintable(error));QCOMPARE(restored.modifierJson(),project.modifierJson());
        QVERIFY(restored.modifiers[0].cropAnimation.animated);QCOMPARE(restored.modifiers[0].cropAnimation.keys.size(),2);
        QVERIFY((restored.modifiersAtFrame(5)[0].crop.transform.position-mid.transform.position).length()<1e-5f);
        // Saved while static, the keys come back too.
        project.modifiers[0].crop=project.modifiers[0].staticCrop();project.modifiers[0].cropAnimation.animated=false;
        QVERIFY(Project::fromJson(project.json(dir.filePath("scene.vgsproj")),dir.path(),&restored,&error));
        QVERIFY(!restored.modifiers[0].cropAnimation.animated);QCOMPARE(restored.modifiers[0].cropAnimation.keys.size(),2);
        // Older projects have no animation at all.
        auto legacy=json;auto mods=legacy["modifiers"].toArray();auto item=mods[0].toObject();auto crop=item["crop"].toObject();crop.remove("animation");item["crop"]=crop;mods[0]=item;legacy["modifiers"]=mods;
        QVERIFY(Project::fromJson(legacy,dir.path(),&restored,&error));QVERIFY(!restored.modifiers[0].cropAnimation.animated);QVERIFY(restored.modifiers[0].cropAnimation.keys.isEmpty());
    }
    void bakeAntialiasingSizeAndPersistence() {
        // The renderer's 0.3 px^2 low-pass as a distance: at 2.5 m on a 1080 px screen with a
        // 45 degree field of view a pixel is about 1.9 mm, and the size about 1.05 mm.
        Project project;project.modifiers.clear();Modifier bake;bake.id=Project::newId();bake.name="Bake";bake.type=ModifierType::BakeAntialiasing;
        project.modifiers={bake};QVERIFY(std::abs(project.antialiasingBake()-0.00105)<0.00002);
        project.modifiers[0].bakeDistance=5;project.modifiers[0].bakeScreenHeight=2160;QVERIFY(std::abs(project.antialiasingBake()-0.00105)<0.00002);
        project.modifiers[0].enabled=false;QCOMPARE(project.antialiasingBake(),0.);project.modifiers[0].enabled=true;
        QTemporaryDir dir;project.asset=dir.filePath("source.mint");Project restored;QString error;
        QVERIFY2(Project::fromJson(project.json(dir.filePath("scene.vgsproj")),dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers[0].type,ModifierType::BakeAntialiasing);QCOMPARE(restored.modifiers[0].bakeDistance,5.);QCOMPARE(restored.modifiers[0].bakeScreenHeight,2160);
    }
    void pruneKeepsWhatCountsAndPersists() {
        // Up to the share, lowest first, never above the protection, never what was not measured.
        const std::vector<float> scores{0.01f,0.5f,0.02f,-1,0.9f,0.03f,0.2f,0.3f,0.4f,0.6f};
        auto removed=[&](const QVector<PruneFilter> &filters) {QList<int> out;const auto keep=pruneKeep(scores,filters);for (int i=0;i<int(keep.size());++i) if (!keep[size_t(i)]) out<<i;return out;};
        QCOMPARE(removed({{20,0.25}}),(QList<int>{0,2}));                 // 20% of 10: the two lowest
        QCOMPARE(removed({{50,0.25}}),(QList<int>{0,2,5,6}));             // only four score below 0.25
        QCOMPARE(removed({{90,1000}}),(QList<int>{0,1,2,4,5,6,7,8,9}));   // -1 is never removed
        QCOMPARE(removed({{50,0}}),QList<int>{});                         // a capture where everything counts
        QCOMPARE(removed({{10,1},{0,1}}),(QList<int>{0}));
        // Filters apply in turn: the second takes its share of the eight left, the next lowest.
        QCOMPARE(removed({{20,1},{20,1}}),(QList<int>{0,2,5}));
        // What each filter did: asked for five, the protection let four go; the second sees the
        // six left, asks for one and finds none below its protection.
        std::vector<PruneStats> stats;pruneKeep(scores,{{50,0.25},{20,0.25}},&stats);QCOMPARE(stats.size(),size_t(2));
        QCOMPARE(stats[0].records,size_t(10));QCOMPARE(stats[0].asked,size_t(5));QCOMPARE(stats[0].removed,size_t(4));QVERIFY(stats[0].limited());
        QCOMPARE(stats[1].records,size_t(6));QCOMPARE(stats[1].asked,size_t(1));QCOMPARE(stats[1].removed,size_t(0));QVERIFY(stats[1].limited());
        QCOMPARE(pruneSampleTimes(1,2).size(),size_t(4));for (double t:pruneSampleTimes(1,2)) QVERIFY(t>1 && t<2);
        // Faint and tiny splats score below the ones that make the picture.
        vgs::Frame f;f.count=0;
        auto add=[&](float x,float y,float z,float scale,float opacity) {f.position.insert(f.position.end(),{x,y,z});f.rotation.insert(f.rotation.end(),{0,0,0,1});
            f.scale.insert(f.scale.end(),{scale,scale,scale});f.opacity.push_back(opacity);f.active.push_back(1);++f.count;};
        for (int i=0;i<25;++i) add((i%5)*.1f,(i/5)*.1f,0,.04f,.9f);
        add(.2f,.2f,.02f,.0003f,.9f);add(.15f,.15f,-.02f,.02f,.01f);
        const auto s=contributionScores({f,f},true);QCOMPARE(s.size(),f.count);
        for (int i=0;i<25;++i) {QVERIFY(s[size_t(i)]>s[25]);QVERIFY(s[size_t(i)]>s[26]);}
        // A dead record was not measured.
        auto dead=f;dead.active[3]=0;QCOMPARE(contributionScores({dead},false)[3],-1.f);
        // Saved and read back.
        Project project;project.modifiers.clear();Modifier prune;prune.id=Project::newId();prune.name="Prune";prune.type=ModifierType::PruneLowContribution;
        QCOMPARE(prune.prune.percent,15.);prune.prune={22,0.4};project.modifiers={prune};QCOMPARE(CompiledModifiers(project).prunes.size(),1);
        QTemporaryDir dir;project.asset=dir.filePath("source.mint");Project restored;QString error;
        QVERIFY2(Project::fromJson(project.json(dir.filePath("scene.vgsproj")),dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers[0].type,ModifierType::PruneLowContribution);QCOMPARE(restored.modifiers[0].prune.percent,22.);QCOMPARE(restored.modifiers[0].prune.protectAbove,0.4);
    }
    void projectAtTheLimitsReadsBack() {
        // A crop fitted to a single point sits at the 0.0001 m minimum; stored as a float it reads
        // a rounding below it, and the project must still open.
        QTemporaryDir dir;Project project;project.asset=dir.filePath("source.pgs");
        auto &crop=project.modifiers[0].crop;crop.radius=crop.radiusZ=crop.height=crop.width=crop.depth=0.0001f;
        Project back;QString error;QVERIFY2(Project::fromJson(project.json(dir.filePath("a.vgsproj")),dir.path(),&back,&error),qPrintable(error));
        QVERIFY(back.modifiers[0].crop.height>=0.0001f*0.999f);
    }
    void audioModifierPersistsBesideTheProject() {
        // A file is kept relative to the project, so a folder moved whole still finds it; the
        // capture's own track has no file. Every active Audio modifier is mixed, each with its
        // volume, looped or not.
        QTemporaryDir dir;Project project;project.modifiers.clear();project.asset=dir.filePath("source.vgs");
        Modifier own;own.id=Project::newId();own.name="Own";own.type=ModifierType::Audio;own.audioOffset=-.5;
        Modifier file=own;file.id=Project::newId();file.name="Song";file.audioFile=dir.filePath("music/song.mp3");file.audioOffset=1.25;file.audioVolume=.4;file.audioLoop=true;
        project.modifiers={own,file};QCOMPARE(project.audioModifiers().size(),2);QCOMPARE(project.audioModifiers()[1]->name,QString("Song"));
        project.modifiers[1].enabled=false;QCOMPARE(project.audioModifiers().size(),1);QCOMPARE(project.audioModifiers()[0]->name,QString("Own"));project.modifiers[1].enabled=true;
        const auto json=project.json(dir.filePath("scene.vgsproj"));
        QCOMPARE(json["modifiers"].toArray()[1].toObject()["audio"].toObject()["file"].toString(),QString("music/song.mp3"));
        Project restored;QString error;QVERIFY2(Project::fromJson(json,dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers[0].audioFile,QString());QCOMPARE(restored.modifiers[0].audioOffset,-.5);
        QCOMPARE(QFileInfo(restored.modifiers[1].audioFile).absoluteFilePath(),QFileInfo(dir.filePath("music/song.mp3")).absoluteFilePath());QCOMPARE(restored.modifiers[1].audioOffset,1.25);
        QCOMPARE(restored.modifiers[1].audioVolume,.4);QVERIFY(restored.modifiers[1].audioLoop);QCOMPARE(restored.modifiers[0].audioVolume,1.);QVERIFY(!restored.modifiers[0].audioLoop);
    }
    void removeGreenSeesTheColourOfItsPlaceInTheStack() {
        // Top to bottom: a Remove green above Color tests the source's colour, despilled or
        // not; one below it tests what Color leaves.
        Modifier green;green.id=Project::newId();green.type=ModifierType::RemoveGreen;green.green.minimumSaturation=.3f;green.green.hueTolerance=45;green.green.linearRgb=false;
        Modifier colour;colour.id=Project::newId();colour.type=ModifierType::Colour;colour.colourSaturation=0; // grey: nothing green is left
        const QVector3D spill(.2f,.8f,.2f),despilled(.5f,.5f,.5f);
        CompiledModifiers above(QVector<Modifier>{green,colour});QVERIFY(!above.greens[0].afterColour);
        QVERIFY(above.removesColour(spill,despilled));QVERIFY(above.removesColour(spill));
        CompiledModifiers below(QVector<Modifier>{colour,green});QVERIFY(below.greens[0].afterColour);QVERIFY(below.greensAfterColour());
        QVERIFY(!below.removesColour(spill,despilled));QVERIFY(!below.removesColour(spill)); // saturation 0 greys it
    }
    void colourModifierIsOneMatrix() {
        Modifier m;m.type=ModifierType::Colour;
        auto near=[](const std::array<float,9> &a,const std::array<float,9> &b) {for (int i=0;i<9;++i) if (std::abs(a[i]-b[i])>1e-6f) return false;return true;};
        QVERIFY(near(m.colourMatrix(),{1,0,0,0,1,0,0,0,1}));
        m.colourExposure=1;QVERIFY(near(m.colourMatrix(),{2,0,0,0,2,0,0,0,2}));m.colourExposure=0;
        // No saturation: every channel is the luminance.
        m.colourSaturation=0;const auto grey=m.colourMatrix();for (int r=0;r<3;++r) {QVERIFY(std::abs(grey[r*3]-.2126f)<1e-6f);QVERIFY(std::abs(grey[r*3+1]-.7152f)<1e-6f);QVERIFY(std::abs(grey[r*3+2]-.0722f)<1e-6f);}
        m.colourSaturation=1;m.colourTemperature=1;const auto warm=m.colourMatrix();QVERIFY(warm[0]>1 && warm[8]<1 && std::abs(warm[4]-1)<1e-6f);m.colourTemperature=0;
        // Two in the stack compose, in order; a disabled one does nothing; despill is reported.
        Project p;p.modifiers.clear();Modifier a=m;a.id=Project::newId();a.name="Bright";a.colourExposure=1;Modifier b=m;b.id=Project::newId();b.name="Grey";b.colourSaturation=0;b.colourDespill=Modifier::DespillAlways;b.colourDespillStrength=.6;
        p.modifiers={a,b};const CompiledModifiers both(p);QVERIFY(both.colourChanges && both.despill && both.despillPreview);QCOMPARE(both.despillStrength,.6);
        std::array<float,9> expected{};const auto ma=a.colourMatrix(),mb=b.colourMatrix();for (int r=0;r<3;++r) for (int c=0;c<3;++c) for (int k=0;k<3;++k) expected[r*3+c]+=mb[r*3+k]*ma[k*3+c];
        QVERIFY(near(both.colour,expected));
        p.modifiers[0].enabled=p.modifiers[1].enabled=false;QVERIFY(!CompiledModifiers(p).colourChanges);
        // Saved and read back.
        p.modifiers[1].enabled=true;QTemporaryDir dir;p.asset=dir.filePath("source.mint");Project restored;QString error;
        QVERIFY2(Project::fromJson(p.json(dir.filePath("a.vgsproj")),dir.path(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.modifiers[1].type,ModifierType::Colour);QCOMPARE(restored.modifiers[1].colourSaturation,0.);QCOMPARE(restored.modifiers[1].colourDespill,int(Modifier::DespillAlways));QCOMPARE(restored.modifiers[1].colourDespillStrength,.6);
        QCOMPARE(restored.modifiers[0].colourExposure,1.);QCOMPARE(restored.modifiers[1].colourGreenGain,.97);QCOMPARE(restored.modifiers[1].colourViewChroma,.5);QVERIFY(restored.modifiers[1].colourRecoverSkin);
    }
    void erasePicksPersistPerChunk() {
        Project project;project.modifiers.clear();
        Modifier a;a.id=Project::newId();a.name="Floaters";a.type=ModifierType::Erase;a.erased[0]={1,2,3,4,10,200000};a.erased[7]={5};
        Modifier b=a;b.id=Project::newId();b.name="More";b.erased.clear();b.erased[0]={4,6};
        project.modifiers={a,b};const CompiledModifiers both(project);
        QCOMPARE(both.erased.at(0),(std::vector<uint32_t>{1,2,3,4,6,10,200000}));QVERIFY(both.erases(7,5));QVERIFY(!both.erases(7,4));QVERIFY(!both.erases(3,1));
        QCOMPARE(both.eraseKeep(0,8),(std::vector<uint8_t>{1,0,0,0,0,1,0,1}));QVERIFY(!both.showErased);
        project.modifiers[1].enabled=false;QCOMPARE(CompiledModifiers(project).erased.at(0),a.erased[0]);project.modifiers[1].enabled=true;
        // Runs keep the file small; read back exactly.
        QTemporaryDir dir;project.asset=dir.filePath("source.vgs");const auto json=project.json(dir.filePath("a.vgsproj"));
        QVERIFY(json["modifiers"].toArray()[0].toObject()["erase"].toObject()["chunks"].toArray()[0].toObject()["records"].toString().size()<40);
        Project back;QString error;QVERIFY2(Project::fromJson(json,dir.path(),&back,&error),qPrintable(error));
        QCOMPARE(back.modifiers[0].erased,a.erased);QCOMPARE(back.modifiers[1].erased,b.erased);
        // Malformed runs are refused.
        auto broken=json;auto modifiers=broken["modifiers"].toArray();auto item=modifiers[0].toObject();auto erase=item["erase"].toObject();
        auto chunks=erase["chunks"].toArray();auto chunk=chunks[0].toObject();chunk["records"]="AAAA";chunks[0]=chunk;erase["chunks"]=chunks;item["erase"]=erase;modifiers[0]=item;broken["modifiers"]=modifiers;
        QVERIFY(!Project::fromJson(broken,dir.path(),&back,&error));
    }
    void despillMovesFromExportSettingsToAModifier() {
        // A project that despilled in its export settings reads with a Colour modifier that does.
        QTemporaryDir dir;Project old;old.modifiers.clear();old.asset=dir.filePath("source.vgs");
        old.captureSettings.despill=true;old.captureSettings.despillStrength=.7;old.captureSettings.greenGain=1.1;old.captureSettings.viewChromaScale=.3;old.captureSettings.recoverSkin=false;
        Project read;QString error;QVERIFY2(Project::fromJson(old.json(dir.filePath("a.vgsproj")),dir.path(),&read,&error),qPrintable(error));
        QVERIFY(!read.captureSettings.despill);QCOMPARE(read.modifiers.size(),1);const auto &m=read.modifiers[0];
        QCOMPARE(m.type,ModifierType::Colour);QCOMPARE(m.name,QString("Despill"));QCOMPARE(m.colourDespill,int(Modifier::DespillOnExport));QCOMPARE(m.colourDespillStrength,.7);
        QCOMPARE(m.colourGreenGain,1.1);QCOMPARE(m.colourViewChroma,.3);QVERIFY(!m.colourRecoverSkin);
        // Read again, it stays one modifier.
        Project again;QVERIFY(Project::fromJson(read.json(dir.filePath("a.vgsproj")),dir.path(),&again,&error));QCOMPARE(again.modifiers.size(),1);
        // A Colour modifier's despill is off by default, with THE4DSCANNER's settings.
        Modifier fresh;QCOMPARE(fresh.colourDespill,int(Modifier::DespillNone));QCOMPARE(fresh.colourDespillStrength,1.);QCOMPARE(fresh.colourGreenGain,.97);QCOMPARE(fresh.colourViewChroma,.5);QVERIFY(fresh.colourRecoverSkin);QCOMPARE(fresh.colourOpacity,1.);
    }
    void isolationMatchesNthNeighbourMedianAndEdgeCases() {
        std::vector<QVector3D> points;for (int y=0;y<5;++y) for (int x=0;x<5;++x) points.push_back({x*.01f,y*.01f,0});points.push_back({100,100,100});points.push_back({101,100,100});
        IsolationFilter filter{4,300};std::vector<uint8_t> keep(points.size(),1);std::vector<float> expectedDistances;
        for (size_t i=0;i<points.size();++i) {std::vector<float> distances;for (size_t j=0;j<points.size();++j) if (i!=j) distances.push_back((points[i]-points[j]).length());std::sort(distances.begin(),distances.end());expectedDistances.push_back(distances[3]);}
        auto sorted=expectedDistances;std::sort(sorted.begin(),sorted.end());const float threshold=sorted[sorted.size()/2]*3;applyIsolation(points,keep,{filter});
        for (size_t i=0;i<keep.size();++i) QCOMPARE(bool(keep[i]),expectedDistances[i]<=threshold);QCOMPARE(keep.back(),uint8_t(0));QCOMPARE(keep[0],uint8_t(1));
        std::vector<QVector3D> small{{0,0,0},{1,0,0}};std::vector<uint8_t> smallMask{1,1};applyIsolation(small,smallMask,{filter});QCOMPARE(smallMask,std::vector<uint8_t>({1,1}));
        std::vector<QVector3D> duplicates(10,QVector3D());std::vector<uint8_t> duplicateMask(10,1);applyIsolation(duplicates,duplicateMask,{filter});QCOMPARE(std::count(duplicateMask.begin(),duplicateMask.end(),uint8_t(1)),10);
        std::fill(keep.begin(),keep.end(),1);keep.back()=0;applyIsolation(points,keep,{filter});QCOMPARE(keep.back(),uint8_t(0));
        std::fill(keep.begin(),keep.end(),1);QVERIFY_EXCEPTION_THROWN(applyIsolation(points,keep,{filter},[] {return true;}),std::runtime_error);
        std::vector<QVector3D> large;for (int y=0;y<80;++y) for (int x=0;x<80;++x) large.push_back({x*.01f,y*.01f,0});for (int i=0;i<4;++i) large.push_back({100.f+i,100,100});std::vector<uint8_t> largeMask(large.size(),1);applyIsolation(large,largeMask,{{4,400}});QCOMPARE(std::count(largeMask.begin(),largeMask.end(),uint8_t(1)),6400);
    }
    void linearRgbGreenFilterAndLegacyDefaults() {
        GreenFilter filter;QVERIFY(filter.linearRgb);QCOMPARE(filter.minimumSaturation,.5f);QCOMPARE(filter.hueTolerance,45.f);
        QVERIFY(filter.matches({.4f,.6f,.4f}));QVERIFY(!filter.matches({.6f,.6f,.6f}));QVERIFY(!filter.matches({.6f,.4f,.4f}));
        filter.linearRgb=false;QVERIFY(!filter.matches({.4f,.6f,.4f}));
        QTemporaryDir dir;Project project;project.asset=dir.filePath("source.mint");project.modifiers[0].type=ModifierType::RemoveGreen;
        auto json=project.json(dir.filePath("scene.vgsproj"));auto rows=json["modifiers"].toArray();auto row=rows[0].toObject();auto green=row["green"].toObject();green.remove("colourSpace");row["green"]=green;rows[0]=row;json["modifiers"]=rows;
        Project restored;QString error;QVERIFY(Project::fromJson(json,dir.path(),&restored,&error));QVERIFY(!restored.modifier()->green.linearRgb);
    }
    void animationAndIsolationIgnoreUnrelatedPropertiesAndPersist() {
        QTemporaryDir dir;Project project;project.asset=dir.filePath("source.mint");project.modifiers.clear();
        for (auto type:{ModifierType::AnimateTransform,ModifierType::PurgeIsolated}) {
            Modifier modifier;modifier.id=Project::newId();modifier.name=type==ModifierType::AnimateTransform ? "Animate transform" : "Purge Isolated";modifier.type=type;
            // Unused crop/filter properties must never become active by accident.
            modifier.crop.enabled=true;modifier.crop.transform.scale={0,0,0};modifier.green.minimumSaturation=0;modifier.green.hueTolerance=180;
            project.modifiers.append(modifier);
        }
        project.selectedModifier=project.modifiers.back().id;QString error;Project restored;
        QVERIFY(Project::fromJson(project.json(dir.filePath("scene.vgsproj")),dir.path(),&restored,&error));
        QCOMPARE(restored.modifierJson(),project.modifierJson());QCOMPARE(restored.selectedModifier,project.selectedModifier);
        CompiledModifiers filters(restored);QVERIFY(filters.crops.isEmpty());QVERIFY(filters.greens.isEmpty());QVERIFY(filters.keeps({100,100,100},{0,1,0}));
        {   // Keep and Remove crops: inside a Keep and in no Remove survives, Remove wins where they overlap.
            Project crops;crops.modifiers.clear();
            Modifier keep;keep.id=Project::newId();keep.name="Keep";keep.crop.enabled=true;keep.crop.radius=2;keep.crop.height=2;
            Modifier remove=keep;remove.id=Project::newId();remove.name="Remove";remove.crop.remove=true;remove.crop.radius=.5f;remove.crop.showRemovedInRed=false;
            crops.modifiers={keep,remove};CompiledModifiers both(crops);
            QVERIFY(both.keepsPosition({1.5f,1,0}));QVERIFY(!both.keepsPosition({.2f,1,0}));QVERIFY(!both.keepsPosition({3,1,0}));
            crops.modifiers={remove};CompiledModifiers onlyRemove(crops);
            QVERIFY(onlyRemove.keepsPosition({3,1,0}));QVERIFY(!onlyRemove.keepsPosition({.2f,1,0}));
            crops.modifiers={keep,remove};crops.selectedModifier=remove.id;crops.asset=dir.filePath("source.mint");Project back;
            QVERIFY(Project::fromJson(crops.json(dir.filePath("crops.vgsproj")),dir.path(),&back,&error));
            QVERIFY(!back.modifiers[0].crop.remove && back.modifiers[0].crop.showRemovedInRed);
            QVERIFY(back.modifiers[1].crop.remove && !back.modifiers[1].crop.showRemovedInRed);
        }
        PresetStore presets(dir.filePath("presets"));QString path;EditorPreset preset;
        QVERIFY(presets.save("Placeholders",project,&path,&error,PresetScope::Editor));QVERIFY(presets.read(path,PresetScope::Editor,&preset,&error));
        QCOMPARE(preset.settings.modifierJson(),project.modifierJson());
    }
    void modifierUnionGreenFilterAndPersistence() {
        Project project;project.modifiers.clear();
        Modifier left;left.id=Project::newId();left.name="Left crop";left.crop.enabled=true;left.crop.radius=.5f;left.crop.height=2;left.crop.transform.position={-1.5f,0,0};
        Modifier right=left;right.id=Project::newId();right.name="Right crop";right.crop.shape=CropShape::Box;right.crop.width=right.crop.depth=1;right.crop.transform.position={1.5f,0,0};
        Modifier green;green.id=Project::newId();green.name="Green removal";green.type=ModifierType::RemoveGreen;green.green.linearRgb=false;green.green.minimumSaturation=.8f;green.green.hueTolerance=20;
        project.modifiers={left,right,green};project.selectedModifier=green.id;
        CompiledModifiers filters(project);
        QVERIFY(filters.keeps({-1.5f,1,0},{1,0,0}));QVERIFY(filters.keeps({1.5f,1,0},{0,0,1}));
        QVERIFY(!filters.keeps({0,1,0},{1,1,1}));QVERIFY(!filters.keeps({1.5f,1,0},{0,1,0}));
        QVERIFY(!green.green.matches({.5f,.5f,.5f}));QVERIFY(!green.green.matches({1,1,0}));QVERIFY(!green.green.matches({.3f,1,.3f}));
        QVERIFY(green.green.matches({.2f,1,.2f}));QVERIFY(green.green.matches({0,1,0}));QVERIFY(!green.green.matches({0,0,0}));
        green.green.hueTolerance=180;green.green.minimumSaturation=0;QVERIFY(green.green.matches({1,0,0}));QVERIFY(!green.green.matches({.5f,.5f,.5f}));
        QTemporaryDir dir;project.asset=dir.filePath("capture.mint");QString error;const auto json=project.json(dir.filePath("scene.vgsproj"));
        QCOMPARE(json["version"].toInt(),8);QCOMPARE(json["modifiers"].toArray().size(),3);QVERIFY(!json.contains("layers"));Project restored;
        QVERIFY2(Project::fromJson(json,dir.path(),&restored,&error),qPrintable(error));QCOMPARE(restored.modifierJson(),project.modifierJson());QCOMPARE(restored.selectedModifier,green.id);
        project.modifiers[0].enabled=false;QVERIFY(!CompiledModifiers(project).keeps({-1.5f,1,0},{1,0,0}));
        project.modifiers[1].enabled=false;QVERIFY(CompiledModifiers(project).keeps({100,100,100},{1,0,0}));
        project.modifiers[2].enabled=false;QVERIFY(CompiledModifiers(project).keeps({100,100,100},{0,1,0}));
        auto invalid=json;auto items=invalid["modifiers"].toArray();auto item=items[1].toObject();item["id"]=items[0].toObject()["id"];items[1]=item;invalid["modifiers"]=items;
        QVERIFY(!Project::fromJson(invalid,dir.path(),&restored,&error));
        for (int i=0;i<100;++i) {auto copy=right;copy.id=Project::newId();project.modifiers.append(copy);}
        QVERIFY(Project::fromJson(project.json(dir.filePath("many.vgsproj")),dir.path(),&restored,&error));QCOMPARE(restored.modifiers.size(),103);
    }
    void systemPresetsAreListedBesideTheUsers() {
        // Presets beside the program are listed with the user's, marked as built in; one the
        // user saves with the same name takes its place, and the built-in file is untouched.
        QTemporaryDir directory;Project settings;QString path,error;
        PresetStore shipped(directory.filePath("system"));QVERIFY2(shipped.save("Studio base",settings,&path,&error,PresetScope::Editor),qPrintable(error));
        QVERIFY(QFile::rename(path,directory.filePath("system/Studio Base.preset")));
        PresetStore store(directory.filePath("user"),directory.filePath("system"));
        auto list=store.list();QCOMPARE(list.size(),1);QVERIFY(list[0].system);QCOMPARE(list[0].name,QString("Studio base"));
        EditorPreset preset;QVERIFY2(store.read(list[0].path,PresetScope::Editor,&preset,&error),qPrintable(error));
        QVERIFY2(store.save("Studio base",settings,&path,&error,PresetScope::Editor),qPrintable(error));
        list=store.list();QCOMPARE(list.size(),1);QVERIFY(!list[0].system);QCOMPARE(list[0].path,path);
        QVERIFY(QFileInfo::exists(directory.filePath("system/Studio Base.preset")));
        QVERIFY(PresetStore(directory.filePath("user")).list().size()==1);
    }
    void reusablePresetStorage() {
        QTemporaryDir directory; PresetStore store(directory.filePath("presets")); Project settings;
        settings.asset = "D:/private-shoot/capture.mint"; settings.time = 12; settings.in = 10; settings.out = 20;
        settings.transform.position = {1,2,3}; settings.transform.rotation = {10,20,30}; settings.transform.scale = {2,3,4}; settings.transform.shear = {0.1f,0.2f,0.3f};
        settings.crop().enabled = true; settings.crop().radius = 1.5f; settings.crop().height = 2.5f;
        settings.crop().shape = CropShape::Box; settings.crop().width = 2; settings.crop().depth = 3;
        settings.captureSettings.title = "Reusable settings"; settings.captureSettings.despill = true;
        settings.crop().transform.position = {4,5,6}; settings.crop().transform.rotation = {30,20,10}; settings.crop().transform.scale = {0.5f,2,3};
        settings.spaces[1] = CoordinateSpace::Local; settings.pointSize = 9; settings.grid = false; settings.speed = 0.5; settings.loop = false;
        settings.camera.preset = ViewPreset::Front; settings.camera.orthographic = true; settings.camera.yaw = settings.camera.pitch = 0;
        QString path,error; QVERIFY2(store.save("Reusable: ../T4DS?",settings,&path,&error,PresetScope::Editor),qPrintable(error));
        QCOMPARE(QFileInfo(path).absolutePath(),store.directory()); QCOMPARE(store.list().size(),1);
        PresetStore reloaded(store.directory()); EditorPreset preset;
        QVERIFY2(reloaded.read(path,&preset,&error),qPrintable(error)); QCOMPARE(preset.name,QString("Reusable: ../T4DS?"));
        QCOMPARE(preset.settings.transform.position,settings.transform.position); QCOMPARE(preset.settings.transform.shear,settings.transform.shear);
        QCOMPARE(preset.settings.crop().transform.position,settings.crop().transform.position); QCOMPARE(preset.settings.crop().transform.scale,settings.crop().transform.scale);
        QCOMPARE(preset.settings.crop().radius,settings.crop().radius); QCOMPARE(preset.settings.crop().enabled,true);
        QCOMPARE(preset.settings.crop().shape,CropShape::Box); QCOMPARE(preset.settings.crop().depth,3.0f); QCOMPARE(preset.settings.captureSettings.despill,false);
        QCOMPARE(preset.settings.spaces[1],CoordinateSpace::Local); QCOMPARE(preset.settings.camera.preset,ViewPreset::Free);
        QCOMPARE(preset.settings.pointSize,5.0);QCOMPARE(preset.settings.grid,true); QCOMPARE(preset.settings.speed,0.5); QCOMPARE(preset.settings.time,0.0);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        QVERIFY(!bytes.contains(settings.asset.toUtf8())); const auto config = QJsonDocument::fromJson(bytes).object()["configuration"].toObject();
        QVERIFY(!config.contains("asset")); QVERIFY(!config.contains("timeline"));
        QVERIFY(!config.contains("captureSettings"));
        QVERIFY(!config.contains("view"));QVERIFY(!config.contains("camera"));QVERIFY(!config.contains("ghost"));QVERIFY(!config.contains("ghostOpacity"));
        auto oldRoot=QJsonDocument::fromJson(bytes).object();QCOMPARE(oldRoot["version"].toInt(),6);QVERIFY(!oldRoot["appliesTo"].toArray().contains("view"));
        oldRoot["version"]=5;oldRoot["appliesTo"]=QJsonArray{"capture-transform","modifiers","view","playback","reference-spaces"};
        auto oldConfiguration=config;oldConfiguration["view"]=QJsonObject{{"pointSize","invalid preview value"},{"grid",false},{"sh",false},{"ghost",true},{"ghostOpacity",.9}};
        oldConfiguration["camera"]=QJsonArray{1,2,3};oldRoot["configuration"]=oldConfiguration;
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(oldRoot).toJson());file.close();
        QVERIFY2(store.read(path,&preset,&error),qPrintable(error));QCOMPARE(preset.settings.pointSize,5.0);QCOMPARE(preset.settings.grid,true);QCOMPARE(preset.settings.camera.preset,ViewPreset::Free);
        QCOMPARE(preset.settings.transform.position,settings.transform.position);
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(bytes);file.close();
        settings.transform.position = {9,8,7}; QString overwritten;
        QVERIFY(store.save("REUSABLE: ../T4DS?",settings,&overwritten,&error,PresetScope::Editor)); QCOMPARE(overwritten,path); QCOMPARE(store.list().size(),1);
        QVERIFY(store.read(path,&preset,&error)); QCOMPARE(preset.settings.transform.position,settings.transform.position);
        QVERIFY(!store.save("   ",settings,nullptr,&error,PresetScope::Editor));
        QFile broken(directory.filePath("presets/broken.json")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("{}"); broken.close();
        QCOMPARE(store.list().size(),1); QVERIFY(!store.read(broken.fileName(),&preset,&error));
        QCOMPARE(preset.settings.transform.position,settings.transform.position);
    }
    void presetScopesAllowSameNameWithoutCrossingData() {
        QTemporaryDir directory; PresetStore store(directory.path()); Project settings; settings.asset = "source.mint";
        settings.transform.position = {1,2,3}; settings.crop().enabled = true; settings.crop().shape = CropShape::Box;
        settings.captureSettings.title = "Metadata title"; settings.captureSettings.despill = true;
        QString editorPath,metadataPath,error;
        QVERIFY(store.save("T4DS",settings,&editorPath,&error,PresetScope::Editor));
        QVERIFY(store.save("T4DS",settings,&metadataPath,&error,PresetScope::Metadata));
        QVERIFY(editorPath!=metadataPath); QCOMPARE(store.list(PresetScope::Editor).size(),1); QCOMPARE(store.list(PresetScope::Metadata).size(),1);
        QCOMPARE(QFileInfo(editorPath).suffix(),QString("preset")); QCOMPARE(QFileInfo(metadataPath).suffix(),QString("presetmetadata"));
        EditorPreset editor,metadata; QVERIFY(store.read(editorPath,&editor,&error)); QVERIFY(store.read(metadataPath,&metadata,&error));
        QCOMPARE(editor.scope,PresetScope::Editor); QCOMPARE(metadata.scope,PresetScope::Metadata);
        QCOMPARE(editor.settings.transform.position,settings.transform.position); QCOMPARE(editor.settings.captureSettings.despill,false);
        QCOMPARE(metadata.settings.transform.position,QVector3D()); QCOMPARE(metadata.settings.crop().enabled,false);
        QCOMPARE(metadata.settings.captureSettings.title,settings.captureSettings.title);
        settings.captureSettings.title = "Updated metadata"; QString updated;
        QVERIFY(store.save("t4ds",settings,&updated,&error,PresetScope::Metadata)); QCOMPARE(updated,metadataPath);
        QVERIFY(store.read(editorPath,&editor,&error)); QCOMPARE(editor.settings.transform.position,QVector3D(1,2,3));
        QFile file(metadataPath); QVERIFY(file.open(QIODevice::ReadOnly)); const auto root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root["scope"].toString(),QString("capture-metadata")); QVERIFY(root["appliesTo"].isArray());
        QCOMPARE(root["format"].toString(),QString("vgs-editor-metadata-preset")); QCOMPARE(root["version"].toInt(),4);
        QCOMPARE(root["configuration"].toObject().size(),1);
        QVERIFY(!store.read(metadataPath,PresetScope::Editor,&editor,&error));
        QVERIFY(!store.read(editorPath,PresetScope::Metadata,&metadata,&error));
        QCOMPARE(metadata.settings.captureSettings.title,QString("Metadata title"));
        const QString renamed = directory.filePath("pretend-editor.preset");
        QVERIFY(QFile::copy(metadataPath,renamed));
        QCOMPARE(store.list(PresetScope::Editor).size(),1);
        QVERIFY(!store.read(renamed,PresetScope::Editor,&editor,&error));
    }
    void mixedAndScopedLegacyPresetsMigrateWithoutOverwriting() {
        QTemporaryDir directory; PresetStore store(directory.path()); Project settings;
        settings.transform.position = {1,2,3}; settings.captureSettings.author = "Legacy author"; settings.captureSettings.despill = true;
        auto mixed = settings.json(directory.filePath("old.json"));
        mixed.remove("format"); mixed.remove("version"); mixed.remove("asset"); mixed.remove("timeline");
        mixed["playback"] = QJsonObject{{"speed",settings.speed},{"loop",settings.loop}};
        QJsonObject legacy{{"format","vgs-editor-preset"},{"version",1},{"name","Shared name"},{"configuration",mixed}};
        const auto original = QJsonDocument(legacy).toJson(); QFile old(directory.filePath("old.json"));
        QVERIFY(old.open(QIODevice::WriteOnly)); QCOMPARE(old.write(original),qint64(original.size())); old.close();
        auto editorEntries = store.list(PresetScope::Editor), metadataEntries = store.list(PresetScope::Metadata);
        QCOMPARE(editorEntries.size(),1); QCOMPARE(metadataEntries.size(),1); QCOMPARE(editorEntries[0].name,metadataEntries[0].name);
        QVERIFY(editorEntries[0].path.endsWith(".preset")); QVERIFY(metadataEntries[0].path.endsWith(".presetmetadata"));
        EditorPreset editor,metadata; QString error;
        QVERIFY(store.read(editorEntries[0].path,PresetScope::Editor,&editor,&error));
        QVERIFY(store.read(metadataEntries[0].path,PresetScope::Metadata,&metadata,&error));
        QCOMPARE(editor.settings.transform.position,settings.transform.position); QCOMPARE(editor.settings.captureSettings.despill,false);
        QCOMPARE(metadata.settings.transform.position,QVector3D()); QCOMPARE(metadata.settings.captureSettings.author,QString("Legacy author"));
        QVERIFY(old.open(QIODevice::ReadOnly)); QCOMPARE(old.readAll(),original); old.close();
        settings.captureSettings.author = "Edited author"; QString saved;
        QVERIFY(store.save("SHARED NAME",settings,&saved,&error,PresetScope::Metadata)); QCOMPARE(saved,metadataEntries[0].path);
        QCOMPARE(store.list(PresetScope::Metadata).size(),1);
        QVERIFY(store.read(saved,PresetScope::Metadata,&metadata,&error)); QCOMPARE(metadata.settings.captureSettings.author,QString("Edited author"));
        QVERIFY(store.read(editorEntries[0].path,PresetScope::Editor,&editor,&error)); QCOMPARE(editor.settings.transform.position,QVector3D(1,2,3));

        // Version-2 JSON presets have one scope; they must create only that new file type.
        legacy["version"] = 2; legacy["name"] = "Metadata only"; legacy["scope"] = "capture-metadata";
        legacy["configuration"] = QJsonObject{{"captureSettings",settings.captureSettings.json()}};
        QFile scoped(directory.filePath("old-metadata.json")); QVERIFY(scoped.open(QIODevice::WriteOnly)); scoped.write(QJsonDocument(legacy).toJson()); scoped.close();
        QCOMPARE(store.list(PresetScope::Editor).size(),1); QCOMPARE(store.list(PresetScope::Metadata).size(),2);

        // A newer scoped JSON template wins over the mixed original with the same name.
        QTemporaryDir priorityDirectory; PresetStore priority(priorityDirectory.path());
        QVERIFY(QFile::copy(old.fileName(),priorityDirectory.filePath("a-mixed.json")));
        legacy["name"] = "Shared name";
        QFile newer(priorityDirectory.filePath("z-scoped.json")); QVERIFY(newer.open(QIODevice::WriteOnly)); newer.write(QJsonDocument(legacy).toJson()); newer.close();
        auto imported = priority.list(PresetScope::Metadata); QCOMPARE(imported.size(),1);
        QVERIFY(priority.read(imported[0].path,PresetScope::Metadata,&metadata,&error)); QCOMPARE(metadata.settings.captureSettings.author,QString("Edited author"));
    }
    void metadataConfigurationAndBoxContainment() {
        CaptureSettings settings; settings.title = "Title"; settings.catalogueId = "catalogue"; settings.author = "Studio";
        settings.plain = true; settings.despill = true; settings.despillStrength = 0.75; settings.shDegree = 2; settings.tags = {"scene","actor"};
        settings.extraJson = "{\"take\": 7}"; QString error; CaptureSettings restored;
        QVERIFY2(CaptureSettings::fromJson(settings.json(),&restored,&error),qPrintable(error)); QCOMPARE(restored.json(),settings.json());
        auto invalid = settings.json(); invalid["extraJson"] = "invalid JSON";
        QVERIFY(!CaptureSettings::fromJson(invalid,&restored,&error)); QCOMPARE(restored.title,settings.title);
        CropVolume crop; crop.enabled = true; crop.radius = crop.radiusZ = 0.4f; crop.height = 1; crop.width = crop.depth = 0.8f;
        QVERIFY(!crop.contains({0.35f,0.5f,0.35f}));
        // Elliptic: radius X 0.4, radius Z 0.8.
        crop.radiusZ = 0.8f; QVERIFY(crop.contains({0,0.5f,0.75f})); QVERIFY(!crop.contains({0.35f,0.5f,0.6f})); QVERIFY(!crop.contains({0.45f,0.5f,0}));
        crop.radiusZ = 0.4f;
        crop.shape = CropShape::Box; QVERIFY(crop.contains({0.35f,0.5f,0.35f}));
        QVERIFY(!crop.contains({0.41f,0.5f,0})); QVERIFY(!crop.contains({0,1.01f,0}));
    }
    void fileHistoryPersistsAndDeduplicates() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString store = dir.filePath("settings.ini");
        const QString capture = dir.filePath("capture.mint"), project = dir.filePath("project.vgsproj");
        {
            QSettings settings(store,QSettings::IniFormat); FileHistory history(settings);
            for (int i=0; i<12; ++i) history.remember(dir.filePath(QString("capture%1.vgs").arg(i)));
            QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
            QVERIFY(!history.recent().contains(dir.filePath("capture0.vgs")));
            history.remember(capture); history.remember(project);
            history.remember(dir.filePath("./capture.mint"));
            QCOMPARE(history.recent().first(),capture); QCOMPARE(history.recent().count(capture),1);
#ifdef Q_OS_WIN
            history.remember(capture.toUpper());
            QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
            history.remember(capture);
#endif
        }
        QSettings restored(store,QSettings::IniFormat); FileHistory history(restored);
        QCOMPARE(history.recent().first(),capture); QCOMPARE(history.recent()[1],project);
        QCOMPARE(restored.value("Files/LastCapture").toString(),capture);
        QCOMPARE(restored.value("Files/LastProject").toString(),project);
        // Missing files retain their recent entry but the dialog starts in a valid directory.
        QCOMPARE(history.openPath("Capture"),dir.path());
        QFile file(capture); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QCOMPARE(history.openPath("Capture"),capture);
        QCOMPARE(history.savePath("Project","new.vgsproj"),dir.filePath("new.vgsproj"));
        history.rememberImage(dir.filePath("preview.png"));
        QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
        QCOMPARE(history.savePath("Image","next.png"),dir.filePath("next.png"));
        history.clearRecent(); QVERIFY(history.recent().isEmpty());
        QCOMPARE(history.openPath("Capture"),capture); // Clearing the menu keeps dialog locations.
    }
    void projectRoundtrip() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Project p; p.asset = dir.filePath("capture with spaces.mint");
        p.transform.position = {1,2,3}; p.transform.rotation = {10,20,30}; p.transform.scale = {0.5f,2,3};
        p.transform.shear = {0.1f,0.2f,0.3f}; p.spaces[0] = CoordinateSpace::Local; p.spaces[2] = CoordinateSpace::Local;
        p.camera.target = {1,2,3}; p.camera.distance = 8; p.time = 2; p.in = 1; p.out = 5; p.pointSize = 4;
        p.camera.preset = ViewPreset::Top; p.camera.pitch = 90; p.camera.orthographic = true;
        p.crop().enabled = true; p.crop().radius = 0.8f; p.crop().height = 2.4f;
        p.crop().transform.position = {1,2,3}; p.crop().transform.rotation = {20,30,40}; p.crop().transform.scale = {1,2,0.5f};
        QString error; const auto path = dir.filePath("sample.vgsproj");
        QVERIFY2(p.write(path,&error),qPrintable(error));
        Project restored; QVERIFY2(Project::read(path,&restored,&error),qPrintable(error));
        QCOMPARE(restored.asset,p.asset); QCOMPARE(restored.transform.position,p.transform.position);
        QCOMPARE(restored.transform.rotation,p.transform.rotation); QCOMPARE(restored.transform.scale,p.transform.scale);
        QCOMPARE(restored.transform.shear,p.transform.shear); QCOMPARE(restored.spaces[0],CoordinateSpace::Local); QCOMPARE(restored.spaces[2],CoordinateSpace::Local);
        QCOMPARE(restored.time,p.time); QCOMPARE(restored.pointSize,p.pointSize);
        QCOMPARE(restored.crop().enabled,p.crop().enabled); QCOMPARE(restored.crop().radius,p.crop().radius);
        QCOMPARE(restored.crop().height,p.crop().height); QCOMPARE(restored.crop().transform.rotation,p.crop().transform.rotation);
        QCOMPARE(restored.camera.preset,p.camera.preset); QCOMPARE(restored.camera.orthographic,true); QCOMPARE(restored.camera.pitch,90.0f);
        QCOMPARE(restored.json(path)["view"].toObject()["sh"].toBool(),true);
        auto legacy = p.json(path); legacy["version"] = 1; legacy.remove("crop");
        auto legacyCamera = legacy["camera"].toObject(); legacyCamera.remove("preset"); legacyCamera.remove("orthographic"); legacyCamera["pitch"] = 12; legacy["camera"] = legacyCamera;
        auto legacyView = legacy["view"].toObject(); legacyView["sh"] = false; legacy["view"] = legacyView;
        QFile legacyFile(path); QVERIFY(legacyFile.open(QIODevice::WriteOnly)); legacyFile.write(QJsonDocument(legacy).toJson()); legacyFile.close();
        QVERIFY(Project::read(path,&restored,&error)); QCOMPARE(restored.json(path)["view"].toObject()["sh"].toBool(),true);
        QCOMPARE(restored.crop().enabled,false); QCOMPARE(restored.camera.preset,ViewPreset::Free); QCOMPARE(restored.camera.orthographic,false);
        // Relocate a complete project directory: relative capture paths remain portable.
        QTemporaryDir relocated; QFile::copy(path,relocated.filePath("copy.vgsproj"));
        QVERIFY(Project::read(relocated.filePath("copy.vgsproj"),&restored,&error));
        QCOMPARE(restored.asset,relocated.filePath("capture with spaces.mint"));
    }
    void invalidProject() {
        QTemporaryDir dir; Project p; p.asset = "x.mint"; p.out = 5;
        auto j = p.json(dir.filePath("bad.vgsproj"));
        auto transform = j["transform"].toObject(); transform["scale"] = QJsonArray{0,1,1}; j["transform"] = transform;
        QFile file(dir.filePath("bad.vgsproj")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(j).toJson()); file.close();
        QString error; Project restored; restored.asset = "unchanged";
        QVERIFY(!Project::read(file.fileName(),&restored,&error)); QCOMPARE(restored.asset,QString("unchanged"));
    }
    void transformOrder() {
        Transform t; t.position = {10,0,0}; t.rotation = {0,0,90}; t.scale = {2,1,1};
        const auto result = t.matrix().map(QVector3D(1,0,0));
        QVERIFY((result-QVector3D(10,2,0)).length()<1e-5f);
    }
    void localRotationCompositionAndCropContainment() {
        Transform t; t.rotation = {25,50,70};
        for (int axis=0; axis<3; ++axis) {
            QVector3D direction; direction[axis] = 1;
            QMatrix4x4 expected = t.rotationMatrix(); expected.rotate(37,direction);
            const auto actual = t.rotatedLocal(axis,37).rotationMatrix();
            for (int i=0; i<3; ++i) for (int j=0; j<3; ++j) QVERIFY(std::abs(expected(i,j)-actual(i,j))<1e-5f);
        }
        CropVolume crop; crop.enabled = true; crop.radius = 0.5f; crop.height = 2;
        crop.transform.position = {2,3,4}; crop.transform.rotation = {45,20,30}; crop.transform.scale = {2,0.5f,1};
        const auto m = crop.transform.matrix();
        QVERIFY(crop.contains(m.map({0.3f,0.5f,0.2f})));
        QVERIFY(!crop.contains(m.map({0.6f,0,0}))); QVERIFY(!crop.contains(m.map({0,2.2f,0})));
        crop.enabled = false; QVERIFY(crop.contains({100,100,100}));
    }
    void affineScalingRoundtripAndLegacyBaseMigration() {
        Transform transform; transform.position = {1,2,3}; transform.rotation = {20,35,60}; transform.scale = {2,3,0.5f};
        QMatrix4x4 stretch; stretch.scale(1.7f,0.8f,1.2f);
        const auto matrix = stretch*transform.matrix(); const auto restored = Transform::fromMatrix(matrix).matrix();
        for (int i=0; i<4; ++i) for (int j=0; j<4; ++j) QVERIFY(std::abs(matrix(i,j)-restored(i,j))<1e-5f);
        QTemporaryDir dir; Project project; project.asset = dir.filePath("capture.mint"); project.out = 1;
        project.crop().enabled = true; project.crop().height = 2; project.crop().transform = transform;
        auto legacy = project.json(dir.filePath("legacy.vgsproj")); legacy["version"] = 2;
        const auto expectedBase = transform.matrix().map(QVector3D(0,-1,0));
        QFile file(dir.filePath("legacy.vgsproj")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(legacy).toJson()); file.close();
        QString error; Project migrated; QVERIFY2(Project::read(file.fileName(),&migrated,&error),qPrintable(error));
        QVERIFY((migrated.crop().transform.position-expectedBase).length()<1e-5f);
        QVERIFY(migrated.spaces[0]==CoordinateSpace::Global);
    }
    void captureIntegration() {
        const QString path = qEnvironmentVariable("EDITOR_TEST_CAPTURE");
        if (path.isEmpty()) QSKIP("Set EDITOR_TEST_CAPTURE to exercise a real MINT/VGS capture.");
        qRegisterMetaType<FramePtr>(); qRegisterMetaType<CaptureInfo>();
        CaptureWorker worker; QSignalSpy opened(&worker,&CaptureWorker::opened);
        QSignalSpy decoded(&worker,&CaptureWorker::decoded); QSignalSpy failed(&worker,&CaptureWorker::failed);
        worker.open(path,1,true);
        QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(opened.size(),1);
        const auto info = qvariant_cast<CaptureInfo>(opened.first()[0]);
        const auto frame = qvariant_cast<FramePtr>(opened.first()[1]);
        QVERIFY(info.duration>0); QVERIFY(info.frames>0); QVERIFY(!frame->points.empty());
        QCOMPARE(frame->records.size(),size_t(frame->total)); QCOMPARE(frame->active.size(),size_t(frame->total));
        QCOMPARE(frame->sh.size(),size_t(frame->total)*size_t(frame->coefficients)*3);
        // Attributes survive the preview unchanged, including opacity and individual scale.
        bool hasTransparency = false, hasScale = false;
        for (const auto &point : frame->points) {
            const auto &record = frame->records[size_t(point.id)];
            for (int j=0; j<3; ++j) {
                QCOMPARE(point.position[j],record.position[j]);
                QCOMPARE(point.color[j],record.color[j]);
            }
            hasTransparency |= record.color[3] < 0.99f;
            hasScale |= record.scale[0] != record.scale[1];
        }
        QVERIFY(hasTransparency); QVERIFY(hasScale);
        size_t expectedPoints = 0;
        for (size_t i=0; i<frame->records.size(); ++i) {
            const auto &record = frame->records[i];
            bool finite = true;
            for (int j=0; j<3; ++j) finite &= std::isfinite(record.position[j]) && std::isfinite(record.color[j]);
            if (frame->active[i] && finite) ++expectedPoints;
        }
        QCOMPARE(frame->points.size(),expectedPoints); // No opacity or individual scale threshold.
        worker.decode(info.duration*0.5,1,true); QCOMPARE(decoded.size(),1);
        worker.decode(info.duration,1,false); QCOMPARE(decoded.size(),2);
        const auto last = qvariant_cast<FramePtr>(decoded.last()[0]); QVERIFY(last->seconds<info.duration);
        QVERIFY(last->sh.empty());
        worker.open(path+".missing",2,false); QCOMPARE(failed.size(),1);
        worker.decode(0,1,false); QCOMPARE(decoded.size(),3); // Failed opens preserve the old capture.
        worker.clear(); worker.decode(0,1,false); QCOMPARE(failed.size(),2);
    }
};
QTEST_GUILESS_MAIN(EditorTests)
#include "editor_tests.moc"
