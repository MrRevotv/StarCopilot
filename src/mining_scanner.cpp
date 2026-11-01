#include "mining_scanner.h"
#include "app_paths.h"
#include "log.h"
#include "ocr_engine.h"
#include "ppocr.h"
#include "screen_capture.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
    constexpr size_t kHistory = 30;
    constexpr size_t kMaxUnknowns = 200;
    constexpr int kManualMaxDigits = 8;
    constexpr unsigned long long kManualTimeoutMs = 45000;

    std::atomic<MiningScanner*> g_manualOwner{ nullptr };

    std::string nowTime(bool withDate) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[32];
        if (withDate) std::snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
        else std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    bool readFile(const std::wstring& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    bool writeFile(const std::wstring& path, const std::string& content) {
        const std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << content;
            if (!f.good()) return false;
        }
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    // La lettura continua ha senso solo col gioco davanti (come la traduzione della chat).
    bool starCitizenInForeground() {
        static DWORD lastPid = 0;
        static bool lastResult = false;
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (!pid) return false;
        if (pid == lastPid) return lastResult;
        bool result = false;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                const wchar_t* file = wcsrchr(path, L'\\');
                result = _wcsicmp(file ? file + 1 : path, L"StarCitizen.exe") == 0;
            }
            CloseHandle(process);
        }
        lastPid = pid;
        lastResult = result;
        return result;
    }

    std::string lower(const std::string& s) { return TextUtils::toLower(TextUtils::trim(s)); }

    // lookup_table.json di mining_ocr: [{"code":"3,170","decoded":"Quantanium (x1)"}, ...]
    // Le firme base sono le voci "(x1)".
    bool parseLookupJson(const std::string& text, std::vector<Mining::Material>& out) {
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_array()) return false;
        for (const auto& item : j) {
            if (!item.is_object()) continue;
            const std::string code = item.value("code", ""), decoded = item.value("decoded", "");
            const std::string suffix = " (x1)";
            if (decoded.size() <= suffix.size() || decoded.compare(decoded.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            std::string digits;
            for (char c : code) {
                if (c >= '0' && c <= '9') digits += c;
            }
            if (digits.empty()) continue;
            Mining::Material m;
            m.name = decoded.substr(0, decoded.size() - suffix.size());
            m.base = std::atoi(digits.c_str());
            m.kind = TextUtils::containsIgnoreCase(m.name, "scrap") ? "salvage" : "minerale";
            if (m.base > 0) out.push_back(m);
        }
        return !out.empty();
    }

}

MiningScanner::~MiningScanner() {
    shutdown();
}

std::wstring MiningScanner::materialsPath() {
    return AppPaths::getAppDataDir() + L"\\mining_materiali.csv";
}

std::wstring MiningScanner::unknownsPath() {
    return AppPaths::getAppDataDir() + L"\\mining_sconosciute.csv";
}

std::wstring MiningScanner::imagesDir() {
    return AppPaths::getAppDataDir() + L"\\debug\\mining";
}

void MiningScanner::start(Host host) {
    if (m_worker.joinable()) return;
    m_host = std::move(host);
    m_stopping = false;
    loadData();
    m_worker = std::thread(&MiningScanner::workerLoop, this);
}

void MiningScanner::shutdown() {
    m_stopping = true;
    cancelManualInput();
    { std::lock_guard<std::mutex> lock(m_jobMutex); }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void MiningScanner::settingsChanged() {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_wake = true;
    }
    m_jobCv.notify_all();
}

void MiningScanner::notify(Event e) {
    if (m_host.notify) m_host.notify(e);
}

// --- Dati ------------------------------------------------------------------------------

