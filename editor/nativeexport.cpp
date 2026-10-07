#include "nativeexport.h"
#include "isolation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <limits>
#include <qfloat16.h>
#include <QQuaternion>
#include <queue>

namespace {
using Bytes=vgs::Bytes;
uint16_t u16(const uint8_t *p) {return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t *p) {return uint32_t(u16(p))|(uint32_t(u16(p+2))<<16);}
uint64_t u64(const uint8_t *p) {return uint64_t(u32(p))|(uint64_t(u32(p+4))<<32);}
void put32(uint8_t *p,uint32_t n) {for (int i=0;i<4;++i) p[i]=uint8_t(n>>(8*i));}
void put64(uint8_t *p,uint64_t n) {for (int i=0;i<8;++i) p[i]=uint8_t(n>>(8*i));}
float f32(const uint8_t *p) {auto bits=u32(p);float result;std::memcpy(&result,&bits,4);return result;}
void putFloat(uint8_t *p,float v) {uint32_t bits;std::memcpy(&bits,&v,4);put32(p,bits);}
float half(const uint8_t *p) {const auto bits=u16(p);qfloat16 value;std::memcpy(&value,&bits,2);return float(value);}
void putHalf(uint8_t *p,float value) {
    if (!std::isfinite(value) || std::abs(value)>65504) throw std::runtime_error("SH exceeds the supported half-float range.");
    qfloat16 h(value);uint16_t bits;std::memcpy(&bits,&h,2);p[0]=uint8_t(bits);p[1]=uint8_t(bits>>8);
}
bool axisUniform(const QMatrix4x4 &m) {
    const float s=m(0,0);if (!(s>0)) return false;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) if (std::abs(m(i,j)-(i==j ? s : 0.f))>1e-6f*std::max(1.f,s)) return false;
    return true;
}
double uniformScale(const QMatrix4x4 &m) {return std::sqrt(double(m(0,0))*m(0,0)+double(m(1,0))*m(1,0)+double(m(2,0))*m(2,0));}
vgs::DecodedPage *find(vgs::DecodedChunk &c,uint32_t id,uint32_t group=0) {
    for (auto &p:c.pages) if (p.descriptor.attribute==id && p.descriptor.group==group) return &p;
    return nullptr;
}
vgs::DecodedPage &get(vgs::DecodedChunk &c,uint32_t id,uint32_t group=0) {
    auto *p=find(c,id,group);if (!p) throw std::runtime_error("Missing native attribute.");return *p;
}
void report(const ExportProgress &progress,const QString &message) {
    if (progress && !progress(0,message)) throw std::runtime_error("Export cancelled.");
}
int planes(int degree) {return degree==3 ? 5 : degree==2 ? 3 : degree;}
std::vector<uint8_t> ranks(vgs::DecodedChunk &c,uint32_t group,bool position) {
    const auto &boundaries=get(c,position ? vgs::PositionRanks : vgs::RotationRanks,group).bytes;
    std::vector<uint8_t> result(size_t(c.groups[group].splats));size_t begin=0;
    for (size_t rank=0;rank<boundaries.size()/4;++rank) {
        const size_t end=u32(boundaries.data()+rank*4);
        if (end<begin || end>result.size()) throw std::runtime_error("Invalid native rank boundaries.");
        std::fill(result.begin()+begin,result.begin()+end,uint8_t(rank+1));begin=end;
    }
    if (begin!=result.size()) throw std::runtime_error("Incomplete native rank boundaries.");return result;
}
void normalizeSchemas(vgs::DecodedChunk &chunk) {
    for (auto &page:chunk.pages) {
        auto &d=page.descriptor;auto &s=d.spec;const auto &g=chunk.groups[d.group];
        const uint64_t n=g.splats,S=g.intervals+1,T=g.intervals;const auto &dict=chunk.groups[0];
        auto raw=[&] {s={};s.rows=page.bytes.size();};
        auto scalar=[&](int channels,uint64_t rows,uint32_t kind,uint32_t family) {s={};s.kind=kind;s.family=family;s.fields.assign(channels,0);s.rows=rows;};
        auto packed=[&](int width,uint64_t rows,uint32_t family,const std::vector<int> &fields) {s={};s.kind=2;s.width=width;s.rows=rows;s.family=family;s.fields=fields;};
        switch (d.attribute) {
        case vgs::ScaleIndices:case vgs::Sh0Base:scalar(3,n,1,3);break;
        case vgs::Lifetimes:scalar(2,n,1,5);break;
        case vgs::Sh0Terms:case vgs::OpacityTerms:packed(8,n,0,{12,12,12,12,12,4});break;
        case vgs::PositionBase:packed(8,n,0,{1,21,21,21});break;
        case vgs::PositionSamples:packed(8,n*S,1,{1,21,21,21});s.samples=S;break;
        case vgs::RotationBase:packed(4,n,6,{1,9,10,10,2});break;
        case vgs::RotationSamples:packed(4,n*S,7,{1,9,10,10,2});s.samples=S;break;
        case vgs::PositionTerms:case vgs::RotationTerms: {
            const bool pos=d.attribute==vgs::PositionTerms;
            auto rowRanks=ranks(chunk,d.group,pos);scalar(pos ? 2 : 1,page.bytes.size()/(pos ? 4 : 2),pos ? 5 : 4,pos ? 9 : 8);s.ranks=std::move(rowRanks);break;
        }
        case vgs::ShStaticIndices:case vgs::ShTemporalIndices: {
            const auto count=uint32_t(page.bytes.size()/(n*4));s={};s.kind=3;s.width=count;s.fields={10,10,10,2};s.rows=n;break;
        }
        case vgs::ShStaticBook:scalar(3,page.bytes.size()/6,0,0);break;
        case vgs::ShTemporalBook:scalar(3,page.bytes.size()/6,0,2);s.entries=s.rows/S;break;
        case vgs::Sh0Trajectories:scalar(3,dict.counts[2]*S,0,1);s.samples=S;break;
        case vgs::OpacityTrajectories:scalar(1,dict.counts[3]*S,0,1);s.samples=S;break;
        case vgs::RotationInitial:scalar(4,dict.counts[4],0,0);break;
        case vgs::RotationDeltas:scalar(4,dict.counts[4]*T,1,4);s.intervals=T;break;
        case vgs::PositionTrajectories:packed(8,dict.counts[5]*S,1,{1,21,21,21});s.samples=S;break;
        default:raw();break;
        }
        d.firstRow=0;d.totalRows=s.rows;d.decodedSize=page.bytes.size();
        if (mgs::attributeSize(s)!=page.bytes.size()) throw std::runtime_error("Invalid native attribute size.");
    }
}
struct Span {size_t row;uint8_t first,last;};
void filterGroups(vgs::DecodedChunk &chunk,const std::vector<std::vector<Span>> &spans) {
    std::vector<vgs::Group> groups{chunk.groups[0]};std::vector<vgs::DecodedPage> pages;
    for (auto &page:chunk.pages) if (page.descriptor.group==0) pages.push_back(std::move(page));
    for (size_t old=1;old<chunk.groups.size();++old) {
        const auto &kept=spans[old];if (kept.empty()) continue;
        auto group=chunk.groups[old];const size_t n=group.splats,newN=kept.size(),newGroup=groups.size();
        const auto positionRanks=(group.flags&1) ? std::vector<uint8_t>{} : ranks(chunk,uint32_t(old),true);
        const auto rotationRanks=(group.flags&2) ? std::vector<uint8_t>{} : ranks(chunk,uint32_t(old),false);
        group.splats=newN;groups.push_back(group);
        for (auto &page:chunk.pages) {
            if (page.descriptor.group!=old) continue;
            auto &d=page.descriptor;const auto id=d.attribute;Bytes output;
            if (id==vgs::Lifetimes) {
                output.reserve(newN*2);for (const auto &span:kept) {output.push_back(span.first);output.push_back(span.last);}
            } else if (id==vgs::RotationTerms || id==vgs::PositionTerms) {
                const auto &rs=id==vgs::PositionTerms ? positionRanks : rotationRanks;std::vector<size_t> offsets(n+1);
                for (size_t row=0;row<n;++row) offsets[row+1]=offsets[row]+rs[row];const size_t width=id==vgs::PositionTerms ? 4 : 2;
                for (const auto &span:kept) output.insert(output.end(),page.bytes.begin()+offsets[span.row]*width,page.bytes.begin()+offsets[span.row+1]*width);
                d.spec.ranks.clear();for (const auto &span:kept) d.spec.ranks.push_back(rs[span.row]);
            } else if (id==vgs::RotationRanks || id==vgs::PositionRanks) {
                const auto &rs=id==vgs::PositionRanks ? positionRanks : rotationRanks;output.resize(page.bytes.size());
                for (size_t rank=1;rank<=output.size()/4;++rank) {
                    size_t count=0;for (const auto &span:kept) if (rs[span.row]<=rank) ++count;
                    put32(output.data()+(rank-1)*4,uint32_t(count));
                }
            } else if (id==vgs::ShStaticIndices || id==vgs::ShTemporalIndices) {
                const size_t count=page.bytes.size()/(n*4);output.resize(count*newN*4);
                for (size_t plane=0;plane<count;++plane) for (size_t row=0;row<newN;++row)
                    std::memcpy(output.data()+(plane*newN+row)*4,page.bytes.data()+(plane*n+kept[row].row)*4,4);
            } else {
                if (page.bytes.size()%n) throw std::runtime_error("Invalid native row stride.");
                const size_t stride=page.bytes.size()/n;output.reserve(newN*stride);
                for (const auto &span:kept) output.insert(output.end(),page.bytes.begin()+span.row*stride,page.bytes.begin()+(span.row+1)*stride);
            }
            page.bytes=std::move(output);d.group=uint32_t(newGroup);pages.push_back(std::move(page));
        }
    }
    chunk.groups=std::move(groups);chunk.pages=std::move(pages);
}
void addEmptyGroup(vgs::DecodedChunk &chunk,int degree) {
    auto &dict=chunk.groups[0];vgs::Group group;group.type=1;group.flags=3;group.splats=1;group.intervals=dict.intervals;chunk.groups.push_back(group);
    auto add=[&](uint32_t id,Bytes bytes) {vgs::DecodedPage p;p.descriptor.group=1;p.descriptor.attribute=id;p.bytes=std::move(bytes);chunk.pages.push_back(std::move(p));};
    add(vgs::ScaleIndices,Bytes(3));add(vgs::Lifetimes,Bytes(2));add(vgs::Sh0Base,Bytes(3));add(vgs::Sh0Terms,Bytes(8));add(vgs::OpacityTerms,Bytes(8));
    add(vgs::PositionSamples,Bytes((dict.intervals+1)*8));Bytes rotations((dict.intervals+1)*4);
    // Quaternion identity: omitted W, the three stored components closest to zero.
    const uint32_t identity=(512u<<20)|(512u<<10)|(256u<<1);
    for (size_t i=0;i<=dict.intervals;++i) put32(rotations.data()+4*i,identity);add(vgs::RotationSamples,std::move(rotations));
    if (degree) {add(vgs::ShStaticIndices,Bytes(planes(degree)*4));add(vgs::ShTemporalIndices,Bytes(planes(degree)*4));}
}
void compactRq(vgs::DecodedChunk &c,uint32_t terms,uint32_t book,int slot,size_t components) {
    const size_t oldStride=c.groups[0].counts[slot]/5,S=c.groups[0].intervals+1,entryBytes=S*components*2;
    std::array<std::set<size_t>,5> used;
    for (auto &p:c.pages) if (p.descriptor.attribute==terms) for (size_t row=0;row<p.bytes.size()/8;++row) {
        const auto word=u64(p.bytes.data()+row*8);for (int stage=0;stage<5;++stage) used[stage].insert(size_t((word>>(stage*12))&4095));
    }
    size_t stride=1;for (auto &set:used) {if (set.empty()) set.insert(0);stride=std::max(stride,set.size());}
    auto &table=get(c,book).bytes;Bytes trimmed(5*stride*entryBytes);std::array<std::vector<size_t>,5> remap;
    for (int stage=0;stage<5;++stage) {
        remap[stage].resize(oldStride);size_t next=0;
        for (auto index:used[stage]) {
            if (index>=oldStride) throw std::runtime_error("Native dictionary index out of range.");remap[stage][index]=next;
            std::memcpy(trimmed.data()+(stage*stride+next)*entryBytes,table.data()+(stage*oldStride+index)*entryBytes,entryBytes);++next;
        }
    }
    for (auto &p:c.pages) if (p.descriptor.attribute==terms) for (size_t row=0;row<p.bytes.size()/8;++row) {
        const auto word=u64(p.bytes.data()+row*8);uint64_t output=word&0xf000000000000000ull;
        for (int stage=0;stage<5;++stage) output|=uint64_t(remap[stage][size_t((word>>(stage*12))&4095)])<<(stage*12);
        put64(p.bytes.data()+row*8,output);
    }
    table=std::move(trimmed);c.groups[0].counts[slot]=stride*5;
}
void compactResidual(vgs::DecodedChunk &c,bool position) {
    const uint32_t terms=position ? vgs::PositionTerms : vgs::RotationTerms;const size_t width=position ? 4 : 2;
    std::set<size_t> used;
    for (const auto &p:c.pages) if (p.descriptor.attribute==terms) for (size_t i=0;i<p.bytes.size()/width;++i) used.insert(u16(p.bytes.data()+i*width));
    if (used.empty()) {
        const std::set<uint32_t> attributes=position ? std::set<uint32_t>{vgs::PositionTrajectories} : std::set<uint32_t>{vgs::RotationInitial,vgs::RotationDeltas,vgs::RotationDeltaLut};
        c.pages.erase(std::remove_if(c.pages.begin(),c.pages.end(),[&](const auto &p) {return attributes.count(p.descriptor.attribute)!=0;}),c.pages.end());
        c.groups[0].counts[position ? 5 : 4]=0;return;
    }
    const size_t slot=position ? 5 : 4,old=c.groups[0].counts[slot],S=c.groups[0].intervals+1,T=c.groups[0].intervals;
    std::vector<size_t> remap(old);size_t next=0;for (auto id:used) {if (id>=old) throw std::runtime_error("Native residual index out of range.");remap[id]=next++;}
    const std::vector<std::pair<uint32_t,size_t>> attributes=position ? std::vector<std::pair<uint32_t,size_t>>{{vgs::PositionTrajectories,S*8}} : std::vector<std::pair<uint32_t,size_t>>{{vgs::RotationInitial,8},{vgs::RotationDeltas,T*4}};
    for (auto [attribute,stride]:attributes) {
        auto &table=get(c,attribute).bytes;Bytes output;output.reserve(used.size()*stride);
        for (auto id:used) output.insert(output.end(),table.begin()+id*stride,table.begin()+(id+1)*stride);table=std::move(output);
    }
    for (auto &p:c.pages) if (p.descriptor.attribute==terms) for (size_t i=0;i<p.bytes.size()/width;++i) {
        const uint32_t value=uint32_t(remap[u16(p.bytes.data()+i*width)]);p.bytes[i*width]=uint8_t(value);p.bytes[i*width+1]=uint8_t(value>>8);
    }
    c.groups[0].counts[slot]=used.size();
}
void compactSh(vgs::DecodedChunk &c,bool temporal) {
    const uint32_t indices=temporal ? vgs::ShTemporalIndices : vgs::ShStaticIndices,book=temporal ? vgs::ShTemporalBook : vgs::ShStaticBook;
    auto *table=find(c,book);if (!table) return;
    size_t coefficients=0;for (auto &p:c.pages) if (p.descriptor.attribute==indices) {coefficients=p.bytes.size()/c.groups[p.descriptor.group].splats/4*3;break;}
    const size_t old=c.groups[0].counts[temporal ? 1 : 0],S=temporal ? c.groups[0].intervals+1 : 1;
    std::vector<std::set<size_t>> used(coefficients);
    for (auto &p:c.pages) if (p.descriptor.attribute==indices) {
        const size_t n=c.groups[p.descriptor.group].splats;
        for (size_t k=0;k<coefficients;++k) for (size_t i=0;i<n;++i) used[k].insert((u32(p.bytes.data()+((k/3)*n+i)*4)>>((2-k%3)*10))&1023);
    }
    size_t count=1;for (auto &set:used) {if (set.empty()) set.insert(0);count=std::max(count,set.size());}
    std::vector<std::vector<size_t>> remap(coefficients,std::vector<size_t>(old));Bytes output(S*coefficients*count*6);
    for (size_t k=0;k<coefficients;++k) {
        size_t next=0;for (auto id:used[k]) {
            if (id>=old) throw std::runtime_error("Native SH index out of range.");remap[k][id]=next;
            for (size_t sample=0;sample<S;++sample) std::memcpy(output.data()+((sample*coefficients+k)*count+next)*6,table->bytes.data()+((sample*coefficients+k)*old+id)*6,6);++next;
        }
    }
    for (auto &p:c.pages) if (p.descriptor.attribute==indices) {
        const size_t n=c.groups[p.descriptor.group].splats;
        for (size_t plane=0;plane<coefficients/3;++plane) for (size_t i=0;i<n;++i) {
            const auto word=u32(p.bytes.data()+(plane*n+i)*4);uint32_t out=word&0xc0000000u;
            for (size_t k=0;k<3;++k) out|=uint32_t(remap[plane*3+k][(word>>((2-k)*10))&1023])<<((2-k)*10);
            put32(p.bytes.data()+(plane*n+i)*4,out);
        }
    }
    table->bytes=std::move(output);c.groups[0].counts[temporal ? 1 : 0]=count;
}
void sliceTime(vgs::DecodedChunk &c,int first,int intervals) {
    const size_t oldT=c.groups[0].intervals,oldS=oldT+1,S=size_t(intervals)+1;
    if (!first && size_t(intervals)==oldT) return;
    auto slice=[&](Bytes &b,size_t stride) {
        const size_t entries=b.size()/(oldS*stride);Bytes out(entries*S*stride);
        for (size_t i=0;i<entries;++i) std::memcpy(out.data()+i*S*stride,b.data()+(i*oldS+size_t(first))*stride,S*stride);b=std::move(out);
    };
    auto *initial=find(c,vgs::RotationInitial),*deltas=find(c,vgs::RotationDeltas),*lut=find(c,vgs::RotationDeltaLut);
    if (first && initial && deltas && lut) for (size_t row=0;row<c.groups[0].counts[4];++row) for (size_t channel=0;channel<4;++channel) {
        const uint16_t bits=u16(initial->bytes.data()+(row*4+channel)*2);qfloat16 h;std::memcpy(&h,&bits,2);float value=float(h);
        for (int sample=0;sample<first;++sample) value+=f32(lut->bytes.data()+deltas->bytes[(row*oldT+sample)*4+channel]*4);
        h=qfloat16(value);uint16_t next;std::memcpy(&next,&h,2);initial->bytes[(row*4+channel)*2]=uint8_t(next);initial->bytes[(row*4+channel)*2+1]=uint8_t(next>>8);
    }
    for (auto &p:c.pages) {
        switch (p.descriptor.attribute) {
        case vgs::Sh0Trajectories:slice(p.bytes,6);break;
        case vgs::OpacityTrajectories:slice(p.bytes,2);break;
        case vgs::PositionTrajectories:case vgs::PositionSamples:slice(p.bytes,8);break;
        case vgs::RotationSamples:slice(p.bytes,4);break;
        case vgs::ShTemporalBook: {
            const size_t stride=p.bytes.size()/oldS;Bytes b(p.bytes.begin()+first*stride,p.bytes.begin()+(first+S)*stride);p.bytes=std::move(b);break;
        }
        case vgs::RotationDeltas: {
            const size_t n=c.groups[0].counts[4];Bytes b(n*intervals*4);
            for (size_t row=0;row<n;++row) std::memcpy(b.data()+row*intervals*4,p.bytes.data()+(row*oldT+first)*4,size_t(intervals)*4);p.bytes=std::move(b);break;
        }
        default:break;
        }
    }
    for (auto &g:c.groups) g.intervals=intervals;
}
// Direct source positions use normalized chunk time as their interpolation weight.
// When trimming, convert just those groups to sampled residual trajectories so a
// shorter chunk cannot change their motion. Existing RQ groups remain untouched.
void convertTrimmedDirectPositions(vgs::DecodedChunk &c,const NativeChunkPlan &plan) {
    const size_t T=c.groups[0].intervals,S=T+1;
    if (!plan.first && size_t(plan.intervals)==T) return;
    size_t count=0;for (size_t group=1;group<c.groups.size();++group) if (c.groups[group].flags&1) count+=c.groups[group].splats;
    if (!count) return;
    const size_t oldCount=c.groups[0].counts[5];
    if (oldCount+count>65536) throw std::runtime_error("The trimmed direct-position groups exceed the native trajectory dictionary limit.");
    const double oldLo=c.groups[0].trajectoryMin,oldHi=c.groups[0].trajectoryMax;
    struct Converted {uint32_t group;std::vector<float> base,trajectory;};std::vector<Converted> converted;
    double lo=oldCount ? oldLo : 0,hi=oldCount ? oldHi : 0;
    auto unpack=[](uint64_t word,double minimum,double maximum) {
        return QVector3D(float(minimum+double((word>>43)&2097151)*(maximum-minimum)/2097151),
                         float(minimum+double((word>>22)&2097151)*(maximum-minimum)/2097151),
                         float(minimum+double((word>>1)&2097151)*(maximum-minimum)/2097151));
    };
    auto pack=[](const float *p,double minimum,double maximum) {
        uint64_t result=0;const int shift[3]={43,22,1};
        for (int axis=0;axis<3;++axis) result|=(maximum>minimum ? uint64_t(std::clamp(std::round((p[axis]-minimum)/(maximum-minimum)*2097151),0.,2097151.)) : 0)<<shift[axis];return result;
    };
    for (uint32_t group=1;group<c.groups.size();++group) {
        const auto &g=c.groups[group];if (!(g.flags&1)) continue;
        Converted data;data.group=group;data.base.resize(g.splats*3);data.trajectory.resize(g.splats*S*3);
        const auto &words=get(c,vgs::PositionSamples,group).bytes;
        for (size_t row=0;row<g.splats;++row) {
            auto evaluate=[&](size_t sample) {
                const size_t start=std::min(sample,T-1);const float r=float(sample)/float(T);
                const auto a=unpack(u64(words.data()+(row*S+start)*8),g.positionMin,g.positionMax),b=unpack(u64(words.data()+(row*S+start+1)*8),g.positionMin,g.positionMax);
                return a+(b-a)*r;
            };
            const auto base=evaluate(size_t(plan.first));for (int axis=0;axis<3;++axis) data.base[row*3+axis]=base[axis];
            for (size_t sample=0;sample<S;++sample) {
                const auto delta=evaluate(sample)-base;
                for (int axis=0;axis<3;++axis) {const float value=delta[axis];data.trajectory[(row*S+sample)*3+axis]=value;lo=std::min(lo,double(value));hi=std::max(hi,double(value));}
            }
        }
        converted.push_back(std::move(data));
    }
    Bytes table((oldCount+count)*S*8);
    if (oldCount) {
        const auto &old=get(c,vgs::PositionTrajectories).bytes;
        for (size_t row=0;row<oldCount*S;++row) {
            const auto p=unpack(u64(old.data()+row*8),oldLo,oldHi);const float values[3]={p.x(),p.y(),p.z()};put64(table.data()+row*8,pack(values,lo,hi));
        }
    }
    size_t next=oldCount;
    auto add=[&](uint32_t id,uint32_t group,Bytes bytes) {vgs::DecodedPage page;page.descriptor.attribute=id;page.descriptor.group=group;page.bytes=std::move(bytes);c.pages.push_back(std::move(page));};
    for (auto &data:converted) {
        auto &g=c.groups[data.group];Bytes bases(g.splats*8),terms(g.splats*4),boundaries(16);
        for (size_t row=0;row<g.splats;++row) {
            put64(bases.data()+row*8,pack(data.base.data()+row*3,g.positionMin,g.positionMax));
            put32(terms.data()+row*4,uint32_t(next)|(0x3c00u<<16));
            for (size_t sample=0;sample<S;++sample) put64(table.data()+(next*S+sample)*8,pack(data.trajectory.data()+(row*S+sample)*3,lo,hi));++next;
        }
        for (int rank=0;rank<4;++rank) put32(boundaries.data()+rank*4,uint32_t(g.splats));
        c.pages.erase(std::remove_if(c.pages.begin(),c.pages.end(),[&](const auto &p) {return p.descriptor.group==data.group && p.descriptor.attribute==vgs::PositionSamples;}),c.pages.end());
        add(vgs::PositionBase,data.group,std::move(bases));add(vgs::PositionTerms,data.group,std::move(terms));add(vgs::PositionRanks,data.group,std::move(boundaries));g.flags&=~1u;
    }
    if (auto *existing=find(c,vgs::PositionTrajectories)) existing->bytes=std::move(table);else add(vgs::PositionTrajectories,0,std::move(table));
    c.groups[0].counts[5]=oldCount+count;c.groups[0].trajectoryMin=lo;c.groups[0].trajectoryMax=hi;
}
struct TrajectoryBook {
    size_t width=0;
    std::vector<std::vector<float>> values;
    std::vector<uint16_t> indices;
};
TrajectoryBook fitBook(const std::vector<float> &features,size_t width,const ExportProgress &progress) {
    const size_t n=features.size()/width;
    struct Cell {size_t begin,end,axis;double score;std::vector<float> mean;};
    std::vector<size_t> order(n);std::iota(order.begin(),order.end(),0);std::vector<Cell> cells;
    auto statistics=[&](size_t begin,size_t end) {
        std::vector<double> sum(width),squares(width);Cell cell{begin,end,0,0,std::vector<float>(width)};
        for (size_t i=begin;i<end;++i) for (size_t k=0;k<width;++k) {const double v=features[order[i]*width+k];sum[k]+=v;squares[k]+=v*v;}
        double largest=-1;
        for (size_t k=0;k<width;++k) {
            cell.mean[k]=float(sum[k]/double(end-begin));const double variance=std::max(0.,squares[k]-sum[k]*sum[k]/double(end-begin));
            cell.score+=variance;if (variance>largest) {largest=variance;cell.axis=k;}
        }
        if (end-begin<2 || largest<1e-14) cell.score=0;return cell;
    };
    std::priority_queue<std::pair<double,size_t>> pending;cells.push_back(statistics(0,n));pending.push({cells[0].score,0});
    while (cells.size()<std::min(size_t(1024),n) && !pending.empty() && pending.top().first>1e-14) {
        const auto index=pending.top().second;pending.pop();const Cell original=cells[index];const size_t mid=(original.begin+original.end)/2,axis=original.axis;
        std::nth_element(order.begin()+original.begin,order.begin()+mid,order.begin()+original.end,[&](size_t a,size_t b) {return features[a*width+axis]<features[b*width+axis];});
        cells[index]=statistics(original.begin,mid);const size_t next=cells.size();cells.push_back(statistics(mid,original.end));
        pending.push({cells[index].score,index});pending.push({cells[next].score,next});
        if ((cells.size()%32)==0) report(progress,QStringLiteral("Fitting rotated SH trajectories: %1 / 1024 entries").arg(cells.size()));
    }
    TrajectoryBook result;result.width=width;result.indices.resize(n);
    for (size_t index=0;index<cells.size();++index) {
        result.values.push_back(std::move(cells[index].mean));
        for (size_t i=cells[index].begin;i<cells[index].end;++i) result.indices[order[i]]=uint16_t(index);
    }
    return result;
}
void rotateSh(vgs::DecodedChunk &c,const Project &project,ExportResult *result,const ExportProgress &progress) {
    auto *staticPage=find(c,vgs::ShStaticBook),*temporalPage=find(c,vgs::ShTemporalBook);if (!staticPage || !temporalPage) return;
    const size_t ns=c.groups[0].counts[0],nt=c.groups[0].counts[1],S=c.groups[0].intervals+1;
    const size_t stored=staticPage->bytes.size()/ns/6,coefficients=stored==9 ? 8 : stored;
    const auto matrix=exportShTransform(project.transform);
    std::vector<size_t> groups,rows;
    for (size_t group=1;group<c.groups.size();++group) for (size_t row=0;row<c.groups[group].splats;++row) {groups.push_back(group);rows.push_back(row);}
    const size_t n=rows.size();
    std::vector<std::pair<size_t,size_t>> lifetimes(n);
    std::vector<const Bytes *> staticIndices(c.groups.size()),temporalIndices(c.groups.size());
    for (size_t group=1;group<c.groups.size();++group) {
        staticIndices[group]=&get(c,vgs::ShStaticIndices,uint32_t(group)).bytes;
        temporalIndices[group]=&get(c,vgs::ShTemporalIndices,uint32_t(group)).bytes;
    }
    for (size_t row=0;row<n;++row) {const auto &life=get(c,vgs::Lifetimes,uint32_t(groups[row])).bytes;lifetimes[row]={life[2*rows[row]],life[2*rows[row]+1]};}
    std::vector<float> staticValues(staticPage->bytes.size()/2),temporalValues(temporalPage->bytes.size()/2);
    for (size_t i=0;i<staticValues.size();++i) staticValues[i]=half(staticPage->bytes.data()+i*2);
    for (size_t i=0;i<temporalValues.size();++i) temporalValues[i]=half(temporalPage->bytes.data()+i*2);
    std::vector<std::vector<uint16_t>> si(coefficients,std::vector<uint16_t>(n)),ti=si;
    for (size_t k=0;k<coefficients;++k) for (size_t i=0;i<n;++i) {
        const auto group=uint32_t(groups[i]);const size_t row=rows[i],count=c.groups[group].splats;
        si[k][i]=uint16_t((u32(staticIndices[group]->data()+((k/3)*count+row)*4)>>((2-k%3)*10))&1023);
        ti[k][i]=uint16_t((u32(temporalIndices[group]->data()+((k/3)*count+row)*4)>>((2-k%3)*10))&1023);
    }
    std::vector<TrajectoryBook> staticBooks,temporalBooks;double squared=0,maximum=0;uint64_t measured=0;
    for (size_t k=0;k<stored;++k) {
        report(progress,QStringLiteral("Rotating and fitting SH coefficient %1 / %2").arg(k+1).arg(stored));
        std::vector<float> base(n*3),features(n*S*3);
        if (k<coefficients) for (size_t source=0;source<coefficients;++source) {
            const auto band=[](size_t coefficient) {return coefficient<3 ? 1 : coefficient<8 ? 2 : 3;};
            if (band(k)!=band(source)) continue;
            const double weight=matrix[(k+1)*16+source+1];if (std::abs(weight)<1e-10) continue;
            for (size_t row=0;row<n;++row) {
                for (size_t channel=0;channel<3;++channel) base[row*3+channel]+=float(weight*staticValues[(source*ns+si[source][row])*3+channel]);
                for (size_t sample=0;sample<S;++sample) for (size_t channel=0;channel<3;++channel)
                    features[(row*S+sample)*3+channel]+=float(weight*temporalValues[((sample*stored+source)*nt+ti[source][row])*3+channel]);
            }
        }
        auto staticBook=fitBook(base,3,progress);
        for (auto &entry:staticBook.values) for (auto &v:entry) {uint8_t bytes[2];putHalf(bytes,v);v=half(bytes);}
        for (size_t row=0;row<n;++row) for (size_t sample=0;sample<S;++sample) for (size_t channel=0;channel<3;++channel)
            features[(row*S+sample)*3+channel]+=base[row*3+channel]-staticBook.values[staticBook.indices[row]][channel];
        // Inactive samples have no rendered colour. Extend the live endpoints instead
        // of fitting arbitrary dictionary values from before birth or after death.
        for (size_t row=0;row<n;++row) {
            const auto [first,last]=lifetimes[row];
            for (size_t sample=0;sample<S;++sample) if (sample<first || sample>=last) {
                const size_t closest=last>first ? (sample<first ? first : last-1) : 0;
                for (size_t channel=0;channel<3;++channel) features[(row*S+sample)*3+channel]=last>first ? features[(row*S+closest)*3+channel] : 0;
            }
        }
        auto temporalBook=fitBook(features,S*3,progress);
        for (auto &entry:temporalBook.values) for (auto &v:entry) {uint8_t bytes[2];putHalf(bytes,v);v=half(bytes);}
        for (size_t row=0;row<n;++row) for (size_t feature=lifetimes[row].first*3;feature<lifetimes[row].second*3;++feature) {
            const double error=features[row*S*3+feature]-temporalBook.values[temporalBook.indices[row]][feature];
            maximum=std::max(maximum,std::abs(error));squared+=error*error;++measured;
        }
        staticBooks.push_back(std::move(staticBook));temporalBooks.push_back(std::move(temporalBook));
    }
    size_t newNs=1,newNt=1;for (const auto &book:staticBooks) newNs=std::max(newNs,book.values.size());for (const auto &book:temporalBooks) newNt=std::max(newNt,book.values.size());
    Bytes staticBytes(stored*newNs*6),temporalBytes(S*stored*newNt*6);
    for (size_t k=0;k<stored;++k) {
        for (size_t row=0;row<staticBooks[k].values.size();++row) for (size_t channel=0;channel<3;++channel) putHalf(staticBytes.data()+((k*newNs+row)*3+channel)*2,staticBooks[k].values[row][channel]);
        for (size_t sample=0;sample<S;++sample) for (size_t row=0;row<temporalBooks[k].values.size();++row) for (size_t channel=0;channel<3;++channel)
            putHalf(temporalBytes.data()+(((sample*stored+k)*newNt+row)*3+channel)*2,temporalBooks[k].values[row][sample*3+channel]);
    }
    for (size_t group=1,offset=0;group<c.groups.size();offset+=c.groups[group++].splats) {
        const size_t count=c.groups[group].splats;auto &s=get(c,vgs::ShStaticIndices,uint32_t(group)).bytes,&t=get(c,vgs::ShTemporalIndices,uint32_t(group)).bytes;
        for (size_t plane=0;plane<stored/3;++plane) for (size_t row=0;row<count;++row) {
            uint32_t sw=0,tw=0;
            for (size_t k=0;k<3;++k) {sw|=uint32_t(staticBooks[plane*3+k].indices[offset+row])<<((2-k)*10);tw|=uint32_t(temporalBooks[plane*3+k].indices[offset+row])<<((2-k)*10);}
            put32(s.data()+(plane*count+row)*4,sw);put32(t.data()+(plane*count+row)*4,tw);
        }
    }
    staticPage->bytes=std::move(staticBytes);temporalPage->bytes=std::move(temporalBytes);c.groups[0].counts[0]=newNs;c.groups[0].counts[1]=newNt;
    if (result) result->notes << QStringLiteral("Rotated SH trajectory refit: coefficient RMS error %1, maximum %2.").arg(measured ? std::sqrt(squared/measured) : 0,0,'g',4).arg(maximum,0,'g',4);
}
uint32_t packQuaternion(const QQuaternion &rotation) {
    const double q[4]={rotation.scalar(),rotation.x(),rotation.y(),rotation.z()};int largest=0;
    for (int i=1;i<4;++i) if (std::abs(q[i])>std::abs(q[largest])) largest=i;
    const double sign=q[largest]<0 ? -1 : 1;uint32_t result=uint32_t(largest)<<30;const int shifts[3]={20,10,1},limits[3]={1023,1023,511};
    for (int i=0,next=0;i<4;++i) if (i!=largest) {result|=uint32_t(std::clamp(std::round((q[i]*sign*std::sqrt(2.)+1)*.5*limits[next]),0.,double(limits[next])))<<shifts[next];++next;}return result;
}
void rotateOrientations(vgs::DecodedChunk &c,const Project &project,const ExportProgress &progress) {
    normalizeSchemas(c);vgs::FrameDecoder decoder(c);const size_t T=c.groups[0].intervals,S=T+1;const auto model=project.transform.matrix();
    const double scale=uniformScale(model),sign=model.determinant()<0 ? -1 : 1;QMatrix3x3 matrix;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) matrix(i,j)=float(sign*model(i,j)/scale);const auto global=QQuaternion::fromRotationMatrix(matrix).normalized();
    std::vector<Bytes> rotations(c.groups.size());for (size_t group=1;group<c.groups.size();++group) rotations[group].resize(c.groups[group].splats*S*4);
    vgs::Frame frame;
    for (size_t sample=0;sample<S;++sample) {
        report(progress,QStringLiteral("Baking native rotations %1 / %2").arg(sample+1).arg(S));decoder.evaluateInto(std::min(sample/double(T),1.-1e-9),false,&frame);
        for (size_t group=1,offset=0;group<c.groups.size();offset+=c.groups[group++].splats) for (size_t row=0;row<c.groups[group].splats;++row) {
            const auto *q=frame.rotation.data()+(offset+row)*4;const auto transformed=(global*QQuaternion(q[3],q[0],q[1],q[2])).normalized();
            put32(rotations[group].data()+(row*S+sample)*4,packQuaternion(transformed));
        }
    }
    const std::set<uint32_t> remove{vgs::RotationSamples,vgs::RotationBase,vgs::RotationTerms,vgs::RotationRanks,vgs::RotationInitial,vgs::RotationDeltas,vgs::RotationDeltaLut};
    c.pages.erase(std::remove_if(c.pages.begin(),c.pages.end(),[&](const auto &page) {return remove.count(page.descriptor.attribute)!=0;}),c.pages.end());
    for (uint32_t group=1;group<c.groups.size();++group) {c.groups[group].flags|=2;vgs::DecodedPage page;page.descriptor.attribute=vgs::RotationSamples;page.descriptor.group=group;page.bytes=std::move(rotations[group]);c.pages.push_back(std::move(page));}
    c.groups[0].counts[4]=0;
}
void transformPositions(vgs::DecodedChunk &c,const Project &p) {
    const auto model=p.transform.matrix();const double scale=uniformScale(model);
    const bool simple=axisUniform(model);
    for (auto &page:c.pages) {
        const auto id=page.descriptor.attribute;
        if (id==vgs::ScaleLut) {for (size_t i=0;i<page.bytes.size()/4;++i) putFloat(page.bytes.data()+i*4,float(f32(page.bytes.data()+i*4)*scale));continue;}
        const bool trajectory=id==vgs::PositionTrajectories;
        if (trajectory && simple) continue; // changing the uniform range leaves these indices exact
        if (!trajectory && id!=vgs::PositionBase && id!=vgs::PositionSamples) continue;
        auto &g=c.groups[page.descriptor.group];const double lo=trajectory ? g.trajectoryMin : g.positionMin,hi=trajectory ? g.trajectoryMax : g.positionMax;
        double newLo=lo*scale+std::min({double(model(0,3)),double(model(1,3)),double(model(2,3))}),newHi=hi*scale+std::max({double(model(0,3)),double(model(1,3)),double(model(2,3))});
        auto transformed=[&](uint64_t word) {
            const QVector3D point(float(lo+double((word>>43)&2097151)*(hi-lo)/2097151),float(lo+double((word>>22)&2097151)*(hi-lo)/2097151),float(lo+double((word>>1)&2097151)*(hi-lo)/2097151));
            return trajectory ? model.mapVector(point) : model.map(point);
        };
        if (!simple) {
            newLo=std::numeric_limits<double>::infinity();newHi=-newLo;
            for (size_t i=0;i<page.bytes.size()/8;++i) {const auto point=transformed(u64(page.bytes.data()+i*8));for (int axis=0;axis<3;++axis) {newLo=std::min(newLo,double(point[axis]));newHi=std::max(newHi,double(point[axis]));}}
        }
        for (size_t i=0;i<page.bytes.size()/8;++i) {
            const auto word=u64(page.bytes.data()+i*8);uint64_t out=word&1;
            const auto point=simple ? QVector3D() : transformed(word);
            const int shifts[3]={43,22,1};
            for (int axis=0;axis<3;++axis) {
                const double value=simple ? (lo+double((word>>shifts[axis])&2097151)*(hi-lo)/2097151)*scale+model(axis,3) : point[axis];
                const uint64_t q=newHi>newLo ? uint64_t(std::clamp(std::round((value-newLo)/(newHi-newLo)*2097151),0.,2097151.)) : 0;
                out|=q<<shifts[axis];
            }
            put64(page.bytes.data()+i*8,out);
        }
        if (trajectory) {g.trajectoryMin=newLo;g.trajectoryMax=newHi;} else {g.positionMin=newLo;g.positionMax=newHi;}
    }
    if (simple) {c.groups[0].trajectoryMin*=scale;c.groups[0].trajectoryMax*=scale;}
}
}

