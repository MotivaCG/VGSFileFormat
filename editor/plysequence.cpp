// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#include "plysequence.h"
#include "previewmemory.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
// A chunk runs while frames stay compatible, up to the format's 255 intervals, and is
// cut where fitting it at export (its records' samples) would exceed kFittingBytes or
// the memory free when first indexed, less 2 GiB left to the rest of the computer; a
// saved layout reopens as it was.
constexpr int kMaxIntervals=255;
constexpr quint64 kFittingBytes=12*previewmemory::GiB;
[[noreturn]] void fail(const QString &message) {throw std::runtime_error(message.toStdString());}
void budget(quint64 bytes) {
    previewmemory::Guard guard;const auto allowed=guard.inspect();
    if (bytes>512*previewmemory::MiB || (previewmemory::systemSnapshot().valid && (allowed.paused || bytes>allowed.growth)))
        fail(QStringLiteral("Not enough free memory to import this PLY safely (%1 MiB working buffer).").arg(bytes/previewmemory::MiB));
}
int scalarBytes(const QByteArray &type) {
    if (type=="char" || type=="uchar" || type=="int8" || type=="uint8") return 1;
    if (type=="short" || type=="ushort" || type=="int16" || type=="uint16") return 2;
    if (type=="int" || type=="uint" || type=="int32" || type=="uint32" || type=="float" || type=="float32") return 4;
    if (type=="double" || type=="float64") return 8;
    return 0;
}
// kind: the scalar type, decided once from the header ('f' float, 'd' double, 'b'/'B' 8, 'h'/'H' 16, 'i'/'I' 32 bits).
struct Property {QByteArray name,type;int offset=0;char kind=0;};
char scalarKind(const QByteArray &type) {
    if (type=="float" || type=="float32") return 'f';if (type=="double" || type=="float64") return 'd';
    if (type=="char" || type=="int8") return 'b';if (type=="uchar" || type=="uint8") return 'B';
    if (type=="short" || type=="int16") return 'h';if (type=="ushort" || type=="uint16") return 'H';
    return type=="int" || type=="int32" ? 'i' : 'I';
}
class Ply {
public:
    QFile file;std::vector<Property> properties;size_t count=0;int stride=0,degree=0,coefficients=0;
    QByteArray schema;bool ascii=false,big=false;uchar *mapped=nullptr;std::vector<double> text;
    explicit Ply(const QString &path) : file(path) {
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read PLY %1: %2").arg(path,file.errorString()));
        auto error=[&](const QString &reason) {fail(QStringLiteral("%1: %2").arg(path,reason));};
        if (file.readLine(16).trimmed()!="ply") error("Missing PLY signature.");
        bool ended=false,vertex=false,format=false;size_t headerBytes=0;
        while (!file.atEnd()) {
            const auto line=file.readLine(65538);headerBytes+=size_t(line.size());if (headerBytes>65536) error("PLY header exceeds 64 KiB.");
            const auto words=line.simplified().split(' ');if (words.isEmpty()) continue;
            if (words[0]=="format") {if (words.size()!=3 || words[2]!="1.0") error("Unsupported PLY format.");
                ascii=words[1]=="ascii";big=words[1]=="binary_big_endian";format=ascii || big || words[1]=="binary_little_endian";}
            else if (words[0]=="element") {
                if (words.size()!=3) error("Invalid PLY element.");
                if (words[1]=="vertex") {if (vertex || !properties.empty()) error("Duplicate vertex element.");vertex=true;bool ok=false;const auto n=words[2].toULongLong(&ok);
                    if (!ok || n>10000000) error("PLY vertex count exceeds 10 million.");count=size_t(n);}
                else if (!vertex) error("The vertex element must come first.");else breakElement_=true;
            } else if (words[0]=="property" && !breakElement_) {
                if (!vertex || words.size()!=3 || !scalarBytes(words[1])) error("Unsupported vertex property (lists are not Gaussian attributes).");
                if (index(words[2])>=0) error("Duplicate vertex property.");
                properties.push_back({words[2],words[1],stride,scalarKind(words[1])});stride+=scalarBytes(words[1]);schema+=line.trimmed()+'\n';
                if (stride>4096) error("Too many vertex properties.");
            } else if (words[0]=="end_header") {ended=true;break;}
        }
        if (!ended || !format || !vertex || !stride) error("Incomplete or unsupported PLY header.");
        for (const auto *name:{"x","y","z"}) if (index(name)<0) error(QStringLiteral("Missing %1.").arg(name));
        int rest=0;for (const auto &p:properties) if (p.name.startsWith("f_rest_")) ++rest;
        if (rest!=0 && rest!=9 && rest!=24 && rest!=45) error("SH requires 0, 9, 24 or 45 f_rest fields.");
        for (int i=0;i<rest;++i) if (index("f_rest_"+QByteArray::number(i))<0) error("Missing SH coefficient.");
        coefficients=rest/3;degree=rest==45 ? 3 : rest==24 ? 2 : rest==9 ? 1 : 0;
        if (index("f_dc_0")>=0) for (const auto *name:{"f_dc_1","f_dc_2","opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3"})
            if (index(name)<0) error(QStringLiteral("Incomplete Gaussian PLY: missing %1.").arg(name));
        if (ascii) {
            budget(count*properties.size()*sizeof(double));text.reserve(count*properties.size());
            for (size_t row=0;row<count;++row) {const auto line=file.readLine(131074);if (line.size()>131072) error("ASCII vertex exceeds 128 KiB.");const auto words=line.simplified().split(' ');if (words.size()!=qsizetype(properties.size())) error("Truncated or invalid ASCII vertex.");
                for (const auto &word:words) {bool ok=false;const double v=word.toDouble(&ok);if (!ok || !std::isfinite(v)) error("Invalid numeric vertex value.");text.push_back(v);}}
        } else {
            const quint64 bytes=quint64(count)*quint64(stride);if (bytes>quint64(file.size()-file.pos())) error("Truncated vertex data.");
            if (bytes) {mapped=file.map(file.pos(),qint64(bytes));if (!mapped) error("Cannot map PLY vertex data.");}
        }
    }
    int index(const QByteArray &name) const {for (size_t i=0;i<properties.size();++i) if (properties[i].name==name) return int(i);return -1;}
    double value(size_t row,int field) const {
        if (ascii) return text[row*properties.size()+size_t(field)];
        const auto &p=properties[size_t(field)];const auto *at=mapped+row*size_t(stride)+size_t(p.offset);
        auto word16=[&] {return big ? qFromBigEndian<quint16>(at) : qFromLittleEndian<quint16>(at);};
        auto word32=[&] {return big ? qFromBigEndian<quint32>(at) : qFromLittleEndian<quint32>(at);};
        switch (p.kind) {
        case 'f': {const auto bits=word32();float v;std::memcpy(&v,&bits,4);return v;}
        case 'd': {const auto bits=big ? qFromBigEndian<quint64>(at) : qFromLittleEndian<quint64>(at);double v;std::memcpy(&v,&bits,8);return v;}
        case 'b': return qint8(*at);
        case 'B': return *at;
        case 'h': return qint16(word16());
        case 'H': return word16();
        case 'i': return double(qint32(word32()));
        default: return double(word32());
        }
    }
    vgs::Frame frame(bool sh) const {
        budget(count*(64+quint64(sh ? coefficients*12 : 0)));vgs::Frame f;f.count=count;f.shCoefficients=sh ? coefficients : 0;
        f.position.resize(count*3);f.rotation.resize(count*4);f.scale.resize(count*3);f.colorDc.resize(count*3);f.opacity.resize(count);f.active.resize(count);
        f.shRest.resize(count*size_t(f.shCoefficients)*3);
        const int xyz[]={index("x"),index("y"),index("z")},dc[]={index("f_dc_0"),index("f_dc_1"),index("f_dc_2")};
        const int rgb[]={index("red"),index("green"),index("blue")},scale[]={index("scale_0"),index("scale_1"),index("scale_2")};
        const int rot[]={index("rot_0"),index("rot_1"),index("rot_2"),index("rot_3")},opacity=index("opacity");
        std::vector<int> rest;for (int i=0;i<coefficients*3;++i) rest.push_back(index("f_rest_"+QByteArray::number(i)));
        for (size_t row=0;row<count;++row) {
            const double visibility=opacity<0 ? 1. : 1/(1+std::exp(-value(row,opacity)));
            if (!std::isfinite(visibility)) fail("Invalid PLY opacity.");
            if (visibility<=1./255) {f.opacity[row]=0;f.rotation[row*4+3]=1;continue;}
            for (int c=0;c<3;++c) {f.position[row*3+c]=float(value(row,xyz[c]));
                f.scale[row*3+c]=scale[c]<0 ? .002f : float(std::exp(value(row,scale[c])));
                f.colorDc[row*3+c]=dc[c]>=0 ? float(.5+.28209479177387814*value(row,dc[c])) : rgb[c]>=0 ? float(value(row,rgb[c])/255.) : .5f;}
            f.opacity[row]=opacity<0 ? 1.f : float(1/(1+std::exp(-value(row,opacity))));
            double norm=0;for (int c=0;c<4;++c) {const double v=rot[c]<0 ? (c==0 ? 1. : 0.) : value(row,rot[c]);norm+=v*v;f.rotation[row*4+size_t((c+3)%4)]=float(v);}
            if (!(norm>1e-30) || !std::isfinite(norm)) fail(QStringLiteral("%1: invalid quaternion in row %2.").arg(file.fileName()).arg(row));
            for (int c=0;c<4;++c) f.rotation[row*4+c]/=float(std::sqrt(norm));
            for (int k=0;k<f.shCoefficients;++k) for (int c=0;c<3;++c) f.shRest[(row*size_t(f.shCoefficients)+size_t(k))*3+size_t(c)]=float(value(row,rest[size_t(c*coefficients+k)]));
            for (int c=0;c<3;++c) if (!std::isfinite(f.position[row*3+c]) || !std::isfinite(f.scale[row*3+c]) || !std::isfinite(f.colorDc[row*3+c]))
                fail(QStringLiteral("%1: non-finite vertex in row %2.").arg(file.fileName()).arg(row));
            if (!std::isfinite(f.opacity[row])) fail("Invalid PLY opacity.");
            f.active[row]=f.opacity[row]>1.f/255 && std::max({f.scale[row*3],f.scale[row*3+1],f.scale[row*3+2]})>1e-4f;
        }
        for (float v:f.shRest) if (!std::isfinite(v)) fail("Non-finite PLY SH coefficient.");
        return f;
    }
private: bool breakElement_=false;
};
PlySequenceSpec discover(const QString &first) {
    PlySequenceSpec s;const QFileInfo file(first);QDir dir=file.dir();
    const bool folders=file.fileName().compare("splat.ply",Qt::CaseInsensitive)==0 && QRegularExpression("^[0-9]+$").match(dir.dirName()).hasMatch();
    const QString stem=folders ? dir.dirName() : file.completeBaseName();if (folders) dir.cdUp();
    const auto match=QRegularExpression("^(.*?)([0-9]+)$").match(stem);
    qint64 number=0,origin=0;
    if (match.hasMatch()) {
        bool ok=false;number=match.captured(2).toLongLong(&ok);if (!ok) fail("PLY frame number exceeds 64 bits.");
        const auto prefix=match.captured(1);const int digits=match.capturedLength(2);
        auto framePath=[&](qint64 n) {const auto name=prefix+QString::number(n).rightJustified(digits,'0');return dir.filePath(folders ? name+"/"+file.fileName() : name+'.'+file.suffix());};
        origin=number;
        while (origin>0 && QFileInfo::exists(framePath(origin-1))) --origin;
        for (qint64 n=number;;++n) {
            const auto index=QString::number(n);if (index.size()>digits) break;
            const auto path=framePath(n);
            if (!QFileInfo::exists(path)) break;
            if (!QFileInfo(path).isFile()) fail(QStringLiteral("PLY sequence entry is not a file: %1").arg(path));
            s.files.append(path);s.numbers.append(n);if (s.files.size()>1000000) fail("PLY sequence exceeds one million frames.");
            if (n==std::numeric_limits<qint64>::max()) break;
        }
    } else {s.files={file.absoluteFilePath()};s.numbers={0};}
    if (s.files.isEmpty()) fail("The selected PLY does not exist.");
    const auto events=dir.filePath("birth_events.tsv");if (QFileInfo::exists(events)) s.births=events;
    s.birthOrigin=origin;
    // Optional Forge manifest. The numbered files remain the authority for where
    // the contiguous sequence ends; a manifest never bridges a missing PLY.
    QDir ancestor=dir;
    for (int level=0;level<4;++level) {
        QFile manifest(ancestor.filePath("sequence.json"));
        if (manifest.exists()) {
            if (!manifest.open(QIODevice::ReadOnly) || manifest.size()>16*1024*1024) fail("Cannot read sequence.json safely.");
            QJsonParseError parse;const auto document=QJsonDocument::fromJson(manifest.readAll(),&parse);
            if (parse.error!=QJsonParseError::NoError || !document.isObject()) fail("Malformed sequence.json.");
            std::map<qint64,QString> labels;QString firstChunk;
            qint64 firstNumber=std::numeric_limits<qint64>::max(),lastNumber=-1;
            for (const auto &entry:document.object()["frames"].toArray()) {const auto object=entry.toObject();if (!object["frame"].isDouble() || !object["chunk"].isString()) continue;
                const qint64 n=qint64(object["frame"].toDouble());labels[n]=object["chunk"].toString();lastNumber=std::max(lastNumber,n);if (n<firstNumber) {firstNumber=n;firstChunk=labels[n];}}
            // Forge publishes the frames decoded from its .vgs, the shown splats alone, so a row
            // is not one splat over time there. Each run (one .vgs) keeps its aligned frames, every
            // row in every frame, and their birth log in .work/vgs_<first frame>, if work is kept.
            QString run=firstChunk,encoded;qint64 runFirst=firstNumber,runLast=lastNumber;
            for (const auto &entry:document.object()["vgs"].toArray()) {
                const auto range=QRegularExpression("^([0-9]+)_([0-9]+)\\.vgs$").match(entry.toString());if (!range.hasMatch()) continue;
                const qint64 a=range.captured(1).toLongLong(),b=range.captured(2).toLongLong();
                if (a<=s.numbers.first() && s.numbers.first()<=b) {run=range.captured(1);encoded=entry.toString();runFirst=a;runLast=b;}
            }
            const QDir aligned(ancestor.filePath(".work/vgs_"+run));
            if (!run.isEmpty() && dir==ancestor && aligned.exists("birth_events.tsv")) {
                QStringList files;QVector<qint64> numbers;
                for (int i=0;i<s.numbers.size() && s.numbers[i]<=runLast;++i) {const auto path=aligned.filePath(QFileInfo(s.files[i]).fileName());if (!QFileInfo::exists(path)) break;files.append(path);numbers.append(s.numbers[i]);}
                if (files.isEmpty()) fail(QStringLiteral("%1 holds none of the selected frames.").arg(aligned.path()));
                s.files=files;s.numbers=numbers;s.births=aligned.filePath("birth_events.tsv");s.birthOrigin=runFirst;
            } else if (dir==ancestor && !encoded.isEmpty() && QFileInfo::exists(ancestor.filePath(encoded)))
                fail(QStringLiteral("These PLYs are the frames Forge decoded from %1, without splat identities: importing them would encode each frame on its own. "
                                    "Open %1 instead, or import the aligned frames Forge keeps with -graciakeepwork (.work/vgs_%2).").arg(encoded,run));
            QString previous;
            for (int i=0;i<s.numbers.size();++i) {auto it=labels.find(s.numbers[i]);if (it==labels.end()) continue;if (i==0 || it->second!=previous) s.chunks.append(i);previous=it->second;}
            if (!s.chunks.isEmpty() && s.chunks.first()!=0) s.chunks.prepend(0);
            break;
        }
        if (!ancestor.cdUp()) break;
    }
    return s;
}
struct Event {qint64 frame;quint32 row,generation;};
std::vector<Event> events(PlySequenceSpec &spec) {
    std::vector<Event> result;if (spec.births.isEmpty()) return result;
    QFile file(spec.births);if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read birth events: %1").arg(spec.births));
    qint64 line=0,maxFrame=-1;
    while (!file.atEnd()) {
        const auto words=file.readLine(4098).simplified().split(' ');++line;if (words.size()==1 && words[0].isEmpty()) continue;
        if (words.size()!=3) fail(QStringLiteral("Malformed birth_events.tsv at line %1.").arg(line));
        bool a=false,b=false,c=false;const auto frame=words[0].toLongLong(&a);const auto row=words[1].toULongLong(&b),generation=words[2].toULongLong(&c);
        if (!a || !b || !c || frame<0 || row>UINT32_MAX || generation>UINT32_MAX || !generation)
            fail(QStringLiteral("Invalid birth_events.tsv at line %1.").arg(line));
        if (result.size()==result.capacity()) budget(std::max<size_t>(1,result.capacity()*2)*sizeof(Event));
        result.push_back({frame,quint32(row),quint32(generation)});maxFrame=std::max(maxFrame,frame);
        if (result.size()>16000000) fail("Birth event log exceeds 16 million entries.");
    }
    std::stable_sort(result.begin(),result.end(),[](const Event &a,const Event &b) {return a.frame<b.frame;});
    Q_UNUSED(maxFrame); // Both Spirula and Forge logs use local indices from the run's first file.
    return result;
}
}

