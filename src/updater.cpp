#include "updater.h"
#include "app_paths.h"
#include "http_client.h"
#include "log.h"
#include "model_downloader.h"
#include "text_utils.h"
#include "version.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <vector>

namespace Updater {

namespace {
    std::vector<int> versionParts(const std::string& text) {
        std::vector<int> parts;
        size_t i = 0;
        while (i < text.size() && !std::isdigit((unsigned char)text[i])) ++i; // "V1.2" -> "1.2"
        while (i < text.size() && parts.size() < 4) {
            if (!std::isdigit((unsigned char)text[i])) break;
            long value = 0;
            while (i < text.size() && std::isdigit((unsigned char)text[i])) {
                value = std::min(value * 10 + (text[i] - '0'), 1000000L);
                ++i;
            }
            parts.push_back((int)value);
            if (i < text.size() && text[i] == '.') ++i;
            else break; // "1.2.0-beta": il resto non conta
        }
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    }

    std::wstring exePath() {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
            if (n == 0) return {};
            if (n < buf.size()) {
                buf.resize(n);
                return buf;
            }
            buf.resize(buf.size() * 2);
        }
    }

    std::wstring folderOf(const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    bool fileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    long long fileSize(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
        return ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }

    // Comincia come un programma di Windows ("MZ")?
    bool looksLikeExe(const std::wstring& path) {
        std::ifstream f(path, std::ios::binary);
        char head[2] = {};
        return f.read(head, 2) && head[0] == 'M' && head[1] == 'Z';
    }

    std::string readSmallFile(const std::wstring& path) {
        std::ifstream f(path, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return TextUtils::trim(s);
    }

    // "Iris-1.3.0.exe" -> "1.3.0"
    std::string versionFromFileName(const std::wstring& name) {
        const std::string n = TextUtils::wideToUtf8(name);
        const std::string prefix = "Iris-", suffix = ".exe";
        if (n.size() <= prefix.size() + suffix.size() || n.compare(0, prefix.size(), prefix) != 0 ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return {};
        }
        return n.substr(prefix.size(), n.size() - prefix.size() - suffix.size());
    }

    // Gli argomenti con cui è stato avviato questo programma (senza il nome e senza --dopo-aggiornamento).
    std::wstring forwardedArguments() {
        std::wstring out;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return out;
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--dopo-aggiornamento") == 0) continue;
            out += L" \"" + std::wstring(argv[i]) + L"\"";
        }
        LocalFree(argv);
        return out;
    }
}

std::string currentVersion() { return IRIS_VERSION_STRING; }

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<int> x = versionParts(a), y = versionParts(b);
    const size_t n = std::max(x.size(), y.size());
    for (size_t i = 0; i < n; ++i) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool parseRelease(const std::string& text, Release& out, std::string& error) {
    out = Release{};
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "risposta di GitHub non leggibile";
        return false;
    }
    if (j.value("draft", false) || j.value("prerelease", false)) {
        error = "l'ultima release è una bozza o una prova";
        return false;
    }
    out.tag = j.value("tag_name", "");
    out.name = j.value("name", "");
    out.notes = j.contains("body") && j["body"].is_string() ? j["body"].get<std::string>() : std::string();
    out.pageUrl = j.value("html_url", "");
    size_t first = 0;
    while (first < out.tag.size() && !std::isdigit((unsigned char)out.tag[first])) ++first;
    out.version = out.tag.substr(first);
    if (out.version.empty()) {
        error = "la release non ha un numero di versione (" + out.tag + ")";
        return false;
    }
    if (!j.contains("assets") || !j["assets"].is_array()) {
        error = "la release non ha file allegati";
        return false;
    }
    const nlohmann::json* chosen = nullptr;
    for (const auto& a : j["assets"]) {
        if (!a.is_object()) continue;
        const std::string name = TextUtils::toLower(a.value("name", ""));
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") != 0) continue;
        if (!chosen || name == "iris.exe") chosen = &a; // iris.exe se c'è, se no il primo .exe
    }
    if (!chosen) {
        error = "la release " + out.tag + " non ha un .exe allegato";
        return false;
    }
    out.assetUrl = chosen->value("browser_download_url", "");
    out.size = chosen->value("size", 0LL);
    const std::string digest = chosen->contains("digest") && (*chosen)["digest"].is_string() ? (*chosen)["digest"].get<std::string>() : "";
    if (digest.rfind("sha256:", 0) == 0) out.sha256 = TextUtils::toLower(digest.substr(7));
    if (out.assetUrl.rfind("https://", 0) != 0) {
        error = "indirizzo del file non valido";
        return false;
    }
    return true;
}

