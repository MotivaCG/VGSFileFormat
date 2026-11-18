// End-to-end fixtures for importing PGS into UE, using the real signed exporter.
#include "pgscapture.h"
#include "pgsexport.h"
#include "meshio.h"
#define VGSBRIDGE_STATIC
#include "vgsbridge.h"
#undef VGSBRIDGE_STATIC
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QTextStream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 3, "usage: pgs_unreal_fixture <output folder> <vgsbridge DLL>");
        QDir dir(QString::fromLocal8Bit(argv[1])); require(QDir().mkpath(dir.path()), "mkdir");
        QFile ply(dir.filePath("source.ply")); require(ply.open(QIODevice::WriteOnly), "PLY");
        QTextStream out(&ply);
        out << "ply\nformat ascii 1.0\nelement vertex 128\n";
        for (const char* name : {"x","y","z","nx","ny","nz","f_dc_0","f_dc_1","f_dc_2"}) out << "property float " << name << '\n';
        for (int k=0;k<9;++k) out << "property float f_rest_" << k << '\n';
        for (const char* name : {"opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3"}) out << "property float " << name << '\n';
        out << "end_header\n";
        for (int i=0;i<128;++i) {
            const double z=1.0-2.0*(i+0.5)/128, r=std::sqrt(1-z*z), a=i*2.3999632297;
            const double x=r*std::cos(a), y=r*std::sin(a);
            out << x*0.3 << ' ' << y*0.3 << ' ' << z*0.3 << ' ' << x << ' ' << y << ' ' << z << " 0.2 0.1 0.05";
            for(int k=0;k<9;++k) out << ' ' << (k+1)*0.001;
            out << " 2 -3.5 -3.5 -4.5 1 0 0 0\n";
        }
        out.flush(); ply.close();
        auto capture=PgsCapture::load(ply.fileName());
        PgsExportOptions options;
        exportPgs(capture, options, dir.filePath("plain.pgs"));
        MeshData mesh;
        mesh.positions={-0.3f,-0.3f,0, 0.3f,-0.3f,0, 0.3f,0.3f,0, -0.3f,0.3f,0};
        mesh.normals={0,0,1,0,0,1,0,0,1,0,0,1}; mesh.uvs={0,0,1,0,1,1,0,1}; mesh.indices={0,1,2,0,2,3};
        const QByteArray embedded=writeGlb(mesh, QImage(), QImage());
        options.glb=embedded;
        exportPgs(capture, options, dir.filePath("mesh.pgs"));
        // An authenticated but unsupported GLB must fail clearly in the UE importer.
        PgsExportOptions unsupported; unsupported.glb=QByteArray("not a GLB");
        exportPgs(capture, unsupported, dir.filePath("unsupported.pgs"));
        QFile source(dir.filePath("mesh.pgs")); require(source.open(QIODevice::ReadOnly), "read mesh");
        auto bytes=source.readAll(); source.close();
        const auto offset=bytes.indexOf(options.glb); require(offset>=0, "embedded GLB not found");
        bytes[offset+20]^=1;
        QFile corrupt(dir.filePath("corrupt.pgs")); require(corrupt.open(QIODevice::WriteOnly), "corrupt");
        corrupt.write(bytes); corrupt.close();
        capture.frame.normal.clear(); options.glb.clear();
        exportPgs(capture, options, dir.filePath("no_normals.pgs"));

        QLibrary library(QString::fromLocal8Bit(argv[2])); require(library.load(), "bridge load");
        auto open=reinterpret_cast<decltype(&vgsb_open_still)>(library.resolve("vgsb_open_still"));
        auto close=reinterpret_cast<decltype(&vgsb_close_still)>(library.resolve("vgsb_close_still"));
        require(open && close, "still exports missing");
        for (const char* name : {"plain.pgs","mesh.pgs","no_normals.pgs"}) {
            vgsb_frame f{}; const uint8_t* proxy=nullptr; size_t size=0;
            auto* still=open(dir.filePath(name).toUtf8().constData(), &f, &proxy, &size);
            require(still && f.count==128 && f.sh_coefficients==3 && f.sh, "frame/SH data");
            require((f.normals!=nullptr)==(QString(name)!="no_normals.pgs"), "normals presence");
            require((size!=0)==(QString(name)=="mesh.pgs"), "proxy presence");
            for(size_t i=0;i<f.count;++i) {
                require(std::isfinite(f.positions[i*3]) && f.scales[i*3]>0, "position/scale");
                require(std::abs(f.rotations[i*4]-1)<0.001f, "wxyz quaternion");
                require(f.radiance[i*4+3]>0 && f.radiance[i*4+3]<=1, "activated opacity");
                if(f.normals) require(std::abs(f.normals[i*3]*f.normals[i*3]+f.normals[i*3+1]*f.normals[i*3+1]+f.normals[i*3+2]*f.normals[i*3+2]-1)<0.01f,"unit normals");
            }
            if(size) require(size==size_t(embedded.size()) && std::memcmp(proxy,embedded.constData(),size)==0, "GLB bytes");
            close(still);
        }
        vgsb_frame frame{}; const uint8_t* proxy=reinterpret_cast<const uint8_t*>(1); size_t size=1;
        require(!open(dir.filePath("corrupt.pgs").toUtf8().constData(),&frame,&proxy,&size),"corrupt payload accepted");
        require(!frame.count && !proxy && !size,"failure did not clear outputs");
        require(!open(nullptr,&frame,&proxy,&size),"null path accepted");
        close(nullptr);
        std::puts("PGS fixtures and synchronous bridge checks passed");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
