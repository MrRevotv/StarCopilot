#include "ppocr.h"
#include "app_paths.h"
#include "log.h"
#include "model_downloader.h"
#include "text_utils.h"
#include "updater.h"

#include <windows.h>

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace PpOcr {

namespace {
    // Fonti fissate, con la loro impronta: si scarica esattamente questo, o niente.
    // Il modello è quello di RapidOCR (ospitato su Hugging Face, SWHL/RapidOCR), lo
    // stesso file che c'è dentro Citizen Assistant.
    constexpr const wchar_t* kModelUrl = L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv3/ch_PP-OCRv3_rec_infer.onnx";
    constexpr const char* kModelSha256 = "897a3ededb38fee0dae2c1ccee38241f37df202c9509e3abca02e9217c5ee615";
    // ONNX Runtime 1.20.1 (Microsoft): il pacchetto ufficiale per Python contiene la
    // stessa onnxruntime.dll delle versioni per C/C++, ed è il più piccolo (11 MB).
    constexpr const wchar_t* kRuntimeUrl =
        L"https://files.pythonhosted.org/packages/dd/80/76979e0b744307d488c79e41051117634b956612cc731f1028eb17ee7294/"
        L"onnxruntime-1.20.1-cp312-cp312-win_amd64.whl";
    constexpr const char* kRuntimePackageSha256 = "19c2d843eb074f385e8bbb753a40df780511061a63f9def1b216bf53860223fb";
    constexpr const char* kRuntimeDllSha256 = "c0926f3b9a7f166e13630f600d78c155562d808936ca3c3a9e0449c755ec7e39";
    constexpr const char* kRuntimeInPackage = "onnxruntime/capi/onnxruntime.dll";

    constexpr int kHeight = 48;

    bool fileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    bool hashIs(const std::wstring& path, const char* expected) {
        std::string hex;
        return Updater::sha256File(path, hex) && hex == expected;
    }

    // Scarica url in dest e controlla l'impronta; se non torna il file si butta.
    bool fetch(const wchar_t* url, const std::wstring& dest, const char* sha256, const std::function<void(long long, long long)>& onProgress,
               const std::atomic<bool>* cancel, std::string& error) {
        if (!ModelDownloader::downloadFile(url, dest, onProgress, cancel, error)) return false;
        if (!hashIs(dest, sha256)) {
            DeleteFileW(dest.c_str());
            error = "il file scaricato non ha l'impronta attesa";
            return false;
        }
        return true;
    }

    // Estrae un file da uno zip con tar.exe di Windows (c'è da Windows 10 1803).
    bool extract(const std::wstring& zip, const char* inside, const std::wstring& toDir, std::string& error) {
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring tar = std::wstring(system) + L"\\tar.exe";
        if (!fileExists(tar)) {
            error = "manca tar.exe di Windows";
            return false;
        }
        std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + toDir + L"\" " + TextUtils::utf8ToWide(inside);
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            error = "tar.exe non parte";
            return false;
        }
        CloseHandle(pi.hThread);
        const DWORD wait = WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        if (code != 0) {
            error = "estrazione non riuscita (tar " + std::to_string(code) + ")";
            return false;
        }
        return true;
    }

    // --- ONNX Runtime --------------------------------------------------------------------------

    struct Engine {
        std::mutex mutex;
        bool tried = false;
        std::string failure;
        HMODULE dll = nullptr;
        const OrtApi* api = nullptr;
        OrtEnv* env = nullptr;
        OrtSession* session = nullptr;
        OrtMemoryInfo* memory = nullptr;
        std::string inputName, outputName;
        std::vector<std::string> characters; // classe -> carattere ("blank" alla 0)
    };
    Engine g;

    bool check(OrtStatus* status, std::string& error) {
        if (!status) return true;
        error = g.api->GetErrorMessage(status);
        g.api->ReleaseStatus(status);
        return false;
    }

    std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
            if (end == text.size()) break;
            start = end + 1;
        }
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    }

    bool loadLocked(std::string& error) {
        const std::wstring dll = runtimePath(), model = modelPath();
        if (!fileExists(dll) || !fileExists(model)) {
            error = "OCR del mining non ancora scaricato";
            return false;
        }
        // Dal percorso completo: Windows ha una sua onnxruntime.dll (vecchia) in System32.
        g.dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g.dll) {
            const DWORD code = GetLastError();
            error = code == ERROR_MOD_NOT_FOUND ? "onnxruntime.dll non si carica: manca Microsoft Visual C++ Redistributable (x64)"
                                                : "onnxruntime.dll non si carica (errore " + std::to_string(code) + ")";
            return false;
        }
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        auto getApiBase = reinterpret_cast<GetApiBase>(GetProcAddress(g.dll, "OrtGetApiBase"));
        const OrtApiBase* base = getApiBase ? getApiBase() : nullptr;
        g.api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (!g.api) {
            error = "onnxruntime.dll di una versione troppo vecchia";
            return false;
        }
        OrtSessionOptions* options = nullptr;
        bool ok = check(g.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "iris", &g.env), error) &&
                  check(g.api->CreateSessionOptions(&options), error) &&
                  // Pochi fili: il modello è piccolo, e il gioco gira accanto.
                  check(g.api->SetIntraOpNumThreads(options, 2), error) && check(g.api->SetInterOpNumThreads(options, 1), error) &&
                  check(g.api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL), error) &&
                  check(g.api->CreateSession(g.env, model.c_str(), options, &g.session), error) &&
                  check(g.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g.memory), error);
        if (options) g.api->ReleaseSessionOptions(options);
        if (!ok) return false;

        OrtAllocator* allocator = nullptr;
        if (!check(g.api->GetAllocatorWithDefaultOptions(&allocator), error)) return false;
        char* name = nullptr;
        if (!check(g.api->SessionGetInputName(g.session, 0, allocator, &name), error)) return false;
        g.inputName = name;
        g.api->AllocatorFree(allocator, name);
        if (!check(g.api->SessionGetOutputName(g.session, 0, allocator, &name), error)) return false;
        g.outputName = name;
        g.api->AllocatorFree(allocator, name);

        // L'alfabeto sta dentro il modello (metadato "character"), come lo legge RapidOCR.
        OrtModelMetadata* meta = nullptr;
        if (!check(g.api->SessionGetModelMetadata(g.session, &meta), error)) return false;
        char* value = nullptr;
        ok = check(g.api->ModelMetadataLookupCustomMetadataMap(meta, allocator, "character", &value), error);
        g.api->ReleaseModelMetadata(meta);
        if (!ok) return false;
        if (!value) {
            error = "il modello non ha l'elenco dei caratteri";
            return false;
        }
        g.characters = { "blank" };
        for (auto& c : splitLines(value)) g.characters.push_back(c);
        g.characters.push_back(" ");
        g.api->AllocatorFree(allocator, value);
        Log::info("OCR del mining: PP-OCRv3 pronto (ONNX Runtime, " + std::to_string(g.characters.size()) + " caratteri)");
        return true;
    }
}

