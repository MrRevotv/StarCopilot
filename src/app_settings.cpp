#include "app_settings.h"
#include "app_paths.h"
#include "languages.h"
#include "text_utils.h"
#include "voice_effect.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
    int toInt(const std::string& s, int fallback) {
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        return (end && end != s.c_str()) ? (int)v : fallback;
    }
    double toDouble(const std::string& s, double fallback) {
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        return (end && end != s.c_str()) ? v : fallback;
    }
    bool toBool(const std::string& s) { return s == "1" || s == "true"; }

    // "a,b,resto con, virgole" -> parti (l'ultima prende tutto il resto).
    std::vector<std::string> splitN(const std::string& s, size_t n) {
        std::vector<std::string> parts;
        size_t pos = 0;
        while (parts.size() + 1 < n) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) break;
            parts.push_back(s.substr(pos, comma - pos));
            pos = comma + 1;
        }
        parts.push_back(s.substr(pos));
        return parts;
    }

    // Una riga sola: a capo e "=" non devono rompere il file.
    std::string oneLine(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            if (c == '\r' || c == '\n') c = ' ';
        }
        return TextUtils::trim(out);
    }
}

AppSettings AppSettings::load() {
    std::ifstream file(AppPaths::settingsPath(), std::ios::binary);
    AppSettings s;
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        s = parse(buffer.str());
    }
    s.sanitize();
    return s;
}

AppSettings AppSettings::parse(const std::string& content) {
    AppSettings s;
    std::istringstream file(content);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (TextUtils::startsWith(line, "\xEF\xBB\xBF")) line.erase(0, 3);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);

        if (key == "userName") s.userName = v;
        else if (key == "homeCity") s.homeCity = v;
        else if (key == "wakeWordEnabled") s.wakeWordEnabled = toBool(v);
        else if (key == "deviceName") { if (!v.empty()) s.deviceNames.push_back(v); }
        else if (key == "micGain") s.micGain = (float)toDouble(v, 1.0);
        else if (key == "vadThreshold") s.vadThreshold = toDouble(v, 0.5);
        else if (key == "endSilenceMs") s.endSilenceMs = toInt(v, 700);
        else if (key == "followUpSeconds") s.followUpSeconds = toInt(v, 6);
        else if (key == "replyStyle") s.replyStyle = v;
        else if (key == "conversationTimeoutMin") s.conversationTimeoutMin = toInt(v, 10);
        else if (key == "speechEnabled") s.speechEnabled = toBool(v);
        else if (key == "voiceEngine") s.voiceEngine = v;
        else if (key == "voiceName") s.voiceName = v;
        else if (key == "voiceEffect") s.voiceEffect = v;
        else if (key == "outputDeviceName") s.outputDeviceName = v;
        else if (key == "speechRate") s.speechRate = toDouble(v, 1.1);
        else if (key == "llmModelFile") s.llmModelFile = v;
        else if (key == "gpuMemoryLimitMb") s.gpuMemoryLimitMb = toInt(v, -1);
        else if (key == "internetEnabled") s.internetEnabled = toBool(v);
        else if (key == "gameEnabled") s.gameEnabled = toBool(v);
        else if (key == "overlayEnabled") s.overlayEnabled = toBool(v);
        else if (key == "overlayOpacity") s.overlayOpacity = toInt(v, 230);
        else if (key == "overlaySize") s.overlaySize = toInt(v, 160);
        else if (key == "overlayX") s.overlayX = toInt(v, -1);
        else if (key == "overlayY") s.overlayY = toInt(v, -1);
        else if (key == "windowX") s.windowX = toInt(v, -1);
        else if (key == "windowY") s.windowY = toInt(v, -1);
        // Chat di gioco (stesse chiavi di Voice to Chat)
        else if (key == "chatEnabled") s.chatEnabled = toBool(v);
        else if (key == "sourceLang") s.sourceLanguageIndex = toInt(v, 0);
        else if (key == "targetLang") s.targetLanguageIndex = toInt(v, 1);
        else if (key == "autoSend") s.autoSend = toBool(v);
        else if (key == "gamingTone") s.gamingTone = toBool(v);
        else if (key == "silenceThreshold") s.silenceThreshold = toDouble(v, 0.01);
        else if (key == "chatOpenEnabled") s.chatOpenEnabled = toBool(v);
        else if (key == "chatOpenKeyVk") s.chatOpenKeyVk = toInt(v, 0x0D);
        else if (key == "chatSendKeyVk") s.chatSendKeyVk = toInt(v, 0x0D);
        else if (key == "chatOpenDelayMs") s.chatOpenDelayMs = toInt(v, 180);
        else if (key == "typingDelayMs") s.typingDelayMs = toInt(v, 12);
        else if (key == "unicodeFallback") s.unicodeFallback = toBool(v);
        else if (key == "ocrEnabled") s.ocrEnabled = toBool(v);
        else if (key == "ocrTargetLanguageIndex") s.ocrTargetLanguageIndex = toInt(v, 0);
        else if (key == "chatFilterTag") s.chatFilterTag = v;
        else if (key == "ocrRegionX") s.ocrRegionX = toInt(v, -1);
        else if (key == "ocrRegionY") s.ocrRegionY = toInt(v, -1);
        else if (key == "ocrRegionW") s.ocrRegionW = toInt(v, -1);
        else if (key == "ocrRegionH") s.ocrRegionH = toInt(v, -1);
        else if (key == "ocrOverlayX") s.ocrOverlayX = toInt(v, -1);
        else if (key == "ocrOverlayY") s.ocrOverlayY = toInt(v, -1);
        else if (key == "ocrOverlayOpacity") s.ocrOverlayOpacity = toInt(v, 235);
        else if (key == "ocrMaxMessages") s.ocrMaxMessages = toInt(v, 6);
        else if (key == "voiceToChatImported") s.voiceToChatImported = toBool(v);
        else if (key == "trigger" || key == "chatTrigger") {
            TalkTrigger t;
            if (TextUtils::startsWith(v, "keyboard,")) {
                t.type = TalkTrigger::Type::Keyboard;
                t.vk = toInt(v.substr(9), 0);
                if (t.vk <= 0 || t.vk > 254) continue;
            } else if (TextUtils::startsWith(v, "joystick2,")) {
                // joystick2,<id>,<pulsante>,<nome>
                auto p = splitN(v.substr(10), 3);
                if (p.size() < 2) continue;
                t.type = TalkTrigger::Type::Joystick;
                t.joystickId = p[0];
                t.buttonIndex = toInt(p[1], 0);
                if (p.size() > 2) t.joystickName = p[2];
            } else {
                continue; // "joystick," della versione molto vecchia di Voice to Chat: si rifà a mano
            }
            (key == "trigger" ? s.triggers : s.chatTriggers).push_back(t);
        }
    }
    return s;
}

