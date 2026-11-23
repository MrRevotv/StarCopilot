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
    using Image = ScreenCapture::Image;

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
            img.pixels.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return img;
    }

    std::vector<uint8_t> luminance(const Image& src) {
        std::vector<uint8_t> lum((size_t)src.width * src.height);
        for (size_t p = 0; p < lum.size(); ++p) {
            const uint8_t b = src.pixels[p * 4], g = src.pixels[p * 4 + 1], r = src.pixels[p * 4 + 2];
            lum[p] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
        }
        return lum;
    }

    // Grigi + contrasto stirato tra il 5% e il 95% dei pixel + inversione + ingrandimento.
    Image prepare(const Image& src, int scale, bool invert) {
        const std::vector<uint8_t> lum = luminance(src);
        int hist[256] = {};
        for (uint8_t v : lum) hist[v]++;
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
        out.pixels.resize((size_t)out.width * out.height * 4);
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
                uint8_t* px = &out.pixels[((size_t)y * out.width + x) * 4];
                px[0] = px[1] = px[2] = g;
                px[3] = 255;
            }
        }
        return out;
    }

    Image inverted(Image img) {
        for (size_t i = 0; i < img.pixels.size(); i += 4) {
            img.pixels[i] = (uint8_t)(255 - img.pixels[i]);
            img.pixels[i + 1] = (uint8_t)(255 - img.pixels[i + 1]);
            img.pixels[i + 2] = (uint8_t)(255 - img.pixels[i + 2]);
        }
        return img;
    }

    std::vector<uint8_t> windowFilter(const std::vector<uint8_t>& in, int w, int h, int k, bool takeMin);

    // Le preparazioni, nell'ordine in cui si provano.
    enum class Prep { Inverted, Plain, Cleaned, CleanedInverted, Strokes, StrokesBinary };

    const char* prepName(Prep p) {
        switch (p) {
        case Prep::Inverted: return "invertita";
        case Prep::Plain: return "normale";
        case Prep::Cleaned: return "senza sfondo";
        case Prep::CleanedInverted: return "senza sfondo, invertita";
        case Prep::Strokes: return "solo i tratti chiari";
        case Prep::StrokesBinary: return "solo i tratti, a soglia";
        }
        return "";
    }

    Image fromGray(const std::vector<uint8_t>& g, int w, int h) {
        Image out;
        out.width = w;
        out.height = h;
        out.pixels.resize((size_t)w * h * 4);
        for (size_t i = 0; i < g.size(); ++i) {
            uint8_t* px = &out.pixels[i * 4];
            px[0] = px[1] = px[2] = g[i];
            px[3] = 255;
        }
        return out;
    }

    // I tratti chiari e sottili, scuri su bianco: si ingrandisce, si stima lo sfondo dietro le
    // lettere (apertura più larga dei tratti) e lo si toglie, poi il resto si stira fino al 99,5%
    // (non fino al pixel più chiaro: il bordo lucido del vetro schiacciava le lettere). Per il
    // testo bianco sul vetro chiaro della ricerca (240 su 210), che le altre preparazioni perdono.
    Image strokes(const Image& src, int scale, bool binary) {
        const std::vector<uint8_t> lum = luminance(src);
        const int w = src.width * scale, h = src.height * scale;
        std::vector<uint8_t> g((size_t)w * h);
        for (int y = 0; y < h; ++y) {
            const double sy = std::max(0.0, (y + 0.5) / scale - 0.5);
            const int y0 = std::min((int)sy, src.height - 1), y1 = std::min(y0 + 1, src.height - 1);
            const double fy = sy - y0;
            for (int x = 0; x < w; ++x) {
                const double sx = std::max(0.0, (x + 0.5) / scale - 0.5);
                const int x0 = std::min((int)sx, src.width - 1), x1 = std::min(x0 + 1, src.width - 1);
                const double fx = sx - x0;
                g[(size_t)y * w + x] = (uint8_t)(lum[(size_t)y0 * src.width + x0] * (1 - fx) * (1 - fy) + lum[(size_t)y0 * src.width + x1] * fx * (1 - fy) +
                                                 lum[(size_t)y1 * src.width + x0] * (1 - fx) * fy + lum[(size_t)y1 * src.width + x1] * fx * fy);
            }
        }
        const int k = std::max(7, 4 * scale + 3);
        const std::vector<uint8_t> background = windowFilter(windowFilter(g, w, h, k, true), w, h, k, false);
        int hist[256] = {};
        for (size_t i = 0; i < g.size(); ++i) {
            g[i] = (uint8_t)(g[i] - background[i]);
            hist[g[i]]++;
        }
        int top = 255;
        for (size_t acc = 0; top > 1 && (acc += hist[top]) * 1000 < g.size() * 5; --top) {}
        top = std::max(top, 12);
        if (binary) {
            // Otsu sui tratti (non sotto un terzo del chiaro: il rumore del vetro resta bianco).
            long long sum = 0, count = (long long)g.size();
            for (int t = 0; t < 256; ++t) sum += (long long)t * hist[t];
            double sumB = 0, wB = 0, bestVar = -1;
            int threshold = top / 2;
            for (int t = 0; t < 256; ++t) {
                wB += hist[t];
                if (wB == 0) continue;
                const double wF = count - wB;
                if (wF == 0) break;
                sumB += (double)t * hist[t];
                const double mB = sumB / wB, mF = (sum - sumB) / wF;
                const double between = wB * wF * (mB - mF) * (mB - mF);
                if (between > bestVar) {
                    bestVar = between;
                    threshold = t;
                }
            }
            threshold = std::max(threshold, top / 3);
            for (auto& v : g) v = v > threshold ? 0 : 255;
        } else {
            for (auto& v : g) v = (uint8_t)(255 - std::min(255, v * 255 / top));
        }
        return fromGray(g, w, h);
    }

    Image prepareAs(const Image& shot, int scale, int maxDim, Prep mode) {
        switch (mode) {
        case Prep::Inverted: return prepare(shot, scale, true);
        case Prep::Plain: return prepare(shot, scale, false);
        // La pulizia della chat letta (ScreenCapture): toglie lo sfondo dietro le lettere, così il
        // testo bianco sul vetro chiaro della ricerca resta l'unica cosa chiara.
        case Prep::Cleaned: return ScreenCapture::prepareForOcr(shot, scale, maxDim);
        case Prep::CleanedInverted: return inverted(ScreenCapture::prepareForOcr(shot, scale, maxDim));
        case Prep::Strokes: return strokes(shot, scale, false);
        case Prep::StrokesBinary: return strokes(shot, scale, true);
        }
        return {};
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

    using Reader = std::function<std::vector<RawLine>(const Image&)>;

    // L'OCR di Windows (C++/WinRT) in un thread suo, con il suo "apartment": chi lo chiama
    // può essere un thread qualsiasi (anche quello dei comandi). Un motore solo per tutte le
    // immagini lette da work().
    void withEngine(const std::string& language, std::string& error, const std::function<void(const Reader&)>& work) {
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
                const Reader reader = [&](const Image& img) {
                    std::vector<RawLine> lines;
                    try {
                        DataWriter writer;
                        writer.WriteBytes(winrt::array_view<const uint8_t>(img.pixels.data(), img.pixels.data() + img.pixels.size()));
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
                    return lines;
                };
                work(reader);
            } catch (const winrt::hresult_error& e) {
                error = "OCR: " + TextUtils::wideToUtf8(std::wstring(e.message()));
            } catch (...) {
                error = "OCR non riuscito";
            }
        }).join();
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

    // Le righe lette (in pixel dell'immagine ingrandita) in coordinate dello schermo.
    Result toResult(const std::vector<RawLine>& lines, const RECT& region, int scale) {
        Result res;
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
        return res;
    }

    // Immagine di controllo: quello che si è letto, con i riquadri verdi dell'OCR (BMP).
    // Una per zona, sovrascritta: resta solo l'ultima lettura.
    std::string saveDebug(const Image& shot, const std::vector<Hit>& hits, const RECT& region, const std::string& tag) {
        const std::wstring dir = debugDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        Image img = shot;
        for (const auto& h : hits) {
            if (h.line) continue;
            const int l = h.rect.left - region.left, t = h.rect.top - region.top, r = h.rect.right - region.left, b = h.rect.bottom - region.top;
            auto put = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
                uint8_t* p = &img.pixels[((size_t)y * img.width + x) * 4];
                p[0] = 0; p[1] = 255; p[2] = 0;
            };
            for (int x = l; x <= r; ++x) { put(x, t); put(x, b); }
            for (int y = t; y <= b; ++y) { put(l, y); put(r, y); }
        }
        const std::wstring path = dir + L"\\" + TextUtils::utf8ToWide(tag) + L".bmp";
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
        fh.bfSize = fh.bfOffBits + (DWORD)img.pixels.size();
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(img.pixels.data(), 1, img.pixels.size(), f);
        fclose(f);
        return TextUtils::wideToUtf8(path);
    }

    // Minimo (o massimo) su una finestra k×k, prima le righe e poi le colonne.
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

    // I tratti chiari e sottili (le lettere) di una cattura: più chiari di almeno 14 livelli dello
    // sfondo stimato lì intorno (apertura 7×7). Il vetro sfumato della ricerca e la mappa sfocata
    // dietro restano fuori, sia sul vetro chiaro (testo 240 su 210) sia su quello scuro.
    std::vector<bool> textMask(const Shot& s) {
        const std::vector<uint8_t> background = windowFilter(windowFilter(s.gray, s.width, s.height, 7, true), s.width, s.height, 7, false);
        std::vector<bool> mask(s.gray.size());
        for (size_t i = 0; i < s.gray.size(); ++i) mask[i] = (int)s.gray[i] - (int)background[i] >= 14;
        return mask;
    }
}

