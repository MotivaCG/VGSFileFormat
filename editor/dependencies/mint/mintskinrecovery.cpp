#include "mintskinrecovery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace {
struct Cell {
    int x, y, z;
    bool operator==(const Cell& c) const { return x == c.x && y == c.y && z == c.z; }
};
struct CellHash {
    size_t operator()(const Cell& c) const {
        return size_t(quint32(c.x) * 73856093u) ^ size_t(quint32(c.y) * 19349663u)
               ^ size_t(quint32(c.z) * 83492791u);
    }
};
using Grid = std::unordered_map<Cell, QVector<int>, CellHash>;
Cell cellFor(const float* p, float size) {
    return {int(std::floor(p[0]/size)), int(std::floor(p[1]/size)), int(std::floor(p[2]/size))};
}
float distance2(const float* a, const float* b) {
    float d = 0;
    for (int c = 0; c < 3; ++c) d += (a[c]-b[c])*(a[c]-b[c]);
    return d;
}
template<typename F> void nearby(const Grid& grid, const Cell& center, F visit) {
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                auto it = grid.find({center.x+x, center.y+y, center.z+z});
                if (it != grid.end()) for (int index : it->second) visit(index);
            }
}
float saturation(const float* c) {
    const float high = std::max({c[0], c[1], c[2]});
    return high > 0 ? (high - std::min({c[0], c[1], c[2]}))/high : 0;
}
// A Gaussian's DC term alone may look like skin even when its visible SH colour
// is white cloth. Be conservative: neutral, bright colour over several directions
// is evidence against a skin material. Clipped white extrapolations do not count.
bool hasNeutralClothAppearance(const MintFrame &frame,int index) {
    if (frame.shRest.size()!=qsizetype(frame.count)*45) return false;
    const float *dc=frame.colorDc.constData()+index*3,*sh=frame.shRest.constData()+index*45;
    int neutral=0;
    for (int ix=-1;ix<=1;++ix) for (int iy=-1;iy<=1;++iy) for (int iz=-1;iz<=1;++iz) {
        if (!ix && !iy && !iz) continue;
        const float length=std::sqrt(float(ix*ix+iy*iy+iz*iz)),x=ix/length,y=iy/length,z=iz/length;
        const float b[15]={-.4886025119f*y,.4886025119f*z,-.4886025119f*x,
            1.0925484306f*x*y,-1.0925484306f*y*z,.3153915653f*(2*z*z-x*x-y*y),
            -1.0925484306f*x*z,.5462742153f*(x*x-y*y),
            -.5900435899f*y*(3*x*x-y*y),2.8906114426f*x*y*z,
            -.4570457995f*y*(4*z*z-x*x-y*y),.3731763326f*z*(2*z*z-3*x*x-3*y*y),
            -.4570457995f*x*(4*z*z-x*x-y*y),1.4453057213f*z*(x*x-y*y),-.5900435899f*x*(x*x-3*y*y)};
        float rgb[3]={dc[0],dc[1],dc[2]};
        for (int k=0;k<15;++k) for (int c=0;c<3;++c) rgb[c]+=b[k]*sh[k*3+c];
        const float low=std::min({rgb[0],rgb[1],rgb[2]}),high=std::max({rgb[0],rgb[1],rgb[2]});
        if (low>.4f && high<1.f && high-low<.12f && ++neutral>=3) return true;
    }
    return false;
}
}

