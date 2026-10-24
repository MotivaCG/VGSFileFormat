// Production backends: all numerical models, half bit patterns and short streams.
#include "mgscodec.h"
#include "pcodec_bridge.h"
#include <cstdio>
#include <cstring>
#include <future>
using mgs::Bytes;
void require(bool ok,const char* message){if(!ok)throw mgs::Error(message);}
void test(const mgs::AttributeSpec& spec,const Bytes& raw){
    for(auto codec:{mgs::VgsEntropy::Fse,mgs::VgsEntropy::Fse8,mgs::VgsEntropy::Pcodec})
        for(int model=0;model<mgs::attributeModelCount(spec);++model){
            const auto packed=mgs::encodeVgsAttribute(spec,model,raw.data(),raw.size(),codec);
            mgs::DecodeStats stats;
            require(mgs::decodeVgsAttribute(spec,model,packed.data(),packed.size(),codec,&stats)==raw,"hybrid roundtrip");
            if(!raw.empty())require(stats.streams>0&&stats.symbols>0,"hybrid profiling");
            auto extra=packed;extra.push_back(0);bool refused=false;
            try{mgs::decodeVgsAttribute(spec,model,extra.data(),extra.size(),codec);}catch(const mgs::Error&){refused=true;}
            require(refused,"hybrid trailing data accepted");
            if(!packed.empty()){
                refused=false;try{mgs::decodeVgsAttribute(spec,model,packed.data(),packed.size()-1,codec);}catch(const mgs::Error&){refused=true;}
                require(refused,"hybrid truncated data accepted");
            }
        }
}
int main(){try{
    mgs::AttributeSpec half;half.kind=0;half.rows=65536;
    Bytes patterns(131072);for(unsigned i=0;i<65536;++i){patterns[2*i]=uint8_t(i);patterns[2*i+1]=uint8_t(i>>8);}
    test(half,patterns);
    for(unsigned kind:{1u,2u,4u,5u}){
        const auto p=mgs::pcoCompress(patterns.data(),patterns.size(),kind);Bytes out(patterns.size());
        mgs::pcoDecompress(p.data(),p.size(),out.data(),out.size(),kind);
        require(out==patterns,"numeric Pcodec changed half bits");
    }
    for(size_t count:{size_t(1),size_t(31),size_t(4097)}){
        mgs::AttributeSpec channels;channels.kind=1;channels.family=3;channels.fields={0,0,0};channels.rows=count;
        Bytes raw(count*3);for(size_t i=0;i<raw.size();++i)raw[i]=uint8_t((i%31)*7);test(channels,raw);
        channels.family=4;channels.intervals=1;test(channels,raw);
    }
    // Backend state is thread-local and restores to MGS/rANS after each call.
    mgs::AttributeSpec small;small.kind=1;small.rows=257;Bytes raw(257,7);
    const auto before=mgs::encodeAttribute(small,0,raw.data(),raw.size());
    auto a=std::async(std::launch::async,[&]{test(small,raw);});
    auto b=std::async(std::launch::async,[&]{test(small,raw);});a.get();b.get();
    require(mgs::encodeAttribute(small,0,raw.data(),raw.size())==before,"MGS stream changed");
    std::puts("FSE/Pcodec exact, malformed streams refused, MGS unchanged");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
