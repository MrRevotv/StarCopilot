#include "screen_capture.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
    // Più largo dei tratti delle lettere (alte circa 54 pixel dopo l'ingrandimento).
    constexpr int kStrokeWindow = 15;

    // Minimo (o massimo) su una finestra k×k: prima le righe, poi le colonne.
    std::vector<uint8_t> windowFilter(const std::vector<uint8_t>& in, int w, int h, int k, bool takeMin) {
        const int r = k / 2;
        std::vector<uint8_t> rows(in.size()), out(in.size());
        auto pick = [takeMin](uint8_t a, uint8_t b) { return takeMin ? std::min(a, b) : std::max(a, b); };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint8_t m = in[(size_t)y * w + x];
                for (int d = std::max(0, x - r); d <= std::min(w - 1, x + r); ++d) m = pick(m, in[(size_t)y * w + d]);
                rows[(size_t)y * w + x] = m;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint8_t m = rows[(size_t)y * w + x];
                for (int d = std::max(0, y - r); d <= std::min(h - 1, y + r); ++d) m = pick(m, rows[(size_t)d * w + x]);
                out[(size_t)y * w + x] = m;
            }
        }
        return out;
    }

    // Toglie lo sfondo dietro la chat (pianeti, nebulose, luci): lo si stima con
    // un minimo e poi un massimo più larghi dei tratti delle lettere e lo si
    // sottrae, così restano i tratti chiari e sottili, ancora sfumati. Idea da
    // mining_ocr (pulizia dell'immagine), provata su una chat disegnata sopra
    // sfondi in movimento (vtc_tests --ocr-sfondo): 48 messaggi letti giusti su
    // 72 invece di 26. Binarizzare (Otsu, soglia adattiva) ne dava 9-11: l'OCR
    // di Windows legge meglio le lettere sfumate.
    void removeBackground(std::vector<uint8_t>& v, int w, int h) {
        // Testo scuro su fondo chiaro (un menu, un cartello): si capovolge, il
        // filtro cerca tratti chiari. Il testo è la minoranza dei pixel: sta
        // dalla parte della "coda" lontana dalla mediana. (La sola mediana
        // sbagliava con un pianeta chiaro dietro la chat.)
        int hist[256] = {};
        for (uint8_t x : v) hist[x]++;
        auto percentile = [&](size_t part) {
            size_t acc = 0;
            for (int t = 0; t < 256; ++t) {
                if ((acc += hist[t]) * 100 >= v.size() * part) return t;
            }
            return 255;
        };
        const int dark = percentile(1), median = percentile(50), bright = percentile(99);
        if (median - dark > (bright - median) * 2) {
            for (auto& x : v) x = (uint8_t)(255 - x);
        }
        const std::vector<uint8_t> background = windowFilter(windowFilter(v, w, h, kStrokeWindow, true), w, h, kStrokeWindow, false);
        int hi = 1;
        for (size_t i = 0; i < v.size(); ++i) {
            v[i] = (uint8_t)(v[i] - background[i]); // lo sfondo stimato non supera mai il pixel
            hi = std::max(hi, (int)v[i]);
        }
        // Il più chiaro diventa bianco. (Lasciando fuori lo 0,2% più chiaro, per
        // le stelle, se ne leggevano meno: 42 su 72.)
        for (auto& x : v) x = (uint8_t)std::min(255, x * 255 / hi);
    }
}

