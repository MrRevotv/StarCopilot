#include "app_paths.h"
#include "text_utils.h"

#include <windows.h>
#include <shlobj.h>

namespace {
    std::wstring appDataRoot() {
        PWSTR path = nullptr;
        std::wstring result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path) {
            result = path;
        }
        CoTaskMemFree(path);
        return result;
    }

    std::wstring g_dataDirOverride;

    bool isAscii(const std::wstring& s) {
        for (wchar_t c : s) if (c > 127) return false;
        return true;
    }
}

namespace AppPaths {

    std::wstring getAppDataDir() { return g_dataDirOverride.empty() ? appDataRoot() + L"\\Iris" : g_dataDirOverride; }
    std::wstring getModelsDir() { return appDataRoot() + L"\\VoiceToChat\\models"; }

    void setDataDirOverride(const std::wstring& dir) {
        g_dataDirOverride = dir;
        while (!g_dataDirOverride.empty() && (g_dataDirOverride.back() == L'\\' || g_dataDirOverride.back() == L'/')) {
            g_dataDirOverride.pop_back();
        }
    }

    bool dataDirOverridden() { return !g_dataDirOverride.empty(); }

    bool ensureDirectoriesExist() {
        for (const std::wstring& dir : { appDataRoot() + L"\\Iris", getAppDataDir(), appDataRoot() + L"\\VoiceToChat", getModelsDir() }) {
            if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
        return true;
    }

    std::wstring whisperModelPath() { return getModelsDir() + L"\\ggml-large-v3-turbo-q8_0.bin"; }
    std::wstring vadModelPath() { return getModelsDir() + L"\\ggml-silero-v6.2.0.bin"; }
    std::wstring llmModelPath(const std::string& fileName) {
        return getModelsDir() + L"\\" + (fileName.empty() ? std::wstring(L"Qwen2.5-7B-Instruct-Q4_K_M.gguf") : TextUtils::utf8ToWide(fileName));
    }
    std::wstring neuralVoiceModelPath() { return getModelsDir() + L"\\Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf"; }
    std::wstring neuralVoiceMmprojPath() { return getModelsDir() + L"\\mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf"; }
    std::wstring settingsPath() { return getAppDataDir() + L"\\settings.cfg"; }
    std::wstring memoryPath() { return getAppDataDir() + L"\\memoria.txt"; }
    std::wstring logPath() { return getAppDataDir() + L"\\log.txt"; }
    std::wstring glossaryPath() { return getAppDataDir() + L"\\glossary.txt"; }
    std::wstring ocrDebugPath() { return getAppDataDir() + L"\\debug_ocr_chat.txt"; }
    std::wstring voiceToChatSettingsPath() { return appDataRoot() + L"\\VoiceToChat\\settings.cfg"; }
    std::wstring voiceToChatGlossaryPath() { return appDataRoot() + L"\\VoiceToChat\\glossary.txt"; }

    std::string toLibraryPath(const std::wstring& widePath) {
        if (isAscii(widePath)) return TextUtils::wideToUtf8(widePath);
        DWORD len = GetShortPathNameW(widePath.c_str(), nullptr, 0);
        if (len > 0) {
            std::wstring shortPath(len, L'\0');
            DWORD written = GetShortPathNameW(widePath.c_str(), shortPath.data(), len);
            shortPath.resize(written);
            if (written > 0 && isAscii(shortPath)) return TextUtils::wideToUtf8(shortPath);
        }
        return TextUtils::wideToUtf8(widePath);
    }

} // namespace AppPaths
