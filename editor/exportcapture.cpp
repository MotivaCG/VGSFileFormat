#include "exportcapture.h"
#include "nativeexport.h"
#include "mintfile.h"
#include "mintskinrecovery.h"
#include "despillcolor.h"
#include "vgsdecoder/vgsdecoder.h"
#include "vgssign.h"
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <qfloat16.h>
#include <limits>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace {
constexpr double C0 = 0.28209479177387814, Pi = 3.14159265358979323846;
using Matrix = Eigen::Matrix3d;
using ShMatrix = Eigen::Matrix<double,16,16>;
using Colour = std::array<float,3>;
void report(const ExportProgress &progress,int value,const QString &message) {
    if (progress && !progress(value,message)) throw std::runtime_error("Export cancelled.");
}
Matrix linear(const QMatrix4x4 &m) {
    Matrix a; for (int i=0;i<3;++i) for (int j=0;j<3;++j) a(i,j)=m(i,j); return a;
}
Eigen::Matrix<double,16,1> basis(const Eigen::Vector3d &d) {
    double x=d.x(),y=d.y(),z=d.z(); Eigen::Matrix<double,16,1> b;
    b << C0,-0.4886025119029199*y,0.4886025119029199*z,-0.4886025119029199*x,
        1.0925484305920792*x*y,-1.0925484305920792*y*z,0.31539156525252005*(2*z*z-x*x-y*y),
        -1.0925484305920792*x*z,0.5462742152960396*(x*x-y*y),
        -0.5900435899266435*y*(3*x*x-y*y),2.890611442640554*x*y*z,
        -0.4570457994644658*y*(4*z*z-x*x-y*y),0.3731763325901154*z*(2*z*z-3*x*x-3*y*y),
        -0.4570457994644658*x*(4*z*z-x*x-y*y),1.445305721320277*z*(x*x-y*y),
        -0.5900435899266435*x*(x*x-3*y*y); return b;
}
// Gauss-Legendre integration is exact for rotations of degree <= 3. General affine
// direction warps are projected to the chosen finite SH band (not closed under shear).
ShMatrix angularBake(const Matrix &a) {
    if (a.isApprox(Matrix::Identity(),1e-12)) return ShMatrix::Identity();
    const Matrix inverse=a.inverse(); ShMatrix result=ShMatrix::Zero();
    const int nz=24,np=48;
    for (int k=0;k<nz;++k) {
        double z=std::cos(Pi*(k+.75)/(nz+.5)),derivative=0;
        for (int step=0;step<24;++step) {
            double p=1,prev=0;
            for (int j=1;j<=nz;++j) { double next=((2*j-1)*z*p-(j-1)*prev)/j; prev=p;p=next; }
            derivative=nz*(z*p-prev)/(z*z-1); double next=z-p/derivative;
            if (std::abs(next-z)<1e-15) { z=next;break; } z=next;
        }
        const double weight=2/((1-z*z)*derivative*derivative)*2*Pi/np;
        for (int j=0;j<np;++j) {
            double phi=2*Pi*j/np,r=std::sqrt(std::max(0.,1-z*z));
            Eigen::Vector3d d(r*std::cos(phi),r*std::sin(phi),z);
            result.noalias() += weight*basis(d)*basis((inverse*d).normalized()).transpose();
        }
    }
    return result;
}
void processColour(vgs::Frame &frame,const CaptureSettings &settings,const ExportProgress &progress) {
    if (!settings.despill) return;
    MintFrame original,corrected;
    if (settings.recoverSkin && settings.despillStrength>0) {
        original.count=frame.count;
        original.position=QVector<float>(frame.position.begin(),frame.position.end());
        original.colorDc=QVector<float>(frame.colorDc.begin(),frame.colorDc.end());
        original.opacity=QVector<float>(frame.opacity.begin(),frame.opacity.end());
        original.active=QVector<quint8>(frame.active.begin(),frame.active.end());
        if (frame.shCoefficients) {
            original.shRest.fill(0,qsizetype(frame.count)*45);
            for (size_t row=0;row<frame.count;++row) for (int k=0;k<frame.shCoefficients*3;++k)
                original.shRest[qsizetype(row)*45+k]=frame.shRest[row*frame.shCoefficients*3+k];
        }
    }
    const float strength=float(settings.despillStrength);
    for (size_t i=0;i<frame.count;++i) {
        float *c=frame.colorDc.data()+3*i;
        float in[3]={std::clamp(c[0],0.f,1.f),std::clamp(float(c[1]*settings.greenGain),0.f,1.f),std::clamp(c[2],0.f,1.f)},out[3];
        despillColor(in,MintDespillOptions{},out);
        const float raw[3]={std::clamp(c[0],0.f,1.f),std::clamp(c[1],0.f,1.f),std::clamp(c[2],0.f,1.f)};
        for (int j=0;j<3;++j) c[j]+=(in[j]-raw[j])+strength*(out[j]-in[j]);
        if (strength>=1) c[1]=std::min(c[1],.5f*(c[0]+c[2]));
        for (int k=0;k<frame.shCoefficients;++k) {
            float *v=frame.shRest.data()+(i*frame.shCoefficients+k)*3;
            const float excess=v[1]-.5f*(v[0]+v[2]),removed=strength*excess;
            v[0]+=.7152f*removed;v[1]-=.2848f*removed;v[2]+=.7152f*removed;
            const float luminance=.2126f*v[0]+.7152f*v[1]+.0722f*v[2];
            for (int j=0;j<3;++j) v[j]=luminance+float(settings.viewChromaScale)*(v[j]-luminance);
        }
        if ((i%8192)==0) report(progress,0,QStringLiteral("Processing colours"));
    }
    if (original.count) {
        corrected.colorDc=QVector<float>(frame.colorDc.begin(),frame.colorDc.end()); QVector<float> delta;
        if (!mintSkinRecovery(original,corrected,&delta,progress)) throw std::runtime_error("Export cancelled.");
        for (size_t i=0;i<frame.colorDc.size();++i) frame.colorDc[i]+=strength*delta[qsizetype(i)];
        if (strength>=1) for (size_t i=0;i<frame.count;++i) frame.colorDc[3*i+1]=std::min(frame.colorDc[3*i+1],.5f*(frame.colorDc[3*i]+frame.colorDc[3*i+2]));
    }
}
class Baker {
    QMatrix4x4 model,cropInverse;
    Matrix a; ShMatrix sh;
    const Project &project;
    int coefficients;
public:
    Baker(const Project &p,int degree) : model(p.transform.matrix()),a(linear(model)),project(p),coefficients((degree+1)*(degree+1)-1) {
        if (!a.allFinite() || std::abs(a.determinant())<1e-15) throw std::runtime_error("Cannot export a singular capture transform.");
        bool invertible=true; cropInverse=p.crop.transform.matrix().inverted(&invertible);
        if (p.crop.enabled && !invertible) throw std::runtime_error("Cannot export a singular crop transform.");
        sh=angularBake(a);
    }
    vgs::Frame bake(const vgs::Frame &source,const ExportProgress &progress) const {
        vgs::Frame input=source; processColour(input,project.captureSettings,progress);
        vgs::Frame out;out.shCoefficients=coefficients;out.seconds=source.seconds;
        for (size_t i=0;i<input.count;++i) {
            if ((i%8192)==0) report(progress,0,QStringLiteral("Baking transforms and crop"));
            if (!input.active[i]) continue;
            const QVector3D world=model.map(QVector3D(input.position[3*i],input.position[3*i+1],input.position[3*i+2]));
            if (project.crop.enabled) {
                const QVector3D p=cropInverse.map(world);const auto &c=project.crop;
                if (p.y()<0 || p.y()>c.height || (c.shape==CropShape::Box
                    ? std::abs(p.x())>c.width*.5f || std::abs(p.z())>c.depth*.5f
                    : p.x()*p.x()+p.z()*p.z()>c.radius*c.radius)) continue;
            }
            Eigen::Quaterniond q(input.rotation[4*i+3],input.rotation[4*i],input.rotation[4*i+1],input.rotation[4*i+2]);
            Eigen::Vector3d scale(input.scale[3*i],input.scale[3*i+1],input.scale[3*i+2]);
            if (!world.isNull() && (!std::isfinite(world.x()) || !std::isfinite(world.y()) || !std::isfinite(world.z()))) throw std::runtime_error("Non-finite Gaussian position.");
            if (!q.coeffs().allFinite() || q.norm()<1e-12 || !scale.allFinite() || scale.minCoeff()<0) throw std::runtime_error("Invalid Gaussian covariance.");
            q.normalize(); Matrix axes=a*q.toRotationMatrix(); Matrix covariance=axes*scale.array().square().matrix().asDiagonal()*axes.transpose();
            Eigen::SelfAdjointEigenSolver<Matrix> eigen(covariance);
            if (eigen.info()!=Eigen::Success) throw std::runtime_error("Gaussian covariance decomposition failed.");
            Matrix orientation=eigen.eigenvectors(); if (orientation.determinant()<0) orientation.col(0)*=-1;
            Eigen::Quaterniond rotation(orientation);rotation.normalize();
            for (int c=0;c<3;++c) { out.position.push_back(world[c]);out.scale.push_back(float(std::sqrt(std::max(0.,eigen.eigenvalues()[c])))); }
            for (int c=0;c<4;++c) out.rotation.push_back(float(rotation.coeffs()[c]));
            out.opacity.push_back(input.opacity[i]);out.active.push_back(1);
            Eigen::Matrix<double,16,3> colours=Eigen::Matrix<double,16,3>::Zero();
            for (int c=0;c<3;++c) colours(0,c)=(input.colorDc[3*i+c]-.5)/C0;
            for (int k=0;k<input.shCoefficients;++k) for (int c=0;c<3;++c) colours(k+1,c)=input.shRest[(i*input.shCoefficients+k)*3+c];
            colours=(sh*colours).eval();
            if (!colours.allFinite() || !std::isfinite(input.opacity[i])) throw std::runtime_error("Non-finite Gaussian colour or opacity.");
            for (int c=0;c<3;++c) out.colorDc.push_back(float(.5+C0*colours(0,c)));
            for (int k=0;k<coefficients;++k) for (int c=0;c<3;++c) out.shRest.push_back(float(colours(k+1,c)));
        }
        out.count=out.active.size();return out;
    }
};
void u16(vgs::Bytes &b,uint16_t value) { b.push_back(uint8_t(value));b.push_back(uint8_t(value>>8)); }
void u32(vgs::Bytes &b,uint32_t value) { for (int i=0;i<4;++i) b.push_back(uint8_t(value>>(8*i))); }
void u64(vgs::Bytes &b,uint64_t value) { for (int i=0;i<8;++i) b.push_back(uint8_t(value>>(8*i))); }
float half(float value,vgs::Bytes *bytes=nullptr) {
    if (!std::isfinite(value) || std::abs(value)>65504) throw std::runtime_error("Colour exceeds the VGS half-float range.");
    qfloat16 h(value);uint16_t bits;std::memcpy(&bits,&h,2);if (bytes) u16(*bytes,bits);return float(h);
}
void f32(vgs::Bytes &b,float value) { uint32_t bits;std::memcpy(&bits,&value,4);u32(b,bits); }
uint64_t packPosition(const float *p,double lo,double hi) {
    uint64_t q[3]; for (int i=0;i<3;++i) q[i]=hi>lo ? uint64_t(std::clamp(std::round((p[i]-lo)/(hi-lo)*2097151.),0.,2097151.)) : 0;
    return (q[0]<<43)|(q[1]<<22)|(q[2]<<1);
}
uint32_t packRotation(const float *xyzw) {
    double q[4]={xyzw[3],xyzw[0],xyzw[1],xyzw[2]};int largest=0;
    for (int i=1;i<4;++i) if (std::abs(q[i])>std::abs(q[largest])) largest=i;
    const double sign=q[largest]<0 ? -1 : 1;uint32_t word=uint32_t(largest)<<30;int j=0;
    for (int i=0;i<4;++i) if (i!=largest) {
        const int shift[3]={20,10,1},limit[3]={1023,1023,511};
        word|=uint32_t(std::clamp(std::round((q[i]*sign*std::sqrt(2.)+1)*.5*limit[j]),0.,double(limit[j])))<<shift[j];++j;
    } return word;
}
// Two-stage per-coefficient RGB dictionaries. Small sets are represented exactly
// (apart from half storage); larger sets use bounded grids followed by residuals.
struct ColourBook {
    std::vector<Colour> values;
    std::vector<uint16_t> indices;
    ColourBook(const std::vector<Colour> &input,int capacity) {
        std::map<Colour,uint16_t> exact; bool small=true;
        for (const auto &v : input) { exact.emplace(v,0);if (exact.size()>size_t(capacity)) {small=false;break;} }
        indices.reserve(input.size());
        if (small) {
            for (auto &entry : exact) {entry.second=uint16_t(values.size());values.push_back(entry.first);}
            if (values.empty()) values.push_back({0,0,0});
            for (const auto &v : input) indices.push_back(exact.at(v));
        } else {
            Colour lo=input.front(),hi=lo;
            for (const auto &v : input) for (int c=0;c<3;++c) {lo[c]=std::min(lo[c],v[c]);hi[c]=std::max(hi[c],v[c]);}
            int bins[3]={capacity==4096 ? 16 : 8,capacity==4096 ? 16 : 8,capacity==4096 ? 16 : 8};
            if (capacity==1024) {int axis=0;for (int c=1;c<3;++c) if (hi[c]-lo[c]>hi[axis]-lo[axis]) axis=c;bins[axis]=16;}
            for (int r=0;r<bins[0];++r) for (int g=0;g<bins[1];++g) for (int b=0;b<bins[2];++b)
                values.push_back({lo[0]+(hi[0]-lo[0])*r/(bins[0]-1),lo[1]+(hi[1]-lo[1])*g/(bins[1]-1),lo[2]+(hi[2]-lo[2])*b/(bins[2]-1)});
            for (const auto &v : input) {
                int id=0;for (int c=0;c<3;++c) {int index=hi[c]>lo[c] ? int(std::round((v[c]-lo[c])/(hi[c]-lo[c])*(bins[c]-1))) : 0;id=id*bins[c]+std::clamp(index,0,bins[c]-1);}
                indices.push_back(uint16_t(id));
            }
        }
        for (auto &v : values) for (auto &c : v) c=half(c);
    }
    std::vector<Colour> residual(const std::vector<Colour> &input) const {
        auto out=input;for (size_t i=0;i<out.size();++i) for (int c=0;c<3;++c) out[i][c]-=values[indices[i]][c];return out;
    }
};
void add(vgs::DecodedChunk &chunk,uint32_t attribute,uint32_t group,vgs::Bytes bytes,int planes=0,uint64_t n=0) {
    vgs::DecodedPage page;page.descriptor.attribute=attribute;page.descriptor.group=group;
    auto &s=page.descriptor.spec;
    if (planes) {s.kind=3;s.width=planes;s.rows=n;s.fields={10,10,10,2};}
    else s.rows=bytes.size();
    page.descriptor.totalRows=s.rows;page.bytes=std::move(bytes);chunk.pages.push_back(std::move(page));
}
vgs::DecodedChunk packFrame(vgs::Frame frame,int degree) {
    // A fully cropped frame has a single dead sentinel, so the existing format's
    // nonempty-array rules are satisfied without drawing or exporting a live point.
    if (!frame.count) {
        frame.count=1;frame.position={0,0,0};frame.rotation={0,0,0,1};frame.scale={0,0,0};
        frame.opacity={0};frame.colorDc={.5f,.5f,.5f};frame.active={0};frame.shRest.resize(frame.shCoefficients*3);
    }
    const size_t n=size_t(frame.count); const int planes=degree==3 ? 5 : degree==2 ? 3 : degree;
    vgs::DecodedChunk chunk;vgs::Group shared,group;shared.intervals=group.intervals=1;
    group.type=1;group.flags=3;group.splats=n;
    const auto extent=std::minmax_element(frame.position.begin(),frame.position.end());
    group.positionMin=*extent.first;group.positionMax=*extent.second;
    chunk.groups={shared,group};vgs::Bytes positions,rotations,scales,lifetimes,sh0Indices,sh0Terms,opacityTerms;
    std::set<float> unique(frame.scale.begin(),frame.scale.end());std::vector<float> scaleLut;
    bool exactScale=unique.size()<=256;
    if (exactScale) scaleLut.assign(unique.begin(),unique.end());
    else {
        float minimum=std::numeric_limits<float>::max(),maximum=0;
        for (float s : frame.scale) if (s>0) {minimum=std::min(minimum,s);maximum=std::max(maximum,s);}
        scaleLut.push_back(0);for (int j=0;j<255;++j) scaleLut.push_back(float(std::exp(std::log(minimum)+(std::log(maximum)-std::log(minimum))*j/254)));
    }
    while (scaleLut.size()<256) scaleLut.push_back(scaleLut.back());
    vgs::Bytes scaleTable;for (auto value : scaleLut) f32(scaleTable,value);add(chunk,vgs::ScaleLut,0,std::move(scaleTable));
    std::vector<Colour> dc(n);
    for (size_t i=0;i<n;++i) for (int c=0;c<3;++c) dc[i][c]=float((frame.colorDc[3*i+c]-.5)/C0);
    ColourBook dcFirst(dc,4096),dcSecond(dcFirst.residual(dc),4096);
    const size_t dcStride=std::max(dcFirst.values.size(),dcSecond.values.size());
    chunk.groups[0].counts[2]=5*dcStride;chunk.groups[0].counts[3]=5*4096;
    vgs::Bytes dcTable,opacityTable;
    for (int stage=0;stage<5;++stage) for (size_t i=0;i<dcStride;++i) for (int sample=0;sample<2;++sample) for (int c=0;c<3;++c)
        half(stage==0 && i<dcFirst.values.size() ? dcFirst.values[i][c] : stage==1 && i<dcSecond.values.size() ? dcSecond.values[i][c] : 0,&dcTable);
    for (int stage=0;stage<5;++stage) for (int i=0;i<4096;++i) for (int sample=0;sample<2;++sample) half(stage==0 ? float(i)/4095 : 0,&opacityTable);
    add(chunk,vgs::Sh0Trajectories,0,std::move(dcTable));add(chunk,vgs::Sh0Lut,0,vgs::Bytes(512));add(chunk,vgs::OpacityTrajectories,0,std::move(opacityTable));
    for (size_t i=0;i<n;++i) {
        const auto packed=packPosition(frame.position.data()+3*i,group.positionMin,group.positionMax);
        u64(positions,packed);u64(positions,packed);const auto rotation=packRotation(frame.rotation.data()+4*i);u32(rotations,rotation);u32(rotations,rotation);
        for (int c=0;c<3;++c) {
            float value=frame.scale[3*i+c];auto next=std::lower_bound(scaleLut.begin(),scaleLut.end(),value);size_t index=std::min(size_t(next-scaleLut.begin()),size_t(255));
            if (index && std::abs(value-scaleLut[index-1])<std::abs(value-scaleLut[index])) --index;scales.push_back(uint8_t(index));sh0Indices.push_back(0);
        }
        lifetimes.push_back(0);lifetimes.push_back(frame.active[i] ? 1 : 0);
        u64(sh0Terms,uint64_t(dcFirst.indices[i]) | (uint64_t(dcSecond.indices[i])<<12));
        u64(opacityTerms,uint64_t(std::clamp(std::round(frame.opacity[i]*4095),0.f,4095.f)));
    }
    add(chunk,vgs::ScaleIndices,1,std::move(scales));add(chunk,vgs::PositionSamples,1,std::move(positions));
    add(chunk,vgs::RotationSamples,1,std::move(rotations));add(chunk,vgs::Lifetimes,1,std::move(lifetimes));
    add(chunk,vgs::Sh0Base,1,std::move(sh0Indices));add(chunk,vgs::Sh0Terms,1,std::move(sh0Terms));add(chunk,vgs::OpacityTerms,1,std::move(opacityTerms));
    if (planes) {
        std::vector<ColourBook> first,second;
        for (int k=0;k<planes*3;++k) {
            std::vector<Colour> values(n,Colour{0,0,0});
            if (k<frame.shCoefficients) for (size_t i=0;i<n;++i) for (int c=0;c<3;++c) values[i][c]=frame.shRest[(i*frame.shCoefficients+k)*3+c];
            first.emplace_back(values,1024);second.emplace_back(first.back().residual(values),1024);
        }
        size_t ns=1,nt=1;for (const auto &b : first) ns=std::max(ns,b.values.size());for (const auto &b : second) nt=std::max(nt,b.values.size());
        chunk.groups[0].counts[0]=ns;chunk.groups[0].counts[1]=nt;
        vgs::Bytes staticBook,temporalBook,staticIndices,temporalIndices;
        for (const auto &b : first) for (size_t i=0;i<ns;++i) for (int c=0;c<3;++c) half(i<b.values.size() ? b.values[i][c] : 0,&staticBook);
        for (int sample=0;sample<2;++sample) for (const auto &b : second) for (size_t i=0;i<nt;++i) for (int c=0;c<3;++c) half(i<b.values.size() ? b.values[i][c] : 0,&temporalBook);
        for (int plane=0;plane<planes;++plane) for (size_t i=0;i<n;++i) {
            uint32_t sw=0,tw=0;for (int c=0;c<3;++c) {sw|=uint32_t(first[plane*3+c].indices[i])<<((2-c)*10);tw|=uint32_t(second[plane*3+c].indices[i])<<((2-c)*10);}
            u32(staticIndices,sw);u32(temporalIndices,tw);
        }
        add(chunk,vgs::ShStaticBook,0,std::move(staticBook));add(chunk,vgs::ShTemporalBook,0,std::move(temporalBook));
        add(chunk,vgs::ShStaticIndices,1,std::move(staticIndices),planes,n);add(chunk,vgs::ShTemporalIndices,1,std::move(temporalIndices),planes,n);
    }
    return assembleNativeChunk(std::move(chunk));
}
class FileSource : public vgsdec::Source {
public:
    QFile file;
    explicit FileSource(const QString &path):file(path) { if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString()); }
    bool read(uint64_t offset,size_t count,uint8_t *out) override {return file.seek(qint64(offset)) && file.read(reinterpret_cast<char *>(out),qint64(count))==qint64(count);}
    uint64_t size() const override {return uint64_t(file.size());}
};
vgs::Frame copyFrame(const vgsdec::Frame &f) {
    vgs::Frame out;out.count=f.splatCount;out.seconds=f.seconds;out.shCoefficients=f.shCoefficients;
    const size_t n=size_t(f.splatCount);out.position.assign(f.positions,f.positions+3*n);out.rotation.assign(f.rotations,f.rotations+4*n);
    out.scale.assign(f.scales,f.scales+3*n);out.opacity.assign(f.opacities,f.opacities+n);out.colorDc.assign(f.colors,f.colors+3*n);out.active.assign(f.active,f.active+n);
    if (f.sphericalHarmonics) out.shRest.assign(f.sphericalHarmonics,f.sphericalHarmonics+n*f.shCoefficients*3);return out;
}
vgs::Frame copyFrame(const MintFrame &f) {
    vgs::Frame out;out.count=f.count;out.seconds=f.seconds;out.shCoefficients=f.shRest.isEmpty() ? 0 : 15;
    auto copy=[](const auto &v,auto &dest) {dest.assign(v.begin(),v.end());};
    copy(f.position,out.position);copy(f.rotation,out.rotation);copy(f.scale,out.scale);copy(f.opacity,out.opacity);copy(f.colorDc,out.colorDc);copy(f.active,out.active);copy(f.shRest,out.shRest);return out;
}
}