void MiningScanner::loadData() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string text;
    if (readFile(materialsPath(), text)) {
        std::vector<std::string> errors;
        m_materials = Mining::parseTable(text, &errors);
        for (const auto& e : errors) Log::warn("Mining, tabella dei materiali: " + e);
        if (m_materials.empty()) {
            Log::warn("Mining: la tabella dei materiali è vuota o illeggibile, uso quella predefinita (il file resta com'è)");
            m_materials = Mining::defaultMaterials();
        } else if (text.find("roc o fps") == std::string::npos) {
            // Tabella salvata prima dei depositi a terra (ROC e a mano): si aggiungono una volta,
            // e la nuova intestazione ricorda che è già fatto (se poi li togli, restano tolti).
            for (const auto& g : Mining::groundDeposits()) {
                const bool present = std::any_of(m_materials.begin(), m_materials.end(), [&](const Mining::Material& m) { return m.kind == g.kind; });
                if (!present) m_materials.push_back(g);
            }
            writeFile(materialsPath(), Mining::formatTable(m_materials));
            Log::info("Mining: aggiunti alla tabella i depositi a terra (ROC 4000, a mano 3000)");
        }
    } else {
        // Primo avvio: la tabella dentro il programma diventa un file modificabile.
        m_materials = Mining::defaultMaterials();
        writeFile(materialsPath(), Mining::formatTable(m_materials));
    }
    m_unknowns.clear();
    if (readFile(unknownsPath(), text)) {
        for (const std::string& line : TextUtils::splitLines(text)) {
            std::stringstream ss(line);
            std::string v, t, c;
            std::getline(ss, v, ';');
            std::getline(ss, t, ';');
            std::getline(ss, c, ';');
            const int value = std::atoi(v.c_str());
            if (value <= 0) continue;
            m_unknowns.push_back({ value, TextUtils::trim(t), std::max(1, std::atoi(c.c_str())) });
        }
    }
}

void MiningScanner::saveMaterials() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        text = Mining::formatTable(m_materials);
    }
    if (!writeFile(materialsPath(), text)) Log::warn("Mining: tabella dei materiali non salvata");
}

void MiningScanner::saveUnknowns() {
    std::string text = "# Signature lette che non tornano con nessun materiale: valore;quando;volte\n";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& u : m_unknowns) text += std::to_string(u.value) + ";" + u.time + ";" + std::to_string(u.count) + "\n";
    }
    if (!writeFile(unknownsPath(), text)) Log::warn("Mining: elenco delle sconosciute non salvato");
}

std::vector<Mining::Material> MiningScanner::materials() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_materials;
}

std::string MiningScanner::materialsText() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return Mining::formatTable(m_materials);
}

bool MiningScanner::setMaterialsText(const std::string& text, std::string& message) {
    std::vector<std::string> errors;
    std::vector<Mining::Material> parsed = Mining::parseTable(text, &errors);
    if (parsed.empty()) {
        message = "Nessun materiale valido: niente salvato.";
        if (!errors.empty()) message += " (" + errors.front() + ")";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_materials = parsed;
    }
    saveMaterials();
    message = "Salvati " + std::to_string(parsed.size()) + " materiali.";
    if (!errors.empty()) {
        message += " Righe saltate: " + std::to_string(errors.size()) + " (" + errors.front() + (errors.size() > 1 ? ", ..." : "") + ").";
    }
    notify(Event::Data);
    return true;
}

void MiningScanner::resetMaterials() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_materials = Mining::defaultMaterials();
    }
    saveMaterials();
    notify(Event::Data);
}

bool MiningScanner::importFile(const std::wstring& path, bool replace, std::string& message) {
    std::string bytes;
    if (!readFile(path, bytes) || bytes.empty()) {
        message = "Il file non si apre o è vuoto.";
        return false;
    }
    std::vector<Mining::Material> imported;
    std::string csv;
    std::string format;
    if (Mining::decodeMscDatabase(bytes, csv)) {
        imported = Mining::parseTable(csv);
        format = "database di Mining Signature Calculator";
    } else if (parseLookupJson(bytes, imported)) {
        format = "tabella di mining_ocr";
    } else {
        std::vector<std::string> errors;
        imported = Mining::parseTable(bytes, &errors);
        format = "CSV";
        if (imported.empty()) {
            message = "Nessun materiale valido nel file" + (errors.empty() ? std::string(".") : " (" + errors.front() + ").");
            return false;
        }
    }
    if (imported.empty()) {
        message = "Il file (" + format + ") non contiene materiali.";
        return false;
    }
    int added = 0, updated = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (replace) {
            m_materials = imported;
            added = (int)imported.size();
        } else {
            for (const auto& m : imported) {
                auto it = std::find_if(m_materials.begin(), m_materials.end(), [&](const Mining::Material& x) { return lower(x.name) == lower(m.name); });
                if (it == m_materials.end()) {
                    m_materials.push_back(m);
                    ++added;
                } else if (it->base != m.base || it->kind != m.kind) {
                    it->base = m.base;
                    it->kind = m.kind;
                    ++updated;
                }
            }
        }
    }
    saveMaterials();
    message = "Importato (" + format + "): " + std::to_string(added) + (replace ? " materiali." : " aggiunti, " + std::to_string(updated) + " aggiornati.");
    notify(Event::Data);
    return true;
}

