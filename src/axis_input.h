#pragma once

#include "axis_config.h"

#include <optional>
#include <string>
#include <vector>

// Axis Visualizer dentro Iris: le periferiche e la loro lettura, come in Axis
// Visualizer (stessi id, stessi assi, stessi pulsanti):
//  - "dinput:<guid>": cloche, manette, pedaliere (DirectInput di Iris, JoystickInput);
//  - "xinput:N": pad Xbox (levette, grilletti separati, croce come cappello);
//  - "mouse": il cursore rispetto al centro dello schermo, rotella, 5 pulsanti;
//  - "keyboard": assi dai tasti (WASD...), i tasti scelti come pulsanti.
namespace Axis {

    struct DeviceInfo {
        std::string id, name;
        std::string kind; // "controller", "mouse", "keyboard"
        std::vector<std::string> axisNames, buttonNames;
        int hats = 0;
        int defaultX = 0, defaultY = 1, defaultZ = -1;
        bool defaultInvertY = false;
        bool connected = true;
    };

    // Quelle collegate adesso (più mouse e tastiera, sempre).
    std::vector<DeviceInfo> devices();
    // Una periferica, anche scollegata (nome dall'ultima volta che la si è vista).
    DeviceInfo describe(const std::string& id);

    // La rotella del mouse (scatti), dall'aggancio del mouse dell'overlay.
    void addMouseWheel(double notches);

    struct InputState {
        bool connected = false;
        std::vector<double> axes; // -1..1, nell'ordine della periferica
        std::vector<std::string> axisNames;
        std::vector<bool> buttons;
        std::vector<std::string> buttonNames;
        std::vector<int> hats;    // gradi, -1 = al centro
        double wheel = 0;         // -1..1, sfuma da sola
        double x = 0, y = 0;      // gli assi scelti per il modulo (inversioni comprese)
        std::optional<double> z;
    };

    // La lettura di una periferica per un modulo.
    class Source {
    public:
        Source(const Pane& pane, const Settings& settings);
        void update(double deltaSeconds);
        const InputState& state() const { return m_state; }

    private:
        std::optional<double> mapped(int index, bool invert) const;
        Pane m_pane;
        KeyboardKeys m_keys;
        int m_mouseRange = 300;
        InputState m_state;
        double m_bar = 0;           // tastiera: la barra che sale e scende
        double m_lastWheelTotal = 0;
    };

    // Per la scelta dei tasti della tastiera: il primo premuto adesso (-1 = nessuno).
    int firstPressedKey();

} // namespace Axis
