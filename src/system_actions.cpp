#include "system_actions.h"
#include "log.h"
#include "text_utils.h"
#include "wake_word.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>

namespace SystemActions {

namespace {
    std::mutex g_appsMutex;
    std::vector<App> g_apps;
    std::chrono::steady_clock::time_point g_appsTime;

    template <typename T>
    struct ComPtr {
        T* p = nullptr;
        ~ComPtr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T* operator->() { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    // Parole che si dicono ma non fanno parte del nome ("apri il programma Spotify").
    std::string cleanQuery(const std::string& query) {
        static const char* const skip[] = { "il", "lo", "la", "l", "i", "gli", "le", "un", "una", "programma", "applicazione", "app",
                                            "gioco", "di", "del", "per", "favore", "apri", "avvia", "lancia", "the" };
        std::string out;
        std::string n = WakeWord::normalize(query);
        size_t pos = 0;
        while (pos <= n.size()) {
            size_t sp = n.find(' ', pos);
            std::string w = n.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            if (!w.empty() && std::none_of(std::begin(skip), std::end(skip), [&](const char* s) { return w == s; })) {
                if (!out.empty()) out += ' ';
                out += w;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        return out.empty() ? n : out;
    }

    bool shellOpen(const std::wstring& target) {
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) != FALSE;
    }

    bool withVolume(const std::function<bool(IAudioEndpointVolume*)>& fn) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) return false;
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return false;
        ComPtr<IAudioEndpointVolume> volume;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&volume))) return false;
        return fn(volume.p);
    }

    void pressKey(WORD vk) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = vk;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

std::vector<App> listApps() {
    std::lock_guard<std::mutex> lock(g_appsMutex);
    const auto now = std::chrono::steady_clock::now();
    if (!g_apps.empty() && now - g_appsTime < std::chrono::minutes(5)) return g_apps;

    std::vector<App> apps;
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) {
        ComPtr<IEnumShellItems> items;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
            IShellItem* item = nullptr;
            while (items->Next(1, &item, nullptr) == S_OK) {
                PWSTR name = nullptr, parsing = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing))) {
                    apps.push_back({ TextUtils::wideToUtf8(name), std::wstring(L"shell:AppsFolder\\") + parsing });
                }
                CoTaskMemFree(name);
                CoTaskMemFree(parsing);
                item->Release();
            }
        }
    }
    if (apps.empty()) Log::warn("Programmi: elenco del menu Start vuoto");
    g_apps = apps;
    g_appsTime = now;
    return apps;
}

int bestAppMatch(const std::vector<std::string>& names, const std::string& query, double* scoreOut) {
    const std::string q = cleanQuery(query);
    if (q.empty()) return -1;
    int best = -1;
    double bestScore = 0.0;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string n = WakeWord::normalize(names[i]);
        if (n.empty()) continue;
        double s = TextUtils::similarity(n, q);
        if (n == q) s = 1.0;
        else if (TextUtils::startsWith(n, q + " ") || TextUtils::startsWith(n, q)) s = std::max(s, 0.92 - 0.002 * (double)(n.size() - q.size()));
        else if (TextUtils::containsWord(n, q)) s = std::max(s, 0.85 - 0.002 * (double)(n.size() - q.size()));
        else {
            // "word" -> "Microsoft Word": ogni parola detta, confrontata con ogni parola del nome.
            double wordBest = 0.0;
            size_t pos = 0;
            while (pos <= n.size()) {
                size_t sp = n.find(' ', pos);
                std::string w = n.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                if (w.size() >= 3) wordBest = std::max(wordBest, TextUtils::similarity(w, q) - 0.08);
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            s = std::max(s, wordBest);
        }
        // Disinstallazioni e guide non sono mai quello che si vuole aprire.
        if (n.find("uninstall") != std::string::npos || n.find("disinstalla") != std::string::npos) s -= 0.3;
        if (s > bestScore) {
            bestScore = s;
            best = (int)i;
        }
    }
    if (scoreOut) *scoreOut = bestScore;
    return bestScore >= 0.62 ? best : -1;
}

