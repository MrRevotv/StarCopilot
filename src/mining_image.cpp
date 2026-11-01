#include "mining_image.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// stb_image (già tra le librerie di llama.cpp) per leggere i PNG delle prove e per
// la decompressione zlib dei file di Mining Signature Calculator. "static": non
// tocca la copia che usa mtmd.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb/stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace MiningImage {

namespace {
    int reflect101(int i, int n) {
        if (n == 1) return 0;
        while (i < 0 || i >= n) {
            if (i < 0) i = -i;
            if (i >= n) i = 2 * n - 2 - i;
        }
        return i;
    }

    Gray blank(int w, int h, uint8_t v = 0) {
        Gray g;
        g.w = w;
        g.h = h;
        g.p.assign((size_t)w * h, v);
        return g;
    }

    // Sfocatura 3×3 [1 2 1]/4 (quella di OpenCV con sigma 0).
    Gray gaussian3(const Gray& s) {
        Gray rows = blank(s.w, s.h), out = blank(s.w, s.h);
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                const int v = s.at(reflect101(x - 1, s.w), y) + 2 * s.at(x, y) + s.at(reflect101(x + 1, s.w), y);
                rows.p[(size_t)y * s.w + x] = (uint8_t)((v + 2) / 4);
            }
        }
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                const int v = rows.at(x, reflect101(y - 1, s.h)) + 2 * rows.at(x, y) + rows.at(x, reflect101(y + 1, s.h));
                out.p[(size_t)y * s.w + x] = (uint8_t)((v + 2) / 4);
            }
        }
        return out;
    }

    // Canny con Sobel 3×3 e modulo L1, come cv2.Canny(img, low, high).
    Gray canny(const Gray& s, int low, int high) {
        const int w = s.w, h = s.h;
        std::vector<int> gx((size_t)w * h), gy((size_t)w * h), mag((size_t)w * h);
        auto px = [&](int x, int y) { return (int)s.at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int dx = (px(x + 1, y - 1) + 2 * px(x + 1, y) + px(x + 1, y + 1)) - (px(x - 1, y - 1) + 2 * px(x - 1, y) + px(x - 1, y + 1));
                const int dy = (px(x - 1, y + 1) + 2 * px(x, y + 1) + px(x + 1, y + 1)) - (px(x - 1, y - 1) + 2 * px(x, y - 1) + px(x + 1, y - 1));
                const size_t i = (size_t)y * w + x;
                gx[i] = dx;
                gy[i] = dy;
                mag[i] = std::abs(dx) + std::abs(dy);
            }
        }
        auto m = [&](int x, int y) { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : mag[(size_t)y * w + x]; };
        std::vector<uint8_t> state((size_t)w * h, 0); // 1 = debole, 2 = forte
        std::vector<int> stack;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const int v = mag[i];
                if (v <= low) continue;
                const double ax = std::abs(gx[i]), ay = std::abs(gy[i]);
                bool localMax;
                if (ay < ax * 0.41421356237) {
                    localMax = v > m(x - 1, y) && v >= m(x + 1, y);
                } else if (ay > ax * 2.41421356237) {
                    localMax = v > m(x, y - 1) && v >= m(x, y + 1);
                } else {
                    const int sgn = ((gx[i] ^ gy[i]) < 0) ? -1 : 1;
                    localMax = v > m(x - sgn, y - 1) && v > m(x + sgn, y + 1);
                }
                if (!localMax) continue;
                if (v > high) {
                    state[i] = 2;
                    stack.push_back((int)i);
                } else {
                    state[i] = 1;
                }
            }
        }
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % w, y = i / w;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t j = (size_t)ny * w + nx;
                    if (state[j] == 1) {
                        state[j] = 2;
                        stack.push_back((int)j);
                    }
                }
            }
        }
        Gray out = blank(w, h);
        for (size_t i = 0; i < state.size(); ++i) out.p[i] = state[i] == 2 ? 255 : 0;
        return out;
    }

    // Massimo (o minimo) su un rettangolo kw×kh con ancora al centro, fuori dall'immagine ignorato.
    Gray rectFilter(const Gray& s, int kw, int kh, bool takeMax) {
        const int ax = kw / 2, ay = kh / 2;
        Gray rows = blank(s.w, s.h), out = blank(s.w, s.h);
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                int best = takeMax ? 0 : 255;
                for (int d = x - ax; d <= x + kw - 1 - ax; ++d) {
                    if (d < 0 || d >= s.w) continue;
                    const int v = s.at(d, y);
                    best = takeMax ? std::max(best, v) : std::min(best, v);
                }
                rows.p[(size_t)y * s.w + x] = (uint8_t)best;
            }
        }
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                int best = takeMax ? 0 : 255;
                for (int d = y - ay; d <= y + kh - 1 - ay; ++d) {
                    if (d < 0 || d >= s.h) continue;
                    const int v = rows.at(x, d);
                    best = takeMax ? std::max(best, v) : std::min(best, v);
                }
                out.p[(size_t)y * s.w + x] = (uint8_t)best;
            }
        }
        return out;
    }

    double percentile(const Gray& g, double q) {
        long long hist[256] = {};
        for (uint8_t v : g.p) hist[v]++;
        const double rank = q / 100.0 * (double)(g.p.size() - 1);
        const long long lo = (long long)std::floor(rank), hi = (long long)std::ceil(rank);
        auto valueAt = [&](long long r) {
            long long acc = 0;
            for (int v = 0; v < 256; ++v) {
                acc += hist[v];
                if (acc > r) return v;
            }
            return 255;
        };
        const int a = valueAt(lo), b = valueAt(hi);
        return a + (b - a) * (rank - (double)lo);
    }

    struct Blob {
        int x0, y0, x1, y1;
    };

    // Componenti connesse (8 vicini): il rettangolo che le contiene (come boundingRect dei contorni esterni).
    std::vector<Blob> blobs(const Gray& mask) {
        std::vector<Blob> out;
        std::vector<uint8_t> seen(mask.p.size(), 0);
        std::vector<int> stack;
        for (int y = 0; y < mask.h; ++y) {
            for (int x = 0; x < mask.w; ++x) {
                const size_t i = (size_t)y * mask.w + x;
                if (!mask.p[i] || seen[i]) continue;
                Blob b{ x, y, x, y };
                seen[i] = 1;
                stack.push_back((int)i);
                while (!stack.empty()) {
                    const int j = stack.back();
                    stack.pop_back();
                    const int cx = j % mask.w, cy = j / mask.w;
                    b.x0 = std::min(b.x0, cx);
                    b.y0 = std::min(b.y0, cy);
                    b.x1 = std::max(b.x1, cx);
                    b.y1 = std::max(b.y1, cy);
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || ny < 0 || nx >= mask.w || ny >= mask.h) continue;
                            const size_t k = (size_t)ny * mask.w + nx;
                            if (mask.p[k] && !seen[k]) {
                                seen[k] = 1;
                                stack.push_back((int)k);
                            }
                        }
                    }
                }
                out.push_back(b);
            }
        }
        return out;
    }

    double cubicWeight(double t, int k) {
        constexpr double A = -0.75;
        switch (k) {
        case 0: return ((A * (t + 1) - 5 * A) * (t + 1) + 8 * A) * (t + 1) - 4 * A;
        case 1: return ((A + 2) * t - (A + 3)) * t * t + 1;
        case 2: { const double u = 1 - t; return ((A + 2) * u - (A + 3)) * u * u + 1; }
        default: {
            const double u = 1 - t;
            const double w0 = ((A * (t + 1) - 5 * A) * (t + 1) + 8 * A) * (t + 1) - 4 * A;
            const double w1 = ((A + 2) * t - (A + 3)) * t * t + 1;
            const double w2 = ((A + 2) * u - (A + 3)) * u * u + 1;
            return 1 - w0 - w1 - w2;
        }
        }
    }

    bool writeBmp(const std::wstring& path, const std::vector<uint8_t>& bgra, int w, int h) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = w;
        ih.biHeight = -h;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + (DWORD)bgra.size();
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bgra.data(), 1, bgra.size(), f);
        fclose(f);
        return true;
    }
}

