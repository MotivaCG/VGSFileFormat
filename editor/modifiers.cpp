#include "project.h"
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <QQuaternion>

// Position, scale and shear linearly; rotation along the shortest quaternion path.
static Transform interpolate(const Transform &a,const Transform &b,float alpha) {
    Transform result;result.position=a.position*(1-alpha)+b.position*alpha;result.scale=a.scale*(1-alpha)+b.scale*alpha;result.shear=a.shear*(1-alpha)+b.shear*alpha;
    const auto rotation=QQuaternion::slerp(QQuaternion::fromRotationMatrix(a.rotationMatrix().normalMatrix()),QQuaternion::fromRotationMatrix(b.rotationMatrix().normalMatrix()),alpha);
    QMatrix4x4 matrix;matrix.rotate(rotation);result.rotation=Transform::fromMatrix(matrix).rotation;return result;
}
Transform TransformAnimation::evaluate(double frame) const {
    if (keys.isEmpty()) return {};
    if (frame<=keys.front().frame) return keys.front().offset;
    if (frame>=keys.back().frame) return keys.back().offset;
    const auto after=std::upper_bound(keys.begin(),keys.end(),frame,[](double time,const TransformKeyframe &key) {return time<key.frame;});
    const float alpha=float((frame-(after-1)->frame)/(after->frame-(after-1)->frame));
    return interpolate((after-1)->offset,after->offset,alpha);
}
CropVolume CropAnimation::evaluate(const CropVolume &base,double frame) const {
    if (keys.isEmpty()) return base;
    auto pose=[&](const CropKeyframe &key) {CropVolume c=base;c.transform=key.transform;c.radius=key.radius;c.radiusZ=key.radiusZ;c.height=key.height;c.width=key.width;c.depth=key.depth;return c;};
    if (frame<=keys.front().frame) return pose(keys.front());
    if (frame>=keys.back().frame) return pose(keys.back());
    const auto after=std::upper_bound(keys.begin(),keys.end(),frame,[](double time,const CropKeyframe &key) {return time<key.frame;});
    const auto &a=*(after-1),&b=*after;const float alpha=float((frame-a.frame)/(b.frame-a.frame));
    auto mix=[&](float x,float y) {return x*(1-alpha)+y*alpha;};
    CropVolume c=base;c.transform=interpolate(a.transform,b.transform,alpha);
    c.radius=mix(a.radius,b.radius);c.radiusZ=mix(a.radiusZ,b.radiusZ);c.height=mix(a.height,b.height);c.width=mix(a.width,b.width);c.depth=mix(a.depth,b.depth);
    return c;
}
void CropAnimation::setKey(int frame,const CropVolume &crop) {
    const CropKeyframe value{frame,crop.transform,crop.radius,crop.radiusZ,crop.height,crop.width,crop.depth};
    auto key=std::lower_bound(keys.begin(),keys.end(),frame,[](const CropKeyframe &key,int f) {return key.frame<f;});
    if (key!=keys.end() && key->frame==frame) *key=value;
    else keys.insert(key,value);
}
void CropAnimation::removeKey(int frame) {keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const auto &key) {return key.frame==frame;}),keys.end());}
QJsonArray CropAnimation::json() const {
    auto vector=[](QVector3D v) {return QJsonArray{v.x(),v.y(),v.z()};};QJsonArray result;
    for (const auto &key:keys) result.append(QJsonObject{{"frame",key.frame},{"position",vector(key.transform.position)},{"rotation",vector(key.transform.rotation)},
        {"scale",vector(key.transform.scale)},{"shear",vector(key.transform.shear)},{"radius",key.radius},{"radiusZ",key.radiusZ},{"height",key.height},{"width",key.width},{"depth",key.depth}});
    return result;
}
CropVolume Modifier::staticCrop() const {
    if (!cropAnimation.animated) return crop;
    CropVolume c=crop;const auto &s=cropAnimation.still;c.transform=s.transform;
    c.radius=s.radius;c.radiusZ=s.radiusZ;c.height=s.height;c.width=s.width;c.depth=s.depth;return c;
}
QVector<Modifier> Project::modifiersAtFrame(double frame) const {
    auto result=modifiers;
    for (auto &m:result) if (m.type==ModifierType::Crop && m.cropAnimation.active()) m.crop=m.cropAnimation.evaluate(m.crop,frame);
    return result;
}
bool Project::hasAnimatedCrop() const {for (const auto &m:modifiers) if (m.active() && m.type==ModifierType::Crop && m.cropAnimation.active()) return true;return false;}
void Project::showCropsAtFrame(double frame) {
    for (auto &m:modifiers) if (m.type==ModifierType::Crop && m.cropAnimation.active()) m.crop=m.cropAnimation.evaluate(m.crop,frame);
}
void TransformAnimation::setKey(int frame,const Transform &offset) {
    auto key=std::lower_bound(keys.begin(),keys.end(),frame,[](const TransformKeyframe &key,int f) {return key.frame<f;});
    if (key!=keys.end() && key->frame==frame) key->offset=offset;
    else keys.insert(key,{frame,offset});
}
void TransformAnimation::removeKey(int frame) {keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const auto &key) {return key.frame==frame;}),keys.end());}
QJsonArray TransformAnimation::json() const {
    auto vector=[](QVector3D v) {return QJsonArray{v.x(),v.y(),v.z()};};QJsonArray result;
    for (const auto &key:keys) result.append(QJsonObject{{"frame",key.frame},{"position",vector(key.offset.position)},{"rotation",vector(key.offset.rotation)},{"scale",vector(key.offset.scale)},{"shear",vector(key.offset.shear)}});
    return result;
}
QMatrix4x4 Project::animationMatrix(double frame) const {
    QMatrix4x4 result;for (const auto &modifier:modifiers) if (modifier.active() && modifier.type==ModifierType::AnimateTransform) result*=modifier.animation.evaluate(frame).matrix();return result;
}
Transform Project::transformAtFrame(double frame) const {return hasAnimation() ? Transform::fromMatrix(transform.matrix()*animationMatrix(frame)) : transform;}
double Project::walkSpeed() const {double speed=0;for (const auto &m:modifiers) if (m.active() && m.type==ModifierType::Walk) speed+=m.walkSpeed;return speed;}
double Project::walkDistance(double seconds) const {return walkSpeed()*(seconds-in);}
bool Project::hasAnimation() const {for (const auto &m:modifiers) if (m.active() && m.type==ModifierType::AnimateTransform && !m.animation.keys.isEmpty()) return true;return false;}
bool Project::hasAnimatedMotion() const {
    for (const auto &m:modifiers) if (m.active() && m.type==ModifierType::AnimateTransform && m.animation.keys.size()>1) {
        const auto first=m.animation.keys.front().offset.matrix();for (const auto &key:m.animation.keys) {const auto next=key.offset.matrix();for (int i=0;i<16;++i) if (std::abs(first.constData()[i]-next.constData()[i])>1e-6f) return true;}
    }return false;
}
void Project::setAnimatedPose(int frame,const Transform &pose) {
    auto *selected=modifier();if (!selected || selected->type!=ModifierType::AnimateTransform) return;
    QMatrix4x4 prefix=transform.matrix(),suffix;bool after=false;
    for (const auto &m:modifiers) {
        if (m.id==selectedModifier) {after=true;continue;}
        if (m.active() && m.type==ModifierType::AnimateTransform) {if (after) suffix*=m.animation.evaluate(frame).matrix();else prefix*=m.animation.evaluate(frame).matrix();}
    }
    selected->animation.setKey(frame,Transform::fromMatrix(prefix.inverted()*pose.matrix()*suffix.inverted()));
}

