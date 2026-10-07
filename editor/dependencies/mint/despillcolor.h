#pragma once
#include <algorithm>
#include <cmath>
// Adapted from Gracia Converter DESPILL.md 2.5.
#include "mintfile.h"
inline void despillColor(const float in[3], const MintDespillOptions& options, float out[3])
{
    float R = in[0], G = in[1], B = in[2];
    const float mx = std::max(std::max(R, G), B);
    const float mn = std::min(std::min(R, G), B);
    const float delta = mx - mn;
    const float saturation = (mx > 0.0f) ? (delta / mx) : 0.0f;

    // Stage one: desaturate inside the green wedge, leaving hue and value alone.
    if (delta > 0.0f) {
        float hue;
        if (R == mx)      hue = (G - B) / delta;
        else if (G == mx) hue = 2.0f + (B - R) / delta;
        else              hue = 4.0f + (R - G) / delta;
        hue *= 60.0f;
        if (hue < 0.0f) hue += 360.0f;

        if (hue >= 60.0f && hue <= 180.0f) {
            const float s = 1.0f - std::fabs(120.0f - hue) / 60.0f;
            const float factor = options.sineProfile
                                     ? std::sin(3.14159265358979323846f * s * 0.5f)
                                     : std::sqrt(std::max(0.0f, s));
            const float sat = saturation * (1.0f - factor);

            const float v = mx;
            const int sector = int(hue / 60.0f) % 6;
            const float f = hue / 60.0f - std::floor(hue / 60.0f);
            const float p = v * (1.0f - sat);
            const float q = v * (1.0f - sat * f);
            const float t = v * (1.0f - sat * (1.0f - f));
            switch (sector) {
            case 0: R = v; G = t; B = p; break;
            case 1: R = q; G = v; B = p; break;
            case 2: R = p; G = v; B = t; break;
            case 3: R = p; G = q; B = v; break;
            case 4: R = t; G = p; B = v; break;
            default: R = v; G = p; B = q; break;
            }
        }
    }

    // Stage two: weighted-average despill with luminance give-back. The gate must
    // be applied to the give-back as well as to the detection, or a tiny detected
    // spill produces a huge correction. See the note in DESPILL.md 2.5.
    const float w = options.gateBySaturation
                        ? std::pow(saturation, float(options.saturationExponent))
                        : 0.0f;
    const float rw = float(options.redWeight), bw = float(options.blueWeight);
    const float both = rw + bw;
    auto estimate = [w, rw, bw, both](float r, float b) {
        return (1.0f - w) * (rw * r + bw * b) / both + w * std::max(r, b);
    };

    // Half the removed green is handed back, each channel taking a share inversely
    // proportional to its weight in the estimator.
    const float spill = G - estimate(R, B);
    if (spill > 0.0f) {
        const float r2 = std::min(1.0f, R + 0.5f * (both - rw) / both * spill);
        const float b2 = std::min(1.0f, B + 0.5f * (both - bw) / both * spill);
        R = r2;
        B = b2;
        G = estimate(r2, b2);
    }

    out[0] = R;
    out[1] = G;
    out[2] = B;
}
