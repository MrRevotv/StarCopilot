#include "log.h"
#include "app_paths.h"

#include <windows.h>
#include <cstdio>
#include <mutex>

namespace {
    std::mutex g_mutex;
    constexpr long long kMaxBytes = 1024 * 1024;

    void write(const char* level, const std::string& message) {
        std::lock_guard<std::mutex> lock(g_mutex);
        AppPaths::ensureDirectoriesExist();
        const std::wstring path = AppPaths::logPath();

        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            if (size > kMaxBytes) {
                const std::wstring old = AppPaths::getAppDataDir() + L"\\log.old.txt";
                MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond, t.wMilliseconds, level, message.c_str());
        fclose(f);
    }
}

namespace Log {
    void info(const std::string& message) { write("INFO", message); }
    void warn(const std::string& message) { write("AVVISO", message); }
    void error(const std::string& message) { write("ERRORE", message); }
}
