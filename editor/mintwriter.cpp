#include "mintwriter.h"
#include "nativeexport.h"
#include "vgsframe.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
constexpr uint32_t sharedSlots[]={8,112,128,168,184,208,232,248,264,304,320};
constexpr uint32_t groupSlots[]={56,136,200,216,232,248,264,280,296,312,328,344,360,376,392};
void put64(vgs::Bytes &bytes,size_t offset,uint64_t value) {
    if (offset+8>bytes.size()) throw std::runtime_error("Invalid MINT header offset.");
    for (int i=0;i<8;++i) bytes[offset+i]=uint8_t(value>>(8*i));
}
void putDouble(vgs::Bytes &bytes,size_t offset,double value) {uint64_t bits;std::memcpy(&bits,&value,8);put64(bytes,offset,bits);}
uint64_t padded(uint64_t size) {return size ? (size/256+1)*256 : 0;}
const vgs::Bytes *array(const vgs::DecodedChunk &chunk,uint32_t group,uint32_t attribute) {
    for (const auto &page:chunk.pages) if (page.descriptor.group==group && page.descriptor.attribute==attribute) return &page.bytes;
    return nullptr;
}
vgs::DecodedChunk prepare(vgs::DecodedChunk chunk,int degree,uint64_t intervals) {
    setNativeShDegree(chunk,degree,3);
    if (chunk.groups.empty() || chunk.groups.front().type!=0) throw std::runtime_error("MINT export requires a shared dictionary block.");
    if (chunk.groups.size()==1) {vgs::Group empty;empty.type=1;empty.intervals=intervals;chunk.groups.push_back(empty);}
    return chunk;
}
struct BlockPlan {vgs::Bytes header;std::vector<uint64_t> sizes;uint64_t payloadSize=0;};
BlockPlan blockPlan(const vgs::DecodedChunk &chunk,uint32_t group) {
    const auto &g=chunk.groups[group];const bool shared=g.type==0;
    if (!shared && g.type!=1) throw std::runtime_error("Unsupported MINT block type.");
    BlockPlan result;result.header.resize(shared ? 336 : 416);const auto *arraySlots=shared ? sharedSlots : groupSlots;const int count=shared ? 11 : 15;
    if (shared) {
        const int counts[]={144,152,160,200,224,296};for (int i=0;i<6;++i) put64(result.header,counts[i],g.counts[i]);
        putDouble(result.header,280,g.trajectoryMin);putDouble(result.header,288,g.trajectoryMax);
    } else {
        put64(result.header,8,g.intervals);put64(result.header,16,g.splats);putDouble(result.header,24,g.positionMin);putDouble(result.header,32,g.positionMax);put64(result.header,408,g.meshSamples);
    }
    for (int i=0;i<count;++i) {
        const auto *bytes=array(chunk,group,(shared ? vgs::ScaleLut : vgs::ScaleIndices)+i);const uint64_t size=bytes ? bytes->size() : 0;
        result.sizes.push_back(size);put64(result.header,arraySlots[i],result.payloadSize);put64(result.header,arraySlots[i]+8,padded(size));result.payloadSize+=padded(size);
    }
    return result;
}
}
void writeMintSequence(const vgs::Header &source,const std::function<vgs::DecodedChunk(size_t)> &provider,
                       const vgs::WriteSink &sink,const vgs::Progress &progress) {
    if (source.chunks.empty() || !source.timeDenominator || !provider || !sink) throw std::runtime_error("Invalid MINT sequence.");
    const size_t count=source.chunks.size();const double tick=double(source.timeNumerator)/source.timeDenominator;
    auto report=[&](size_t done) {if (progress && !progress(int(done),int(2*count))) throw std::runtime_error("Export cancelled.");};
    std::vector<std::vector<BlockPlan>> plans(count);size_t metadataSize=48+56+24*count;
    std::array<double,6> bounds{};bool any=false;
    for (size_t ci=0;ci<count;++ci) {
        report(ci);auto chunk=prepare(provider(ci),source.shDegree,source.chunks[ci].intervals);
        for (uint32_t group=0;group<chunk.groups.size();++group) {plans[ci].push_back(blockPlan(chunk,group));metadataSize+=32+plans[ci].back().header.size();}
        vgs::FrameDecoder decoder(chunk,tick);
        for (uint64_t sample=0;sample<source.chunks[ci].intervals;++sample) {
            const auto frame=decoder.evaluate(double(sample)/source.chunks[ci].intervals,false);
            for (size_t row=0;row<frame.count;++row) if (frame.active[row]) {
                for (int axis=0;axis<3;++axis) {const double v=frame.position[row*3+axis];bounds[axis]=any ? std::min(bounds[axis],v) : v;bounds[axis+3]=any ? std::max(bounds[axis+3],v) : v;}
                any=true;
            }
        }
    }
    metadataSize=(metadataSize+255)/256*256;vgs::Bytes metadata(metadataSize);
    put64(metadata,0,6);put64(metadata,8,1);put64(metadata,16,metadataSize);put64(metadata,24,1);put64(metadata,40,48);
    put64(metadata,48,count);for (int i=0;i<6;++i) putDouble(metadata,56+i*8,bounds[i]);
    size_t descriptor=104+count*24;uint64_t payloadOffset=0;
    for (size_t ci=0;ci<count;++ci) {
        const size_t entry=104+ci*24;putDouble(metadata,entry,source.chunks[ci].intervals*tick);put64(metadata,entry+8,plans[ci].size());put64(metadata,entry+16,descriptor);
        for (size_t group=0;group<plans[ci].size();++group) {
            const auto &plan=plans[ci][group];put64(metadata,descriptor,group==0 ? 3 : 1);put64(metadata,descriptor+8,payloadOffset);put64(metadata,descriptor+16,plan.payloadSize);put64(metadata,descriptor+24,plan.header.size());
            std::copy(plan.header.begin(),plan.header.end(),metadata.begin()+descriptor+32);descriptor+=32+plan.header.size();payloadOffset+=plan.payloadSize;
        }
    }
    sink(0,metadata.data(),metadata.size());uint64_t offset=metadataSize;const uint8_t zeros[256]={};
    for (size_t ci=0;ci<count;++ci) {
        report(count+ci);auto chunk=prepare(provider(ci),source.shDegree,source.chunks[ci].intervals);
        if (chunk.groups.size()!=plans[ci].size()) throw std::runtime_error("MINT source changed during export.");
        for (uint32_t group=0;group<chunk.groups.size();++group) {
            const auto &plan=plans[ci][group];if (blockPlan(chunk,group).header!=plan.header) throw std::runtime_error("MINT attributes changed during export.");
            for (size_t i=0;i<plan.sizes.size();++i) {
                const auto *bytes=array(chunk,group,(group==0 ? vgs::ScaleLut : vgs::ScaleIndices)+uint32_t(i));const auto size=plan.sizes[i];
                if (size) {sink(offset,bytes->data(),size);offset+=size;uint64_t padding=padded(size)-size;
                    while (padding) {const size_t n=size_t(std::min<uint64_t>(padding,sizeof zeros));sink(offset,zeros,n);offset+=n;padding-=n;}}
            }
        }
    }
    report(2*count);
}
