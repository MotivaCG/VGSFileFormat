//! Private C ABI for VGS. Caller-owned, aligned buffers; no keys or VGS writer here.
use pco::standalone::{simple_compress, simple_decompress_into};
use std::alloc::{GlobalAlloc, Layout};

// Share the caller's allocator, including Emscripten memory-growth bookkeeping.
extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn realloc(p: *mut u8, n: usize) -> *mut u8;
}
struct Allocator;
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if l.align() <= 16 { return malloc(l.size()); }
        let Some(n)=l.size().checked_add(l.align()).and_then(|n|n.checked_add(std::mem::size_of::<usize>())) else{return std::ptr::null_mut()};
        let p=malloc(n);if p.is_null(){return p;}
        let aligned=((p as usize)+std::mem::size_of::<usize>()+l.align()-1)&!(l.align()-1);
        ((aligned as *mut usize).sub(1)).write(p as usize);aligned as *mut u8
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { if l.align()<=16{free(p)}else{free(((p as *mut usize).sub(1)).read() as *mut u8)} }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        if l.align()<=16{return realloc(p,n);}
        let q=self.alloc(Layout::from_size_align_unchecked(n,l.align()));
        if !q.is_null(){std::ptr::copy_nonoverlapping(p,q,l.size().min(n));self.dealloc(p,l);}q
    }
}
#[global_allocator]
static ALLOC: Allocator = Allocator;

#[no_mangle]
pub unsafe extern "C" fn vgs_pco_compress(p: *const u8, bytes: usize, kind: u32, size: *mut usize) -> *mut u8 {
    if bytes == 0 || p.is_null() || size.is_null() { return std::ptr::null_mut(); }
    let cfg = pco::ChunkConfig::default().with_enable_8_bit(true);
    macro_rules! enc { ($t:ty) => {{
        if bytes % std::mem::size_of::<$t>() != 0 { return std::ptr::null_mut(); }
        simple_compress(std::slice::from_raw_parts(p as *const $t, bytes/std::mem::size_of::<$t>()), &cfg)
    }} }
    let encoded = match kind { 1=>enc!(u8), 2=>enc!(u16), 4=>enc!(u32), 5=>enc!(half::f16), _=>return std::ptr::null_mut() };
    match encoded {
        Ok(v)=>{ *size=v.len(); Box::into_raw(v.into_boxed_slice()) as *mut u8 },
        Err(_)=>std::ptr::null_mut()
    }
}

#[no_mangle]
pub unsafe extern "C" fn vgs_pco_release(p: *mut u8, size: usize) {
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(p,size)));
}

#[no_mangle]
pub unsafe extern "C" fn vgs_pco_decompress(p: *const u8, size: usize, out: *mut u8, bytes: usize, kind: u32) -> i32 {
    if p.is_null() || out.is_null() || size==0 || bytes==0 { return 1; }
    let data=std::slice::from_raw_parts(p,size);
    macro_rules! dec { ($t:ty) => {{
        if bytes % std::mem::size_of::<$t>() != 0 { return 1; }
        let dst=std::slice::from_raw_parts_mut(out as *mut $t,bytes/std::mem::size_of::<$t>());
        match simple_decompress_into(data,dst) {
            Ok(progress) if progress.finished && progress.n_processed==dst.len()=>0,
            _=>1
        }
    }} }
    match kind {1=>dec!(u8),2=>dec!(u16),4=>dec!(u32),5=>dec!(half::f16),_=>1}
}