namespace ScreenCapture {

Image capture(int x, int y, int width, int height) {
    Image img;
    if (width <= 0 || height <= 0) return img;
    HDC screen = GetDC(nullptr);
    if (!screen) return img;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(mem, bmp);
    // CAPTUREBLT: include anche le finestre "a livelli" (overlay di altri programmi).
    BitBlt(mem, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    img.pixels.resize((size_t)width * height * 4);
    if (GetDIBits(mem, bmp, 0, height, img.pixels.data(), &bi, DIB_RGB_COLORS) == 0) img.pixels.clear();
    else {
        img.width = width;
        img.height = height;
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

std::vector<Point> rotatedCorners(int x, int y, int width, int height, double degrees) {
    const double a = degrees * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double cx = x + width / 2.0, cy = y + height / 2.0, hw = width / 2.0, hh = height / 2.0;
    const double dx[4] = { -hw, hw, hw, -hw }, dy[4] = { -hh, -hh, hh, hh };
    std::vector<Point> out(4);
    for (int i = 0; i < 4; ++i) out[i] = Point{ cx + dx[i] * c - dy[i] * s, cy + dx[i] * s + dy[i] * c };
    return out;
}

Bounds rotatedBounds(int x, int y, int width, int height, double degrees) {
    const std::vector<Point> p = rotatedCorners(x, y, width, height, degrees);
    double x1 = p[0].x, y1 = p[0].y, x2 = p[0].x, y2 = p[0].y;
    for (const auto& q : p) {
        x1 = std::min(x1, q.x);
        y1 = std::min(y1, q.y);
        x2 = std::max(x2, q.x);
        y2 = std::max(y2, q.y);
    }
    Bounds b;
    b.x = (int)std::floor(x1);
    b.y = (int)std::floor(y1);
    b.width = (int)std::ceil(x2) - b.x;
    b.height = (int)std::ceil(y2) - b.y;
    return b;
}

Image straighten(const Image& source, double centerX, double centerY, int width, int height, double degrees) {
    Image out;
    if (width <= 0 || height <= 0 || source.empty()) return out;
    out.width = width;
    out.height = height;
    out.pixels.assign((size_t)width * height * 4, 0);
    const double a = degrees * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    auto at = [&](int px, int py, int ch) -> double {
        if (px < 0 || py < 0 || px >= source.width || py >= source.height) return 0.0;
        return source.pixels[((size_t)py * source.width + px) * 4 + ch];
    };
    for (int v = 0; v < height; ++v) {
        for (int u = 0; u < width; ++u) {
            // Il centro del pixel (u, v) della zona, girato, nelle coordinate di source.
            const double dx = u + 0.5 - width / 2.0, dy = v + 0.5 - height / 2.0;
            const double sx = centerX + dx * c - dy * s - 0.5, sy = centerY + dx * s + dy * c - 0.5;
            const int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
            const double fx = sx - x0, fy = sy - y0;
            uint8_t* p = &out.pixels[((size_t)v * width + u) * 4];
            for (int ch = 0; ch < 3; ++ch) {
                const double value = at(x0, y0, ch) * (1 - fx) * (1 - fy) + at(x0 + 1, y0, ch) * fx * (1 - fy) +
                                     at(x0, y0 + 1, ch) * (1 - fx) * fy + at(x0 + 1, y0 + 1, ch) * fx * fy;
                p[ch] = (uint8_t)std::clamp((int)std::lround(value), 0, 255);
            }
            p[3] = 255;
        }
    }
    return out;
}

Image captureRotated(int x, int y, int width, int height, double degrees) {
    if (std::fabs(degrees) < 0.05) return capture(x, y, width, height);
    const Bounds b = rotatedBounds(x, y, width, height, degrees);
    const Image shot = capture(b.x, b.y, b.width, b.height);
    if (shot.empty()) return shot;
    return straighten(shot, x + width / 2.0 - b.x, y + height / 2.0 - b.y, width, height, degrees);
}

Image prepareForOcr(const Image& src, int scale, int maxDimension) {
    if (src.empty()) return src;
    // Mai oltre il lato massimo dell'OCR (altrimenti rifiuta l'immagine).
    while (scale > 1 && (src.width * scale > maxDimension || src.height * scale > maxDimension)) scale--;
    const int w = src.width * scale, h = src.height * scale;

    // Ingrandimento con interpolazione bilineare, già in scala di grigi.
    std::vector<uint8_t> gray((size_t)w * h);
    auto lum = [&](int px, int py) -> float {
        px = std::clamp(px, 0, src.width - 1);
        py = std::clamp(py, 0, src.height - 1);
        const uint8_t* p = &src.pixels[((size_t)py * src.width + px) * 4];
        return 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
    };
    for (int y = 0; y < h; ++y) {
        float sy = (y + 0.5f) / scale - 0.5f;
        int y0 = (int)std::floor(sy);
        float fy = sy - y0;
        for (int x = 0; x < w; ++x) {
            float sx = (x + 0.5f) / scale - 0.5f;
            int x0 = (int)std::floor(sx);
            float fx = sx - x0;
            float v = (lum(x0, y0) * (1 - fx) + lum(x0 + 1, y0) * fx) * (1 - fy) +
                      (lum(x0, y0 + 1) * (1 - fx) + lum(x0 + 1, y0 + 1) * fx) * fy;
            gray[(size_t)y * w + x] = (uint8_t)std::clamp(v, 0.0f, 255.0f);
        }
    }

    // Contrasto: si allarga l'intervallo tra il 2% più scuro e il 2% più chiaro.
    int hist[256] = {};
    for (uint8_t v : gray) hist[v]++;
    const size_t total = gray.size(), cut = total / 50;
    int lo = 0, hi = 255;
    for (size_t acc = 0; lo < 255 && (acc += hist[lo]) <= cut; ++lo) {}
    for (size_t acc = 0; hi > 0 && (acc += hist[hi]) <= cut; --hi) {}
    if (hi - lo < 20) { lo = 0; hi = 255; }

    for (auto& g : gray) g = (uint8_t)std::clamp((g - lo) * 255 / (hi - lo), 0, 255);
    removeBackground(gray, w, h);

    Image out;
    out.width = w;
    out.height = h;
    out.pixels.resize((size_t)w * h * 4);
    for (size_t i = 0; i < gray.size(); ++i) {
        out.pixels[i * 4 + 0] = gray[i];
        out.pixels[i * 4 + 1] = gray[i];
        out.pixels[i * 4 + 2] = gray[i];
        out.pixels[i * 4 + 3] = 255;
    }
    return out;
}

uint64_t hash(const Image& image) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)image.width ^ ((uint64_t)image.height << 32);
    for (size_t i = 0; i < image.pixels.size(); i += 3) {
        h ^= image.pixels[i];
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace ScreenCapture
