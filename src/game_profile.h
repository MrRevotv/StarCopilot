#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

// Il profilo di un gioco (per ora Star Citizen): tasti, comandi vocali, nomi
// dei luoghi, correzioni, calibrazione della mappa stellare. È un file JSON in
// %appdata%\Iris\star_citizen.json, modificabile a mano (anche con commenti //):
// Iris lo rilegge da sola quando lo salvi.
//
// Al primo avvio, se c'è la vecchia Iris (C#, %appdata%\Iris\config), ne
// importa tasti, frasi, alias e calibrazione.

struct GameCommand {
    std::string intent;                      // set_route, press, hold, sequence, quantum_travel...
    std::vector<std::string> phrases;        // "rotta per {destination}"
    std::map<std::string, std::string> args; // es. key = landing_gear
    std::string say;                         // conferma a voce (facoltativa), {destination} si sostituisce

    // Nome breve e stabile del comando, per il modello: "press:landing_gear", "set_route".
    std::string id() const;
};

struct StarMapSettings {
    // Coordinate relative alla finestra del gioco (0..1), valide a ogni risoluzione.
    double searchBoxX = 0.281, searchBoxY = 0.124;
    double searchBoxWidth = 0.16, searchBoxHeight = 0.035;
    std::array<double, 4> resultsRegion{ 0.03, 0.10, 0.30, 0.55 };     // x, y, larghezza, altezza
    std::array<double, 4> setRouteRegion{ 0.60, 0.70, 0.40, 0.28 };
    std::array<double, 4> cancelRouteRegion{ 0.20, 0.55, 0.20, 0.07 };
    std::vector<std::string> setRouteLabels{ "IMPOSTA ROTTA", "SET ROUTE", "IMPOSTA DESTINAZIONE", "SET DESTINATION", "IMPOSTA PERCORSO" };
    std::vector<std::string> cancelRouteLabels{ "ANNULLA", "ANNULLA ROTTA", "CANCEL", "CANCEL ROUTE", "CLEAR ROUTE" };
    std::vector<std::string> searchPlaceholders{ "CERCA LOCALITA", "SEARCH LOCATION" };
    int mapOpenTimeoutMs = 4000; // si aspetta che la mappa sia aperta (casella di ricerca visibile), non un tempo fisso
    int focusSettleMs = 400;
    int typeCharDelayMs = 45;
    int resultsTimeoutMs = 3000;
    int clickSettleMs = 600;
    int mouseMoveSteps = 12;
    int mouseMoveStepDelayMs = 4;
    int typingAttempts = 3;      // se il testo non entra nel campo: riclicca e riscrive
    bool smoothMouseMove = true;
    bool clearFieldFirst = true;
    bool closeMapAfterSetRoute = true;
};

struct OcrSettings {
    std::string language = "it";
    int upscale = 2;
    double matchThreshold = 0.68;
    bool saveDebugShots = true; // %appdata%\Iris\debug: le immagini lette con i riquadri dell'OCR
};

struct GameProfile {
    std::string name = "Star Citizen";
    std::string processName = "StarCitizen";
    std::string windowTitle = "Star Citizen";
    bool dryRun = false;       // true = non preme niente, scrive solo cosa farebbe
    int keyHoldMs = 45;
    int keyGapMs = 90;
    int stepDelayMs = 1000;    // pausa tra i passi delle azioni lunghe (rotta, sequenze, salto quantico); i tasti singoli no
    std::map<std::string, std::string> keys;    // nome logico -> tasto ("landing_gear" -> "N", "request_landing" -> "LAlt+N")
    double matchThreshold = 0.72;
    std::map<std::string, std::string> corrections; // "a nulla" -> "annulla" (anche più parole)
    std::vector<GameCommand> commands;
    std::map<std::string, std::map<std::string, std::vector<std::string>>> aliases; // slot -> nome vero -> come lo pronunci
    StarMapSettings starMap;
    OcrSettings ocr;

    // Il profilo predefinito di Star Citizen (tasti predefiniti del gioco, comandi in italiano).
    static GameProfile defaults();

    // Lettura tollerante (commenti // e /* */, virgole in più). false = JSON rotto.
    static bool parse(const std::string& json, GameProfile& out, std::string& error);
    std::string toJson() const;

    // La vecchia Iris (C#): settings.json + dictionary.json. Si parte dai
    // predefiniti e si tiene tutto quello che avevi personalizzato tu.
    static bool importOldIris(const std::string& settingsJson, const std::string& dictionaryJson, GameProfile& out, std::string& error);
};

// Toglie commenti e virgole finali, per leggere i JSON scritti a mano.
std::string relaxedJson(const std::string& text);