Gray toGray(const ScreenCapture::Image& img) {
    Gray g = blank(img.width, img.height);
    for (size_t i = 0; i < g.p.size(); ++i) {
        const uint8_t* p = &img.pixels[i * 4];
        // Come cv2.COLOR_BGR2GRAY.
        g.p[i] = (uint8_t)((p[2] * 4899 + p[1] * 9617 + p[0] * 1868 + 8192) >> 14);
    }
    return g;
}

ScreenCapture::Image toBgra(const Gray& g) {
    ScreenCapture::Image img;
    img.width = g.w;
    img.height = g.h;
    img.pixels.resize((size_t)g.w * g.h * 4);
    for (size_t i = 0; i < g.p.size(); ++i) {
        img.pixels[i * 4 + 0] = img.pixels[i * 4 + 1] = img.pixels[i * 4 + 2] = g.p[i];
        img.pixels[i * 4 + 3] = 255;
    }
    return img;
}

Gray crop(const Gray& g, const Box& b) {
    const int x0 = std::clamp(b.x, 0, g.w), y0 = std::clamp(b.y, 0, g.h);
    const int x1 = std::clamp(b.x + b.w, x0, g.w), y1 = std::clamp(b.y + b.h, y0, g.h);
    Gray out = blank(x1 - x0, y1 - y0);
    for (int y = y0; y < y1; ++y) std::memcpy(&out.p[(size_t)(y - y0) * out.w], &g.p[(size_t)y * g.w + x0], (size_t)out.w);
    return out;
}