bool mintSkinRecovery(const MintFrame& original, const MintFrame& corrected,
                      QVector<float>* deltas, const MintProgressFn& progress)
{
    const int n = int(original.count);
    deltas->fill(0.0f, n*3);
    QVector<float> color(n*3), source(n*3), luminance(n);
    QVector<quint8> candidate(n, 0), seen(n, 0);
    QVector<int> seeds;
    Grid seedGrid;
    for (int i = 0; i < n; ++i) {
        if (!original.active[i] || original.opacity[i] <= .005f) continue;
        const float* p = original.position.constData() + i*3;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        float* c = color.data()+i*3;
        float* raw = source.data()+i*3;
        for (int k = 0; k < 3; ++k) {
            c[k] = std::clamp(corrected.colorDc[i*3+k], 0.0f, 1.0f);
            raw[k] = std::clamp(original.colorDc[i*3+k], 0.0f, 1.0f);
        }
        const float sat = saturation(c);
        const float y = .2126f*c[0] + .7152f*c[1] + .0722f*c[2];
        luminance[i] = y;
        candidate[i] = saturation(raw) > .12f && sat < .55f && y > .08f && y < .78f
                       && c[0] >= c[2]-.015f;
        // The original colour disambiguates clean skin from gold/white fabric
        // made pink by a previous green gain. It is a heuristic, not a skin mask.
        const bool seed = original.opacity[i] > .15f && c[0] > c[2]+.015f
            && c[1]-c[2] < .22f*(c[0]-c[2]) && c[1] > c[2]-.025f
            && sat > .30f && sat < .62f && y > .12f && y < .72f
            && raw[0] > raw[2]+.08f && raw[1]-raw[2] < .40f*(raw[0]-raw[2]);
        if ((candidate[i] || seed) && hasNeutralClothAppearance(original,i)) {
            candidate[i]=0;continue;
        }
        if (seed) {
            seeds.append(i);
            seedGrid[cellFor(p, .02f)].append(i);
        }
    }

    // Reject small isolated skin-like patches, such as lettering on footwear.
    // A nearby coherent skin surface is required before extending its colour.
    Grid references;
    int visited = 0;
    for (int seed : seeds) {
        if (seen[seed]) continue;
        QVector<int> component{seed};
        seen[seed] = 1;
        const float* start = original.position.constData()+seed*3;
        float low[3] = {start[0],start[1],start[2]}, high[3] = {start[0],start[1],start[2]};
        for (int head = 0; head < component.size(); ++head) {
            if ((visited++ % 4096) == 0 && progress && !progress(0, QStringLiteral("Finding skin references"))) return false;
            const int i = component[head];
            const float* p = original.position.constData()+i*3;
            for (int c = 0; c < 3; ++c) { low[c] = std::min(low[c],p[c]); high[c] = std::max(high[c],p[c]); }
            nearby(seedGrid, cellFor(p,.02f), [&](int j) {
                if (!seen[j] && distance2(p,original.position.constData()+j*3) <= .02f*.02f) {
                    seen[j] = 1;
                    component.append(j);
                }
            });
        }
        if (component.size() < 250 || distance2(low,high) <= .12f*.12f) continue;
        for (int i : component) references[cellFor(original.position.constData()+i*3,.055f)].append(i);
    }

    for (int i = 0; i < n; ++i) {
        if ((i % 4096) == 0 && progress && !progress(100*i/std::max(1,n), QStringLiteral("Recovering skin color"))) return false;
        if (!candidate[i]) continue;
        const float* p = original.position.constData()+i*3;
        std::array<std::pair<float,int>,16> nearest;
        nearest.fill({std::numeric_limits<float>::infinity(),-1});
        nearby(references,cellFor(p,.055f),[&](int j) {
            const float d = distance2(p,original.position.constData()+j*3);
            if (d >= .055f*.055f || d >= nearest.back().first) return;
            const auto at = std::lower_bound(nearest.begin(),nearest.end(),std::make_pair(d,j));
            std::move_backward(at,nearest.end()-1,nearest.end());
            *at = {d,j};
        });
        if (nearest[7].first >= .05f*.05f) continue;
        float sum = 0, target[3] = {0,0,0};
        for (const auto& item : nearest) {
            const int j = item.second;
            if (j < 0) continue;
            const float weight = original.opacity[j]/std::max(item.first,.002f*.002f);
            sum += weight;
            for (int c = 0; c < 3; ++c) target[c] += weight*color[j*3+c]/luminance[j];
        }
        // Recovery only undoes the despill: each channel may move back towards its source
        // colour, never past it. A reference that is not skin - red and white cloth next to
        // a face looks the same once despilled - cannot then tint the skin beyond either.
        for (int c = 0; c < 3; ++c) {
            const float now = color[i*3+c], was = source[i*3+c];
            const float shift = std::clamp(target[c]/sum*luminance[i]-now,-.2f,.2f);
            (*deltas)[i*3+c] = .95f*std::clamp(shift,std::min(was-now,0.0f),std::max(was-now,0.0f));
        }
    }
    return true;
}
