#include "pcodec_bridge.h"
#include <cstring>
extern "C" {
uint8_t* vgs_pco_compress(const uint8_t*,size_t,unsigned,size_t*);
void vgs_pco_release(uint8_t*,size_t);
int vgs_pco_decompress(const uint8_t*,size_t,uint8_t*,size_t,unsigned);
}
namespace mgs {
Bytes pcoCompress(const uint8_t* data,size_t size,unsigned kind) {
    if(!size)return {};
    std::vector<uint64_t> aligned((size+7)/8);std::memcpy(aligned.data(),data,size);
    size_t n=0;auto* p=vgs_pco_compress(reinterpret_cast<const uint8_t*>(aligned.data()),size,kind,&n);
    if(!p)throw Error("Pcodec compression failed");
    try {Bytes result(p,p+n);vgs_pco_release(p,n);return result;}
    catch(...){vgs_pco_release(p,n);throw;}
}
void pcoDecompress(const uint8_t* data,size_t size,uint8_t* out,size_t bytes,unsigned kind) {
    if(!bytes){if(size)throw Error("nonempty Pcodec stream");return;}
    const size_t alignment=kind==4?4:kind==1?1:2;
    if(reinterpret_cast<uintptr_t>(out)%alignment ||
       vgs_pco_decompress(data,size,out,bytes,kind))throw Error("invalid Pcodec stream");
}
}