ScreenCapture::Image crop(const ScreenCapture::Image& img, const Box& b) {
    ScreenCapture::Image out;
    const int x0 = std::clamp(b.x, 0, img.width), y0 = std::clamp(b.y, 0, img.height);
    const int x1 = std::clamp(b.x + b.w, x0, img.width), y1 = std::clamp(b.y + b.h, y0, img.height);
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.pixels.resize((size_t)out.width * out.height * 4);
    for (int y = y0; y < y1; ++y) {
        std::memcpy(&out.pixels[(size_t)(y - y0) * out.width * 4], &img.pixels[((size_t)y * img.width + x0) * 4], (size_t)out.width * 4);
    }
    return out;
}

int otsuThreshold(const Gray& g) {
    long long hist[256] = {};
    for (uint8_t v : g.p) hist[v]++;
    const double total = (double)g.p.size();
    double sum = 0;
    for (int i = 0; i < 256; ++i) sum += (double)i * hist[i];
    double sumB = 0, wB = 0, best = -1;
    int threshold = 0;
    for (int t = 0; t < 256; ++t) {
        wB += (double)hist[t];
        if (wB == 0) continue;
        const double wF = total - wB;
        if (wF == 0) break;
        sumB += (double)t * hist[t];
        const double mB = sumB / wB, mF = (sum - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) {
            best = between;
            threshold = t;
        }
    }
    return threshold;
}

Gray binarize(const Gray& g, int threshold) {
    Gray out = blank(g.w, g.h);
    for (size_t i = 0; i < g.p.size(); ++i) out.p[i] = g.p[i] > threshold ? 255 : 0;
    return out;
}