Failure describeFailure(int status, const std::string& networkError) {
    if (!networkError.empty() || status <= 0) {
        return { "niente connessione a GitHub" + (networkError.empty() ? std::string() : " (" + networkError + ")"), 30 };
    }
    if (status == 403 || status == 429) {
        // Senza account GitHub concede 60 controlli l'ora per rete: lo si lascia in pace un'ora.
        return { "GitHub ha limitato i controlli da questa rete (troppe richieste, errore " + std::to_string(status) + ")", 60 };
    }
    if (status == 404) return { "nessuna versione pubblicata", 240 };
    if (status >= 500) return { "GitHub non risponde bene in questo momento (errore " + std::to_string(status) + ")", 30 };
    return { "GitHub ha risposto con un errore inatteso (" + std::to_string(status) + ")", 120 };
}

int retryDelayMinutes(int baseMinutes, int failures) {
    long long minutes = std::max(1, baseMinutes);
    for (int i = 1; i < failures && minutes < 240; ++i) minutes *= 2;
    return (int)std::min(240LL, minutes);
}

bool fetchLatest(Release& out, std::string& error, const std::atomic<bool>* cancel, int* retryMinutes) {
    Http::Options o;
    o.timeoutMs = 15000;
    o.cancel = cancel;
    o.headers = { L"Accept: application/vnd.github+json", L"X-GitHub-Api-Version: 2022-11-28" };
    const Http::Response r = Http::get(std::string("https://api.github.com/repos/") + kRepository + "/releases/latest", o);
    if (!r.ok()) {
        const Failure f = describeFailure(r.error.empty() ? r.status : 0, r.error);
        error = f.text;
        if (retryMinutes) *retryMinutes = f.retryMinutes;
        return false;
    }
    if (retryMinutes) *retryMinutes = 240;
    return parseRelease(r.body, out, error);
}

std::wstring updatesDir() { return AppPaths::getAppDataDir() + L"\\aggiornamenti"; }

std::wstring downloadedPath(const std::string& version) { return updatesDir() + L"\\Iris-" + TextUtils::utf8ToWide(version) + L".exe"; }

bool sha256File(const std::wstring& path, std::string& hex) {
    hex.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buf(1 << 20);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), read, 0));
    }
    unsigned char digest[32] = {};
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    char part[3];
    for (unsigned char c : digest) {
        std::snprintf(part, sizeof(part), "%02x", c);
        hex += part;
    }
    return true;
}

