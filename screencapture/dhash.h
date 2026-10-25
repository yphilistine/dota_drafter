#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace dota2 {

struct Matrix8 {
    float v[64];
    bool empty() const {
        for (float x : v) if (x != 0.0f) return false;
        return true;
    }
};

struct HeroHashEntry {
    const char* name;
    Matrix8     mat;
};

struct HeroMatch {
    const char* name;
    float       score;
    bool confident() const { return score >= 0.80f; }
};

inline float pearson(const Matrix8& a, const Matrix8& b) {
    float dot = 0.0f;
    for (int i = 0; i < 64; ++i) dot += a.v[i] * b.v[i];
    return dot / 63.0f;
}

inline Matrix8 computeMatrix(const uint8_t* bgra, int w, int h) {
    Matrix8 out{};
    if (!bgra || w <= 0 || h <= 0) return out;

    constexpr int SZ = 8;

    std::vector<float> grey(static_cast<size_t>(w) * h);
    for (int i = 0; i < w * h; ++i) {
        float b = bgra[i*4+0], g = bgra[i*4+1], r = bgra[i*4+2];
        grey[i] = 0.114f*b + 0.587f*g + 0.299f*r;
    }

    const float cw = static_cast<float>(w) / SZ;
    const float ch = static_cast<float>(h) / SZ;
    for (int dy = 0; dy < SZ; ++dy) {
        const float y0  = dy * ch;
        const float y1  = y0 + ch;
        const int   iy0 = static_cast<int>(y0);
        const int   iy1 = (std::min)(h, static_cast<int>(std::ceil(y1)));
        for (int dx = 0; dx < SZ; ++dx) {
            const float x0  = dx * cw;
            const float x1  = x0 + cw;
            const int   ix0 = static_cast<int>(x0);
            const int   ix1 = (std::min)(w, static_cast<int>(std::ceil(x1)));
            float sum = 0.0f, area = 0.0f;
            for (int y = iy0; y < iy1; ++y) {
                float wy = (std::min)(y + 1.0f, y1) - (std::max)(static_cast<float>(y), y0);
                for (int x = ix0; x < ix1; ++x) {
                    float wx = (std::min)(x + 1.0f, x1) - (std::max)(static_cast<float>(x), x0);
                    sum  += grey[y*w+x] * wx * wy;
                    area += wx * wy;
                }
            }
            out.v[dy*SZ+dx] = sum / area;
        }
    }

    float mean = 0.0f;
    for (int i = 0; i < 64; ++i) mean += out.v[i];
    mean /= 64.0f;
    float var = 0.0f;
    for (int i = 0; i < 64; ++i) { float d=out.v[i]-mean; var+=d*d; }
    float sigma = std::sqrt(var/64.0f);
    if (sigma < 1e-6f) sigma = 1e-6f;
    for (int i = 0; i < 64; ++i) out.v[i] = (out.v[i]-mean)/sigma;

    return out;
}

template<typename BitmapT>
inline Matrix8 computeMatrix(const BitmapT& bmp) {
    if (bmp.empty()) return {};
    return computeMatrix(bmp.pixels.data(), bmp.width, bmp.height);
}

class HeroRecognizer {
public:
    explicit HeroRecognizer(const HeroHashEntry* db, size_t count)
        : db_(db), count_(count) {}

    HeroMatch recognize(const uint8_t* bgra, int w, int h) const {
        return findNearest(computeMatrix(bgra, w, h));
    }

    template<typename BitmapT>
    HeroMatch recognize(const BitmapT& bmp) const {
        if (bmp.empty()) return {"(empty)", -1.0f};
        return recognize(bmp.pixels.data(), bmp.width, bmp.height);
    }

    size_t size() const { return count_; }

private:
    HeroMatch findNearest(const Matrix8& q) const {
        if (!db_ || count_ == 0) return {nullptr, -1.0f};
        HeroMatch best{"unknown", -1.0f};
        for (size_t i = 0; i < count_; ++i) {
            float s = pearson(q, db_[i].mat);
            if (s > best.score) {
                best.score = s;
                best.name  = db_[i].name;
                if (s > 0.999f) break;
            }
        }
        return best;
    }
    const HeroHashEntry* db_;
    size_t               count_;
};

using DHash = uint64_t;

}
