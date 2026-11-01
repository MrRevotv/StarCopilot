#include "axis_server.h"
#include "log.h"
#include "text_utils.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>

namespace {
    std::string base64(const unsigned char* data, size_t n) {
        static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < n; i += 3) {
            const unsigned v = (data[i] << 16) | (i + 1 < n ? data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
            out += abc[(v >> 18) & 63];
            out += abc[(v >> 12) & 63];
            out += i + 1 < n ? abc[(v >> 6) & 63] : '=';
            out += i + 2 < n ? abc[v & 63] : '=';
        }
        return out;
    }

    // Sec-WebSocket-Accept = base64(SHA-1(chiave + GUID del protocollo)).
    std::string acceptKey(const std::string& key) {
        const std::string text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        BCRYPT_ALG_HANDLE alg = nullptr;
        unsigned char digest[20] = {};
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
            BCryptHash(alg, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), digest, sizeof(digest));
            BCryptCloseAlgorithmProvider(alg, 0);
        }
        return base64(digest, sizeof(digest));
    }

    bool sendAll(SOCKET s, const char* data, size_t n) {
        while (n > 0) {
            const int sent = send(s, data, (int)std::min<size_t>(n, 1 << 20), 0);
            if (sent <= 0) return false;
            data += sent;
            n -= (size_t)sent;
        }
        return true;
    }

    bool sendText(SOCKET s, const std::string& text) {
        std::string frame;
        frame += (char)0x81; // FIN + testo
        const size_t n = text.size();
        if (n < 126) {
            frame += (char)n;
        } else if (n < 65536) {
            frame += (char)126;
            frame += (char)((n >> 8) & 255);
            frame += (char)(n & 255);
        } else {
            frame += (char)127;
            for (int i = 7; i >= 0; --i) frame += (char)((n >> (8 * i)) & 255);
        }
        frame += text;
        return sendAll(s, frame.data(), frame.size());
    }

    void sendHttp(SOCKET s, int status, const char* reason, const std::string& type, const std::string& body) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        sendAll(s, head.data(), head.size());
        sendAll(s, body.data(), body.size());
    }

    // Solo lettere e cifre (al massimo 16): la chiave finisce dentro il codice della pagina.
    std::string safeKey(const std::string& key) {
        std::string out;
        for (char c : key) {
            if (out.size() == 16) break;
            if (std::isalnum((unsigned char)c)) out += c;
        }
        return out;
    }

    std::string htmlEscape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
            }
        }
        return out;
    }

    std::string replaceAll(std::string s, const std::string& what, const std::string& with) {
        for (size_t pos = 0; (pos = s.find(what, pos)) != std::string::npos; pos += with.size()) s.replace(pos, what.size(), with);
        return s;
    }
}

void AxisServer::setPages(std::string overlayHtml, std::string indexHtml) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlayHtml = std::move(overlayHtml);
    m_indexHtml = std::move(indexHtml);
}

std::string AxisServer::lastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

bool AxisServer::start(int port, std::string& error) {
    if (m_running && port == m_port) return true;
    stop();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "rete di Windows non disponibile";
        return false;
    }
    m_stopping = false;
    std::vector<SOCKET> listeners;
    int failure = 0;
    // 127.0.0.1 e ::1: OBS scrive "localhost", che può voler dire l'uno o l'altro.
    for (int family : { AF_INET, AF_INET6 }) {
        SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
        int result;
        if (family == AF_INET) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((u_short)port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            result = bind(s, (sockaddr*)&a, sizeof(a));
        } else {
            DWORD v6only = 1;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
            sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_port = htons((u_short)port);
            a.sin6_addr = in6addr_loopback;
            result = bind(s, (sockaddr*)&a, sizeof(a));
        }
        if (result != 0 || listen(s, 16) != 0) {
            if (family == AF_INET) failure = WSAGetLastError();
            closesocket(s);
            continue;
        }
        listeners.push_back(s);
    }
    if (listeners.empty() || failure != 0) {
        for (SOCKET s : listeners) closesocket(s);
        error = failure == WSAEADDRINUSE || failure == WSAEACCES
                    ? "la porta " + std::to_string(port) + " è già usata da un altro programma (Axis Visualizer aperto con la stessa porta?): scegline un'altra"
                    : "server non avviato (errore " + std::to_string(failure) + ")";
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = error;
        }
        Log::warn("Axis: server per OBS non avviato sulla porta " + std::to_string(port) + ": " + error);
        WSACleanup();
        return false;
    }
    m_listeners.assign(listeners.begin(), listeners.end());
    m_port = port;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error.clear();
    }
    m_running = true;
    m_acceptThread = std::thread([this]() { acceptLoop(); });
    Log::info("Axis: server per OBS su http://localhost:" + std::to_string(port) + "/");
    return true;
}

