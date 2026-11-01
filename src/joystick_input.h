#pragma once

#include <string>
#include <vector>

// Joystick, HOTAS e controller via DirectInput: fino a 128 pulsanti per
// periferica (i VKB/Virpil ne hanno ben più dei 32 che vede la vecchia API
// winmm). Ogni periferica ha un id stabile (il GUID di istanza di Windows),
// che non cambia se ne colleghi un'altra o la ricolleghi in un'altra porta.
//
// Uso: initialize() una volta, poi poll() a intervalli regolari (lo fa il
// thread di HotkeyListener) e isButtonDown() per leggere lo stato.
namespace JoystickInput {

    struct DeviceInfo {
        std::string id;   // GUID di istanza, es. "{6F1D2B60-...}"
        std::string name; // es. "VKBsim Gladiator EVO"
        int buttonCount = 0;
    };

    bool initialize();
    void shutdown();

    // Legge lo stato di tutte le periferiche (e ogni tanto cerca quelle
    // collegate nel frattempo). Thread-safe.
    void poll();

    std::vector<DeviceInfo> devices();

    // id di una periferica scollegata: false.
    bool isButtonDown(const std::string& deviceId, int buttonIndex);

    // Per convertire le impostazioni della versione vecchia (winmm usava un
    // numero 0-15): la n-esima periferica collegata, o stringa vuota.
    std::string idForLegacyIndex(int legacyIndex);

    // Il nome di una periferica, anche se ora è scollegata (dall'ultima volta che la si è vista).
    std::string nameOf(const std::string& deviceId);

    // --- Assi (per l'Axis Visualizer dentro Iris) ------------------------------------
    // Gli assi nell'ordine in cui DirectInput li elenca, tenendo solo X, Y, Z, Rx, Ry,
    // Rz e i primi due slider: lo stesso ordine di Axis Visualizer, così i suoi numeri
    // d'asse (AxisX = 0, AxisZ = 5...) valgono uguali qui.
    struct Layout {
        std::vector<std::string> axisNames;   // "X Axis", "Rotazione Z"...
        std::vector<int> axisKinds;           // 0 X, 1 Y, 2 Z, 3 Rx, 4 Ry, 5 Rz, 6 slider
        std::vector<std::string> buttonNames; // "Button 1"...
        int hats = 0;
    };
    struct Snapshot {
        bool connected = false;
        std::vector<double> axes;  // -1..1
        std::vector<bool> buttons;
        std::vector<int> hats;     // gradi 0-359, -1 = al centro
    };
    bool layoutOf(const std::string& deviceId, Layout& out);
    bool snapshot(const std::string& deviceId, Snapshot& out);

    // "{5AB9B330-8904-11F1-...}" <-> "5ab9b330-8904-11f1-..." (il formato di Axis Visualizer).
    std::string toAxisId(const std::string& deviceId);
    std::string fromAxisId(const std::string& axisGuid);

} // namespace JoystickInput