bool download(const Release& release, const std::function<void(long long, long long)>& onProgress, const std::atomic<bool>* cancel,
              std::wstring& path, std::string& error) {
    CreateDirectoryW(AppPaths::getAppDataDir().c_str(), nullptr);
    CreateDirectoryW(updatesDir().c_str(), nullptr);
    path = downloadedPath(release.version);
    const std::wstring okFile = path + L".ok";
    std::string hash;

    // Già scaricato e controllato?
    if (fileExists(path) && fileExists(okFile) && sha256File(path, hash) && hash == readSmallFile(okFile) &&
        (release.sha256.empty() || hash == release.sha256)) {
        return true;
    }
    DeleteFileW(path.c_str());
    DeleteFileW(okFile.c_str());

    if (!ModelDownloader::downloadFile(TextUtils::utf8ToWide(release.assetUrl), path, onProgress, cancel, error)) {
        if (error.empty()) error = "download non riuscito";
        return false;
    }
    auto reject = [&](const std::string& why) {
        DeleteFileW(path.c_str());
        error = why;
        Log::warn("Aggiornamento " + release.version + " scartato: " + why);
        return false;
    };
    if (release.size > 0 && fileSize(path) != release.size) return reject("il file scaricato non ha la dimensione giusta");
    if (!looksLikeExe(path)) return reject("il file scaricato non è un programma");
    if (!sha256File(path, hash)) return reject("impossibile controllare il file scaricato");
    if (!release.sha256.empty() && hash != release.sha256) return reject("l'impronta SHA-256 non corrisponde (file rovinato?)");
    std::ofstream(okFile, std::ios::binary | std::ios::trunc) << hash;
    Log::info("Aggiornamento " + release.version + " scaricato e controllato (SHA-256 " + hash + ")");
    return true;
}