QString Project::newId() {return QUuid::createUuid().toString(QUuid::WithoutBraces);}
Project::Project() {
    Modifier m;m.id=newId();m.name="Crop";modifiers.append(m);selectedModifier=m.id;
}
Modifier *Project::modifier() {
    for (auto &m:modifiers) if (m.id==selectedModifier) return &m;return nullptr;
}
const Modifier *Project::modifier() const {return const_cast<Project *>(this)->modifier();}
CropVolume &Project::crop() {auto *m=modifier();return m && m->type==ModifierType::Crop ? m->crop : inactiveCrop_;}
const CropVolume &Project::crop() const {return const_cast<Project *>(this)->crop();}
bool GreenFilter::matches(const QVector3D &rgb) const {
    auto channel=[&](float v) {v=std::clamp(v,0.f,1.f);return linearRgb ? (v<=.04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f)) : v;};
    const float r=channel(rgb.x()),g=channel(rgb.y()),b=channel(rgb.z());
    const float maximum=std::max({r,g,b}),minimum=std::min({r,g,b}),chroma=maximum-minimum;
    if (chroma<=0 || maximum<=0 || chroma/maximum<minimumSaturation) return false;
    float hue=maximum==r ? (g-b)/chroma : maximum==g ? 2+(b-r)/chroma : 4+(r-g)/chroma;
    hue*=60;if (hue<0) hue+=360;
    const float distance=std::abs(hue-120);
    return std::min(distance,360-distance)<=hueTolerance;
}
CompiledModifiers::CompiledModifiers(const Project &project):CompiledModifiers(project.modifiers) {}
CompiledModifiers::CompiledModifiers(const QVector<Modifier> &modifiers) {
    for (const auto &modifier:modifiers) if (modifier.active()) {
        if (modifier.type==ModifierType::RemoveGreen) greens.append(modifier.green);
        else if (modifier.type==ModifierType::PurgeIsolated) isolations.append(modifier.isolation);
        else if (modifier.type==ModifierType::Crop) {
            bool valid=false;auto inverse=modifier.crop.transform.matrix().inverted(&valid);
            if (!valid) throw std::runtime_error("A crop modifier has a singular transform.");
            crops.append({inverse,modifier.crop});
        }
    }
}
// Kept when inside some Keep crop (or when there is none) and inside no Remove crop:
// removing wins where the two overlap.
bool CompiledModifiers::keepsPosition(const QVector3D &world) const {
    bool anyKeep=false,insideKeep=false;
    for (const auto &crop:crops) {
        const auto p=crop.inverse.map(world);const auto &c=crop.volume;
        const bool inside=p.y()>=0 && p.y()<=c.height && (c.shape==CropShape::Box ? std::abs(p.x())<=c.width*.5f && std::abs(p.z())<=c.depth*.5f : c.insideEllipse(p.x(),p.z()));
        if (c.remove) {if (inside) return false;}
        else {anyKeep=true;insideKeep|=inside;}
    }
    return !anyKeep || insideKeep;
}
bool CompiledModifiers::removesColour(const QVector3D &rgb) const {for (const auto &filter:greens) if (filter.matches(rgb)) return true;return false;}
QJsonArray Project::modifierJson() const {
    auto vector=[](const QVector3D &v) {return QJsonArray{v.x(),v.y(),v.z()};};QJsonArray result;
    for (const auto &m:modifiers) {
            const char *type="crop";
            switch (m.type) {
            case ModifierType::Crop: break;
            case ModifierType::RemoveGreen: type="remove-green";break;
            case ModifierType::AnimateTransform: type="animate-transform";break;
            case ModifierType::PurgeIsolated: type="purge-isolated";break;
            case ModifierType::Walk: type="walk";break;
            }
            QJsonObject item{{"id",m.id},{"name",m.name},{"enabled",m.active()},{"type",type},{"timeline","full"}};
            if (m.type==ModifierType::Crop) {const auto c=m.staticCrop();item["crop"]=QJsonObject{{"space","world"},{"shape",c.shape==CropShape::Box ? "box" : "cylinder"},
                {"radius",c.radius},{"radiusZ",c.radiusZ},{"height",c.height},{"width",c.width},{"depth",c.depth},{"position",vector(c.transform.position)},
                {"rotation",vector(c.transform.rotation)},{"scale",vector(c.transform.scale)},{"shear",vector(c.transform.shear)},
                {"mode",c.remove ? "remove" : "keep"},{"editPreview",c.showRemovedInRed ? "red" : "hide"}};
                // Kept while static too, so switching back to animated loses nothing.
                if (m.cropAnimation.animated || !m.cropAnimation.keys.isEmpty()) {auto crop=item["crop"].toObject();
                    crop["animation"]=QJsonObject{{"mode",m.cropAnimation.animated ? "animated" : "static"},{"interpolation","linear-slerp"},{"keys",m.cropAnimation.json()}};item["crop"]=crop;}}
            else if (m.type==ModifierType::RemoveGreen) item["green"]=QJsonObject{{"minimumSaturation",m.green.minimumSaturation},{"hueTolerance",m.green.hueTolerance},{"targetHue",120},{"colourSource","dc"},{"colourSpace",m.green.linearRgb ? "linear-rgb" : "srgb"}};
            else if (m.type==ModifierType::AnimateTransform) item["animation"]=QJsonObject{{"space","reference-offset"},{"interpolation","linear-slerp"},{"keys",m.animation.json()}};
            else if (m.type==ModifierType::Walk) item["walk"]=QJsonObject{{"speed",m.walkSpeed},{"axis","+z"},{"units","m/s"},{"display",m.walkKmh ? "km/h" : "m/s"}};
            else if (m.type==ModifierType::PurgeIsolated) item["isolation"]=QJsonObject{{"neighbour",m.isolation.neighbour},{"medianPercent",m.isolation.medianPercent}};
            result.append(item);
    }
    return result;
}
