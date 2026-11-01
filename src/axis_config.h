#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

// Axis Visualizer dentro Iris: la configurazione. Stesso modello (e stessi nomi
// dei campi JSON) di Axis Visualizer, così il suo settings.json si importa così
// com'è: profili, moduli (ASSI, BARRE, PULSANTI), periferiche, nomi, colori,
// posizioni delle finestre. Salvata in %appdata%\Iris\axis.json.
//
// Periferiche: "dinput:<guid minuscolo>", "xinput:0".."xinput:3", "mouse", "keyboard".
namespace Axis {

    enum class Kind { Assi, Barre, Pulsanti };

    struct Pane {
        Kind kind = Kind::Assi;
        std::string deviceId;            // vuoto = modulo spento
        std::string label;
        std::string align = "Sinistra";  // "Sinistra", "Centro", "Destra"
        int size = 200;                  // larghezza in pixel
        int group = 0;                   // 0 = finestra sua; stesso numero = stessa finestra
        bool locked = false;
        std::optional<double> left, top; // posizione della finestra (pixel dello schermo)
        // ASSI
        int axisX = 0, axisY = 1, axisZ = 2; // -1 = nessuno
        bool invertX = false, invertY = false, invertZ = false;
        double trailSeconds = 0.35;
        bool showValues = false;
        // BARRE
        std::vector<int> barAxes;        // vuoto = tutti
        // PULSANTI
        std::vector<int> buttons;        // vuoto = tutti (tastiera: i tasti scelti)
        int buttonColumns = 0;
        bool showHat = true;
        // Nomi scelti dall'utente
        std::map<int, std::string> axisLabels, buttonLabels;
        std::string dotColor = "#2AA198", barColor = "#00E5FF";

        bool enabled() const { return !deviceId.empty(); }
        std::string displayName() const; // l'etichetta, o il tipo, in maiuscolo
    };

    struct Profile {
        std::string name = "PREDEFINITO";
        std::vector<Pane> panes;
        std::vector<std::string> triggers; // eseguibili che lo aprono ("StarCitizen.exe")
    };

    struct KeyboardKeys {
        int up = 0x20, down = 0x11, left = 0x41, right = 0x44, barPlus = 0x57, barMinus = 0x53;
        double barTravelSeconds = 2.0;
    };

    struct Settings {
        bool serverEnabled = true;
        int serverPort = 8787;           // Axis Visualizer usa 8777: così possono stare aperti tutti e due
        std::vector<Profile> profiles;
        int activeProfile = 0;
        bool autoOverlayWithGame = true;
        std::string backgroundColor = "#000000", accentColor = "#2AA198";
        double opacity = 0.75;
        int dotRadius = 12, textSize = 11;
        bool boldText = false;
        double smoothing = 0.25;
        int mouseRange = 300;
        bool showGrid = true, showFrames = true, showTitles = true;
        int columns = 0, gap = 8;
        bool locked = false;
        KeyboardKeys keyboard;

        Profile& active();
        const Profile& active() const;
        void sanitize();
    };

    // Il JSON di Iris o quello di Axis Visualizer (campi con l'iniziale maiuscola).
    Settings parse(const std::string& json);
    std::string serialize(const Settings& settings);

    std::wstring path();                       // %appdata%\Iris\axis.json
    std::wstring axisVisualizerSettingsPath(); // %appdata%\AxisVisualizer\settings.json
    bool exists();
    Settings load();
    bool save(const Settings& settings);

    // I profili e l'aspetto di Axis Visualizer, se c'è. message: cosa è stato importato o perché no.
    bool importFromAxisVisualizer(Settings& out, std::string& message);

    // I moduli raggruppati come finestre: chiave per OBS ("g1", "m3") e nome.
    struct WindowGroup {
        std::string key, name;
        std::vector<int> panes; // indici nel profilo attivo
    };
    std::vector<WindowGroup> windowGroups(const Profile& profile);

    // Il nome dell'eseguibile senza cartella né ".exe", minuscolo ("starcitizen").
    std::string processName(const std::string& executable);

} // namespace Axis