std::wstring debugDir() {
    return AppPaths::getAppDataDir() + L"\\debug";
}

Result readImage(const Image& shot, const RECT& region, const OcrSettings& settings, const std::string& debugTag,
                 const std::function<bool(const Result&)>& accept) {
    Result best;
    if (shot.width < 8 || shot.height < 8) {
        best.error = "zona troppo piccola o fuori dallo schermo";
        return best;
    }
    int scale = std::clamp(settings.upscale, 1, 4);
    const int maxDim = maxDimension();
    while (scale > 1 && (shot.width * scale > maxDim || shot.height * scale > maxDim)) scale--;

    // Senza accept: invertita, e normale solo se l'invertita legge poco (come sempre).
    const std::vector<Prep> order = accept ? std::vector<Prep>{ Prep::Inverted, Prep::Cleaned, Prep::Strokes, Prep::StrokesBinary,
                                                                Prep::CleanedInverted, Prep::Plain }
                                           : std::vector<Prep>{ Prep::Inverted, Prep::Plain };
    std::string error;
    withEngine(settings.language, error, [&](const Reader& recognize) {
        int bestLetters = -1;
        for (const Prep mode : order) {
            if (!accept && mode == Prep::Plain && (bestLetters >= 3 || !error.empty())) break;
            const std::vector<RawLine> lines = recognize(prepareAs(shot, scale, maxDim, mode));
            Result r = toResult(lines, region, scale);
            r.variant = prepName(mode);
            if (accept && accept(r)) {
                best = std::move(r);
                return;
            }
            const int n = letters(lines);
            if (n > bestLetters) {
                best = std::move(r);
                bestLetters = n;
            }
        }
    });
    if (best.error.empty()) best.error = error;
    if (settings.saveDebugShots && !debugTag.empty()) best.savedImage = saveDebug(shot, best.hits, region, debugTag);
    return best;
}

Result read(const RECT& region, const OcrSettings& settings, const std::string& debugTag) {
    return readImage(capture(region), region, settings, debugTag, nullptr);
}

Result readUntil(const RECT& region, const OcrSettings& settings, const std::string& debugTag, const std::function<bool(const Result&)>& accept) {
    return readImage(capture(region), region, settings, debugTag, accept ? accept : [](const Result&) { return false; });
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

Shot grayOf(const Image& image) {
    Shot s;
    if (image.empty()) return s;
    s.width = image.width;
    s.height = image.height;
    s.gray = luminance(image);
    return s;
}

Shot grab(const RECT& region) {
    return grayOf(capture(region));
}

double textChange(const Shot& before, const Shot& after) {
    if (before.empty() || after.empty() || before.width != after.width || before.height != after.height) return 1.0;
    const std::vector<bool> a = textMask(before), b = textMask(after);
    size_t changed = 0;
    for (size_t i = 0; i < a.size(); ++i) changed += a[i] != b[i] ? 1 : 0;
    return (double)changed / (double)a.size();
}

} // namespace ScreenOcr
