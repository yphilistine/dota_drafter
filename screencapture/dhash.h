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

    const float sx = static_cast<float>(w) / SZ;
    const float sy = static_cast<float>(h) / SZ;
    for (int dy = 0; dy < SZ; ++dy) {
        float fy  = (dy + 0.5f) * sy - 0.5f;
        int   iy0 = (std::max)(0, static_cast<int>(fy));
        int   iy1 = (std::min)(h-1, iy0+1);
        float ay  = fy - static_cast<float>(iy0);
        for (int dx = 0; dx < SZ; ++dx) {
            float fx  = (dx + 0.5f) * sx - 0.5f;
            int   ix0 = (std::max)(0, static_cast<int>(fx));
            int   ix1 = (std::min)(w-1, ix0+1);
            float ax  = fx - static_cast<float>(ix0);
            out.v[dy*SZ+dx] =
                (1-ay)*(1-ax)*grey[iy0*w+ix0] + (1-ay)*ax*grey[iy0*w+ix1] +
                   ay *(1-ax)*grey[iy1*w+ix0] +    ay *ax*grey[iy1*w+ix1];
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