bool MiningScanner::exportCsv(const std::wstring& path, std::string& message) {
    std::string text = "name,base_signature,kind\n";
    for (const auto& m : materials()) text += m.name + "," + std::to_string(m.base) + "," + m.kind + "\n";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        message = "Non riesco a scrivere il file.";
        return false;
    }
    f << text;
    message = "Esportati " + std::to_string(materials().size()) + " materiali.";
    return f.good();
}

std::vector<MiningScanner::Unknown> MiningScanner::unknowns() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_unknowns;
}

void MiningScanner::noteUnknown(int value) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_unknowns.begin(), m_unknowns.end(), [&](const Unknown& u) { return u.value == value; });
        if (it != m_unknowns.end()) {
            Unknown u = *it;
            u.count++;
            u.time = nowTime(true);
            m_unknowns.erase(it);
            m_unknowns.insert(m_unknowns.begin(), u);
        } else {
            m_unknowns.insert(m_unknowns.begin(), Unknown{ value, nowTime(true), 1 });
            if (m_unknowns.size() > kMaxUnknowns) m_unknowns.pop_back();
        }
    }
    saveUnknowns();
    notify(Event::Data);
}

void MiningScanner::removeUnknown(int value) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_unknowns.erase(std::remove_if(m_unknowns.begin(), m_unknowns.end(), [&](const Unknown& u) { return u.value == value; }), m_unknowns.end());
    }
    saveUnknowns();
    notify(Event::Data);
}

void MiningScanner::clearUnknowns() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_unknowns.clear();
    }
    saveUnknowns();
    notify(Event::Data);
}

bool MiningScanner::identify(int value, const std::string& material, int quantity, std::string& message) {
    const std::string name = TextUtils::trim(material);
    if (value <= 0 || name.empty()) {
        message = "Servono il valore e il nome del materiale.";
        return false;
    }
    if (quantity <= 0) {
        message = "La quantità deve essere un numero intero positivo.";
        return false;
    }
    if (value % quantity != 0) {
        message = Mining::formatValue(value) + " non è divisibile per " + std::to_string(quantity) + ": controlla la quantità.";
        return false;
    }
    const int base = value / quantity;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_materials.begin(), m_materials.end(), [&](const Mining::Material& x) { return lower(x.name) == lower(name); });
        if (it != m_materials.end()) {
            message = it->name + ": firma base " + (it->base == base ? "già " + std::to_string(base) + "." : std::to_string(it->base) + " → " + std::to_string(base) + ".");
            it->base = base;
        } else {
            Mining::Material m{ name, base, TextUtils::containsIgnoreCase(name, "scrap") ? "salvage" : "minerale" };
            m_materials.push_back(m);
            message = "Aggiunto " + name + " con firma base " + std::to_string(base) + ".";
        }
        m_unknowns.erase(std::remove_if(m_unknowns.begin(), m_unknowns.end(), [&](const Unknown& u) { return u.value == value; }), m_unknowns.end());
    }
    saveMaterials();
    saveUnknowns();
    notify(Event::Data);
    return true;
}

// --- Letture ----------------------------------------------------------------------------

MiningScanner::Reading MiningScanner::lastReading() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last;
}

std::vector<MiningScanner::Reading> MiningScanner::history() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<Reading>(m_history.begin(), m_history.end());
}

void MiningScanner::enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        // Tasto premuto più volte di fila mentre legge: basta una lettura.
        if (job.kind == Job::Kind::Scan && std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job& j) { return j.kind == Job::Kind::Scan; })) return;
        m_jobs.push_back(std::move(job));
    }
    m_jobCv.notify_all();
}