bool launchApp(const std::string& query, std::string& out) {
    // Qualche nome di sistema che non è nel menu Start.
    const std::string q = cleanQuery(query);
    struct Builtin { const char* name; const wchar_t* target; const char* label; };
    static const Builtin builtins[] = {
        { "impostazioni", L"ms-settings:", "Impostazioni" },
        { "esplora file", L"explorer.exe", "Esplora file" },
        { "esplora risorse", L"explorer.exe", "Esplora file" },
        { "gestione attivita", L"taskmgr.exe", "Gestione attività" },
        { "gestione attività", L"taskmgr.exe", "Gestione attività" },
        { "task manager", L"taskmgr.exe", "Gestione attività" },
        { "cartella download", L"shell:Downloads", "Download" },
        { "download", L"shell:Downloads", "Download" },
        { "documenti", L"shell:Personal", "Documenti" },
        { "desktop", L"shell:Desktop", "Desktop" },
    };
    for (const auto& b : builtins) {
        if (q == b.name) {
            if (!shellOpen(b.target)) { out = "non sono riuscita ad aprire " + std::string(b.label); return false; }
            out = b.label;
            return true;
        }
    }

    std::vector<App> apps = listApps();
    std::vector<std::string> names;
    for (const auto& a : apps) names.push_back(a.name);
    double score = 0.0;
    int index = bestAppMatch(names, query, &score);
    if (index < 0) {
        out = "nessun programma installato si chiama \"" + query + "\"";
        return false;
    }
    Log::info("Apro \"" + apps[index].name + "\" (somiglianza " + std::to_string((int)std::lround(score * 100)) + "%)");
    if (!shellOpen(apps[index].launchPath)) {
        out = "non sono riuscita ad aprire " + apps[index].name;
        return false;
    }
    out = apps[index].name;
    return true;
}

bool openUrl(const std::string& url, std::string& out) {
    std::string u = TextUtils::trim(url);
    if (u.empty()) { out = "indirizzo vuoto"; return false; }
    if (u.find("://") == std::string::npos) {
        // "youtube" -> youtube.com
        if (u.find('.') == std::string::npos) u += ".com";
        u = "https://" + u;
    }
    if (!TextUtils::startsWith(u, "https://") && !TextUtils::startsWith(u, "http://")) {
        out = "posso aprire solo indirizzi web";
        return false;
    }
    if (!shellOpen(TextUtils::utf8ToWide(u))) { out = "il browser non si è aperto"; return false; }
    out = u;
    return true;
}

bool volume(const std::string& action, int level, std::string& out) {
    bool ok = withVolume([&](IAudioEndpointVolume* v) {
        float current = 0.0f;
        v->GetMasterVolumeLevelScalar(&current);
        if (action == "set") v->SetMasterVolumeLevelScalar(std::clamp(level, 0, 100) / 100.0f, nullptr);
        else if (action == "up") v->SetMasterVolumeLevelScalar(std::min(1.0f, current + (level > 0 ? level : 10) / 100.0f), nullptr);
        else if (action == "down") v->SetMasterVolumeLevelScalar(std::max(0.0f, current - (level > 0 ? level : 10) / 100.0f), nullptr);
        else if (action == "mute") v->SetMute(TRUE, nullptr);
        else if (action == "unmute") v->SetMute(FALSE, nullptr);
        else if (action != "get") return false;
        if (action != "mute" && action != "get") v->SetMute(FALSE, nullptr);
        float now = 0.0f;
        BOOL muted = FALSE;
        v->GetMasterVolumeLevelScalar(&now);
        v->GetMute(&muted);
        out = muted ? "audio disattivato" : "volume al " + std::to_string((int)std::lround(now * 100)) + "%";
        return true;
    });
    if (!ok && out.empty()) out = "non riesco a cambiare il volume";
    return ok;
}

bool mediaKey(const std::string& action, std::string& out) {
    WORD vk = 0;
    if (action == "play_pause" || action == "play" || action == "pause") vk = VK_MEDIA_PLAY_PAUSE;
    else if (action == "next") vk = VK_MEDIA_NEXT_TRACK;
    else if (action == "previous") vk = VK_MEDIA_PREV_TRACK;
    else if (action == "stop") vk = VK_MEDIA_STOP;
    if (!vk) { out = "comando musicale sconosciuto"; return false; }
    pressKey(vk);
    out = "fatto";
    return true;
}

} // namespace SystemActions
