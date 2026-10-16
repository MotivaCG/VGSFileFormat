// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "loopmorph.h"
#include "dependencies/nanoflann/nanoflann.hpp"
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
#include <limits>
#include <unordered_map>

namespace loopmorph {
namespace {
using Vec=Eigen::Vector3d;using Mat=Eigen::Matrix3d;using Quat=Eigen::Quaterniond;using Sparse=Eigen::SparseMatrix<double>;

struct Points {
    std::vector<Eigen::Vector3f> p;
    size_t kdtree_get_point_count() const {return p.size();}
    float kdtree_get_pt(size_t index,int axis) const {return p[index][axis];}
    template<class BBox> bool kdtree_get_bbox(BBox &) const {return false;}
};
using Tree=nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float,Points>,Points,3>;
// Points and the tree over them, together so the tree's reference to them stays valid.
struct Index {
    Points points;std::unique_ptr<Tree> tree;
    explicit Index(std::vector<Eigen::Vector3f> p) {points.p=std::move(p);tree=std::make_unique<Tree>(3,points,nanoflann::KDTreeSingleIndexAdaptorParams(10));tree->buildIndex();}
    // The nearest `k` (fewer when there are fewer points), nearest first, with squared distances.
    size_t nearest(const Vec &at,size_t k,size_t *indices,float *distances) const {
        k=std::min(k,points.p.size());nanoflann::KNNResultSet<float> result(k);result.init(indices,distances);
        const float query[]={float(at.x()),float(at.y()),float(at.z())};tree->findNeighbors(result,query,nanoflann::SearchParameters());return k;
    }
};
// A frame's splats for finding their like in it: position and colour together, a unit of
// colour counting as kColour metres. Matching by both lines red up with red and white with
// white, so a pattern stays a pattern rather than meeting its own other colour halfway.
constexpr double kColour=.05;
struct Surface {
    struct Cloud6 {
        std::vector<std::array<float,6>> p;
        size_t kdtree_get_point_count() const {return p.size();}
        float kdtree_get_pt(size_t index,int axis) const {return p[index][size_t(axis)];}
        template<class BBox> bool kdtree_get_bbox(BBox &) const {return false;}
    } points;
    using Tree6=nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float,Cloud6>,Cloud6,6>;
    std::unique_ptr<Tree6> tree;
    Surface(const std::vector<float> &position,const std::vector<float> &colour) {
        const size_t n=position.size()/3;points.p.resize(n);
        for (size_t i=0;i<n;++i) for (int c=0;c<3;++c) {points.p[i][size_t(c)]=position[3*i+size_t(c)];points.p[i][size_t(c)+3]=float(kColour*std::clamp(colour[3*i+size_t(c)],0.f,1.f));}
        tree=std::make_unique<Tree6>(6,points,nanoflann::KDTreeSingleIndexAdaptorParams(10));tree->buildIndex();
    }
    // The splat most like one at `at` of colour `colour`.
    size_t nearest(const Eigen::Vector3d &at,const float *colour) const {
        size_t index=0;float distance=0;nanoflann::KNNResultSet<float> result(1);result.init(&index,&distance);
        float query[6];for (int c=0;c<3;++c) {query[c]=float(at[c]);query[c+3]=float(kColour*std::clamp(colour[c],0.f,1.f));}
        tree->findNeighbors(result,query,nanoflann::SearchParameters());return index;
    }
    Eigen::Vector3d position(size_t i) const {return {points.p[i][0],points.p[i][1],points.p[i][2]};}
};
// Runs body(first, last) over [0, n) on the machine's cores.
void parallel(size_t n,const std::function<void(size_t,size_t)> &body) {
    const unsigned workers=n<8192 ? 1u : std::min(16u,std::max(1u,std::thread::hardware_concurrency()));
    if (workers==1) {body(0,n);return;}
    std::vector<std::future<void>> jobs;const size_t step=(n+workers-1)/workers;
    for (unsigned w=0;w<workers;++w) {const size_t first=w*step,last=std::min(n,first+step);if (first<last) jobs.push_back(std::async(std::launch::async,body,first,last));}
    std::exception_ptr failure;for (auto &job:jobs) {try {job.get();} catch (...) {if (!failure) failure=std::current_exception();}}
    if (failure) std::rethrow_exception(failure);
}
Vec at(const std::vector<float> &v,size_t i) {return {v[3*i],v[3*i+1],v[3*i+2]};}
Vec vec(const QVector3D &v) {return {v.x(),v.y(),v.z()};}
void check(const std::function<bool()> &cancelled) {if (cancelled && cancelled()) throw std::runtime_error("Loop morph canceled.");}
double smoothstep(double a,double b,double x) {const double t=std::clamp((x-a)/(b-a),0.,1.);return t*t*(3-2*t);}

// The rotation that best takes the `from` points onto the `to` ones (Kabsch), and the
// translation with it. Fewer than three pairs, or three in a line, leave the turn undecided:
// a translation alone.
void rigid(const std::vector<Vec> &from,const std::vector<Vec> &to,Mat *rotation,Vec *translation) {
    Vec a=Vec::Zero(),b=Vec::Zero();for (size_t i=0;i<from.size();++i) {a+=from[i];b+=to[i];}
    a/=double(from.size());b/=double(from.size());*rotation=Mat::Identity();
    if (from.size()>=3) {
        Mat h=Mat::Zero();for (size_t i=0;i<from.size();++i) h+=(from[i]-a)*(to[i]-b).transpose();
        Eigen::JacobiSVD<Mat> svd(h,Eigen::ComputeFullU|Eigen::ComputeFullV);const auto &sv=svd.singularValues();
        if (sv[0]>0 && sv[1]>1e-6*sv[0]) {
            Mat d=Mat::Identity();d(2,2)=(svd.matrixV()*svd.matrixU().transpose()).determinant()<0 ? -1 : 1;
            *rotation=svd.matrixV()*d*svd.matrixU().transpose();
        }
    }
    *translation=b-*rotation*a;
}
// The rotation nearest a matrix (its polar factor), never a reflection.
Mat nearestRotation(const Mat &m) {
    Eigen::JacobiSVD<Mat> svd(m,Eigen::ComputeFullU|Eigen::ComputeFullV);Mat d=Mat::Identity();
    d(2,2)=(svd.matrixU()*svd.matrixV().transpose()).determinant()<0 ? -1 : 1;return svd.matrixU()*d*svd.matrixV().transpose();
}
struct Skin {std::array<int,4> node{};std::array<float,4> weight{};};

// A frame's deformation graph: nodes spread evenly over its splats, each with a rotation and
// the place it moves to, and every splat skinned to its four nearest nodes, weighted as
// Sumner et al. do, by how much nearer than the fifth they are.
struct Graph {
    std::vector<Vec> rest,fitted,correction;std::vector<Quat> turn;
    std::vector<std::pair<int,int>> edges; // directed, both ways
    std::unique_ptr<Index> nodes;
    std::vector<Skin> skins; // one per splat of the frame
    double spacing=0,anchor=0;
    Sparse laplacian;
    Eigen::SimplicialLDLT<Sparse> between; // laplacian + anchor: the frames in between