Box findBadge(const Gray& gray) {
    const int w = gray.w, h = gray.h;
    Box all{ 0, 0, w, h };
    if (h < 12 || w < 24) return all;

    const Gray blurred = gaussian3(gray);
    const Gray edges = canny(blurred, 30, 120);
    const Gray otsu = binarize(blurred, otsuThreshold(blurred));
    const int brightThreshold = std::clamp((int)percentile(gray, 80), 60, 180);

    Gray combined = blank(w, h);
    for (size_t i = 0; i < combined.p.size(); ++i) {
        const bool bright = gray.p[i] > brightThreshold;
        combined.p[i] = (edges.p[i] || (otsu.p[i] && bright)) ? 255 : 0;
    }
    // Dilatazione orizzontale: segnaposto e cifre vicine diventano un blocco solo.
    const int kw = std::max(8, (int)(w * 0.08)), kh = std::max(2, (int)(h * 0.03));
    const Gray dilated = rectFilter(combined, kw, kh, true);

    const double area = (double)w * h;
    const double minArea = std::max(40.0, area * 0.0005), maxArea = area * 0.95;
    double bestScore = -1;
    Box best;
    bool found = false;
    for (const Blob& b : blobs(dilated)) {
        const int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
        const double a = (double)bw * bh;
        if (a < minArea || a > maxArea) continue;
        const double aspect = bw / (double)bh;
        if (aspect < 1.1 || aspect > 9.0) continue;
        int maxBright = 0, edgeCount = 0;
        for (int y = b.y0; y <= b.y1; ++y) {
            for (int x = b.x0; x <= b.x1; ++x) {
                maxBright = std::max(maxBright, (int)gray.at(x, y));
                edgeCount += edges.at(x, y) ? 1 : 0;
            }
        }
        const double edgeDensity = edgeCount / a;
        // Il testo dell'HUD ha bordi netti e una buona luminosità.
        if (maxBright < 65 || edgeDensity < 0.015) continue;
        const double aspectScore = 1.0 - std::min(1.0, std::abs(aspect - 3.2) / 3.2);
        const double brightScore = std::min(1.0, maxBright / 255.0);
        const double edgeScore = std::min(1.0, edgeDensity / 0.25);
        const double positionScore = (b.y0 + bh / 2.0) / h;
        const double score = aspectScore * 0.35 + edgeScore * 0.35 + brightScore * 0.15 + positionScore * 0.15;
        if (score > bestScore) {
            bestScore = score;
            best = Box{ b.x0, b.y0, bw, bh };
            found = true;
        }
    }
    if (!found) return all;
    const int x1 = std::max(0, best.x - 6), y1 = std::max(0, best.y - 4);
    const int x2 = std::min(w, best.x + best.w + 6), y2 = std::min(h, best.y + best.h + 4);
    return Box{ x1, y1, std::max(10, x2 - x1), std::max(10, y2 - y1) };
}

int iconCut(const Gray& g) {
    const int w = g.w, h = g.h;
    if (w < 12 || h < 6) return 0;
    const Gray t = binarize(g, otsuThreshold(g));
    const int minVal = std::max(1, (int)(h * 0.05));
    std::vector<bool> active(w);
    for (int x = 0; x < w; ++x) {
        int count = 0;
        for (int y = 0; y < h; ++y) count += t.at(x, y) ? 1 : 0;
        active[x] = count > minVal;
    }
    std::vector<std::pair<int, int>> segments;
    bool in = false;
    int start = 0;
    for (int x = 0; x < w; ++x) {
        if (active[x] && !in) { start = x; in = true; }
        else if (!active[x] && in) { segments.push_back({ start, x - 1 }); in = false; }
    }
    if (in) segments.push_back({ start, w - 1 });
    if (segments.empty()) return 0;

    // Cifre vicine (e la virgola) sono un blocco solo.
    const int maxDigitGap = std::max(2, (int)(h * 0.12));
    std::vector<std::pair<int, int>> merged;
    auto cur = segments[0];
    for (size_t i = 1; i < segments.size(); ++i) {
        if (segments[i].first - cur.second - 1 <= maxDigitGap) cur.second = segments[i].second;
        else {
            merged.push_back(cur);
            cur = segments[i];
        }
    }
    merged.push_back(cur);
    if (merged.size() >= 2) {
        const int gap = merged[1].first - merged[0].second - 1;
        const int minGap = std::max(3, (int)(h * 0.15));
        // L'icona sta a sinistra (nel primo 35%) e c'è uno spazio vero prima delle cifre.
        if (merged[0].first < w * 0.35 && gap >= minGap) {
            const int split = std::max(0, merged[1].first - 4);
            if (split > 0) return split;
        }
    }
    return 0;
}