std::wstring pendingUpdate(std::string* version) {
    std::wstring best;
    std::string bestVersion = currentVersion();
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((updatesDir() + L"\\Iris-*.exe").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return {};
    do {
        const std::string v = versionFromFileName(fd.cFileName);
        if (v.empty() || compareVersions(v, bestVersion) <= 0 || isBroken(v)) continue;
        const std::wstring path = updatesDir() + L"\\" + fd.cFileName;
        std::string hash;
        // Solo un file controllato, e non toccato dopo il controllo.
        if (!fileExists(path + L".ok") || !sha256File(path, hash) || hash != readSmallFile(path + L".ok")) continue;
        best = path;
        bestVersion = v;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (version && !best.empty()) *version = bestVersion;
    return best;
}

bool install(const std::wstring& newExe, std::string& error) {
    const std::wstring exe = exePath();
    if (exe.empty() || !fileExists(newExe)) {
        error = "file dell'aggiornamento non trovato";
        return false;
    }
    const std::string protectedFolder = "Windows non mi lascia sostituire il programma in " + TextUtils::wideToUtf8(folderOf(exe)) +
                                        " (cartella protetta? spostalo per esempio sul Desktop)";
    // 1. La versione nuova accanto a quella in uso (stesso disco: poi basta un cambio di nome, che non
    //    si interrompe a metà), mandata davvero sul disco e controllata: uguale a quella scaricata.
    const std::wstring staged = exe + L".new";
    DeleteFileW(staged.c_str());
    if (!CopyFileW(newExe.c_str(), staged.c_str(), FALSE)) {
        error = protectedFolder;
        return false;
    }
    {
        HANDLE f = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(f);
            CloseHandle(f);
        }
    }
    std::string expected, copied;
    if (!sha256File(newExe, expected) || !sha256File(staged, copied) || expected != copied || !looksLikeExe(staged)) {
        DeleteFileW(staged.c_str());
        error = "la copia della versione nuova non è uguale a quella scaricata (disco pieno?)";
        return false;
    }
    // 2. Un programma in esecuzione non si può sovrascrivere, ma si può rinominare.
    std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());
    if (fileExists(old)) old = exe + L"." + std::to_wstring(GetTickCount64()) + L".old"; // quello di prima è ancora in uso
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(staged.c_str());
        error = protectedFolder;
        return false;
    }
    // 3. La nuova al suo posto: un cambio di nome, tutto o niente.
    if (!MoveFileExW(staged.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(staged.c_str());
        error = "impossibile mettere la versione nuova nella cartella del programma";
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" --dopo-aggiornamento" + forwardedArguments();
    const std::wstring dir = folderOf(exe);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        // Si torna com'era: meglio la versione vecchia che niente.
        DeleteFileW(exe.c_str());
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        error = "la versione nuova non si avvia (errore " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log::info("Aggiornamento installato: avvio la versione nuova (" + TextUtils::wideToUtf8(newExe) + ")");
    return true;
}

void cleanup() {
    const std::wstring exe = exePath();
    if (!exe.empty()) {
        // L'exe di prima, rinominato dall'aggiornamento.
        const std::wstring dir = folderOf(exe);
        const std::wstring name = exe.substr(dir.size() + 1);
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((exe + L"*.old").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring file = fd.cFileName;
                if (file.size() > name.size() && file.compare(0, name.size(), name) == 0) DeleteFileW((dir + L"\\" + file).c_str());
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        DeleteFileW((exe + L".new").c_str()); // una copia rimasta a metà (installazione interrotta)
    }
    // Gli scaricati di versioni già installate (o più vecchie).
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((updatesDir() + L"\\Iris-*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        std::wstring base = file;
        for (const wchar_t* ext : { L".ok", L".part" }) {
            const size_t n = wcslen(ext);
            if (base.size() > n && base.compare(base.size() - n, n, ext) == 0) base.resize(base.size() - n);
        }
        const std::string v = versionFromFileName(base);
        if (!v.empty() && compareVersions(v, currentVersion()) <= 0) DeleteFileW((updatesDir() + L"\\" + file).c_str());
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

namespace {
    std::wstring brokenListPath() { return updatesDir() + L"\\scartate.txt"; }
}

bool isBroken(const std::string& version) {
    std::ifstream f(brokenListPath(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (compareVersions(TextUtils::trim(line), version) == 0 && !TextUtils::trim(line).empty()) return true;
    }
    return false;
}

void markBroken(const std::string& version) {
    if (version.empty() || isBroken(version)) return;
    CreateDirectoryW(updatesDir().c_str(), nullptr);
    std::ofstream(brokenListPath(), std::ios::binary | std::ios::app) << version << "\r\n";
    const std::wstring path = downloadedPath(version);
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".ok").c_str());
}

void confirmInstalled() {
    std::string version;
    if (pendingUpdate(&version).empty()) return; // installata quella giusta
    Log::warn("Aggiornamento: dopo l'installazione questa è ancora la " + currentVersion() + ", non la " + version +
              ": l'exe pubblicato non è quella versione. Non lo installo più (aspetto la prossima).");
    markBroken(version);
}

} // namespace Updater

// --- Controllo in sottofondo ---------------------------------------------------------

namespace {
    constexpr auto kFirstCheck = std::chrono::seconds(20);
    constexpr auto kInterval = std::chrono::hours(4);

    std::string nowHHMM() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
        return buf;
    }

    // L'ora tra "minutes" minuti ("22:13").
    std::string clockIn(int minutes) {
        const time_t later = time(nullptr) + (time_t)minutes * 60;
        tm local{};
        localtime_s(&local, &later);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
        return buf;
    }
}

void UpdateManager::start(std::function<bool()> enabled, std::function<void()> notify) {
    if (m_thread.joinable()) return;
    m_enabled = std::move(enabled);
    m_notify = std::move(notify);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.current = Updater::currentVersion();
        // Scaricato ma non installato (es. all'avvio la cartella non era scrivibile).
        std::string version;
        const std::wstring pending = Updater::pendingUpdate(&version);
        if (!pending.empty()) {
            m_readyPath = pending;
            m_state.phase = Phase::Ready;
            m_state.latest = version;
        }
    }
    m_stopping = false;
    m_thread = std::thread([this]() { loop(); });
}

void UpdateManager::stop() {
    m_stopping = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void UpdateManager::checkNow() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
        m_manual = true;
    }
    m_cv.notify_all();
}

UpdateManager::State UpdateManager::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void UpdateManager::set(const std::function<void(State&)>& change) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        change(m_state);
    }
    if (m_notify) m_notify();
}

