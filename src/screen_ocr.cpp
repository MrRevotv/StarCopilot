#include "screen_ocr.h"
#include "app_paths.h"
#include "log.h"
#include "text_utils.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using namespace winrt::Windows::Globalization;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

namespace ScreenOcr {

namespace {
    struct Image {
        std::vector<uint8_t> bgra;
        int width = 0, height = 0;
    };

    Image capture(const RECT& r) {
        Image img;
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return img;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // dall'alto in basso
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(mem, dib);
            BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
            GdiFlush();
            img.width = w;
            img.height = h;
            img.bgra.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return img;
    }

    // Grigi + contrasto stirato tra il 5% e il 95% dei pixel + inversione + ingrandimento.
    Image prepare(const Image& src, int scale, bool invert) {
        std::vector<uint8_t> lum((size_t)src.width * src.height);
        int hist[256] = {};
        for (size_t p = 0; p < lum.size(); ++p) {
            const uint8_t b = src.bgra[p * 4], g = src.bgra[p * 4 + 1], r = src.bgra[p * 4 + 2];
            lum[p] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
            hist[lum[p]]++;
        }
        const size_t total = lum.size();
        int lo = 0, hi = 255;
        size_t acc = 0;
        for (int v = 0; v < 256; ++v) { acc += hist[v]; if (acc > total * 5 / 100) { lo = v; break; } }
        acc = 0;
        for (int v = 255; v >= 0; --v) { acc += hist[v]; if (acc > total * 5 / 100) { hi = v; break; } }
        if (hi <= lo) { lo = 0; hi = 255; }

        Image out;
        out.width = src.width * scale;
        out.height = src.height * scale;
        out.bgra.resize((size_t)out.width * out.height * 4);
        for (int y = 0; y < out.height; ++y) {
            // Ingrandimento bilineare: il testo sottile del gioco resta leggibile.
            const double sy = std::max(0.0, (y + 0.5) / scale - 0.5);
            const int y0 = std::min((int)sy, src.height - 1), y1 = std::min(y0 + 1, src.height - 1);
            const double fy = sy - y0;
            for (int x = 0; x < out.width; ++x) {
                const double sx = std::max(0.0, (x + 0.5) / scale - 0.5);
                const int x0 = std::min((int)sx, src.width - 1), x1 = std::min(x0 + 1, src.width - 1);
                const double fx = sx - x0;
                const double v = lum[(size_t)y0 * src.width + x0] * (1 - fx) * (1 - fy) + lum[(size_t)y0 * src.width + x1] * fx * (1 - fy) +
                                 lum[(size_t)y1 * src.width + x0] * (1 - fx) * fy + lum[(size_t)y1 * src.width + x1] * fx * fy;
                double n = std::clamp((v - lo) / (double)(hi - lo), 0.0, 1.0);
                if (invert) n = 1.0 - n;
                const uint8_t g = (uint8_t)(n * 255);
                uint8_t* px = &out.bgra[((size_t)y * out.width + x) * 4];
                px[0] = px[1] = px[2] = g;
                px[3] = 255;
            }
        }
        return out;
    }

    struct RawWord {
        std::string text;
        double x, y, w, h;
    };
    struct RawLine {
        std::string text;
        std::vector<RawWord> words;
    };

    std::mutex g_engineMutex;

