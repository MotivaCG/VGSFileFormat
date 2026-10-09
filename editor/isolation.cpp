// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "isolation.h"
#include "dependencies/nanoflann/nanoflann.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <atomic>
#include <future>
#include <thread>

namespace {
struct Cloud {
    std::vector<QVector3D> points;
    size_t kdtree_get_point_count() const {return points.size();}
    float kdtree_get_pt(size_t index,int axis) const {return points[index][axis];}
    template<class BBox> bool kdtree_get_bbox(BBox &) const {return false;}
};
}
void applyIsolation(const std::vector<QVector3D> &positions,std::vector<uint8_t> &keep,const QVector<IsolationFilter> &filters,const std::function<bool()> &cancelled) {
    if (positions.size()!=keep.size()) throw std::runtime_error("Invalid isolation mask.");
    for (const auto &filter:filters) {
        Cloud cloud;std::vector<size_t> rows;rows.reserve(positions.size());cloud.points.reserve(positions.size());
        for (size_t row=0;row<positions.size();++row) if (keep[row]) {
            const auto &p=positions[row];if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z())) {keep[row]=0;continue;}
            rows.push_back(row);cloud.points.push_back(p);
        }
        const size_t n=cloud.points.size(),k=size_t(std::max(1,filter.neighbour));if (n<=k) continue;
        using Tree=nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float,Cloud>,Cloud,3>;
        Tree tree(3,cloud,nanoflann::KDTreeSingleIndexAdaptorParams(10));tree.buildIndex();
        std::vector<float> nth(n);std::atomic<size_t> next{0};std::atomic_bool stop{false};
        auto queryRows=[&](bool caller) {
            std::vector<size_t> indices(k+1);std::vector<float> distances(k+1);
            while (!stop) {
                const size_t begin=next.fetch_add(256);if (begin>=n) break;
                // Progress/cancellation callbacks stay on the calling thread.
                if (caller && cancelled && cancelled()) {stop=true;throw std::runtime_error("Processing canceled.");}
                for (size_t row=begin;row<std::min(n,begin+256);++row) {
                    nanoflann::KNNResultSet<float> result(k+1);result.init(indices.data(),distances.data());const auto &p=cloud.points[row];const float query[]={p.x(),p.y(),p.z()};tree.findNeighbors(result,query,nanoflann::SearchParameters());
                    size_t found=0;for (size_t j=0;j<k+1;++j) if (indices[j]!=row && ++found==k) {nth[row]=std::sqrt(std::max(0.f,distances[j]));break;}
                }
            }
        };
        const unsigned workers=n<4096 ? 1 : std::min(8u,std::max(1u,std::thread::hardware_concurrency()));std::vector<std::future<void>> jobs;
        if (cancelled && cancelled()) throw std::runtime_error("Processing canceled.");
        for (unsigned i=1;i<workers;++i) jobs.push_back(std::async(std::launch::async,[&] {queryRows(false);}));
        std::exception_ptr failure;try {queryRows(true);} catch (...) {stop=true;failure=std::current_exception();}
        for (auto &job:jobs) {try {job.get();} catch (...) {stop=true;if (!failure) failure=std::current_exception();}}
        if (failure) std::rethrow_exception(failure);
        if (cancelled && cancelled()) throw std::runtime_error("Processing canceled.");
        auto medians=nth;std::nth_element(medians.begin(),medians.begin()+n/2,medians.end());const double median=medians[n/2];if (!(median>0) || !std::isfinite(median)) continue;
        const double limit=median*filter.medianPercent/100.;for (size_t row=0;row<n;++row) if (nth[row]>limit) keep[rows[row]]=0;
    }
}