void AppSettings::importVoiceToChat(const std::string& voiceToChatContent) {
    const AppSettings v = parse(voiceToChatContent); // stesse chiavi: i suoi "trigger" finiscono in v.triggers
    chatTriggers = v.triggers;
    sourceLanguageIndex = v.sourceLanguageIndex;
    targetLanguageIndex = v.targetLanguageIndex;
    autoSend = v.autoSend;
    gamingTone = v.gamingTone;
    silenceThreshold = v.silenceThreshold;
    chatOpenEnabled = v.chatOpenEnabled;
    chatOpenKeyVk = v.chatOpenKeyVk;
    chatSendKeyVk = v.chatSendKeyVk;
    chatOpenDelayMs = v.chatOpenDelayMs;
    typingDelayMs = v.typingDelayMs;
    unicodeFallback = v.unicodeFallback;
    ocrEnabled = v.ocrEnabled;
    ocrTargetLanguageIndex = v.ocrTargetLanguageIndex;
    chatFilterTag = v.chatFilterTag;
    ocrRegionX = v.ocrRegionX;
    ocrRegionY = v.ocrRegionY;
    ocrRegionW = v.ocrRegionW;
    ocrRegionH = v.ocrRegionH;
    ocrOverlayX = v.ocrOverlayX;
    ocrOverlayY = v.ocrOverlayY;
    ocrOverlayOpacity = v.ocrOverlayOpacity;
    ocrMaxMessages = v.ocrMaxMessages;
    voiceToChatImported = true;
    sanitize();
}