vgs::DecodedChunk packExportFrame(const vgs::Frame &frame,int degree) {
    if (degree<0 || degree>3) throw std::runtime_error("Invalid export SH degree.");
    return packFrame(frame,degree);
}
std::array<double,256> exportShTransform(const Transform &transform) {
    const auto matrix=angularBake(linear(transform.matrix()));std::array<double,256> result;
    for (int row=0;row<16;++row) for (int col=0;col<16;++col) result[row*16+col]=matrix(row,col);return result;
}

vgs::Frame bakeExportFrame(const vgs::Frame &frame,const Project &project,int degree,const ExportProgress &progress) {
    if (degree<0 || degree>3) throw std::runtime_error("Invalid export SH degree.");
    return Baker(project,degree).bake(frame,progress);
}

ExportResult exportCaptureFile(const Project &project,const QString &destination,const ExportProgress &progress) {
    QString error;if (!project.captureSettings.validate(&error)) throw std::runtime_error(error.toStdString());
    if (QFileInfo(project.asset).absoluteFilePath().compare(QFileInfo(destination).absoluteFilePath(),Qt::CaseInsensitive)==0)
        throw std::runtime_error("Choose a destination different from the source capture.");
    report(progress,0,QStringLiteral("Opening source capture"));
    std::unique_ptr<MintFile> mint;std::unique_ptr<FileSource> source;std::unique_ptr<vgsdec::Capture> capture;
    ExportResult result;
    double rate,duration;int sourceDegree=3;vgs::Header header,sourceHeader;vgs::EncodeOptions options;
    std::unique_ptr<vgs::MintLogicalSource> nativeMint;bool native=supportsNativeTransform(project);
    if (QFileInfo(project.asset).suffix().compare("mint",Qt::CaseInsensitive)==0) {
        mint=std::make_unique<MintFile>();if (!mint->open(project.asset,&error)) throw std::runtime_error(error.toStdString());
        if (!mint->frameRateProblem().isEmpty()) {native=false;result.notes << QStringLiteral("The source uses varying sample rates and is resampled at the timeline frame rate.");}
        rate=mint->frameRate();duration=mint->duration();
        header.timeNumerator=1000000;header.timeDenominator=uint32_t(std::round(rate*header.timeNumerator));
    } else {
        source=std::make_unique<FileSource>(project.asset);capture=std::make_unique<vgsdec::Capture>(vgsdec::Capture::openStream(*source));capture->setCachePolicy({0,0,128ull*1024*1024});
        rate=capture->frameRate();duration=capture->duration();sourceDegree=capture->shDegree();
        std::array<uint8_t,vgs::FixedHeaderSize> prefix{};if (!source->read(0,prefix.size(),prefix.data())) throw std::runtime_error("Cannot read source header.");
        vgs::Bytes structural(size_t(vgs::structuralSize(prefix.data(),prefix.size())));
        if (!source->read(0,structural.size(),structural.data())) throw std::runtime_error("Cannot read source structure.");
        const auto original=vgs::readHeader(structural.data(),structural.size());header.timeNumerator=original.timeNumerator;header.timeDenominator=original.timeDenominator;
        header.motionType=original.motionType;header.movingSpeed=original.movingSpeed;sourceHeader=original;
        for (const auto &extra : original.extras) {
            if (extra.type==vgs::ThumbnailExtra || extra.type==vgs::AudioExtra) continue;
            vgs::Bytes bytes(size_t(extra.size));
            if (!source->read(extra.offset,bytes.size(),bytes.data()) || vgs::digest(bytes.data(),bytes.size())!=extra.digest) throw std::runtime_error(vgs::InvalidCapture);
            options.extras.push_back({extra.type,extra.format,std::move(bytes)});
        }
    }
    if (!std::isfinite(rate) || rate<1 || rate>1000 || !std::isfinite(duration) || duration<=0 || duration*rate>1000000)
        throw std::runtime_error("Invalid or excessive export timeline.");
    const int available=std::max(1,int(std::ceil(duration*rate-1e-6)));
    const int first=std::clamp(int(std::round(project.in*rate)),0,available-1),last=std::clamp(int(std::round(project.out*rate)),first,available-1);
    result.frames=last-first+1;
    const int degree=project.captureSettings.shDegree<0 ? sourceDegree : project.captureSettings.shDegree;
    if (native && mint) {
        const auto &bytes=mint->bytes();report(progress,0,QStringLiteral("Reading native temporal dictionaries"));nativeMint=std::make_unique<vgs::MintLogicalSource>(reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()),degree);
        sourceHeader=nativeMint->header();header.timeNumerator=sourceHeader.timeNumerator;header.timeDenominator=sourceHeader.timeDenominator;
    }
    const auto nativePlan=native ? nativeExportPlan(sourceHeader,first,result.frames) : std::vector<NativeChunkPlan>{};
    header.shDegree=degree;header.frameCount=header.durationTicks=result.frames;header.chunks.resize(native ? nativePlan.size() : size_t(result.frames));
    uint64_t outputTick=0;
    for (size_t i=0;i<header.chunks.size();++i) {header.chunks[i].startTick=outputTick;header.chunks[i].intervals=native ? nativePlan[i].intervals : 1;outputTick+=header.chunks[i].intervals;}

    options.shDegree=degree;options.signer=vgs::authoringSigner();options.compression=project.captureSettings.plain ? vgs::Compression::None : vgs::Compression::Auto;
    options.playbackMode=project.captureSettings.playbackMode<0 ? (capture ? vgs::PlaybackMode(capture->playbackMode()) : vgs::PlaybackMode::Loop) : vgs::PlaybackMode(project.captureSettings.playbackMode);
    options.motionType=header.motionType;options.movingSpeed=header.movingSpeed;
    const auto &s=project.captureSettings;auto utf=[](const QString &v) {return v.toUtf8().toStdString();};
    auto &m=options.metadata;m.id=utf(s.catalogueId);m.title=utf(s.title);m.author=utf(s.author);m.projectName=utf(s.projectName);m.takeName=utf(s.takeName);
    m.captureStudio=utf(s.studio);m.copyright=utf(s.copyright);m.softwareName=utf(s.softwareName);m.softwareVersion=utf(s.softwareVersion);
    for (const auto &tag : s.tags) m.tags.push_back(utf(tag));
    QJsonObject provenance{{"editorExportVersion",2},{"colourProcessingVersion",2},{"sampling",native ? "native-temporal" : "native-frame-hold"},{"sourceInFrame",first},{"sourceOutFrame",last},
        {"transform",project.json({})["transform"]},{"crop",project.json({})["crop"]},{"processing",s.json()}};
    if (!s.extraJson.trimmed().isEmpty()) {const auto doc=QJsonDocument::fromJson(s.extraJson.toUtf8());provenance["userMetadata"]=doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());}
    for (const auto &extra : options.extras) if (extra.type==vgs::MetadataExtra2) provenance["sourceMetadata"]=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(extra.bytes.data()),qsizetype(extra.bytes.size()))).object();
    options.extras.erase(std::remove_if(options.extras.begin(),options.extras.end(),[](const auto &e) {return e.type==vgs::MetadataExtra2;}),options.extras.end());
    const auto json=QJsonDocument(provenance).toJson(QJsonDocument::Compact);options.extras.push_back({vgs::MetadataExtra2,vgs::JsonUtf8,vgs::Bytes(json.begin(),json.end())});
    QTemporaryFile temporary(QFileInfo(destination).absolutePath()+"/.vgs-export-XXXXXX");if (!temporary.open()) throw std::runtime_error(temporary.errorString().toStdString());
    const Baker baker(project,degree);int currentPercent=0;bool writing=false;
    auto innerProgress=[&](int,const QString &message) {report(progress,currentPercent,message);return true;};
    auto provider=[&](size_t index) {
        report(progress,currentPercent,QStringLiteral("%1 %2 %3 / %4").arg(writing ? "Writing" : "Preparing",native ? "native block" : "frame").arg(index+1).arg(native ? header.chunks.size() : size_t(result.frames)));
        if (native) {
            const auto &plan=nativePlan[index];vgs::DecodedChunk chunk;
            if (nativeMint) {report(progress,currentPercent,QStringLiteral("Loading native attributes"));chunk=nativeMint->chunk(plan.sourceIndex);}
            else {
                const auto &entry=sourceHeader.chunks[plan.sourceIndex];vgs::Bytes bytes(size_t(entry.size));
                if (!source->read(entry.offset,bytes.size(),bytes.data())) throw std::runtime_error("Cannot read native source chunk.");
                chunk=assembleNativeChunk(vgs::decodeChunk(sourceHeader,plan.sourceIndex,bytes.data(),bytes.size()));
                setNativeShDegree(chunk,sourceDegree,degree);
            }
            if (s.despill) {
                MintDespillOptions processing;processing.strength=s.despillStrength;processing.greenGain=s.greenGain;
                processing.viewChromaScale=s.viewChromaScale;processing.recoverSkinColour=s.recoverSkin;
                report(progress,currentPercent,QStringLiteral("Processing native colour dictionaries"));
                if (!MintFile::despillLogical(&chunk,1/rate,processing,innerProgress,&error)) throw std::runtime_error(error.toStdString());
            }
            return editNativeChunk(std::move(chunk),plan,project,writing ? nullptr : &result,innerProgress);
        }
        const double seconds=std::min(double(first+int(index))/rate,duration-1e-7);vgs::Frame frame;
        if (mint) {MintFrame decoded;if (!mint->decode(seconds,&decoded,true,&error)) throw std::runtime_error(error.toStdString());frame=copyFrame(decoded);}
        else frame=copyFrame(capture->setTime(seconds,true));
        auto baked=baker.bake(frame,innerProgress);
        if (!writing) {result.kept+=baked.count;result.removed+=std::count(frame.active.begin(),frame.active.end(),uint8_t(1))-baked.count;}
        return packFrame(std::move(baked),degree);
    };
    vgs::encodeSequence(header,provider,[&](uint64_t offset,const uint8_t *data,size_t count) {
        if (!temporary.seek(qint64(offset)) || temporary.write(reinterpret_cast<const char *>(data),qint64(count))!=qint64(count)) throw std::runtime_error(temporary.errorString().toStdString());
    },options,[&](int done,int total) {writing=done>=int(header.chunks.size());currentPercent=total ? 85*done/total : 0;report(progress,currentPercent,QStringLiteral("Encoding capture"));return true;});
    if (!result.kept) throw std::runtime_error("The selected range contains no Gaussian centres inside the crop.");
    if (!temporary.flush()) throw std::runtime_error(temporary.errorString().toStdString());
    {
        FileSource verifySource(temporary.fileName());auto verified=vgsdec::Capture::openStream(verifySource);verified.setCachePolicy({0,0,64ull*1024*1024});
        for (int i=0;i<result.frames;++i) {report(progress,85+10*i/result.frames,QStringLiteral("Verifying frame %1 / %2").arg(i+1).arg(result.frames));verified.setTime(i/rate,true);}
    }
    QSaveFile output(destination);output.setDirectWriteFallback(false);if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error(output.errorString().toStdString());
    temporary.seek(0);while (!temporary.atEnd()) {
        report(progress,95,QStringLiteral("Saving verified capture"));auto bytes=temporary.read(4*1024*1024);
        if (bytes.isEmpty() || output.write(bytes)!=bytes.size()) throw std::runtime_error("Cannot save the exported capture.");
    }
    report(progress,99,QStringLiteral("Finishing export"));if (!output.commit()) throw std::runtime_error(output.errorString().toStdString());
    result.notes << (native ? QStringLiteral("Native temporal blocks are retained; unused Gaussian records and dictionary entries are removed.")
                          : QStringLiteral("Native-frame sampling; frames are held between samples. Attributes are requantized to the existing VGS dictionaries."));
    if (!native) result.notes << QStringLiteral("Nonuniform scale/shear and variable-rate sources currently use sampled export, which can produce larger files.");
    const Matrix a=linear(project.transform.matrix()),metric=a.transpose()*a;
    if (!metric.isApprox(Matrix::Identity()*metric.trace()/3,1e-5)) result.notes << QStringLiteral("SH under nonuniform scale or shear is projected to the selected SH degree.");
    if (capture && (capture->hasAudio() || capture->hasThumbnail())) result.notes << QStringLiteral("Source audio and thumbnail are omitted because timeline and framing may have changed.");
    return result;
}