std::wstring folder() { return AppPaths::getModelsDir() + L"\\ppocr"; }
std::wstring runtimePath() { return folder() + L"\\onnxruntime.dll"; }
std::wstring modelPath() { return folder() + L"\\ch_PP-OCRv3_rec_infer.onnx"; }

bool filesPresent() { return fileExists(runtimePath()) && fileExists(modelPath()); }

bool ensureFiles(const std::function<void(long long, long long)>& onProgress, const std::atomic<bool>* cancel, std::string& error) {
    CreateDirectoryW(AppPaths::getModelsDir().c_str(), nullptr);
    CreateDirectoryW(folder().c_str(), nullptr);
    if (!fileExists(modelPath())) {
        Log::info("OCR del mining: scarico il modello PP-OCRv3 (10 MB)");
        if (!fetch(kModelUrl, modelPath(), kModelSha256, onProgress, cancel, error)) return false;
    }
    if (!fileExists(runtimePath())) {
        Log::info("OCR del mining: scarico ONNX Runtime 1.20.1 (11 MB)");
        const std::wstring package = folder() + L"\\onnxruntime.zip";
        const std::wstring temp = folder() + L"\\estrazione";
        if (!fetch(kRuntimeUrl, package, kRuntimePackageSha256, onProgress, cancel, error)) return false;
        CreateDirectoryW(temp.c_str(), nullptr);
        const std::wstring extracted = temp + L"\\onnxruntime\\capi\\onnxruntime.dll";
        bool ok = extract(package, kRuntimeInPackage, temp, error);
        if (ok && !hashIs(extracted, kRuntimeDllSha256)) {
            ok = false;
            error = "onnxruntime.dll estratta non ha l'impronta attesa";
        }
        if (ok && !MoveFileExW(extracted.c_str(), runtimePath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ok = false;
            error = "impossibile mettere a posto onnxruntime.dll";
        }
        DeleteFileW(extracted.c_str());
        RemoveDirectoryW((temp + L"\\onnxruntime\\capi").c_str());
        RemoveDirectoryW((temp + L"\\onnxruntime").c_str());
        RemoveDirectoryW(temp.c_str());
        DeleteFileW(package.c_str());
        if (!ok) return false;
    }
    return true;
}

namespace {
    unsigned long long stampOf(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
        return ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    }
    unsigned long long g_stamp = 0;
}

namespace {
    std::atomic<bool> g_enabled{ true };
}

void setEnabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

bool load(std::string& error) {
    if (!g_enabled) {
        error = "PP-OCR spento";
        return false;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.session) return true;
    // Un tentativo fallito non si ripete a ogni lettura: si riprova solo se i file sono
    // cambiati nel frattempo (per esempio appena scaricati).
    const unsigned long long stamp = stampOf(runtimePath()) * 31 + stampOf(modelPath());
    if (g.tried && stamp == g_stamp) {
        error = g.failure;
        return false;
    }
    g_stamp = stamp;
    g.tried = true;
    if (loadLocked(error)) return true;
    g.failure = error;
    Log::warn("OCR del mining: PP-OCR non disponibile (" + error + "), uso l'OCR di Windows");
    // Quello che si è aperto a metà si chiude: al prossimo tentativo si riparte da zero.
    if (g.api) {
        if (g.session) g.api->ReleaseSession(g.session);
        if (g.memory) g.api->ReleaseMemoryInfo(g.memory);
        if (g.env) g.api->ReleaseEnv(g.env);
    }
    g.session = nullptr;
    g.memory = nullptr;
    g.env = nullptr;
    g.api = nullptr;
    if (g.dll) FreeLibrary(g.dll);
    g.dll = nullptr;
    return false;
}

bool ready() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.session != nullptr;
}