struct PlySequence::State {
    PlySequenceSpec spec;QVector<Chunk> chunks;QVector<qint64> recordOffsets;QTemporaryFile records;
    int degree=0;double fps=30,bounds[6]={0,0,0,0,0,0};
};
PlySequence::~PlySequence()=default;
PlySequence::PlySequence(const QString &first,const PlySequenceSpec &saved,double fps,const Progress &progress) : d(new State) {
    setFps(fps);d->spec=saved.empty() ? discover(first) : saved;const auto fixed=d->spec.chunks;d->spec.chunks.clear();
    quint64 fitting=std::numeric_limits<quint64>::max();
    if (saved.chunks.isEmpty()) {const auto memory=previewmemory::systemSnapshot();const quint64 spare=2*previewmemory::GiB;
        fitting=std::min(kFittingBytes,memory.valid && memory.physicalAvailable>spare ? memory.physicalAvailable-spare : 0);}
    if (!d->records.open()) fail("Cannot create the PLY identity spool.");
    auto births=events(d->spec);size_t nextEvent=0;
    std::vector<quint32> generations;
    std::vector<Record> records;std::vector<int> current;std::vector<double> previousScale;std::vector<uint8_t> previousActive;QByteArray previousSchema;
    size_t previousCount=0;int chunkStart=0,chunkDegree=0;bool bounds=false;
    auto flush=[&](int end) {if (end==chunkStart) return;
        const qint64 offset=d->records.pos(),bytes=qint64(records.size()*sizeof(Record));
        if (d->records.write(reinterpret_cast<const char *>(records.data()),bytes)!=bytes) fail("Cannot spool PLY identities.");
        d->chunks.append({chunkStart,end-chunkStart,records.size(),chunkDegree});d->recordOffsets.append(offset);d->spec.chunks.append(chunkStart);records.clear();chunkStart=end;
    };
    for (int frame=0;frame<d->spec.files.size();++frame) {
        if (progress) progress(frame,d->spec.files.size(),d->spec.files[frame]);
        Ply ply(d->spec.files[frame]);auto source=ply.frame(false);d->degree=std::max(d->degree,ply.degree);
        if (d->spec.rowOffset>int(source.count)) fail("Birth row offset exceeds the PLY population.");
        generations.resize(size_t(source.count),0);std::vector<uint8_t> born(size_t(source.count),0);size_t birthCount=0;
        const qint64 number=d->spec.numbers[frame]-d->spec.birthOrigin;
        while (nextEvent<births.size() && births[nextEvent].frame<=number) {
            const auto &event=births[nextEvent++];const quint64 row=quint64(event.row)+quint64(d->spec.rowOffset);
            if (row>=source.count) fail(QStringLiteral("Birth row %1 is outside %2 (%3 rows). A birth log goes with frames that hold every row, "
                                                       "as Forge's .work/vgs_* and spirula's frames/ do; otherwise check the room row offset.").arg(row).arg(d->spec.files[frame]).arg(source.count));
            auto &generation=generations[quint32(row)];if (event.generation<=generation) fail("Birth generations must increase for each row.");generation=event.generation;
            if (event.frame==number && !born[size_t(row)]) {born[size_t(row)]=1;++birthCount;}
        }
        const quint64 perFrame=std::max<quint64>(1,source.count)*(48+quint64(ply.coefficients*12))*2;
        const int maxFrames=int(std::clamp<quint64>(fitting/perFrame,1,kMaxIntervals));
        // Compare the stored log scales before activation: exponentiation to float
        // can hide small changes that still denote a new splat identity.
        const int scaleFields[]={ply.index("scale_0"),ply.index("scale_1"),ply.index("scale_2")};
        std::vector<double> scales(source.count*3);
        for (size_t row=0;row<source.count;++row) if (source.active[row]) for (int c=0;c<3;++c)
            scales[row*3+c]=scaleFields[c]<0 ? 0. : ply.value(row,scaleFields[c]);
        size_t changedRows=0,liveRows=0;
        if (frame>chunkStart && source.count==previousCount) for (size_t row=0;row<source.count;++row) {
            if (!source.active[row]) continue;++liveRows;bool changed=born[row] || !previousActive[row];
            for (int c=0;c<3;++c) changed |= scales[row*3+c]!=previousScale[row*3+c];changedRows+=changed;
        }
        const quint64 projectedWork=(records.size()+changedRows)*quint64(frame-chunkStart+2)*(44+quint64(ply.coefficients*12))*2;
        bool split=frame>0 && (source.count!=previousCount || ply.schema!=previousSchema || frame-chunkStart>=maxFrames ||
            projectedWork>fitting || (liveRows>0 && changedRows==liveRows) ||
            (!fixed.isEmpty() && fixed.contains(frame)) || (birthCount>0 && birthCount>=source.count-quint64(d->spec.rowOffset)));
        // A chunk's closing sample is the next chunk's first frame: a splat still there,
        // unchanged, runs into it, so the last interval moves towards where it goes.
        if (split && source.count==previousCount && ply.schema==previousSchema)
            for (size_t row=0;row<source.count;++row) {
                if (current[row]<0 || !source.active[row] || born[row]) continue;
                bool same=true;for (int c=0;c<3;++c) same &= scales[row*3+c]==previousScale[row*3+c];
                if (same) records[size_t(current[row])].last=frame;
            }
        if (split) flush(frame);
        if (frame==chunkStart) {chunkDegree=ply.degree;current.assign(size_t(source.count),-1);previousActive.assign(size_t(source.count),0);previousScale=scales;}
        budget(source.count*24+records.size()*sizeof(Record));
        for (size_t row=0;row<source.count;++row) {
            if (source.active[row]) {
                bool changed=born[row] || !previousActive[row];
                for (int c=0;c<3;++c) changed |= scales[row*3+c]!=previousScale[row*3+c];
                if (changed || current[row]<0) {current[row]=int(records.size());records.push_back({quint32(row),generations[quint32(row)],frame,frame});}
                else records[size_t(current[row])].last=frame;
                for (int c=0;c<3;++c) {const double v=source.position[row*3+c];if (!bounds) d->bounds[c]=d->bounds[c+3]=v;else {d->bounds[c]=std::min(d->bounds[c],v);d->bounds[c+3]=std::max(d->bounds[c+3],v);}}bounds=true;
            } else current[row]=-1;
        }
        previousCount=size_t(source.count);previousSchema=ply.schema;previousActive=std::move(source.active);previousScale=std::move(scales);
    }
    flush(d->spec.files.size());
    if (!saved.empty() && !fixed.isEmpty() && fixed!=d->spec.chunks) fail("The saved PLY chunk layout no longer matches the source files.");
    if (progress) progress(frames(),frames(),{});
}
const PlySequenceSpec &PlySequence::spec() const {return d->spec;}
const QVector<PlySequence::Chunk> &PlySequence::chunks() const {return d->chunks;}
int PlySequence::frames() const {return d->spec.files.size();}
int PlySequence::degree() const {return d->degree;}
double PlySequence::fps() const {return d->fps;}
void PlySequence::setFps(double fps) {if (!std::isfinite(fps) || fps<1 || fps>1000) fail("PLY frame rate must be between 1 and 1000 fps.");d->fps=fps;}
const double *PlySequence::bounds() const {return d->bounds;}
size_t PlySequence::chunkAt(double seconds) const {const int frame=std::clamp(int(std::floor(seconds*d->fps+1e-6)),0,frames()-1);
    auto it=std::upper_bound(d->chunks.begin(),d->chunks.end(),frame,[](int n,const Chunk &c) {return n<c.first;});return size_t(it-d->chunks.begin()-1);}