void MiningScanner::scan(const std::string& source) {
    Job j;
    j.kind = Job::Kind::Scan;
    j.source = source;
    enqueue(std::move(j));
}

void MiningScanner::submitValue(int value, const std::string& source) {
    Job j;
    j.kind = Job::Kind::Value;
    j.value = value;
    j.source = source;
    enqueue(std::move(j));
}

void MiningScanner::submitText(const std::string& text, const std::string& source) {
    Job j;
    j.kind = Job::Kind::Text;
    j.text = text;
    j.source = source;
    enqueue(std::move(j));
}

void MiningScanner::workerLoop() {
    while (!m_stopping) {
        const AppSettings s = m_host.settings ? m_host.settings() : AppSettings{};
        const bool regionOk = s.miningRegionW > 0 && s.miningRegionH > 0;
        const bool continuous = s.miningEnabled && s.miningAuto && regionOk;
        Job job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            auto ready = [&] { return m_stopping.load() || !m_jobs.empty() || m_wake.load(); };
            if (continuous) m_jobCv.wait_for(lock, std::chrono::milliseconds(s.miningIntervalMs), ready);
            else m_jobCv.wait(lock, ready);
            m_wake = false;
            if (m_stopping) break;
            if (!m_jobs.empty()) {
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                have = true;
            }
        }
        const AppSettings now = m_host.settings ? m_host.settings() : AppSettings{};
        if (have) {
            if (job.kind == Job::Kind::Scan) {
                runScan(now, job.source, false);
            } else {
                Reading r;
                r.valid = true;
                r.time = nowTime(false);
                r.tick = GetTickCount64();
                r.source = job.source;
                const auto mats = materials();
                if (job.kind == Job::Kind::Value) {
                    r.decoded = Mining::decodeValue(job.value, mats, now.miningMaxQuantity);
                    r.ocrText = std::to_string(job.value);
                } else {
                    r.decoded = Mining::decodeText(job.text, mats, now.miningMaxQuantity);
                    r.ocrText = job.text;
                    if (!r.decoded.hasValue) r.error = "nessun numero nel testo";
                }
                publish(std::move(r), true);
            }
        } else if (continuous && starCitizenInForeground()) {
            runScan(now, "continua", true);
        }
    }
}

