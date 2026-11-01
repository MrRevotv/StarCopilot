#include "autostart.h"
#include "log.h"

#include <windows.h>

namespace Autostart {

namespace {
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kValue = L"Iris";

    std::wstring exePath() {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        return std::wstring(buf, n);
    }
}

std::wstring commandFor(const std::wstring& exe) { return L"\"" + exe + L"\" " + kArgument; }

bool enabled(std::wstring* command) {
    wchar_t buf[2048] = {};
    DWORD size = sizeof(buf);
    const LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &size);
    if (r != ERROR_SUCCESS) return false;
    if (command) *command = buf;
    return true;
}

bool pointsHere() {
    std::wstring command;
    if (!enabled(&command)) return false;
    return _wcsicmp(command.c_str(), commandFor(exePath()).c_str()) == 0;
}

bool set(bool on, std::string* error) {
    HKEY key = nullptr;
    LSTATUS r = RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) {
        if (error) *error = "il registro di Windows non si apre (errore " + std::to_string(r) + ")";
        return false;
    }
    if (on) {
        const std::wstring command = commandFor(exePath());
        r = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        if (error) *error = "il registro di Windows non accetta la modifica (errore " + std::to_string(r) + ")";
        return false;
    }
    Log::info(on ? "Avvio con Windows: attivato" : "Avvio con Windows: disattivato");
    return true;
}

} // namespace Autostart