std::string AppSettings::serialize() const {
    std::ostringstream o;
    o << "userName=" << oneLine(userName) << "\n";
    o << "homeCity=" << oneLine(homeCity) << "\n";
    o << "wakeWordEnabled=" << (wakeWordEnabled ? 1 : 0) << "\n";
    for (const auto& d : deviceNames) o << "deviceName=" << oneLine(d) << "\n";
    o << "micGain=" << micGain << "\n";
    o << "vadThreshold=" << vadThreshold << "\n";
    o << "endSilenceMs=" << endSilenceMs << "\n";
    o << "followUpSeconds=" << followUpSeconds << "\n";
    o << "replyStyle=" << oneLine(replyStyle) << "\n";
    o << "conversationTimeoutMin=" << conversationTimeoutMin << "\n";
    o << "speechEnabled=" << (speechEnabled ? 1 : 0) << "\n";
    o << "voiceEngine=" << oneLine(voiceEngine) << "\n";
    o << "voiceName=" << oneLine(voiceName) << "\n";
    o << "voiceEffect=" << oneLine(voiceEffect) << "\n";
    o << "outputDeviceName=" << oneLine(outputDeviceName) << "\n";
    o << "speechRate=" << speechRate << "\n";
    o << "llmModelFile=" << oneLine(llmModelFile) << "\n";
    o << "gpuMemoryLimitMb=" << gpuMemoryLimitMb << "\n";
    o << "internetEnabled=" << (internetEnabled ? 1 : 0) << "\n";
    o << "gameEnabled=" << (gameEnabled ? 1 : 0) << "\n";
    o << "overlayEnabled=" << (overlayEnabled ? 1 : 0) << "\n";
    o << "overlayOpacity=" << overlayOpacity << "\n";
    o << "overlaySize=" << overlaySize << "\n";
    o << "overlayX=" << overlayX << "\n";
    o << "overlayY=" << overlayY << "\n";
    o << "windowX=" << windowX << "\n";
    o << "windowY=" << windowY << "\n";
    auto writeTriggers = [&](const char* key, const std::vector<TalkTrigger>& list) {
        for (const auto& t : list) {
            if (t.type == TalkTrigger::Type::Keyboard) {
                o << key << "=keyboard," << t.vk << "\n";
            } else if (!t.joystickId.empty()) {
                o << key << "=joystick2," << t.joystickId << "," << t.buttonIndex << "," << oneLine(t.joystickName) << "\n";
            }
        }
    };
    writeTriggers("trigger", triggers);
    // Chat di gioco
    o << "chatEnabled=" << (chatEnabled ? 1 : 0) << "\n";
    writeTriggers("chatTrigger", chatTriggers);
    o << "sourceLang=" << sourceLanguageIndex << "\n";
    o << "targetLang=" << targetLanguageIndex << "\n";
    o << "autoSend=" << (autoSend ? 1 : 0) << "\n";
    o << "gamingTone=" << (gamingTone ? 1 : 0) << "\n";
    o << "silenceThreshold=" << silenceThreshold << "\n";
    o << "chatOpenEnabled=" << (chatOpenEnabled ? 1 : 0) << "\n";
    o << "chatOpenKeyVk=" << chatOpenKeyVk << "\n";
    o << "chatSendKeyVk=" << chatSendKeyVk << "\n";
    o << "chatOpenDelayMs=" << chatOpenDelayMs << "\n";
    o << "typingDelayMs=" << typingDelayMs << "\n";
    o << "unicodeFallback=" << (unicodeFallback ? 1 : 0) << "\n";
    o << "ocrEnabled=" << (ocrEnabled ? 1 : 0) << "\n";
    o << "ocrTargetLanguageIndex=" << ocrTargetLanguageIndex << "\n";
    o << "chatFilterTag=" << oneLine(chatFilterTag) << "\n";
    o << "ocrRegionX=" << ocrRegionX << "\n";
    o << "ocrRegionY=" << ocrRegionY << "\n";
    o << "ocrRegionW=" << ocrRegionW << "\n";
    o << "ocrRegionH=" << ocrRegionH << "\n";
    o << "ocrOverlayX=" << ocrOverlayX << "\n";
    o << "ocrOverlayY=" << ocrOverlayY << "\n";
    o << "ocrOverlayOpacity=" << ocrOverlayOpacity << "\n";
    o << "ocrMaxMessages=" << ocrMaxMessages << "\n";
    o << "voiceToChatImported=" << (voiceToChatImported ? 1 : 0) << "\n";
    return o.str();
}

void AppSettings::sanitize() {
    micGain = std::clamp(micGain, 0.2f, 8.0f);
    vadThreshold = std::clamp(vadThreshold, 0.1, 0.95);
    endSilenceMs = std::clamp(endSilenceMs, 300, 2500);
    followUpSeconds = std::clamp(followUpSeconds, 0, 30);
    if (replyStyle != "intere" && replyStyle != "gioco" && replyStyle != "brevi") replyStyle = "gioco";
    conversationTimeoutMin = std::clamp(conversationTimeoutMin, 1, 240);
    speechRate = std::clamp(speechRate, 0.5, 2.0);
    if (gpuMemoryLimitMb < -1) gpuMemoryLimitMb = -1;
    overlayOpacity = std::clamp(overlayOpacity, 60, 255);
    overlaySize = std::clamp(overlaySize, 80, 480);
    if (voiceName.empty()) voiceName = "Elsa";
    if (voiceEngine != "neurale" && voiceEngine != "windows") voiceEngine = "neurale";
    if (!VoiceEffect::isValid(voiceEffect)) voiceEffect = "nessuno";
    // Chat di gioco (come in Voice to Chat)
    sourceLanguageIndex = clampLanguageIndex(sourceLanguageIndex);
    targetLanguageIndex = clampLanguageIndex(targetLanguageIndex);
    ocrTargetLanguageIndex = clampLanguageIndex(ocrTargetLanguageIndex);
    silenceThreshold = std::clamp(silenceThreshold, 0.0005, 0.2);
    chatOpenDelayMs = std::clamp(chatOpenDelayMs, 0, 2000);
    typingDelayMs = std::clamp(typingDelayMs, 0, 200);
    ocrOverlayOpacity = std::clamp(ocrOverlayOpacity, 60, 255);
    ocrMaxMessages = std::clamp(ocrMaxMessages, 1, 20);
    if (chatOpenKeyVk <= 0 || chatOpenKeyVk > 254) chatOpenKeyVk = 0x0D;
    if (chatSendKeyVk <= 0 || chatSendKeyVk > 254) chatSendKeyVk = 0x0D;
}

bool AppSettings::save() const {
    AppPaths::ensureDirectoriesExist();
    const std::string content = serialize();
    // Prima in un file a parte, poi al posto di quello vero: se il PC si
    // spegne a metà, le impostazioni di prima restano buone.
    const std::wstring path = AppPaths::settingsPath();
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << content;
        if (!f.good()) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