bool recognize(const MiningImage::Gray& image, Line& out, std::string& error) {
    out = Line{};
    if (!load(error)) return false;
    if (image.w < 4 || image.h < 4) {
        error = "immagine troppo piccola";
        return false;
    }
    int width = 0;
    std::vector<float> tensor = toTensor(image, width);
    const int64_t shape[4] = { 1, 3, kHeight, width };
    OrtValue* input = nullptr;
    OrtValue* output = nullptr;
    bool ok = check(g.api->CreateTensorWithDataAsOrtValue(g.memory, tensor.data(), tensor.size() * sizeof(float), shape, 4,
                                                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input),
                    error);
    if (ok) {
        const char* inputs[] = { g.inputName.c_str() };
        const char* outputs[] = { g.outputName.c_str() };
        ok = check(g.api->Run(g.session, nullptr, inputs, &input, 1, outputs, 1, &output), error);
    }
    if (ok) {
        float* probs = nullptr;
        OrtTensorTypeAndShapeInfo* info = nullptr;
        ok = check(g.api->GetTensorMutableData(output, reinterpret_cast<void**>(&probs)), error) &&
             check(g.api->GetTensorTypeAndShape(output, &info), error);
        if (ok) {
            size_t count = 0;
            int64_t dims[3] = {};
            ok = check(g.api->GetDimensionsCount(info, &count), error) && count == 3 && check(g.api->GetDimensions(info, dims, 3), error);
            g.api->ReleaseTensorTypeAndShapeInfo(info);
            if (ok) out = ctcDecode(probs, (int)dims[1], (int)dims[2], g.characters);
            else if (error.empty()) error = "uscita del modello inattesa";
        }
    }
    if (output) g.api->ReleaseValue(output);
    if (input) g.api->ReleaseValue(input);
    return ok;
}

