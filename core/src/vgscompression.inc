// Authoring-only policy selection. Cost is measured on all pages, with one codec
// per attribute and a global <1% bound against the existing rANS representation.
#include "vgsdecodeprofile.inc"
struct CompressionCandidate {
    uint32_t codec=Raw,model=0;
    uint64_t payload=0,storage=0;
};
std::vector<CompressionCandidate> compressionCandidates(const Spec& s){
    std::vector<CompressionCandidate> result;
    for(uint32_t codec:{uint32_t(Rans),uint32_t(Fse),uint32_t(Fse8),uint32_t(PcodecSymbols)})
        for(int model=0;model<mgs::attributeModelCount(s);++model)result.push_back({codec,uint32_t(model)});
    for(uint32_t kind:{1u,2u,4u,5u})if(kind!=5||s.kind==0)result.push_back({PcodecNumeric,kind});
    return result;
}
Bytes compressPage(const Spec& s,uint32_t codec,uint32_t model,const uint8_t* data,size_t size,mgs::EntropySearch search){
    if(codec==Raw)return Bytes(data,data+size);
    if(codec==PcodecNumeric)return mgs::pcoCompress(data,size,model);
    return mgs::encodeVgsAttribute(s,int(model),data,size,mgs::VgsEntropy(codec),search);
}
struct CompressionCosts {
    uint32_t family=0;
    uint64_t raw=0,rawStorage=0;
    std::vector<CompressionCandidate> candidates;
};
std::vector<Policy> compressionPolicies(const std::map<uint32_t,CompressionCosts>& costs){
    struct Option {CompressionCandidate candidate;double work;};
    std::vector<std::vector<Option>> options;
    std::vector<uint32_t> attributes;
    uint64_t baseline=0;
    for(const auto& kv:costs){
        const auto& t=kv.second;attributes.push_back(kv.first);
        std::map<uint32_t,CompressionCandidate> best;
        best[Raw]={Raw,0,t.raw,t.rawStorage};
        for(const auto& c:t.candidates){
            if(c.payload==UINT64_MAX)continue;
            auto it=best.find(c.codec);
            if(it==best.end()||c.payload<it->second.payload)best[c.codec]=c;
        }
        const auto rans=best.find(Rans);
        baseline=add(baseline,rans!=best.end()&&rans->second.payload<t.raw?rans->second.storage:t.rawStorage);
        std::vector<Option> choices;
        for(const auto& item:best)choices.push_back({item.second,decoderCost(kv.first,item.first,t.raw)});
        options.push_back(std::move(choices));
    }
    // Integer bound, strictly below +1%; unchanged directories/header add the same
    // overhead to both formats. Alignment is included in each measured storage cost.
    const uint64_t limit=add(baseline,baseline?((baseline-1)/100):0);
    std::vector<uint64_t> remaining(options.size()+1);
    for(size_t i=options.size();i-->0;){uint64_t least=UINT64_MAX;for(const auto& o:options[i])least=std::min(least,o.candidate.storage);remaining[i]=add(remaining[i+1],least);}
    struct State {uint64_t bytes=0;double work=0;std::vector<uint32_t> path;};
    std::vector<State> states(1);
    for(size_t i=0;i<options.size();++i){
        std::vector<State> next;
        for(const auto& s:states)for(size_t j=0;j<options[i].size();++j){
            const auto& o=options[i][j];const uint64_t bytes=add(s.bytes,o.candidate.storage);
            if(add(bytes,remaining[i+1])>limit)continue;
            auto path=s.path;path.push_back(uint32_t(j));next.push_back({bytes,s.work+o.work,std::move(path)});
        }
        std::sort(next.begin(),next.end(),[](const State& a,const State& b){return a.bytes<b.bytes||(a.bytes==b.bytes&&a.work<b.work);});
        states.clear();double work=std::numeric_limits<double>::infinity();
        for(auto& s:next)if(s.work<work){work=s.work;states.push_back(std::move(s));}
        if(states.empty())throw Error("no feasible VGS compression policy");
        // Bound authoring memory on unusual captures; retain both cheapest and
        // fastest frontier endpoints. Every retained state still obeys the bound.
        if(states.size()>8192){std::vector<State> reduced;for(size_t k=0;k<8192;++k)reduced.push_back(std::move(states[k*(states.size()-1)/8191]));states=std::move(reduced);}
    }
    std::vector<Policy> result;
    if(states.empty())return result;
    const auto& selected=states.back();
    for(size_t i=0;i<attributes.size();++i){
        const uint32_t attribute=attributes[i];const auto& c=options[i][selected.path[i]].candidate;
        Policy p;p.attribute=attribute;p.codec=c.codec;p.model=c.model;p.family=costs.at(attribute).family;
        if(attribute==OpacityScales||attribute==Normals)p.flags=OptionalAttribute;
        result.push_back(p);
    }
    return result;
}