    Skin skin(const Vec &p) const {
        std::array<size_t,5> index{};std::array<float,5> distance{};const size_t found=nodes->nearest(p,5,index.data(),distance.data());
        Skin s;const float far=found==5 ? std::sqrt(distance[4]) : std::sqrt(distance[found-1])*1.5f+1e-6f;float total=0;
        for (size_t k=0;k<4;++k) {
            if (k>=found) {s.node[k]=int(index[0]);s.weight[k]=0;continue;}
            const float w=std::max(0.f,1-std::sqrt(distance[k])/far);s.node[k]=int(index[k]);s.weight[k]=w*w;total+=w*w;
        }
        if (total<=0) {s.weight={1,0,0,0};total=1;}
        for (auto &w:s.weight) w/=total;
        return s;
    }
    void build(const std::vector<float> &position,size_t target) {
        const size_t n=position.size()/3;Vec low=at(position,0),high=low;
        for (size_t i=1;i<n;++i) {low=low.cwiseMin(at(position,i));high=high.cwiseMax(at(position,i));}
        const double diagonal=std::max(1e-6,(high-low).norm());target=std::clamp<size_t>(target,1,n);
        // A surface fills voxels as their area: the count goes as 1/size^2.
        double size=diagonal/30;std::unordered_map<uint64_t,std::pair<Vec,int>> cells;
        auto key=[&](size_t i) {const Vec c=((at(position,i)-low)/size).array().floor();
            return uint64_t(std::min(c.x(),2097151.))|(uint64_t(std::min(c.y(),2097151.))<<21)|(uint64_t(std::min(c.z(),2097151.))<<42);};
        for (int attempt=0;;++attempt) {
            cells.clear();for (size_t i=0;i<n;++i) {auto &cell=cells.try_emplace(key(i),Vec::Zero(),0).first->second;cell.first+=at(position,i);++cell.second;}
            const double ratio=double(cells.size())/double(target);if ((ratio>0.75 && ratio<1.33) || attempt==7) break;
            size*=std::clamp(std::sqrt(ratio),0.5,2.0);
        }
        spacing=size;rest.clear();for (const auto &[k,cell]:cells) rest.push_back(cell.first/cell.second);
        std::sort(rest.begin(),rest.end(),[](const Vec &a,const Vec &b) {return std::lexicographical_compare(a.data(),a.data()+3,b.data(),b.data()+3);});
        std::vector<Eigen::Vector3f> points;for (const auto &p:rest) points.push_back(p.cast<float>());nodes=std::make_unique<Index>(std::move(points));
        // Neighbours: the nearest nodes within about two cells, and always the nearest three,
        // so no node is left without any.
        std::set<std::pair<int,int>> links;const size_t count=rest.size();
        for (size_t j=0;j<count;++j) {
            std::array<size_t,9> index{};std::array<float,9> distance{};const size_t found=nodes->nearest(rest[j],9,index.data(),distance.data());
            int kept=0;for (size_t k=0;k<found;++k) {
                if (index[k]==j) continue;
                if (kept>=3 && std::sqrt(distance[k])>2.2*spacing) break;
                links.insert({int(std::min(j,index[k])),int(std::max(j,index[k]))});++kept;
            }
        }
        edges.clear();for (const auto &[a,b]:links) {edges.push_back({a,b});edges.push_back({b,a});}
        std::vector<Eigen::Triplet<double>> triplets;
        for (const auto &[j,k]:edges) {triplets.push_back({j,j,1});triplets.push_back({k,k,1});triplets.push_back({j,k,-1});triplets.push_back({k,j,-1});}
        laplacian.resize(int(count),int(count));laplacian.setFromTriplets(triplets.begin(),triplets.end());
        fitted=rest;turn.assign(count,Quat::Identity());correction.assign(count,Vec::Zero());
        skins.resize(n);parallel(n,[&](size_t first,size_t last) {for (size_t i=first;i<last;++i) skins[i]=skin(at(position,i));});
    }
    // Where a point goes with the nodes at these places and turns.
    Vec deform(const Skin &s,const Vec &p,const std::vector<Vec> &place,const std::vector<Mat> &turns) const {
        Vec out=Vec::Zero();for (int k=0;k<4;++k) if (s.weight[k]>0) out+=double(s.weight[k])*(turns[size_t(s.node[k])]*(p-rest[size_t(s.node[k])])+place[size_t(s.node[k])]);
        return out;
    }
    // Fits the nodes so this frame lands on the other: `marks` on `targets`, then its splats
    // onto the other frame's surface, as rigid as possible throughout (Sorkine and Alexa's
    // local/global steps on the graph).
    void fit(const std::vector<float> &position,const std::vector<float> &colour,const Surface &other,const Vec &otherCentre,const std::vector<Vec> &marks,const std::vector<Vec> &targets,const std::function<bool()> &cancelled) {
        const size_t count=rest.size(),n=position.size()/3;
        Mat r0;Vec t0;
        if (!marks.empty()) rigid(marks,targets,&r0,&t0);
        else {Vec centre=Vec::Zero();for (size_t i=0;i<n;++i) centre+=at(position,i);r0=Mat::Identity();t0=otherCentre-centre/double(n);}
        std::vector<Vec> place(count);std::vector<Mat> turns(count,r0);
        for (size_t j=0;j<count;++j) place[j]=r0*rest[j]+t0;
        struct Row {Skin skin;Vec point,target;double weight;size_t splat=0;};
        std::vector<Row> rows;
        const double edgeWeight=double(edges.size());
        for (size_t m=0;m<marks.size();++m) rows.push_back({skin(marks[m]),marks[m],targets[m],4*edgeWeight/double(marks.size())});
        const size_t markers=rows.size();
        // The surface term pulls a sample of the splats, spread over the whole frame.
        const size_t samples=std::min<size_t>(n,4000),stride=std::max<size_t>(1,n/samples);
        std::vector<size_t> sampled;for (size_t i=0;i<n && sampled.size()<samples;i+=stride) sampled.push_back(i);
        const double damping=1e-3;
        auto solve=[&](int iterations,bool surface) {
            std::vector<Eigen::Triplet<double>> triplets;
            for (int k=0;k<laplacian.outerSize();++k) for (Sparse::InnerIterator it(laplacian,k);it;++it) triplets.push_back({int(it.row()),int(it.col()),it.value()});
            for (size_t j=0;j<count;++j) triplets.push_back({int(j),int(j),damping});
            std::vector<Row> all(rows.begin(),rows.begin()+std::ptrdiff_t(markers));
            if (surface) for (size_t i:sampled) all.push_back({skins[i],at(position,i),Vec::Zero(),edgeWeight/double(sampled.size()),i});
            for (const auto &row:all) for (int a=0;a<4;++a) for (int b=0;b<4;++b)
                if (row.skin.weight[a]>0 && row.skin.weight[b]>0) triplets.push_back({row.skin.node[a],row.skin.node[b],row.weight*row.skin.weight[a]*row.skin.weight[b]});
            Sparse system(static_cast<Eigen::Index>(count),static_cast<Eigen::Index>(count));system.setFromTriplets(triplets.begin(),triplets.end());
            Eigen::SimplicialLDLT<Sparse> solver(system);if (solver.info()!=Eigen::Success) throw std::runtime_error("The loop morph's deformation could not be solved.");
            for (int iteration=0;iteration<iterations;++iteration) {
                check(cancelled);
                if (surface) {
                    // The most alike splat of the other frame, by place and colour, ignored beyond
                    // a radius that closes in from four node spacings to one as the fit settles.
                    const double radius=spacing*(4-3*double(iteration)/std::max(1,iterations-1));
                    parallel(all.size()-markers,[&](size_t first,size_t last) {for (size_t r=markers+first;r<markers+last;++r) {
                        auto &row=all[r];const Vec now=deform(row.skin,row.point,place,turns);const Vec found=other.position(other.nearest(now,colour.data()+3*row.splat));
                        row.target=(found-now).norm()<=radius ? found : now;}});
                }
                Eigen::MatrixXd rhs(int(count),3);
                for (size_t j=0;j<count;++j) rhs.row(int(j))=damping*place[j].transpose();
                for (const auto &[j,k]:edges) {const Vec v=turns[size_t(j)]*(rest[size_t(j)]-rest[size_t(k)]);rhs.row(j)+=v.transpose();rhs.row(k)-=v.transpose();}
                for (const auto &row:all) {
                    Vec offset=Vec::Zero();for (int a=0;a<4;++a) if (row.skin.weight[a]>0) offset+=double(row.skin.weight[a])*(turns[size_t(row.skin.node[a])]*(row.point-rest[size_t(row.skin.node[a])]));
                    const Vec want=row.target-offset;for (int a=0;a<4;++a) if (row.skin.weight[a]>0) rhs.row(row.skin.node[a])+=(row.weight*row.skin.weight[a]*want).transpose();
                }
                const Eigen::MatrixXd solution=solver.solve(rhs);if (!solution.allFinite()) throw std::runtime_error("The loop morph's deformation did not converge.");
                for (size_t j=0;j<count;++j) place[j]=solution.row(int(j)).transpose();
                // Each node's turn: the rotation that best carries its edges where they went.
                std::vector<Mat> spread(count,Mat::Zero());
                for (const auto &[j,k]:edges) spread[size_t(j)]+=(place[size_t(j)]-place[size_t(k)])*(rest[size_t(j)]-rest[size_t(k)]).transpose();
                for (size_t j=0;j<count;++j) if (spread[j].norm()>1e-12) turns[j]=nearestRotation(spread[j]);
            }
        };
        if (markers) solve(12,false);
        solve(24,true);
        fitted=place;for (size_t j=0;j<count;++j) turn[j]=Quat(turns[j]).normalized();
        // The frames in between: every node turned part of the way, its place solved as rigid
        // as possible, held weakly where it would be on a straight line.
        anchor=1e-3;Sparse system=laplacian;for (size_t j=0;j<count;++j) system.coeffRef(int(j),int(j))+=anchor;
        between.compute(system);if (between.info()!=Eigen::Success) throw std::runtime_error("The loop morph's in-between frames could not be solved.");
        correction.assign(count,Vec::Zero());const auto end=pose(1);
        for (size_t j=0;j<count;++j) correction[j]=fitted[j]-end.first[j];
    }
    // The nodes' places and turns `s` of the way: none at 0, the fit at 1. The solve at 1
    // misses the fit by what the fit is not rigid, which is added back in proportion.
    std::pair<std::vector<Vec>,std::vector<Mat>> pose(double s) const {
        const size_t count=rest.size();std::vector<Mat> turns(count);
        for (size_t j=0;j<count;++j) turns[j]=Quat::Identity().slerp(s,turn[j]).normalized().toRotationMatrix();
        Eigen::MatrixXd rhs(int(count),3);
        for (size_t j=0;j<count;++j) rhs.row(int(j))=(anchor*((1-s)*rest[j]+s*fitted[j])).transpose();
        for (const auto &[j,k]:edges) {const Vec v=turns[size_t(j)]*(rest[size_t(j)]-rest[size_t(k)]);rhs.row(j)+=v.transpose();rhs.row(k)-=v.transpose();}
        const Eigen::MatrixXd solution=between.solve(rhs);std::vector<Vec> place(count);
        for (size_t j=0;j<count;++j) place[j]=solution.row(int(j)).transpose()+s*correction[j];
        return {place,turns};
    }
};
Vec centreOf(const Cloud &c) {Vec s=Vec::Zero();for (size_t i=0;i<c.size();++i) s+=at(c.position,i);return s/double(std::max<size_t>(1,c.size()));}
}

void Cloud::reserve(size_t n) {
    position.reserve(3*n);rotation.reserve(4*n);scale.reserve(3*n);colour.reserve(3*n);opacity.reserve(n);sh.reserve(n*size_t(shCoefficients)*3);
}
void Cloud::append(const Cloud &from,size_t i) {
    position.insert(position.end(),from.position.begin()+std::ptrdiff_t(3*i),from.position.begin()+std::ptrdiff_t(3*i+3));
    rotation.insert(rotation.end(),from.rotation.begin()+std::ptrdiff_t(4*i),from.rotation.begin()+std::ptrdiff_t(4*i+4));
    scale.insert(scale.end(),from.scale.begin()+std::ptrdiff_t(3*i),from.scale.begin()+std::ptrdiff_t(3*i+3));
    colour.insert(colour.end(),from.colour.begin()+std::ptrdiff_t(3*i),from.colour.begin()+std::ptrdiff_t(3*i+3));
    opacity.push_back(from.opacity[i]);
    const size_t terms=size_t(shCoefficients)*3;if (terms) sh.insert(sh.end(),from.sh.begin()+std::ptrdiff_t(terms*i),from.sh.begin()+std::ptrdiff_t(terms*(i+1)));
}
bool predict(const std::vector<Pair> &pairs,const QVector3D &from,bool toEnd,QVector3D *to) {
    if (pairs.size()<3) return false;
    std::vector<Vec> a,b;for (const auto &p:pairs) {a.push_back(vec(toEnd ? p.start : p.end));b.push_back(vec(toEnd ? p.end : p.start));}
    Mat rotation;Vec translation;rigid(a,b,&rotation,&translation);
    // Three pairs in a line leave the turn undecided: rigid() then gives none.
    if (rotation.isIdentity(1e-12)) {
        Vec c=Vec::Zero();for (const auto &v:a) c+=v;c/=double(a.size());
        Mat spread=Mat::Zero();for (const auto &v:a) spread+=(v-c)*(v-c).transpose();
        Eigen::SelfAdjointEigenSolver<Mat> eigen(spread);if (eigen.eigenvalues()[1]<1e-10*std::max(1e-12,eigen.eigenvalues()[2])) return false;
    }
    // The pairs' misfit, spread by inverse squared distance with a 5 cm floor.
    const Vec p=vec(from);Vec misfit=Vec::Zero();double total=0;
    for (size_t i=0;i<a.size();++i) {const double w=1/((p-a[i]).squaredNorm()+.0025);misfit+=w*(b[i]-(rotation*a[i]+translation));total+=w;}
    const Vec out=rotation*p+translation+misfit/total;*to=QVector3D(float(out.x()),float(out.y()),float(out.z()));
    return true;
}
// A mis-paired pair throws off the others' predictions too - most of all those it has to
// reach far from - so the worst deviation need not be its own. Instead, the pair whose
// leaving out makes the rest agree best is set aside, when that clearly helps (their median
// deviation at most half what it was), and the search goes on among the rest.
std::vector<float> deviations(const std::vector<Pair> &pairs) {
    const size_t n=pairs.size();std::vector<bool> aside(n,false);
    auto judge=[&](const std::vector<bool> &out) {
        std::vector<float> d(n);
        for (size_t i=0;i<n;++i) {
            std::vector<Pair> others;for (size_t j=0;j<n;++j) if (j!=i && !out[j]) others.push_back(pairs[j]);
            QVector3D predicted;d[i]=predict(others,pairs[i].start,true,&predicted) ? (predicted-pairs[i].end).length() : std::numeric_limits<float>::quiet_NaN();
        }
        return d;
    };
    auto median=[&](const std::vector<float> &d,const std::vector<bool> &out) {
        std::vector<float> kept;for (size_t i=0;i<n;++i) if (!out[i] && std::isfinite(d[i])) kept.push_back(d[i]);
        if (kept.empty()) return std::numeric_limits<float>::quiet_NaN();
        std::sort(kept.begin(),kept.end());return kept[(kept.size()-1)/2];
    };
    auto out=judge(aside);
    for (;;) {
        const float now=median(out,aside);size_t remaining=0;for (size_t i=0;i<n;++i) remaining+=!aside[i];
        if (!std::isfinite(now) || now<=.01f || remaining<5) break; // already agreeing, or too few to tell
        size_t best=n;float bestMedian=now;std::vector<float> bestOut;
        for (size_t i=0;i<n;++i) {
            if (aside[i]) continue;auto trial=aside;trial[i]=true;const auto d=judge(trial);const float m=median(d,trial);
            if (std::isfinite(m) && m<bestMedian) {best=i;bestMedian=m;bestOut=d;}
        }
        if (best==n || bestMedian>.5f*now) break;
        aside[best]=true;out=bestOut;
    }
    return out;
}
float endOpacity(double s) {return float(1-smoothstep(0.35,1.0,s));}
float startOpacity(double s) {return float(smoothstep(0.0,0.65,s));}

struct Morph::Data {
    Cloud end,start;
    Graph forward,backward; // on the end frame, taking it to the start; and the other way
    std::vector<uint32_t> endPartner,startPartner; // each splat's nearest in the other frame, once deformed onto it
    double error=0;
};
Morph::Morph(Cloud end,Cloud start,const std::vector<Pair> &pairs,const std::function<bool()> &cancelled) : d_(std::make_unique<Data>()) {
    if (!end.size() || !start.size()) throw std::runtime_error("The loop morph needs splats on both the End and the Start frame.");
    // Both frames come from one capture and share its SH degree; if not, base colour only.
    if (end.shCoefficients!=start.shCoefficients) {end.sh.clear();start.sh.clear();end.shCoefficients=start.shCoefficients=0;}
    d_->end=std::move(end);d_->start=std::move(start);auto &d=*d_;
    check(cancelled);const Surface endSurface(d.end.position,d.end.colour),startSurface(d.start.position,d.start.colour);
    check(cancelled);d.forward.build(d.end.position,500);check(cancelled);d.backward.build(d.start.position,500);
    std::vector<Vec> onEnd,onStart;for (const auto &p:pairs) {onEnd.push_back(vec(p.end));onStart.push_back(vec(p.start));}
    d.forward.fit(d.end.position,d.end.colour,startSurface,centreOf(d.start),onEnd,onStart,cancelled);
    d.backward.fit(d.start.position,d.start.colour,endSurface,centreOf(d.end),onStart,onEnd,cancelled);
    check(cancelled);
    // Each splat's partner: the most alike of the other frame, by place and colour, once
    // deformed onto it - so the colour it takes on is near its own.
    auto partners=[](const Graph &g,const Cloud &own,const Surface &other,std::vector<uint32_t> &out) {
        const auto pose=g.pose(1);const auto &place=pose.first;const auto &turns=pose.second;out.resize(own.size());
        parallel(own.size(),[&](size_t first,size_t last) {for (size_t i=first;i<last;++i) {
            out[i]=uint32_t(other.nearest(g.deform(g.skins[i],at(own.position,i),place,turns),own.colour.data()+3*i));}});
    };
    partners(d.forward,d.end,startSurface,d.endPartner);partners(d.backward,d.start,endSurface,d.startPartner);
    if (!pairs.empty()) {
        const auto pose=d.forward.pose(1);const auto &place=pose.first;const auto &turns=pose.second;double total=0;
        for (size_t m=0;m<pairs.size();++m) total+=(d.forward.deform(d.forward.skin(onEnd[m]),onEnd[m],place,turns)-onStart[m]).norm();
        d.error=total/double(pairs.size());
    }
}
Morph::~Morph()=default;
double Morph::markerError() const {return d_->error;}
size_t Morph::nodes() const {return d_->forward.rest.size()+d_->backward.rest.size();}
Cloud Morph::evaluate(double s) const {
    const auto &d=*d_;s=std::clamp(s,0.,1.);
    const float fadeEnd=endOpacity(s),fadeStart=startOpacity(s);const bool ends=fadeEnd>0,starts=fadeStart>0;
    const size_t ne=ends ? d.end.size() : 0,ns=starts ? d.start.size() : 0,n=ne+ns;const int sh=d.end.shCoefficients;const size_t terms=size_t(sh)*3;
    Cloud out;out.shCoefficients=sh;out.position.resize(3*n);out.rotation.resize(4*n);out.scale.resize(3*n);out.colour.resize(3*n);out.opacity.resize(n);out.sh.resize(terms*n);
    // One frame's splats, carried `along` of the way by its graph, their colour and SH `mix`
    // of the way to their partner's.
    auto carry=[&](const Graph &g,const Cloud &own,const Cloud &other,const std::vector<uint32_t> &partner,double along,double mix,float fade,size_t offset) {
        const auto pose=g.pose(along);const auto &place=pose.first;const auto &turns=pose.second;
        std::vector<Eigen::Vector4d> quats(turns.size());for (size_t j=0;j<turns.size();++j) quats[j]=Quat(turns[j]).coeffs();
        parallel(own.size(),[&](size_t first,size_t last) {for (size_t i=first;i<last;++i) {
            const size_t o=offset+i,p=partner[i];const auto &skin=g.skins[i];
            const Vec moved=g.deform(skin,at(own.position,i),place,turns);for (int c=0;c<3;++c) out.position[3*o+c]=float(moved[c]);
            // The skin's turn, averaged on one side of the quaternion sphere, before the splat's own.
            Eigen::Vector4d sum=Eigen::Vector4d::Zero();const Eigen::Vector4d &reference=quats[size_t(skin.node[0])];
            for (int k=0;k<4;++k) if (skin.weight[k]>0) {const Eigen::Vector4d &q=quats[size_t(skin.node[k])];sum+=double(skin.weight[k])*(q.dot(reference)<0 ? Eigen::Vector4d(-q) : q);}
            const Quat turn=Quat(sum[3],sum[0],sum[1],sum[2]).normalized();
            const Quat own_q=Quat(own.rotation[4*i+3],own.rotation[4*i],own.rotation[4*i+1],own.rotation[4*i+2]).normalized();
            const Quat q=(turn*own_q).normalized();out.rotation[4*o]=float(q.x());out.rotation[4*o+1]=float(q.y());out.rotation[4*o+2]=float(q.z());out.rotation[4*o+3]=float(q.w());
            for (int c=0;c<3;++c) {out.scale[3*o+c]=own.scale[3*i+c];out.colour[3*o+c]=float((1-mix)*own.colour[3*i+c]+mix*other.colour[3*p+c]);}
            for (size_t k=0;k<terms;++k) out.sh[terms*o+k]=float((1-mix)*own.sh[terms*i+k]+mix*other.sh[terms*p+k]);
            out.opacity[o]=own.opacity[i]*fade;
        }});
    };
    if (ends) carry(d.forward,d.end,d.start,d.endPartner,s,s,fadeEnd,0);
    if (starts) carry(d.backward,d.start,d.end,d.startPartner,1-s,1-s,fadeStart,ne);
    return out;
}
}