MiningScanner::Reading MiningScanner::readImage(const ScreenCapture::Image& image, const std::vector<Mining::Material>& materials,
                                                int maxQuantity, const std::wstring& saveImagesDir) {
    Reading r;
    r.valid = true;
    const auto t0 = std::chrono::steady_clock::now();
    if (image.width < 8 || image.height < 8) {
        r.error = "zona troppo piccola o fuori dallo schermo";
        return r;
    }
    const MiningImage::Steps st = MiningImage::process(image, 120);
    r.badgeFound = !(st.badge.x == 0 && st.badge.y == 0 && st.badge.w == image.width && st.badge.h == image.height);

    // Tentativi, dal più affidabile: prima PP-OCR (il riconoscitore di Citizen Assistant)
    // sul numero ritagliato, poi, se non basta, l'OCR di Windows su più versioni dell'immagine.
    struct Tried {
        std::string name;
        ScreenCapture::Image image; // per le immagini di controllo
        std::string text;
        Mining::Decoded decoded;
        float confidence = -1.0f;   // solo PP-OCR la dà
    };
    std::vector<Tried> tried;
    std::string ocrError;
    int chosen = -1;
    auto clean = [](const Tried& t) { return !t.decoded.matches.empty() && !t.decoded.corrected; };

    // 1) PP-OCR: solo riconoscimento, sulle cifre (badge senza icona, ritaglio stretto),
    //    in grigi e in bianco e nero (Otsu), poi sull'immagine col contrasto (CLAHE).
    std::string ppError;
    if (PpOcr::load(ppError)) {
        const MiningImage::Gray digits = PpOcr::tight(r.badgeFound ? st.cropped : MiningImage::toGray(image));
        const std::pair<const char*, MiningImage::Gray> inputs[] = {
            { "PP-OCR", digits },
            { "PP-OCR binaria", MiningImage::binarize(digits, MiningImage::otsuThreshold(digits)) },
            { "PP-OCR contrasto", st.enhanced },
        };
        for (const auto& [name, gray] : inputs) {
            PpOcr::Line line;
            std::string error;
            if (!PpOcr::recognize(gray, line, error)) {
                ocrError = "PP-OCR: " + error;
                continue;
            }
            Tried t;
            t.name = name;
            t.image = MiningImage::toBgra(gray);
            t.text = line.text;
            t.confidence = line.confidence;
            t.decoded = Mining::decodeText(t.text, materials, maxQuantity);
            tried.push_back(std::move(t));
            // Sicura e in tabella: basta così (come il "single read" di Citizen Assistant).
            if (clean(tried.back()) && line.confidence >= 0.85f) {
                chosen = (int)tried.size() - 1;
                break;
            }
        }
    }

    // 2) OCR di Windows, dalla versione più pulita alla più grezza: la prima in tabella vince.
    if (chosen < 0) {
        std::vector<std::pair<std::string, ScreenCapture::Image>> variants;
        variants.push_back({ "binaria", MiningImage::toBgra(MiningImage::pad(MiningImage::darkTextOnLight(st.binary), 24, 255)) });
        variants.push_back({ "contrasto", MiningImage::toBgra(MiningImage::pad(MiningImage::darkTextOnLight(st.enhanced), 24, 255)) });
        {
            // Come faceva Mining Signature Calculator (tutta la zona): utile quando il badge non si trova.
            const int scale = std::clamp(OcrEngineWrapper::maxImageDimension() / std::max(1, std::max(image.width, image.height)), 1, 3);
            variants.push_back({ "zona intera, contrasto", MiningImage::toBgra(MiningImage::simpleContrast(image, scale)) });
        }
        {
            MiningImage::Box digits = st.badge;
            digits.x += st.iconCut;
            digits.w -= st.iconCut;
            const ScreenCapture::Image color = MiningImage::crop(image, digits);
            const int scale = std::clamp(120 / std::max(1, color.height), 1, 6);
            variants.push_back({ "pulizia chat", ScreenCapture::prepareForOcr(color, scale, OcrEngineWrapper::maxImageDimension()) });
        }
        {
            const int scale = std::clamp(360 / std::max(1, image.height), 1, 4);
            variants.push_back({ "zona intera, pulizia chat", ScreenCapture::prepareForOcr(image, scale, OcrEngineWrapper::maxImageDimension()) });
        }
        for (auto& [name, variantImage] : variants) {
            std::string error;
            const auto lines = OcrEngineWrapper::recognize("en", variantImage, error);
            if (!error.empty()) ocrError = error;
            Tried t;
            t.name = name;
            t.text = OcrEngineWrapper::joinLines(lines);
            for (char& c : t.text) {
                if (c == '\n') c = ' ';
            }
            t.decoded = Mining::decodeText(t.text, materials, maxQuantity);
            t.image = std::move(variantImage);
            tried.push_back(std::move(t));
            if (clean(tried.back())) {
                chosen = (int)tried.size() - 1;
                break;
            }
        }
    }

    if (chosen < 0) {
        // Nessuna lettura pulita: la migliore tra quelle fatte (in tabella > corretta > solo un numero),
        // con un punto in più a ogni altra versione che ha letto lo stesso valore e la sicurezza di PP-OCR.
        int bestScore = -1;
        for (size_t i = 0; i < tried.size(); ++i) {
            const auto& d = tried[i].decoded;
            if (!d.hasValue) continue;
            int score = (d.matches.empty() ? 0 : 100) + (d.corrected ? 0 : 20) + (int)std::to_string(d.value).size();
            if (tried[i].confidence >= 0) score += (int)(tried[i].confidence * 15);
            for (size_t k = 0; k < tried.size(); ++k) {
                if (k != i && tried[k].decoded.hasValue && tried[k].decoded.value == d.value) score += 10;
            }
            if (score > bestScore) {
                bestScore = score;
                chosen = (int)i;
            }
        }
    }
    if (chosen >= 0) {
        r.variant = chosen;
        r.engine = tried[chosen].name;
        r.confidence = tried[chosen].confidence;
        r.ocrText = tried[chosen].text;
        r.decoded = tried[chosen].decoded;
        // Una cifra "simile" corretta, ma più versioni dell'immagine hanno letto lo stesso numero:
        // la lettura è sicura, il valore probabilmente non è in tabella. Meglio "sconosciuta"
        // (da identificare) che un minerale sbagliato.
        if (r.decoded.digitFix && r.decoded.readValue > 0) {
            int agree = 0;
            for (const auto& t : tried) agree += t.decoded.readValue == r.decoded.readValue ? 1 : 0;
            if (agree >= 2) {
                const int read = r.decoded.readValue;
                const std::string alternative = Mining::summary(r.decoded);
                r.decoded = Mining::decodeValue(read, materials, maxQuantity);
                r.decoded.note = "letto uguale da " + std::to_string(agree) + " versioni dell'immagine; se è una cifra sbagliata: " + alternative;
            }
        }
    } else {
        std::string all;
        for (const auto& t : tried) {
            if (!t.text.empty()) all += (all.empty() ? "" : " | ") + t.text;
        }
        r.ocrText = all;
        r.error = !ocrError.empty() ? ocrError : (r.badgeFound ? "nessun numero letto" : "nessun badge con la signature nella zona");
    }
    r.durationMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (!saveImagesDir.empty()) {
        // Solo l'ultima lettura (si sovrascrive): niente cartelle da centinaia di MB.
        CreateDirectoryW((AppPaths::getAppDataDir() + L"\\debug").c_str(), nullptr);
        CreateDirectoryW(saveImagesDir.c_str(), nullptr);
        MiningImage::saveBmp(saveImagesDir + L"\\01_zona.bmp", image);
        MiningImage::saveBmp(saveImagesDir + L"\\02_badge.bmp", st.cropped);
        MiningImage::saveBmp(saveImagesDir + L"\\03_ingrandita.bmp", st.resized);
        MiningImage::saveBmp(saveImagesDir + L"\\04_contrasto.bmp", st.enhanced);
        MiningImage::saveBmp(saveImagesDir + L"\\05_binaria.bmp", st.binary);
        std::string info = "Badge: x=" + std::to_string(st.badge.x) + " y=" + std::to_string(st.badge.y) + " w=" + std::to_string(st.badge.w) +
                           " h=" + std::to_string(st.badge.h) + (r.badgeFound ? "" : " (non trovato: zona intera)") +
                           ", icona tolta fino alla colonna " + std::to_string(st.iconCut) + "\r\n";
        if (!ppError.empty()) info += "PP-OCR non disponibile: " + ppError + "\r\n";
        for (size_t i = 0; i < tried.size(); ++i) {
            // Nomi dei file senza virgole né spazi strani.
            std::string file = tried[i].name;
            for (char& c : file) {
                if (c == ',' || c == ' ') c = '_';
            }
            MiningImage::saveBmp(saveImagesDir + L"\\1" + std::to_wstring(i) + L"_ocr_" + TextUtils::utf8ToWide(file) + L".bmp", tried[i].image);
            info += "OCR " + tried[i].name + ": \"" + tried[i].text + "\"" +
                    (tried[i].confidence >= 0 ? " (" + std::to_string((int)std::lround(tried[i].confidence * 100)) + "%)" : std::string()) +
                    " -> " + Mining::summary(tried[i].decoded) + "\r\n";
        }
        info += "Scelta: " + (chosen >= 0 ? tried[chosen].name + " -> " + Mining::summary(r.decoded) : "nessuna (" + r.error + ")") + "\r\n";
        std::ofstream f(saveImagesDir + L"\\00_lettura.txt", std::ios::binary | std::ios::trunc);
        f << info;
    }
    return r;
}