Gray resizeToHeight(const Gray& g, int height) {
    if (g.empty() || height <= 0 || g.h == height) return g;
    const int newW = std::max(10, (int)(height * (g.w / (double)g.h)));
    const double sx = g.w / (double)newW, sy = g.h / (double)height;
    // Prima in orizzontale, poi in verticale (bicubico, a = -0.75 come OpenCV).
    std::vector<double> tmp((size_t)newW * g.h);
    for (int x = 0; x < newW; ++x) {
        const double fx = (x + 0.5) * sx - 0.5;
        const int ix = (int)std::floor(fx);
        const double t = fx - ix;
        double wts[4];
        for (int k = 0; k < 4; ++k) wts[k] = cubicWeight(t, k);
        for (int y = 0; y < g.h; ++y) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += wts[k] * g.at(std::clamp(ix - 1 + k, 0, g.w - 1), y);
            tmp[(size_t)y * newW + x] = v;
        }
    }
    Gray out = blank(newW, height);
    for (int y = 0; y < height; ++y) {
        const double fy = (y + 0.5) * sy - 0.5;
        const int iy = (int)std::floor(fy);
        const double t = fy - iy;
        double wts[4];
        for (int k = 0; k < 4; ++k) wts[k] = cubicWeight(t, k);
        for (int x = 0; x < newW; ++x) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += wts[k] * tmp[(size_t)std::clamp(iy - 1 + k, 0, g.h - 1) * newW + x];
            out.p[(size_t)y * newW + x] = (uint8_t)std::clamp((int)std::lround(v), 0, 255);
        }
    }
    return out;
}

Gray clahe(const Gray& g, double clipLimit, int grid) {
    if (g.empty() || grid < 1) return g;
    // Come OpenCV: l'immagine si allunga (per riflessione) fino a un multiplo delle mattonelle.
    const int pw = g.w + (grid - g.w % grid) % grid, ph = g.h + (grid - g.h % grid) % grid;
    const int tw = pw / grid, th = ph / grid;
    const int tileArea = tw * th;
    const int clip = std::max(1, (int)(clipLimit * tileArea / 256.0));
    std::vector<uint8_t> luts((size_t)grid * grid * 256);
    for (int ty = 0; ty < grid; ++ty) {
        for (int tx = 0; tx < grid; ++tx) {
            int hist[256] = {};
            for (int y = ty * th; y < (ty + 1) * th; ++y) {
                for (int x = tx * tw; x < (tx + 1) * tw; ++x) hist[g.at(reflect101(x, g.w), reflect101(y, g.h))]++;
            }
            int clipped = 0;
            for (int i = 0; i < 256; ++i) {
                if (hist[i] > clip) {
                    clipped += hist[i] - clip;
                    hist[i] = clip;
                }
            }
            const int batch = clipped / 256;
            int residual = clipped - batch * 256;
            for (int i = 0; i < 256; ++i) hist[i] += batch;
            if (residual > 0) {
                const int step = std::max(256 / residual, 1);
                for (int i = 0; i < 256 && residual > 0; i += step, --residual) hist[i]++;
            }
            uint8_t* lut = &luts[((size_t)ty * grid + tx) * 256];
            const double scale = 255.0 / tileArea;
            int sum = 0;
            for (int i = 0; i < 256; ++i) {
                sum += hist[i];
                lut[i] = (uint8_t)std::clamp((int)std::lround(sum * scale), 0, 255);
            }
        }
    }
    Gray out = blank(g.w, g.h);
    for (int y = 0; y < g.h; ++y) {
        const double tyf = y / (double)th - 0.5;
        int ty1 = (int)std::floor(tyf), ty2 = ty1 + 1;
        const double ya = tyf - ty1, ya1 = 1 - ya;
        ty1 = std::max(ty1, 0);
        ty2 = std::min(ty2, grid - 1);
        for (int x = 0; x < g.w; ++x) {
            const double txf = x / (double)tw - 0.5;
            int tx1 = (int)std::floor(txf), tx2 = tx1 + 1;
            const double xa = txf - tx1, xa1 = 1 - xa;
            tx1 = std::max(tx1, 0);
            tx2 = std::min(tx2, grid - 1);
            const uint8_t v = g.at(x, y);
            auto L = [&](int ty, int tx) { return (double)luts[((size_t)ty * grid + tx) * 256 + v]; };
            const double r = (L(ty1, tx1) * xa1 + L(ty1, tx2) * xa) * ya1 + (L(ty2, tx1) * xa1 + L(ty2, tx2) * xa) * ya;
            out.p[(size_t)y * g.w + x] = (uint8_t)std::clamp((int)std::lround(r), 0, 255);
        }
    }
    return out;
}

