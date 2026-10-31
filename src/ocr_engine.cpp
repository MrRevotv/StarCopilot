#include "ocr_engine.h"
#include "log.h"
#include "text_utils.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

using namespace winrt::Windows::Globalization;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

namespace {
    std::mutex g_mutex;
    std::map<std::string, OcrEngine> g_engines; // uno per lingua, creato una volta

    // C++/WinRT vuole un "apartment" sul thread che lo usa. Se il thread ne
    // ha già uno (per esempio quello della finestra), va bene così.
    void ensureApartment() {
        thread_local bool done = false;
        if (done) return;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
        }
        done = true;
    }

    OcrEngine engineFor(const std::string& code) {
        auto it = g_engines.find(code);
        if (it != g_engines.end()) return it->second;
        OcrEngine engine = nullptr;
        try {
            if (!code.empty()) {
                Language lang(TextUtils::utf8ToWide(code));
                if (OcrEngine::IsLanguageSupported(lang)) engine = OcrEngine::TryCreateFromLanguage(lang);
            }
            if (!engine) engine = OcrEngine::TryCreateFromUserProfileLanguages();
        } catch (...) {
            engine = nullptr;
        }
        if (engine) {
            g_engines.emplace(code, engine);
            Log::info("OCR: motore per \"" + code + "\" = " + TextUtils::wideToUtf8(engine.RecognizerLanguage().LanguageTag().c_str()));
        }
        return engine;
    }
}

namespace OcrEngineWrapper {

std::vector<std::string> installedLanguages() {
    std::vector<std::string> result;
    ensureApartment();
    try {
        for (const Language& lang : OcrEngine::AvailableRecognizerLanguages()) {
            result.push_back(TextUtils::wideToUtf8(lang.LanguageTag().c_str()));
        }
    } catch (...) {
    }
    return result;
}

bool isLanguageSupported(const std::string& code) {
    ensureApartment();
    try {
        return OcrEngine::IsLanguageSupported(Language(TextUtils::utf8ToWide(code)));
    } catch (...) {
        return false;
    }
}

int maxImageDimension() {
    ensureApartment();
    try {
        return (int)OcrEngine::MaxImageDimension();
    } catch (...) {
        return 2600;
    }
}

std::vector<Line> recognize(const std::string& languageCode, const ScreenCapture::Image& image, std::string& error) {
    std::vector<Line> raw;
    if (image.empty()) { error = "immagine vuota"; return raw; }
    ensureApartment();
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        OcrEngine engine = engineFor(languageCode);
        if (!engine) {
            error = "nessuna lingua OCR installata in Windows";
            return raw;
        }
        DataWriter writer;
        writer.WriteBytes(winrt::array_view<const uint8_t>(image.pixels.data(), image.pixels.data() + image.pixels.size()));
        SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(), BitmapPixelFormat::Bgra8, image.width, image.height);
        OcrResult result = engine.RecognizeAsync(bitmap).get();
        if (!result) return raw;

        for (const auto& line : result.Lines()) {
            Line l;
            float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
            std::string text;
            for (const auto& word : line.Words()) {
                auto r = word.BoundingRect();
                left = std::min(left, r.X);
                top = std::min(top, r.Y);
                right = std::max(right, r.X + r.Width);
                bottom = std::max(bottom, r.Y + r.Height);
                if (!text.empty()) text += ' ';
                text += TextUtils::wideToUtf8(word.Text().c_str());
            }
            if (text.empty()) continue;
            l.text = text;
            l.x = (int)left;
            l.y = (int)top;
            l.width = (int)(right - left);
            l.height = (int)(bottom - top);
            raw.push_back(l);
        }
    } catch (const winrt::hresult_error& e) {
        error = "OCR: " + TextUtils::wideToUtf8(e.message().c_str());
        return {};
    } catch (...) {
        error = "OCR non riuscito";
        return {};
    }

    // Righe sulla stessa "altezza" (il centro verticale entro mezza riga)
    // sono la stessa riga dello schermo: si uniscono, da sinistra a destra.
    std::sort(raw.begin(), raw.end(), [](const Line& a, const Line& b) { return a.y + a.height / 2 < b.y + b.height / 2; });
    std::vector<Line> merged;
    for (const Line& l : raw) {
        if (!merged.empty()) {
            Line& prev = merged.back();
            int prevCenter = prev.y + prev.height / 2, center = l.y + l.height / 2;
            int tolerance = std::max(4, std::min(prev.height, l.height) / 2);
            if (std::abs(center - prevCenter) <= tolerance) {
                bool after = l.x >= prev.x;
                prev.text = after ? prev.text + " " + l.text : l.text + " " + prev.text;
                int right = std::max(prev.x + prev.width, l.x + l.width), bottom = std::max(prev.y + prev.height, l.y + l.height);
                prev.x = std::min(prev.x, l.x);
                prev.y = std::min(prev.y, l.y);
                prev.width = right - prev.x;
                prev.height = bottom - prev.y;
                continue;
            }
        }
        merged.push_back(l);
    }
    return merged;
}

std::string joinLines(const std::vector<Line>& lines) {
    std::string out;
    for (const auto& l : lines) {
        if (!out.empty()) out += '\n';
        out += l.text;
    }
    return out;
}

} // namespace OcrEngineWrapper