    // L'OCR di Windows (C++/WinRT) in un thread suo, con il suo "apartment":
    // chi lo chiama può essere un thread qualsiasi (anche quello dei comandi).
    std::vector<RawLine> recognize(const Image& img, const std::string& language, std::string& error) {
        std::vector<RawLine> lines;
        std::thread([&]() {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
            }
            try {
                std::lock_guard<std::mutex> lock(g_engineMutex);
                OcrEngine engine = nullptr;
                if (!language.empty()) {
                    Language lang(TextUtils::utf8ToWide(language));
                    if (OcrEngine::IsLanguageSupported(lang)) engine = OcrEngine::TryCreateFromLanguage(lang);
                }
                if (!engine) engine = OcrEngine::TryCreateFromUserProfileLanguages();
                if (!engine) {
                    error = "nessuna lingua OCR installata in Windows";
                    return;
                }
                DataWriter writer;
                writer.WriteBytes(winrt::array_view<const uint8_t>(img.bgra.data(), img.bgra.data() + img.bgra.size()));
                SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(), BitmapPixelFormat::Bgra8, img.width, img.height);
                OcrResult result = engine.RecognizeAsync(bitmap).get();
                for (const auto& line : result.Lines()) {
                    RawLine l;
                    l.text = TextUtils::wideToUtf8(std::wstring(line.Text()));
                    for (const auto& word : line.Words()) {
                        auto r = word.BoundingRect();
                        l.words.push_back({ TextUtils::wideToUtf8(std::wstring(word.Text())), r.X, r.Y, r.Width, r.Height });
                    }
                    lines.push_back(l);
                }
            } catch (const winrt::hresult_error& e) {
                error = "OCR: " + TextUtils::wideToUtf8(std::wstring(e.message()));
            } catch (...) {
                error = "OCR non riuscito";
            }
        }).join();
        return lines;
    }

    int letters(const std::vector<RawLine>& lines) {
        int n = 0;
        for (const auto& l : lines) {
            for (char c : l.text) n += std::isalnum((unsigned char)c) ? 1 : 0;
        }
        return n;
    }

    int maxDimension() {
        static int cached = 0;
        if (cached) return cached;
        int value = 2600;
        std::thread([&]() {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
            }
            try {
                value = (int)OcrEngine::MaxImageDimension();
            } catch (...) {
            }
        }).join();
        cached = value;
        return cached;
    }

    // Immagine di controllo: quello che si è letto, con i riquadri verdi dell'OCR (BMP).
    std::string saveDebug(const Image& shot, const std::vector<Hit>& hits, const RECT& region, const std::string& tag) {
        const std::wstring dir = debugDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        Image img = shot;
        for (const auto& h : hits) {
            if (h.line) continue;
            const int l = h.rect.left - region.left, t = h.rect.top - region.top, r = h.rect.right - region.left, b = h.rect.bottom - region.top;
            auto put = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
                uint8_t* p = &img.bgra[((size_t)y * img.width + x) * 4];
                p[0] = 0; p[1] = 255; p[2] = 0;
            };
            for (int x = l; x <= r; ++x) { put(x, t); put(x, b); }
            for (int y = t; y <= b; ++y) { put(l, y); put(r, y); }
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\%02d%02d%02d-", st.wHour, st.wMinute, st.wSecond);
        const std::wstring path = dir + name + TextUtils::utf8ToWide(tag) + L".bmp";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return {};
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = img.width;
        ih.biHeight = -img.height;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + (DWORD)img.bgra.size();
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(img.bgra.data(), 1, img.bgra.size(), f);
        fclose(f);
        return TextUtils::wideToUtf8(path);
    }
}

std::wstring debugDir() {
    return AppPaths::getAppDataDir() + L"\\debug";
}

Result read(const RECT& region, const OcrSettings& settings, const std::string& debugTag) {
    Result res;
    const Image shot = capture(region);
    if (shot.width < 8 || shot.height < 8) {
        res.error = "zona troppo piccola o fuori dallo schermo";
        return res;
    }
    int scale = std::clamp(settings.upscale, 1, 4);
    const int maxDim = maxDimension();
    while (scale > 1 && (shot.width * scale > maxDim || shot.height * scale > maxDim)) scale--;

    std::vector<RawLine> lines = recognize(prepare(shot, scale, true), settings.language, res.error);
    if (letters(lines) < 3 && res.error.empty()) {
        std::string err2;
        std::vector<RawLine> plain = recognize(prepare(shot, scale, false), settings.language, err2);
        if (letters(plain) > letters(lines)) lines = plain;
    }
    for (const auto& l : lines) {
        double x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9;
        for (const auto& w : l.words) {
            x1 = std::min(x1, w.x);
            y1 = std::min(y1, w.y);
            x2 = std::max(x2, w.x + w.w);
            y2 = std::max(y2, w.y + w.h);
        }
        auto toScreen = [&](double x, double y, double w, double h) {
            return RECT{ region.left + (LONG)(x / scale), region.top + (LONG)(y / scale), region.left + (LONG)((x + w) / scale),
                         region.top + (LONG)((y + h) / scale) };
        };
        if (!l.words.empty()) res.hits.push_back({ TextUtils::trim(l.text), toScreen(x1, y1, x2 - x1, y2 - y1), true });
        for (const auto& w : l.words) res.hits.push_back({ TextUtils::trim(w.text), toScreen(w.x, w.y, w.w, w.h), false });
    }
    if (settings.saveDebugShots && !debugTag.empty()) res.savedImage = saveDebug(shot, res.hits, region, debugTag);
    return res;
}

std::string linesText(const Result& result) {
    std::string out;
    for (const auto& h : result.hits) {
        if (!h.line) continue;
        if (!out.empty()) out += '\n';
        out += h.text;
    }
    return out;
}

} // namespace ScreenOcr
