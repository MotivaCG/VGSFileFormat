#include "captureworker.h"
#include "project.h"
#include "filehistory.h"
#include "presetstore.h"
#include "isolation.h"
#include "pruning.h"
#include "displayscaling.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class EditorTests : public QObject {
    Q_OBJECT
private slots:
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
        // What each filter did: asked for five, the protection let four go.
        std::vector<PruneStats> stats;pruneKeep(scores,{{50,0.25},{20,0.25}},&stats);QCOMPARE(stats.size(),size_t(2));
        QCOMPARE(stats[0].records,size_t(10));QCOMPARE(stats[0].asked,size_t(5));QCOMPARE(stats[0].removed,size_t(4));QVERIFY(stats[0].limited());
        QCOMPARE(stats[1].asked,size_t(2));QCOMPARE(stats[1].removed,size_t(2));QVERIFY(!stats[1].limited());
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
