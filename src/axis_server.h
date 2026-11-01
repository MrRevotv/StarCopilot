#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Il server per OBS dell'Axis Visualizer dentro Iris: lo stesso di Axis
// Visualizer (stessa pagina overlay.html, stesso protocollo), su localhost.
// A ogni finestra dell'overlay un indirizzo (http://localhost:porta/g1) da
// incollare in una sorgente Browser di OBS; dentro la pagina un WebSocket
// porta solo i numeri (la forma una volta, poi i valori a 60 al secondo).
// Ascolta solo su questo computer (127.0.0.1 e ::1): niente firewall.
class AxisServer {
public:
    AxisServer() = default;
    ~AxisServer() { stop(); }
    AxisServer(const AxisServer&) = delete;
    AxisServer& operator=(const AxisServer&) = delete;

    // Le pagine (dall'exe): overlay.html con __KEY__, index.html con __ROWS__.
    void setPages(std::string overlayHtml, std::string indexHtml);

    bool start(int port, std::string& error); // già acceso sulla stessa porta: true
    void stop();
    bool running() const { return m_running; }
    int port() const { return m_port; }
    std::string lastError() const;
    bool hasViewers() const { return m_viewers.load() > 0; }

    struct Window {
        std::string key, name;
        std::string layoutJson; // lo StreamWindow di Axis Visualizer
    };
    // Le finestre dell'overlay sono cambiate (vuoto = overlay chiuso).
    void publishLayout(const std::vector<Window>& windows);
    // I numeri dell'ultimo fotogramma: per chiave, "[[...],[...]]" (vuoto = overlay chiuso).
    void publishValues(std::map<std::string, std::string> values);

    std::string urlFor(const std::string& key) const { return "http://localhost:" + std::to_string(m_port) + "/" + key; }

private:
    void acceptLoop();
    void serve(uintptr_t socket);
    void serveSocket(uintptr_t socket, const std::string& key);

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopping{ false };
    std::atomic<int> m_viewers{ 0 };
    int m_port = 0;
    std::vector<uintptr_t> m_listeners;
    std::thread m_acceptThread;
    std::atomic<int> m_activeClients{ 0 };

    mutable std::mutex m_mutex;
    std::string m_error;
    std::string m_overlayHtml, m_indexHtml;
    std::vector<Window> m_windows;
    std::map<std::string, std::string> m_values;
    std::atomic<int> m_layoutVersion{ 0 };
};
