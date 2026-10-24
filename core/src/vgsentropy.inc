// Private stream implementations shared by the VGS numerical models.
unsigned fseBits(const std::vector<uint32_t>& counts,uint32_t A,uint32_t nctx){
    uint32_t largest=0;
    for(uint32_t c=0;c<nctx;++c){uint32_t present=0;for(uint32_t j=0;j<A;++j)present+=counts[size_t(c)*A+j]!=0;largest=std::max(largest,present);}
    return unsigned(std::min(12,std::max(8,bitlen(largest?largest-1:0)+2)));
}
extern "C" size_t FSE_buildCTableU16(FSE_CTable*, const short*, unsigned, unsigned);
extern "C" size_t FSE_buildDTableU16(FSE_DTable*, const short*, unsigned, unsigned);

// The same positive normalized frequencies and varint table representation as
// the matched rANS control. Only the entropy state machine and stream framing differ.
template<class Ctx>
void encodeFseSymbols(Writer& w, const int32_t* sym, size_t N, const Ctx* ctx) {
    uint32_t A=1,nctx=1;
    for(size_t i=0;i<N;++i) {
        if(sym[i]<0||sym[i]>4095) throw Error("FSE symbol exceeds 12 bits");
        A=std::max(A,uint32_t(sym[i])+1);
        if(ctx) nctx=std::max(nctx,uint32_t(ctx[i])+1);
    }
    
    if(uint64_t(A)*nctx>kMaxTableEntries) throw Error("FSE contexts too large");
    std::vector<uint32_t> counts(size_t(nctx)*A),freq(size_t(nctx)*A);
    for(size_t i=0;i<N;++i) ++counts[(ctx?size_t(ctx[i])*A:0)+sym[i]];
    const unsigned bits=fseBits(counts,A,nctx),M=1u<<bits;
    w.varint(A); w.u8(bits); w.varint(nctx);
    const size_t stride=FSE_CTABLE_SIZE_U32(bits,A-1);
    std::vector<uint32_t> ct(size_t(nctx)*stride);
    std::vector<short> norm(A);
    for(uint32_t c=0;c<nctx;++c) {
        uint32_t* f=freq.data()+size_t(c)*A;
        normalize(counts.data()+size_t(c)*A,A,f,M);
        writeTable(w,f,A);
        uint32_t sum=0;
        for(uint32_t j=0;j<A;++j) { norm[j]=short(f[j]);sum+=f[j]; }
        if(sum) {
            const size_t code=FSE_buildCTableU16(ct.data()+size_t(c)*stride,norm.data(),A-1,bits);
            if(FSE_isError(code)) throw Error(FSE_getErrorName(code));
        }
    }
    const unsigned states=N>=4096?4:1;
    w.u8(uint8_t(states));
    Bytes packed(N*2+128);
    BIT_CStream_t bs;
    if(FSE_isError(BIT_initCStream(&bs,packed.data(),packed.size()))) throw Error("FSE initC");
    FSE_CState_t st[4];
    for(unsigned j=0;j<states;++j) { st[j].value=M; st[j].stateLog=bits; }
    unsigned pending=0;
    for(size_t i=N;i-->0;) {
        const unsigned j=unsigned(i%states),c=ctx?uint32_t(ctx[i]):0;
        const auto* table=ct.data()+size_t(c)*stride;
        st[j].stateTable=reinterpret_cast<const uint16_t*>(table)+2;
        st[j].symbolTT=table+1+(M>>1);
        FSE_encodeSymbol(&bs,&st[j],unsigned(sym[i]));
        // Two 12-bit symbols fit even the 32-bit WASM bit container.
        if(++pending==2) { BIT_flushBits(&bs); pending=0; }
    }
    if(pending) BIT_flushBits(&bs);
    for(unsigned j=states;j-->0;) FSE_flushCState(&bs,&st[j]);
    const size_t n=BIT_closeCStream(&bs);
    if(!n||FSE_isError(n)) throw Error("FSE closeC");
    w.varint(n); w.bytes(packed.data(),n);
}

struct FseTables
{
    uint32_t A = 0, nctx = 0, M = 0, mask = 0;
    int bits = 0;
    std::vector<uint32_t> freq, dtable;
    std::vector<uint8_t> active;
    

