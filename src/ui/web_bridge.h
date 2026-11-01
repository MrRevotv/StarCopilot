#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class Assistant;
class UpdateManager;
struct AxisHost;
namespace AxisDraw {
    class PaneView;
}
namespace Axis {
    class Source;
}

// Messaggi dai thread di lavoro alla finestra (che li gira al ponte).
inline constexpr UINT WM_APP_BRIDGE_FIRST = WM_APP + 40;
inline constexpr UINT WM_APP_BRIDGE_CAPTURED = WM_APP + 40; // wParam = 1 se preso, lParam = TalkTrigger*
inline constexpr UINT WM_APP_BRIDGE_SEND = WM_APP + 41;     // lParam = std::string* (JSON da mandare alla pagina)
inline constexpr UINT WM_APP_BRIDGE_RUN = WM_APP + 42;      // lavori rimandati (finestre modali fuori dagli eventi di WebView2)
inline constexpr UINT WM_APP_BRIDGE_LAST = WM_APP + 49;

// Il ponte tra l'interfaccia (HTML/CSS/JS dentro WebView2) e il motore.
//  - verso la pagina: lo stato in JSON ({"t": "settings", "v": {...}}, "status",
//    "conversation", "meters"...), quando cambia;
//  - dalla pagina: i comandi ({"a": "set", "k": "wakeWordEnabled", "v": true},
//    {"a": "sendText", "text": "..."}...).
// Vive nel thread della finestra. Le finestre modali (scelta della zona, file,
// conferme di Windows) partono fuori dagli eventi di WebView2 (WM_APP_BRIDGE_RUN).
class WebBridge {
public:
    using Send = std::function<void(const std::string& json)>;

    WebBridge(Assistant& engine, UpdateManager& updates, AxisHost& axis, HWND window, Send send);
    ~WebBridge();
    WebBridge(const WebBridge&) = delete;
    WebBridge& operator=(const WebBridge&) = delete;

    void onMessage(const std::string& json);
    void onEngineEvent(int event); // Assistant::Event
    void onUpdateChanged();
    void onTimer();                // circa 30 volte al secondo
    void onAppMessage(UINT msg, WPARAM wp, LPARAM lp);
    // Axis Visualizer: l'overlay è cambiato, o una sua finestra è stata spostata.
    // Barra rapida: "chiudi il registro delle scansioni" -> la pagina Mining con la richiesta di luogo e qualità.
    void openMiningLogDialog();
    void onAxisChanged();
    void onAxisMoved(void* positions);

private:
    void sendAll();
    void send(const char* type, const std::string& valueJson);
    void sendSettings();
    void sendStatus();
    void sendConversation();
    void sendLists();
    void sendChatHistory();
    void sendChatNotice();
    void sendFiltered();
    void sendMining();
    void sendMiningData();
    void sendGame();
    void sendUpdate();
    void sendMeters();
    void sendOcrInfo();
    void sendGameLog();
    void sendRefinery();
    void sendGameCommands(bool force);
    void sendMiningLog();
    void handleMiningLog(const std::string& action, const void* message);
    void handleCommands(const std::string& action, const void* message); // editor dei comandi di gioco
    void captureGameKey();
    void handleExtra(const std::string& action, const void* message); // diario, raffineria, avvio, barra rapida
    void toast(const std::string& text, bool ok = true);

    void handle(const std::string& action, const void* message);
    void setSetting(const std::string& key, const void* value);
    void defer(std::function<void()> work);
    void postJson(const std::string& json); // da qualunque thread

    void startCapture(const std::string& target);
    void stopCapture();
    void onCaptured(bool ok, void* trigger);
    void startWork(std::function<void()> work);
    void stopWork();
    void calibrate(int zone);
    void readZone(int zone);
    void chooseOcrRegion();
    void chooseMiningRegion();
    void openMemory();

    // --- Axis Visualizer ---
    void handleAxis(const std::string& action, const void* message);
    void sendAxis();
    void axisEdit(const std::function<void(void*)>& change); // modifica sul JSON (Axis::serialize), poi riletto
    void axisApply(bool reopen);
    void axisRebuildPreviews();
    void axisTickPreviews();
    void axisCheckGames();
    void axisTickCapture();

    AxisHost& m_axis;
    std::vector<std::unique_ptr<AxisDraw::PaneView>> m_axisPreviews; // per indice del modulo (nullptr = spento)
    bool m_axisPreviewsDirty = true;
    std::string m_axisLastLayouts;
    int m_axisCaptureIndex = -1;  // modulo PULSANTI che aspetta un pulsante
    std::unique_ptr<Axis::Source> m_axisCaptureSource;
    std::vector<bool> m_axisCaptureBaseline;
    int m_axisCaptureBaselineKey = -1;
    unsigned long long m_axisCaptureUntil = 0;
    std::string m_axisMessage;

    Assistant& m_engine;
    UpdateManager& m_updates;
    HWND m_window;
    Send m_send;
    std::string m_page = "home";
    unsigned m_ticks = 0;
    bool m_ready = false;

    std::mutex m_deferMutex;
    std::deque<std::function<void()>> m_deferred;

    std::string m_captureTarget; // vuoto = nessuna cattura
    std::atomic<bool> m_captureCancel{ false };
    std::thread m_captureThread;
    // Cattura di un tasto di gioco per l'editor dei comandi (anche con Alt/Ctrl/Shift).
    std::thread m_gameKeyThread;
    std::atomic<bool> m_gameKeyCancel{ false };
    bool m_gameKeyCapturing = false;
    std::string m_lastCommands;

    std::thread m_work; // calibrazione e lettura delle zone della mappa stellare
    std::atomic<bool> m_workCancel{ false };

    std::string m_lastGame, m_lastNotice;
};