void UpdateManager::loop() {
    auto next = std::chrono::steady_clock::now() + kFirstCheck;
    while (!m_stopping) {
        bool manual = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_until(lock, next, [&]() { return m_wake || m_stopping.load(); });
            if (m_stopping) break;
            manual = m_manual;
            m_wake = m_manual = false;
        }
        // Dopo un errore si riprova prima di 4 ore, ma sempre più piano: niente raffiche su GitHub.
        const int minutes = runCheck(manual);
        next = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
    }
}

int UpdateManager::runCheck(bool manual) {
    constexpr int kNormal = (int)std::chrono::duration_cast<std::chrono::minutes>(kInterval).count();
    if (!manual && m_enabled && !m_enabled()) {
        set([](State& s) {
            if (s.phase != Phase::Ready) s.phase = Phase::Disabled;
        });
        return kNormal;
    }
    set([](State& s) {
        s.phase = Phase::Checking;
        s.error.clear();
        s.retryAt.clear();
    });
    // Un errore in più: il prossimo tentativo più in là (30 min, 1 ora, 2, poi ogni 4).
    auto failed = [&](const std::string& error, int baseMinutes, bool keepReady) {
        const int minutes = Updater::retryDelayMinutes(baseMinutes, ++m_failures);
        const std::string retryAt = clockIn(minutes);
        Log::warn("Aggiornamento: controllo non riuscito: " + error + " (riprovo alle " + retryAt + ")");
        set([&](State& s) {
            s.phase = keepReady && !m_readyPath.empty() ? Phase::Ready : Phase::Error;
            s.error = error;
            s.retryAt = retryAt;
            s.checkedAt = nowHHMM();
        });
        return minutes;
    };
    Updater::Release release;
    std::string error;
    int retryMinutes = kNormal;
    if (!Updater::fetchLatest(release, error, &m_stopping, &retryMinutes)) {
        if (m_stopping) return kNormal;
        return failed(error, retryMinutes, true);
    }
    m_failures = 0;
    if (Updater::compareVersions(release.version, Updater::currentVersion()) <= 0) {
        set([&](State& s) {
            s.phase = Phase::UpToDate;
            s.latest = release.version;
            s.notes = release.notes;
            s.pageUrl = release.pageUrl;
            s.checkedAt = nowHHMM();
        });
        return kNormal;
    }
    if (Updater::isBroken(release.version)) {
        // Già scaricata e installata una volta: partiva come un'altra versione. Si aspetta la prossima.
        set([&](State& s) {
            s.phase = Phase::Error;
            s.latest = release.version;
            s.notes = release.notes;
            s.pageUrl = release.pageUrl;
            s.error = "la versione " + release.version + " pubblicata su GitHub non si installa bene: aspetto la prossima";
            s.checkedAt = nowHHMM();
        });
        return kNormal;
    }
    Log::info("Aggiornamento: c'è la versione " + release.version + " (questa è " + Updater::currentVersion() + ")");
    set([&](State& s) {
        s.phase = Phase::Downloading;
        s.latest = release.version;
        s.notes = release.notes;
        s.pageUrl = release.pageUrl;
        s.progress = 0;
    });
    int lastPercent = -1;
    std::wstring path;
    const bool ok = Updater::download(
        release,
        [&](long long done, long long total) {
            const int percent = total > 0 ? (int)(done * 100 / total) : 0;
            if (percent == lastPercent) return;
            lastPercent = percent;
            set([&](State& s) { s.progress = percent; });
        },
        &m_stopping, path, error);
    if (m_stopping) return kNormal;
    if (!ok) return failed("download non riuscito: " + error, 30, false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readyPath = path;
    }
    set([&](State& s) {
        s.phase = Phase::Ready;
        s.progress = 100;
        s.checkedAt = nowHHMM();
    });
    return kNormal;
}

bool UpdateManager::installNow(std::string& error) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = m_readyPath;
    }
    if (path.empty()) {
        error = "nessun aggiornamento pronto";
        return false;
    }
    if (!Updater::install(path, error)) {
        set([&](State& s) { s.error = error; });
        return false;
    }
    return true;
}