void AxisServer::stop() {
    if (!m_running) return;
    m_stopping = true;
    for (uintptr_t s : m_listeners) closesocket((SOCKET)s);
    m_listeners.clear();
    if (m_acceptThread.joinable()) m_acceptThread.join();
    // Le connessioni aperte se ne accorgono da sole (al massimo qualche secondo).
    for (int i = 0; i < 120 && m_activeClients.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    m_running = false;
    WSACleanup();
}

void AxisServer::publishLayout(const std::vector<Window>& windows) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_windows = windows;
    }
    ++m_layoutVersion;
}

void AxisServer::publishValues(std::map<std::string, std::string> values) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values = std::move(values);
}

void AxisServer::acceptLoop() {
    while (!m_stopping) {
        fd_set set;
        FD_ZERO(&set);
        for (uintptr_t s : m_listeners) FD_SET((SOCKET)s, &set);
        timeval timeout{ 0, 250000 };
        if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
        for (uintptr_t s : m_listeners) {
            if (!FD_ISSET((SOCKET)s, &set)) continue;
            SOCKET client = accept((SOCKET)s, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            ++m_activeClients;
            std::thread([this, client]() {
                serve(client);
                --m_activeClients;
            }).detach();
        }
    }
}

void AxisServer::serve(uintptr_t raw) {
    SOCKET s = (SOCKET)raw;
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    std::string request;
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, n);
    }
    // Prima riga: "GET /g1 HTTP/1.1".
    const size_t end = request.find("\r\n");
    const std::string first = request.substr(0, end);
    std::string path;
    if (first.rfind("GET ", 0) == 0) {
        const size_t sp = first.find(' ', 4);
        path = first.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
    }
    path = path.substr(0, path.find_first_of("?#"));
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    while (!path.empty() && path.back() == '/') path.pop_back();

    std::string lower = TextUtils::toLower(request);
    const size_t keyPos = lower.find("\r\nsec-websocket-key:");
    if (lower.find("upgrade: websocket") != std::string::npos && keyPos != std::string::npos) {
        const size_t start = keyPos + 20, stop = request.find("\r\n", start);
        const std::string key = TextUtils::trim(request.substr(start, stop - start));
        const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                                  acceptKey(key) + "\r\n\r\n";
        if (sendAll(s, reply.data(), reply.size())) serveSocket(raw, safeKey(path));
    } else if (first.rfind("GET ", 0) != 0) {
        sendHttp(s, 405, "Method Not Allowed", "text/plain", "");
    } else if (path.empty()) {
        std::string rows, page;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& w : m_windows) {
                rows += "<li><b>" + htmlEscape(w.name) + "</b><br><a href=\"/" + htmlEscape(w.key) + "\">" + htmlEscape(urlFor(w.key)) + "</a></li>";
            }
            if (m_windows.empty()) rows = "<li>Nessuna finestra: l'overlay non e' in funzione. Avvialo dalla pagina Axis Visualizer di Iris.</li>";
            page = replaceAll(m_indexHtml, "__ROWS__", rows);
        }
        sendHttp(s, 200, "OK", "text/html; charset=utf-8", page);
    } else if (path == "favicon.ico") {
        sendHttp(s, 404, "Not Found", "text/plain", "");
    } else {
        std::string page;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            page = replaceAll(m_overlayHtml, "__KEY__", safeKey(path));
        }
        sendHttp(s, 200, "OK", "text/html; charset=utf-8", page);
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
}

void AxisServer::serveSocket(uintptr_t raw, const std::string& key) {
    SOCKET s = (SOCKET)raw;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    ++m_viewers;
    int sentVersion = -1;
    auto lastOff = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    char buf[512];
    while (!m_stopping) {
        // La pagina chiusa (o un messaggio di chiusura): finito.
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n > 0 && ((unsigned char)buf[0] & 0x0F) == 0x8) break;
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) break;

        std::string layoutFrame, valuesFrame;
        const int version = m_layoutVersion.load();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (version != sentVersion) {
                layoutFrame = "{\"t\":\"none\"}";
                for (const auto& w : m_windows) {
                    if (TextUtils::toLower(w.key) == TextUtils::toLower(key)) layoutFrame = "{\"t\":\"l\",\"w\":" + w.layoutJson + "}";
                }
            }
            auto it = m_values.find(key);
            if (it != m_values.end()) valuesFrame = "{\"t\":\"v\",\"v\":" + it->second + "}";
        }
        bool ok = true;
        if (!layoutFrame.empty()) {
            sentVersion = version;
            ok = sendText(s, layoutFrame);
        }
        if (ok && !valuesFrame.empty()) {
            ok = sendText(s, valuesFrame);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } else if (ok) {
            // Overlay chiuso: lo si dice ogni tanto, non sessanta volte al secondo.
            const auto now = std::chrono::steady_clock::now();
            if (now - lastOff > std::chrono::milliseconds(250)) {
                lastOff = now;
                ok = sendText(s, "{\"t\":\"off\"}");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok) break;
    }
    --m_viewers;
}