Gray morphOpen(const Gray& g, int k) {
    if (k <= 1) return g;
    return rectFilter(rectFilter(g, k, k, false), k, k, true);
}

Gray invert(const Gray& g) {
    Gray out = g;
    for (auto& v : out.p) v = (uint8_t)(255 - v);
    return out;
}

Gray pad(const Gray& g, int margin, uint8_t value) {
    Gray out = blank(g.w + 2 * margin, g.h + 2 * margin, value);
    for (int y = 0; y < g.h; ++y) std::memcpy(&out.p[(size_t)(y + margin) * out.w + margin], &g.p[(size_t)y * g.w], (size_t)g.w);
    return out;
}

Gray darkTextOnLight(const Gray& g) {
    if (g.empty()) return g;
    const int t = otsuThreshold(g);
    size_t bright = 0;
    for (uint8_t v : g.p) bright += v > t ? 1 : 0;
    // Il testo è la minoranza: se i chiari sono pochi il testo è chiaro, si inverte.
    return bright * 2 < g.p.size() ? invert(g) : g;
}

Gray simpleContrast(const ScreenCapture::Image& img, int scale) {
    Gray g = toGray(img);
    if (g.empty()) return g;
    g = resizeToHeight(g, g.h * std::max(1, scale));
    double mean = 0;
    for (uint8_t v : g.p) mean += v;
    mean /= (double)g.p.size();
    // Come ImageEnhance.Contrast(2.0) di Pillow: ci si allontana dal grigio medio.
    for (auto& v : g.p) v = (uint8_t)std::clamp((int)std::lround(mean + 2.0 * (v - mean)), 0, 255);
    return g;
}

Steps process(const ScreenCapture::Image& bgra, int targetHeight) {
    Steps s;
    if (bgra.empty()) return s;
    s.original = toGray(bgra);
    s.badge = findBadge(s.original);
    Gray cropped = crop(s.original, s.badge);
    s.iconCut = iconCut(cropped);
    if (s.iconCut > 0) cropped = crop(cropped, Box{ s.iconCut, 0, cropped.w - s.iconCut, cropped.h });
    s.cropped = cropped;
    s.resized = resizeToHeight(cropped, targetHeight);
    s.enhanced = clahe(s.resized, 2.0, 8);
    s.binary = morphOpen(binarize(s.enhanced, otsuThreshold(s.enhanced)), 2);
    return s;
}

bool saveBmp(const std::wstring& path, const Gray& g) {
    if (g.empty()) return false;
    const ScreenCapture::Image img = toBgra(g);
    return writeBmp(path, img.pixels, img.width, img.height);
}

bool saveBmp(const std::wstring& path, const ScreenCapture::Image& img) {
    if (img.empty()) return false;
    return writeBmp(path, img.pixels, img.width, img.height);
}

ScreenCapture::Image loadPng(const std::wstring& path) {
    ScreenCapture::Image img;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return img;
    std::vector<unsigned char> data;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    int w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &channels, 4);
    if (!rgba) return img;
    img.width = w;
    img.height = h;
    img.pixels.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        img.pixels[i * 4 + 0] = rgba[i * 4 + 2];
        img.pixels[i * 4 + 1] = rgba[i * 4 + 1];
        img.pixels[i * 4 + 2] = rgba[i * 4 + 0];
        img.pixels[i * 4 + 3] = 255;
    }
    stbi_image_free(rgba);
    return img;
}

bool zlibInflate(const std::string& data, std::string& out) {
    int outLen = 0;
    char* raw = stbi_zlib_decode_malloc(data.data(), (int)data.size(), &outLen);
    if (!raw) return false;
    out.assign(raw, raw + outLen);
    STBI_FREE(raw);
    return true;
}

} // namespace MiningImage