std::vector<PlySequence::Record> PlySequence::records(size_t chunk) const {
    const auto &c=d->chunks.at(qsizetype(chunk));budget(c.records*sizeof(Record));std::vector<Record> out(c.records);
    const qint64 bytes=qint64(out.size()*sizeof(Record));if (!d->records.seek(d->recordOffsets.at(qsizetype(chunk))) || d->records.read(reinterpret_cast<char *>(out.data()),bytes)!=bytes) fail("Cannot read spooled PLY identities.");return out;
}
vgs::Frame PlySequence::sourceFrame(int index,bool sh) const {return Ply(d->spec.files.at(index)).frame(sh);}
vgs::Frame PlySequence::frame(double seconds,bool sh) {
    const int index=std::clamp(int(std::floor(seconds*d->fps+1e-6)),0,frames()-1);auto input=sourceFrame(index,sh);const size_t ci=chunkAt(seconds);const auto identities=records(ci);
    vgs::Frame out;out.seconds=seconds;out.chunkIndex=int(ci);out.sampleIndex=index-d->chunks[qsizetype(ci)].first;out.count=identities.size();out.shCoefficients=sh ? (d->degree==3 ? 15 : d->degree==2 ? 8 : d->degree==1 ? 3 : 0) : 0;
    budget((out.count+input.count)*(64+quint64(out.shCoefficients*12)));
    const size_t n=size_t(out.count);out.position.resize(n*3);out.rotation.resize(n*4);out.scale.resize(n*3);out.colorDc.resize(n*3);out.opacity.resize(n);out.active.resize(n);out.shRest.resize(n*size_t(out.shCoefficients)*3);
    for (size_t i=0;i<n;++i) {const auto &r=identities[i];const size_t row=r.row;out.active[i]=index>=r.first && index<=r.last;
        if (row>=input.count) fail("The PLY row population changed after import. Reimport the sequence.");
        std::copy_n(input.position.data()+row*3,3,out.position.data()+i*3);std::copy_n(input.rotation.data()+row*4,4,out.rotation.data()+i*4);
        std::copy_n(input.scale.data()+row*3,3,out.scale.data()+i*3);std::copy_n(input.colorDc.data()+row*3,3,out.colorDc.data()+i*3);out.opacity[i]=out.active[i] ? input.opacity[row] : 0;
        for (int k=0;k<input.shCoefficients*3;++k) out.shRest[i*size_t(out.shCoefficients)*3+size_t(k)]=input.shRest[row*size_t(input.shCoefficients)*3+size_t(k)];
    }
    return out;
}
