#include "http_client.h"
#include "html_utils.h"
#include "text_utils.h"

#include <windows.h>
#include <winhttp.h>

namespace Http {

namespace {
    // Come un browser: alcuni siti rispondono male ai programmi sconosciuti.
    constexpr const wchar_t* kUserAgent =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

    struct Handles {
        HINTERNET session = nullptr, connect = nullptr, request = nullptr;
        ~Handles() {
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            if (session) WinHttpCloseHandle(session);
        }
    };

    std::wstring queryHeader(HINTERNET request, DWORD what) {
        DWORD size = 0;
        WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
        if (size == 0) return {};
        std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
        value.resize(size / sizeof(wchar_t));
        return value;
    }

    Response send(const std::wstring& method, const std::string& url, const std::string& body, const std::string& contentType,
                  const Options& options) {
        Response r;
        const std::wstring wurl = TextUtils::utf8ToWide(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[8192] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = _countof(host);
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = _countof(path);
        wchar_t extra[8192] = {};
        uc.lpszExtraInfo = extra;
        uc.dwExtraInfoLength = _countof(extra);
        if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
            r.error = "indirizzo non valido";
            return r;
        }
        const std::wstring fullPath = std::wstring(path) + extra;

        Handles h;
        h.session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h.session) { r.error = "rete non disponibile"; return r; }
        const int t = options.timeoutMs;
        WinHttpSetTimeouts(h.session, t, t, t, t);
        // Pagine compresse: solo gzip. Alcuni server (Open-Meteo) mandano un
        // "deflate" che Windows non riesce a leggere ("lettura interrotta").
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP;
        WinHttpSetOption(h.session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        h.connect = WinHttpConnect(h.session, host, uc.nPort, 0);
        if (!h.connect) { r.error = "server non raggiungibile"; return r; }
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        h.request = WinHttpOpenRequest(h.connect, method.c_str(), fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!h.request) { r.error = "richiesta non creata"; return r; }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(h.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

        std::wstring headers = L"Accept-Language: it-IT,it;q=0.9,en;q=0.8\r\n";
        for (const auto& line : options.headers) headers += line + L"\r\n";
        if (!contentType.empty()) headers += L"Content-Type: " + TextUtils::utf8ToWide(contentType) + L"\r\n";

        if (!WinHttpSendRequest(h.request, headers.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(h.request, nullptr)) {
            const DWORD err = GetLastError();
            r.error = err == ERROR_WINHTTP_TIMEOUT ? "tempo scaduto" : err == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "nessuna connessione a internet"
                                                                                                             : "connessione non riuscita (" + std::to_string(err) + ")";
            return r;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;
        r.contentType = TextUtils::wideToUtf8(queryHeader(h.request, WINHTTP_QUERY_CONTENT_TYPE));

        DWORD urlLen = 0;
        WinHttpQueryOption(h.request, WINHTTP_OPTION_URL, nullptr, &urlLen);
        if (urlLen > 0) {
            std::wstring finalUrl(urlLen / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryOption(h.request, WINHTTP_OPTION_URL, finalUrl.data(), &urlLen)) {
                finalUrl.resize(urlLen / sizeof(wchar_t));
                r.finalUrl = TextUtils::wideToUtf8(finalUrl);
            }
        }
        if (r.finalUrl.empty()) r.finalUrl = url;

        char buffer[16384];
        while (true) {
            if (options.cancel && options.cancel->load()) { r.error = "annullata"; return r; }
            DWORD read = 0;
            if (!WinHttpReadData(h.request, buffer, sizeof(buffer), &read)) {
                r.error = "lettura interrotta (" + std::to_string(GetLastError()) + ")";
                return r;
            }
            if (read == 0) break;
            r.body.append(buffer, read);
            if (r.body.size() >= options.maxBytes) break; // basta così: il resto della pagina non serve
        }
        return r;
    }
}

Response get(const std::string& url, const Options& options) {
    return send(L"GET", url, {}, {}, options);
}

Response post(const std::string& url, const std::string& body, const std::string& contentType, const Options& options) {
    return send(L"POST", url, body, contentType, options);
}

std::string bodyUtf8(const Response& response) {
    const std::string cs = Html::charset(response.contentType, response.body);
    if (cs == "iso-8859-1" || cs == "windows-1252" || cs == "latin1" || cs == "iso-8859-15" || cs == "cp1252") {
        const std::string& b = response.body;
        int n = MultiByteToWideChar(1252, 0, b.data(), (int)b.size(), nullptr, 0);
        if (n <= 0) return b;
        std::wstring w(n, L'\0');
        MultiByteToWideChar(1252, 0, b.data(), (int)b.size(), w.data(), n);
        return TextUtils::wideToUtf8(w);
    }
    return response.body;
}

} // namespace Http