    void read(Reader& r)
    {
        const auto start = decodeStats ? ProfileClock::now() : ProfileClock::time_point{};
        const size_t before = r.remaining();
        const uint64_t a = r.varint();
        bits = r.u8();
        const uint64_t n = r.varint();
        if (a < 1 || a > 4096 || bits < 8 || bits > 12 || n < 1 || mul(a, n) > kMaxTableEntries ||
            mul(n, uint64_t(1) << bits) > (uint64_t(1) << 26))
            throw Error("malformed symbol table");
        A = uint32_t(a);
        nctx = uint32_t(n);
        M = 1u << bits;
        mask = M - 1;
        freq.assign(size_t(nctx) * A, 0);
        dtable.assign(size_t(nctx) * (M+1), 0); active.assign(nctx,0);
        
        for (uint32_t c = 0; c < nctx; ++c) {
            uint32_t* f = &freq[size_t(c) * A];
            const uint8_t mode = r.u8();
            if(mode==3){
                const uint64_t length=r.varint();const auto* data=r.bytes(length);const size_t size=size_t(length);
                std::vector<short> norm(A);unsigned max=A-1,log=0;
                const size_t n=FSE_readNCount(norm.data(),&max,&log,data,size);
                if(FSE_isError(n)||n!=size||log!=unsigned(bits))throw Error("invalid FSE NCount");
                for(uint32_t j=0;j<A;++j)f[j]=uint32_t(norm[j]<0?-norm[j]:norm[j]);
            }else if (mode == 2) {
                for (uint32_t s = 0; s < A; ++s) {
                    const uint64_t v = r.varint();
                    if (v > M)
                        throw Error("malformed frequency");
                    f[s] = uint32_t(v);
                }
            } else if (mode == 1) {
                const uint64_t k = r.varint();
                if (k > A)
                    throw Error("malformed frequency table");
                int64_t s = -1;
                for (uint64_t j = 0; j < k; ++j) {
                    const uint64_t delta=r.varint(), next=uint64_t(s+1);
                    if(delta>=uint64_t(A)-next)throw Error("malformed frequency index");
                    s+=int64_t(delta)+1;
                    const uint64_t frequency=r.varint();
                    if(frequency>=M)throw Error("malformed frequency");
                    const uint64_t v=frequency+1;
                    if (s >= int64_t(A) || v > M)
                        throw Error("malformed frequency table");
                    f[s] = uint32_t(v);
                }
            } else if (mode != 0) {
                throw Error("malformed frequency table");
            }
            uint64_t acc = 0;
            for (uint32_t s = 0; s < A; ++s) {
                
                if (f[s]) {
                    if (acc + f[s] > M)
                        throw Error("frequencies exceed their scale");
                    
                }
                acc += f[s];
            }
            if (acc != 0 && acc != M)
                throw Error("frequencies do not sum to their scale");
            if(acc){
                active[c]=1;std::vector<short> norm(A);
                for(uint32_t j=0;j<A;++j)norm[j]=short(f[j]);
                const size_t code=FSE_buildDTableU16(dtable.data()+size_t(c)*(M+1),norm.data(),A-1,bits);
                if(FSE_isError(code))throw Error(FSE_getErrorName(code));
            }
        }
        if (decodeStats) {
            decodeStats->tableMilliseconds += elapsedMs(start);
            decodeStats->tablePayloadBytes += before - r.remaining();
            decodeStats->maxTableBytes = std::max(decodeStats->maxTableBytes,
                uint64_t(nctx) * (uint64_t(A)*4 + uint64_t(M+1)*4));
        }
    }
};


// Reference FSEU16 table layout: four bytes, a 12-bit symbol and 4-bit bit count.
struct FseEntry { unsigned short newState; unsigned short nbBits:4; unsigned short symbol:12; };
static_assert(sizeof(FseEntry)==4,"FSE U16 ABI mismatch");