void MiningScanner::runScan(const AppSettings& s, const std::string& source, bool continuous) {
    Reading r;
    if (s.miningRegionW <= 0 || s.miningRegionH <= 0) {
        if (continuous) return;
        r.valid = true;
        r.error = "zona non scelta: sceglila nella pagina Mining";
    } else {
        const ScreenCapture::Image image = ScreenCapture::capture(s.miningRegionX, s.miningRegionY, s.miningRegionW, s.miningRegionH);
        if (continuous) {
            // Schermo fermo: stessa lettura di prima, niente OCR.
            const uint64_t h = ScreenCapture::hash(image);
            if (h == m_autoLastHash) return;
            m_autoLastHash = h;
        }
        r = readImage(image, materials(), s.miningMaxQuantity, s.miningSaveImages ? imagesDir() : std::wstring());
    }
    r.time = nowTime(false);
    r.tick = GetTickCount64();
    r.source = source;

    if (continuous) {
        // Si mostra un valore letto due volte di fila, e non di nuovo finché è ancora a schermo.
        // Eccezione (Citizen Assistant): una lettura di PP-OCR sicurissima e in tabella si
        // mostra subito, ma solo se a schermo non c'è già un altro valore: per CAMBIARE un
        // numero mostrato servono sempre due letture uguali (l'OCR può alternare due valori
        // entrambi validi, es. 8.000 e 28.000 con una cifra vicina, e il riquadro ballerebbe).
        if (!r.decoded.hasValue) {
            m_autoCandidate = 0;
            return;
        }
        const bool showing = m_autoShown != 0 && r.tick - m_autoShownTick < (unsigned long long)s.miningOverlaySeconds * 1000ULL;
        const bool sure = !r.decoded.matches.empty() && !r.decoded.corrected && r.confidence >= 0.97f;
        const bool fastLock = sure && !(showing && r.decoded.value != m_autoShown);
        const bool confirmed = r.decoded.value == m_autoCandidate;
        m_autoCandidate = r.decoded.value;
        if (!confirmed && !fastLock) return;
        if (r.decoded.value == m_autoShown && showing) return;
        m_autoShown = r.decoded.value;
        m_autoShownTick = r.tick;
    }
    publish(std::move(r), true);
}