// --- Parti pure ------------------------------------------------------------------------------

std::vector<float> toTensor(const MiningImage::Gray& image, int& width, int maxWidth) {
    MiningImage::Gray resized = MiningImage::resizeToHeight(image, kHeight);
    width = std::clamp(resized.w, 8, maxWidth);
    std::vector<float> t((size_t)3 * kHeight * width, 0.0f); // fuori dall'immagine: 0 (come il riempimento di RapidOCR)
    const int copyW = std::min(width, resized.w);
    for (int y = 0; y < kHeight && y < resized.h; ++y) {
        for (int x = 0; x < copyW; ++x) {
            const float v = (resized.at(x, y) / 255.0f - 0.5f) / 0.5f;
            for (int c = 0; c < 3; ++c) t[((size_t)c * kHeight + y) * width + x] = v;
        }
    }
    return t;
}

Line ctcDecode(const float* probabilities, int steps, int classes, const std::vector<std::string>& characters) {
    Line out;
    double sum = 0;
    int kept = 0, previous = -1;
    for (int t = 0; t < steps; ++t) {
        const float* row = probabilities + (size_t)t * classes;
        int best = 0;
        for (int c = 1; c < classes; ++c) {
            if (row[c] > row[best]) best = c;
        }
        const bool repeat = best == previous;
        previous = best;
        if (best == 0 || repeat) continue;
        if (best < (int)characters.size()) out.text += characters[best];
        sum += row[best];
        ++kept;
    }
    out.confidence = kept ? (float)(sum / kept) : 0.0f;
    return out;
}

MiningImage::Gray tight(const MiningImage::Gray& gray) {
    if (gray.empty()) return gray;
    double mean = 0, sq = 0;
    for (uint8_t v : gray.p) mean += v;
    mean /= gray.p.size();
    for (uint8_t v : gray.p) sq += (v - mean) * (v - mean);
    const double sd = std::sqrt(sq / gray.p.size());
    const int threshold = (int)std::clamp(mean + 1.1 * sd, 70.0, 235.0);
    int x0 = gray.w, x1 = -1, y0 = gray.h, y1 = -1;
    size_t bright = 0;
    for (int y = 0; y < gray.h; ++y) {
        for (int x = 0; x < gray.w; ++x) {
            if (gray.at(x, y) <= threshold) continue;
            ++bright;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    if (bright < std::max<size_t>(12, gray.p.size() / 100) || x1 < x0 || y1 < y0) return gray;
    const int padX = std::max(2, (int)((x1 - x0) * 0.06)), padY = std::max(2, (int)((y1 - y0) * 0.10));
    MiningImage::Box b;
    b.x = std::max(0, x0 - padX);
    b.y = std::max(0, y0 - padY);
    b.w = std::min(gray.w, x1 + padX + 1) - b.x;
    b.h = std::min(gray.h, y1 + padY + 1) - b.y;
    return MiningImage::crop(gray, b);
}

} // namespace PpOcr
