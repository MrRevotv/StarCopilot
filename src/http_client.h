#pragma once

#include <atomic>
#include <string>
#include <vector>

// Richieste web con WinHTTP (già in Windows, nessuna libreria in più):
// ricerche, pagine, meteo. Sincrone, con tempo massimo e limite di dimensione.
namespace Http {

    struct Response {
        int status = 0;
        std::string body;        // byte così come arrivano (vedi bodyUtf8)
        std::string contentType;
        std::string finalUrl;    // dopo eventuali rinvii
        std::string error;       // vuoto = arrivata
        bool ok() const { return error.empty() && status >= 200 && status < 300; }
    };

    struct Options {
        int timeoutMs = 8000;
        size_t maxBytes = 3 * 1024 * 1024;
        std::vector<std::wstring> headers; // "Nome: valore"
        const std::atomic<bool>* cancel = nullptr;
    };

    Response get(const std::string& url, const Options& options = {});
    Response post(const std::string& url, const std::string& body, const std::string& contentType, const Options& options = {});

    // Il corpo in UTF-8: le pagine in latin1/windows-1252 si convertono.
    std::string bodyUtf8(const Response& response);

} // namespace Http