std::string MiningScanner::spokenText(const Mining::Decoded& d) {
    // Solo valori certi e senza alternative: dire il minerale sbagliato è peggio che non dire niente.
    if (!d.hasValue || d.corrected || d.matches.size() != 1) return {};
    const Mining::Match& m = d.matches.front();
    // "Copper (Rame)" -> "Rame"; "Scrap (salvage)" -> "Scrap".
    std::string name = m.name;
    const size_t open = name.find(" ("), close = name.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        const std::string inside = name.substr(open + 2, close - open - 2);
        const bool translation = m.kind == "minerale" && inside != "spazio";
        name = translation ? inside : name.substr(0, open);
    }
    return m.quantity > 1 ? name + " per " + std::to_string(m.quantity) : name;
}

void MiningScanner::announce(const Reading& r) {
    const AppSettings s = m_host.settings ? m_host.settings() : AppSettings{};
    if (!s.miningSpeak || !m_host.speak || r.source == "prova") return;
    const std::string text = spokenText(r.decoded);
    if (text.empty()) return;
    // La stessa roccia si dice una volta sola: avvicinandosi la si rilegge di continuo, l'HUD la
    // perde e la ritrova, l'OCR sbaglia di una cifra (2%). Dopo 12 secondi senza letture si
    // ricomincia: una roccia nuova dello stesso minerale si dice di nuovo.
    const Mining::Match& m = r.decoded.matches.front();
    const std::string key = m.name + "|" + std::to_string(m.quantity);
    const int tolerance = std::max(2, (int)std::lround(std::abs(m_spokenValue) * 0.02));
    const bool recent = r.tick - m_spokenTick < 12000ULL;
    m_spokenTick = r.tick;
    if (recent && key == m_spokenKey && std::abs(r.decoded.value - m_spokenValue) <= tolerance) return;
    m_spokenKey = key;
    m_spokenValue = r.decoded.value;
    m_host.speak(text);
}

