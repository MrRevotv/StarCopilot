#include "model_downloader.h"
#include "log.h"
#include "text_utils.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
    long long fileSize(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return -1;
        return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }

    struct Handles {
        HINTERNET session = nullptr, connect = nullptr, request = nullptr;
        ~Handles() {
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            if (session) WinHttpCloseHandle(session);
        }
    };

    enum class Attempt { Done, Retry, Fail };

    // Un tentativo: riprende dal byte "offset" del .part.
    Attempt attempt(const std::wstring& url, const std::wstring& partPath, long long& total,
                    const ModelDownloader::ProgressCallback& onProgress, const std::atomic<bool>* cancel, std::string& error) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[4096] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = _countof(host);
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = _countof(path);
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) { error = "indirizzo non valido"; return Attempt::Fail; }

        Handles h;
        h.session = WinHttpOpen(L"Iris/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h.session) { error = "rete non disponibile"; return Attempt::Retry; }
        WinHttpSetTimeouts(h.session, 15000, 15000, 30000, 60000);
        h.connect = WinHttpConnect(h.session, host, uc.nPort, 0);
        if (!h.connect) { error = "server non raggiungibile"; return Attempt::Retry; }
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        h.request = WinHttpOpenRequest(h.connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!h.request) { error = "richiesta non creata"; return Attempt::Retry; }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(h.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

        long long offset = std::max(0LL, fileSize(partPath));
        std::wstring headers;
        if (offset > 0) headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";

        if (!WinHttpSendRequest(h.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(h.request, nullptr)) {
            error = "connessione interrotta (" + std::to_string(GetLastError()) + ")";
            return Attempt::Retry;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 416 && offset > 0) {
            // Il .part è già completo (o più lungo del file): si ricontrolla da capo.
            DeleteFileW(partPath.c_str());
            error = "file parziale non valido, ricomincio";
            return Attempt::Retry;
        }
        if (status != 200 && status != 206) {
            error = "il server ha risposto " + std::to_string(status);
            return status >= 500 ? Attempt::Retry : Attempt::Fail;
        }
        if (status == 200 && offset > 0) {
            // Il server non accetta di riprendere: si ricomincia.
            offset = 0;
        }

        long long contentLength = -1;
        wchar_t lenBuf[32] = {};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX)) {
            contentLength = _wtoi64(lenBuf);
        }
        if (contentLength > 0) total = offset + contentLength;

        FILE* f = nullptr;
        if (_wfopen_s(&f, partPath.c_str(), offset > 0 ? L"ab" : L"wb") != 0 || !f) {
            error = "impossibile scrivere sul disco";
            return Attempt::Fail;
        }
        std::vector<char> buffer(1 << 18);
        long long downloaded = offset;
        DWORD available = 0;
        bool ok = true;
        while (true) {
            if (cancel && cancel->load()) { ok = false; error = "annullato"; break; }
            if (!WinHttpQueryDataAvailable(h.request, &available)) { ok = false; error = "connessione interrotta"; break; }
            if (available == 0) break;
            DWORD toRead = std::min<DWORD>(available, (DWORD)buffer.size()), read = 0;
            if (!WinHttpReadData(h.request, buffer.data(), toRead, &read) || read == 0) { ok = false; error = "connessione interrotta"; break; }
            if (fwrite(buffer.data(), 1, read, f) != read) { ok = false; error = "disco pieno?"; fclose(f); return Attempt::Fail; }
            downloaded += read;
            if (onProgress) onProgress(downloaded, total);
        }
        fclose(f);
        if (cancel && cancel->load()) return Attempt::Fail;
        if (!ok) return Attempt::Retry;
        if (total > 0 && downloaded != total) {
            error = "scaricati " + std::to_string(downloaded) + " byte su " + std::to_string(total);
            return Attempt::Retry;
        }
        return Attempt::Done;
    }
}

namespace ModelDownloader {

bool downloadFile(const std::wstring& url, const std::wstring& destPath, const ProgressCallback& onProgress,
                  const std::atomic<bool>* cancel, std::string& error) {
    const std::wstring partPath = destPath + L".part";
    long long total = -1;
    for (int tryNo = 1; tryNo <= 6; ++tryNo) {
        Attempt a = attempt(url, partPath, total, onProgress, cancel, error);
        if (a == Attempt::Done) {
            if (!MoveFileExW(partPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                error = "impossibile rinominare il file scaricato";
                return false;
            }
            return true;
        }
        Log::warn("Download: tentativo " + std::to_string(tryNo) + " non riuscito: " + error);
        if (a == Attempt::Fail) return false;
        // Attese crescenti: 2, 4, 8... secondi.
        for (int i = 0; i < (1 << tryNo) * 10; ++i) {
            if (cancel && cancel->load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return false;
}

bool looksValid(const std::wstring& path, Kind kind) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    unsigned char magic[4] = {};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n != 4) return false;
    if (kind == Kind::Gguf) return magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
    // whisper: "ggml" scritto come intero little-endian 0x67676d6c -> byte "lmgg".
    return magic[0] == 'l' && magic[1] == 'm' && magic[2] == 'g' && magic[3] == 'g';
}

} // namespace ModelDownloader