bool supportsNativeTransform(const Project &p) {
    const auto m=p.transform.matrix();const double scale=uniformScale(m);if (!(scale>0) || !std::isfinite(scale)) return false;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
        double dot=0;for (int k=0;k<3;++k) dot+=double(m(k,i))*m(k,j);
        if (std::abs(dot-(i==j ? scale*scale : 0))>1e-5*scale*scale) return false;
    }
    return true;
}
std::vector<NativeChunkPlan> nativeExportPlan(const vgs::Header &header,int first,int frames) {
    std::vector<NativeChunkPlan> result;
    for (size_t i=0;i<header.chunks.size();++i) {
        const auto &chunk=header.chunks[i];const int begin=std::max(first,int(chunk.startTick)),end=std::min(first+frames,int(chunk.startTick+chunk.intervals));
        if (end>begin) result.push_back({i,begin-int(chunk.startTick),end-begin});
    }
    return result;
}
vgs::DecodedChunk assembleNativeChunk(vgs::DecodedChunk input) {
    vgs::DecodedChunk result;result.groups=std::move(input.groups);
    for (auto &page:input.pages) {
        auto *dest=find(result,page.descriptor.attribute,page.descriptor.group);
        if (!dest) {
            vgs::DecodedPage complete;complete.descriptor=page.descriptor;complete.descriptor.firstRow=0;
            complete.descriptor.spec.rows=page.descriptor.totalRows;complete.descriptor.spec.ranks.clear();
            complete.bytes.resize(page.descriptor.totalRows*(page.bytes.size()/page.descriptor.spec.rows));result.pages.push_back(std::move(complete));dest=&result.pages.back();
        }
        const auto &d=page.descriptor;
        if (d.spec.kind==3) for (size_t plane=0;plane<d.spec.width;++plane)
            std::memcpy(dest->bytes.data()+(plane*d.totalRows+d.firstRow)*4,page.bytes.data()+plane*d.spec.rows*4,size_t(d.spec.rows)*4);
        else std::memcpy(dest->bytes.data()+d.firstRow*(page.bytes.size()/d.spec.rows),page.bytes.data(),page.bytes.size());
        dest->descriptor.spec.ranks.insert(dest->descriptor.spec.ranks.end(),d.spec.ranks.begin(),d.spec.ranks.end());
    }
    normalizeSchemas(result);return result;
}
void setNativeShDegree(vgs::DecodedChunk &c,int sourceDegree,int targetDegree) {
    if (sourceDegree==targetDegree) return;
    if (!targetDegree) {
        c.pages.erase(std::remove_if(c.pages.begin(),c.pages.end(),[](const auto &p) {return p.descriptor.attribute==vgs::ShStaticBook || p.descriptor.attribute==vgs::ShTemporalBook || p.descriptor.attribute==vgs::ShStaticIndices || p.descriptor.attribute==vgs::ShTemporalIndices;}),c.pages.end());
        c.groups[0].counts[0]=c.groups[0].counts[1]=0;return;
    }
    const size_t old=planes(sourceDegree),next=planes(targetDegree),S=c.groups[0].intervals+1;
    if (!old) {
        c.groups[0].counts[0]=c.groups[0].counts[1]=1;
        auto add=[&](uint32_t id,uint32_t group,Bytes bytes) {vgs::DecodedPage p;p.descriptor.attribute=id;p.descriptor.group=group;p.bytes=std::move(bytes);c.pages.push_back(std::move(p));};
        add(vgs::ShStaticBook,0,Bytes(next*3*6));add(vgs::ShTemporalBook,0,Bytes(S*next*3*6));
        for (uint32_t group=1;group<c.groups.size();++group) {add(vgs::ShStaticIndices,group,Bytes(c.groups[group].splats*next*4));add(vgs::ShTemporalIndices,group,Bytes(c.groups[group].splats*next*4));}
    } else for (auto &p:c.pages) {
        const auto id=p.descriptor.attribute;
        if (id==vgs::ShStaticIndices || id==vgs::ShTemporalIndices) p.bytes.resize(c.groups[p.descriptor.group].splats*next*4);
        else if (id==vgs::ShStaticBook) {
            const size_t stride=c.groups[0].counts[0]*6;p.bytes.resize(next*3*stride);
            if (sourceDegree==2 && targetDegree==3) std::fill(p.bytes.begin()+8*stride,p.bytes.begin()+9*stride,0);
        }
        else if (id==vgs::ShTemporalBook) {
            const size_t stride=3*c.groups[0].counts[1]*6;Bytes out(S*next*stride);
            for (size_t sample=0;sample<S;++sample) {
                std::memcpy(out.data()+sample*next*stride,p.bytes.data()+sample*old*stride,std::min(old,next)*stride);
                if (sourceDegree==2 && targetDegree==3) std::fill(out.begin()+sample*next*stride+8*stride/3,out.begin()+sample*next*stride+9*stride/3,0);
            }
            p.bytes=std::move(out);
        }
    }
    normalizeSchemas(c);
}
vgs::DecodedChunk editNativeChunk(vgs::DecodedChunk chunk,const NativeChunkPlan &plan,const Project &project,ExportResult *statistics,const ExportProgress &progress,const vgs::DecodedChunk *classificationSource) {
    report(progress,QStringLiteral("Filtering native Gaussian lifetimes"));
    normalizeSchemas(chunk);const size_t T=chunk.groups[0].intervals;
    vgs::FrameDecoder decoder(chunk,1./30,vgs::FrameDecoder::Contents::Positions);
    std::vector<size_t> offsets(chunk.groups.size()+1);
    for (size_t group=1;group<chunk.groups.size();++group) offsets[group+1]=offsets[group]+chunk.groups[group].splats;
    std::vector<std::vector<Span>> kept(chunk.groups.size());std::vector<std::vector<int>> starts(chunk.groups.size());
    for (size_t group=1;group<chunk.groups.size();++group) starts[group].assign(size_t(chunk.groups[group].splats),-1);
    const auto model=project.transform.matrix();CompiledModifiers modifiers(project);
    std::unique_ptr<vgs::FrameDecoder> colourDecoder;vgs::Frame colourFrame;
    if (!modifiers.greens.isEmpty()) colourDecoder=std::make_unique<vgs::FrameDecoder>(classificationSource ? *classificationSource : chunk);
    std::vector<float> positions;
    std::vector<QVector3D> worldPositions(offsets.back());std::vector<uint8_t> visibility(offsets.back());
    for (int sample=0;sample<=plan.intervals;++sample) {
        const bool closing=sample==plan.intervals;
        if (!closing) {
            const double normalized=std::min((plan.first+sample)/double(T),1.-1e-9);
            decoder.evaluatePositions(normalized,&positions);
            if (colourDecoder) colourDecoder->evaluateInto(normalized,false,&colourFrame);
            for (size_t group=1;group<chunk.groups.size();++group) {
                const auto &life=get(chunk,vgs::Lifetimes,uint32_t(group)).bytes;
                for (size_t row=0;row<chunk.groups[group].splats;++row) {const size_t record=offsets[group]+row,index=record*3;worldPositions[record]=model.map({positions[index],positions[index+1],positions[index+2]});
                    visibility[record]=plan.first+sample>=life[2*row] && plan.first+sample+1<=life[2*row+1] && modifiers.keepsPosition(worldPositions[record]) && (!colourDecoder || !modifiers.removesColour({colourFrame.colorDc[index],colourFrame.colorDc[index+1],colourFrame.colorDc[index+2]}));}
            }
            applyIsolation(worldPositions,visibility,modifiers.isolations,[&] {report(progress,QStringLiteral("Purge Isolated: searching neighbours"));return false;});
        }
        for (size_t group=1;group<chunk.groups.size();++group) {
            const auto &life=get(chunk,vgs::Lifetimes,uint32_t(group)).bytes;
            for (size_t row=0;row<chunk.groups[group].splats;++row) {
                bool active=!closing && plan.first+sample>=life[2*row] && plan.first+sample+1<=life[2*row+1],inside=active;
                if (active) {
                    const size_t record=offsets[group]+row,index=record*3;
                    Q_UNUSED(index);inside=visibility[record];
                }
                if (statistics && active) {if (inside) ++statistics->kept;else ++statistics->removed;}
                if (inside && starts[group][row]<0) starts[group][row]=sample;
                if (!inside && starts[group][row]>=0) {kept[group].push_back({row,uint8_t(starts[group][row]),uint8_t(sample)});starts[group][row]=-1;}
            }
        }
        report(progress,QStringLiteral("Filtering native frame %1 / %2").arg(sample).arg(plan.intervals));
    }
    for (auto &spans:kept) std::stable_sort(spans.begin(),spans.end(),[](const Span &a,const Span &b) {return a.row<b.row;});
    filterGroups(chunk,kept);
    // Preserve the sample arrays and their numerical descriptors; splitting lifetime
    // spans duplicates only affected rows, not a complete capture at every frame.
    int degree=0;for (const auto &p:chunk.pages) if (p.descriptor.attribute==vgs::ShStaticBook) {
        const size_t count=p.bytes.size()/chunk.groups[0].counts[0]/18;degree=count==5 ? 3 : count==3 ? 2 : 1;break;
    }
    if (chunk.groups.size()==1) addEmptyGroup(chunk,degree);
    convertTrimmedDirectPositions(chunk,plan);
    sliceTime(chunk,plan.first,plan.intervals);
    if (!axisUniform(project.transform.matrix())) {
        rotateOrientations(chunk,project,progress);rotateSh(chunk,project,statistics,progress);
        if (statistics) statistics->notes << QStringLiteral("Baked rotations are sampled at native frame times.");
    }
    transformPositions(chunk,project);
    compactRq(chunk,vgs::Sh0Terms,vgs::Sh0Trajectories,2,3);compactRq(chunk,vgs::OpacityTerms,vgs::OpacityTrajectories,3,1);
    compactResidual(chunk,true);compactResidual(chunk,false);compactSh(chunk,false);compactSh(chunk,true);
    normalizeSchemas(chunk);return chunk;
}
