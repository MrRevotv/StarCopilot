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