void MiningScanner::publish(Reading r, bool remember) {
    announce(r);
    const std::string line = "Mining (" + r.source + "): " + (r.error.empty() || r.decoded.hasValue ? Mining::summary(r.decoded) : r.error) +
                             (r.ocrText.empty() ? "" : "   [OCR: \"" + r.ocrText + "\"]") +
                             (r.durationMs > 0 ? "   " + std::to_string(r.durationMs) + " ms" : "");
    Log::info(line);
    const bool unknown = r.decoded.hasValue && r.decoded.matches.empty() && !r.decoded.corrected && r.decoded.value >= 1000;
    const int value = r.decoded.value;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last = r;
        m_history.push_front(r);
        if (m_history.size() > kHistory) m_history.pop_back();
    }
    if (remember && unknown) noteUnknown(value);
    if (m_onReading && m_last.source != "prova" && m_last.decoded.hasValue) m_onReading(m_last);
    notify(Event::Reading);
}

// --- Tastierino -----------------------------------------------------------------------------

MiningScanner::ManualState MiningScanner::manualState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_manual;
}

void MiningScanner::beginManualInput() {
    if (manualState().active) {
        // Secondo tocco del tasto: annulla.
        cancelManualInput();
        return;
    }
    if (m_manualThread.joinable()) m_manualThread.join();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_manual.active = true;
        m_manual.buffer.clear();
    }
    m_manualStartTick = GetTickCount64();
    m_manualThread = std::thread(&MiningScanner::manualThread, this);
    notify(Event::Manual);
}

void MiningScanner::cancelManualInput() {
    bool wasActive;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wasActive = m_manual.active;
        m_manual.active = false;
        m_manual.buffer.clear();
    }
    if (const DWORD id = m_manualThreadId.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (m_manualThread.joinable() && m_manualThread.get_id() != std::this_thread::get_id()) m_manualThread.join();
    if (wasActive) notify(Event::Manual);
}

void MiningScanner::manualThread() {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // la coda dei messaggi del thread
    m_manualThreadId = GetCurrentThreadId();
    g_manualOwner = this;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &MiningScanner::keyboardHook, GetModuleHandleW(nullptr), 0);
    if (!hook) Log::warn("Mining: inserimento manuale senza blocco dei tasti (hook non riuscito)");
    const UINT_PTR timer = SetTimer(nullptr, 0, 500, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            if (!manualState().active) break;
            if (GetTickCount64() - m_manualStartTick > kManualTimeoutMs) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_manual.active = false;
                m_manual.buffer.clear();
                break;
            }
        }
        DispatchMessageW(&msg);
    }
    KillTimer(nullptr, timer);
    if (hook) UnhookWindowsHookEx(hook);
    g_manualOwner = nullptr;
    m_manualThreadId = 0;
    notify(Event::Manual);
}

LRESULT CALLBACK MiningScanner::keyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        MiningScanner* owner = g_manualOwner.load();
        if (owner && !(k->flags & LLKHF_INJECTED)) {
            const DWORD vk = k->vkCode;
            const bool ours = (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || (vk >= '0' && vk <= '9') || vk == VK_BACK || vk == VK_RETURN ||
                              vk == VK_ESCAPE || vk == VK_DECIMAL || vk == VK_OEM_PERIOD || vk == VK_OEM_COMMA;
            if (ours) {
                if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) owner->onManualKey(vk);
                return 1; // il tasto non arriva al gioco
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool MiningScanner::onManualKey(DWORD vk) {
    int digit = -1;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) digit = (int)(vk - VK_NUMPAD0);
    else if (vk >= '0' && vk <= '9') digit = (int)(vk - '0');

    std::string confirmed;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_manual.active) return false;
        m_manualStartTick = GetTickCount64();
        if (digit >= 0) {
            if ((int)m_manual.buffer.size() < kManualMaxDigits && !(m_manual.buffer.empty() && digit == 0)) m_manual.buffer += (char)('0' + digit);
        } else if (vk == VK_BACK) {
            if (!m_manual.buffer.empty()) m_manual.buffer.pop_back();
        } else if (vk == VK_RETURN) {
            confirmed = m_manual.buffer;
            m_manual.active = false;
            m_manual.buffer.clear();
            finished = true;
        } else if (vk == VK_ESCAPE) {
            m_manual.active = false;
            m_manual.buffer.clear();
            finished = true;
        }
    }
    if (finished) {
        PostQuitMessage(0);
        if (!confirmed.empty()) submitValue(std::atoi(confirmed.c_str()), "manuale");
    }
    notify(Event::Manual);
    return true;
}