template<class Out,class CtxOf>
void decodeFseLoop(Reader& r,Out* out,size_t N,const FseTables& t,CtxOf ctxOf) {
    const auto start=decodeStats?ProfileClock::now():ProfileClock::time_point{};
    const unsigned states=r.u8();
    if(states!=1&&states!=4) throw Error("FSE states");
    const uint64_t length=r.varint();const auto* data=r.bytes(length);const size_t n=size_t(length);
    BIT_DStream_t bs;
    if(FSE_isError(BIT_initDStream(&bs,data,n))) throw Error("FSE initD");
    size_t state[4];
    for(unsigned j=0;j<states;++j) { state[j]=BIT_readBits(&bs,t.bits); BIT_reloadDStream(&bs); }
    const auto one=[&](size_t i,unsigned j) {
        const uint32_t c=ctxOf(i,out);
        if(c>=t.nctx||!t.active[c]) throw Error("FSE context");
        const auto* dt=t.dtable.data()+size_t(c)*(t.M+1)+1;
        if(state[j]>=t.M)throw Error("invalid FSE state");
        FseEntry e;std::memcpy(&e,dt+state[j],sizeof(e));
        if(state[j]>=t.M||e.symbol>=t.A||bs.bitsConsumed+e.nbBits>sizeof(size_t)*8)throw Error("invalid FSE state");
        out[i]=Out(e.symbol);
        state[j]=e.newState+BIT_readBits(&bs,e.nbBits);
    };
    size_t i=0;
    if(states==4) {
        for(;i+4<=N;i+=4) {
            one(i,0);one(i+1,1);BIT_reloadDStream(&bs);
            one(i+2,2);one(i+3,3);BIT_reloadDStream(&bs);
        }
    }
    unsigned pending=0;
    for(;i<N;++i) { one(i,unsigned(i%states));if(++pending==2) {BIT_reloadDStream(&bs);pending=0;} }
    BIT_reloadDStream(&bs);
    if(!BIT_endOfDStream(&bs)) throw Error("FSE trailing bitstream");
    for(unsigned j=0;j<states;++j) if(state[j]!=0) throw Error("FSE final state");
    if(decodeStats){decodeStats->symbolMilliseconds+=elapsedMs(start);++decodeStats->streams;decodeStats->symbols+=N;}
}

template<class Out>void checkFseOutput(const FseTables& t){if(sizeof(Out)==1&&t.A>256)throw Error("FSE output alphabet");}
template<class Out,class Ctx>void decodeFseSymbols(Reader& r,Out* out,size_t N,Ctx context){FseTables t;t.read(r);checkFseOutput<Out>(t);decodeFseLoop(r,out,N,t,context);}
void encodePcoSymbols(Writer& w,const int32_t* symbols,size_t N){
    uint32_t A=1;for(size_t i=0;i<N;++i){if(symbols[i]<0||symbols[i]>65535)throw Error("Pcodec symbol alphabet");A=std::max(A,uint32_t(symbols[i])+1);}
    w.varint(A);w.varint(1);w.varint(N);
    const auto packed=pcoCompress(reinterpret_cast<const uint8_t*>(symbols),N*4,4);w.varint(packed.size());w.bytes(packed);
}
template<class Out>void decodePcoSymbols(Reader& r,Out* out,size_t N){
    const auto A=r.varint(),contexts=r.varint(),count=r.varint();
    if(!A||A>65536||contexts!=1||count!=N||(sizeof(Out)==1&&A>256))throw Error("invalid Pcodec symbol header");
    const uint64_t length=r.varint();const auto* data=r.bytes(length);const size_t size=size_t(length);
    const auto start=decodeStats?ProfileClock::now():ProfileClock::time_point{};
    if constexpr(sizeof(Out)==4){pcoDecompress(data,size,reinterpret_cast<uint8_t*>(out),N*4,4);}
    else {std::vector<uint32_t> values(N);pcoDecompress(data,size,reinterpret_cast<uint8_t*>(values.data()),N*4,4);for(size_t i=0;i<N;++i){if(values[i]>=A)throw Error("Pcodec symbol range");out[i]=Out(values[i]);}}
    for(size_t i=0;i<N;++i)if(uint32_t(out[i])>=A)throw Error("Pcodec symbol range");
    if(decodeStats){decodeStats->symbolMilliseconds+=elapsedMs(start);++decodeStats->streams;decodeStats->symbols+=N;decodeStats->tablePayloadBytes+=1;decodeStats->maxTableBytes=std::max<uint64_t>(decodeStats->maxTableBytes,1);}
}
