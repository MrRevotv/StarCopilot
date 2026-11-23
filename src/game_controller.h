#pragma once

#include "command_matcher.h"
#include "game_profile.h"
#include "sc_telemetry.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// I comandi di gioco (Star Citizen): dalla frase al tasto, alla rotta sulla
// mappa stellare. Il "copilota" della vecchia Iris, rifatto e migliorato:
//  - comandi istantanei dal dizionario (niente modello: millisecondi);
//  - il modello di linguaggio può usare gli stessi comandi (strumento game_command)
//    per le frasi libere ("alza gli scudi e accendi i motori");
//  - rotta: aspetta che la mappa sia aperta, verifica con l'OCR che il nome
//    sia stato scritto (se no riclicca e riscrive), aspetta i risultati;
//  - se il gioco è avviato come amministratore e Iris no, lo dice (Windows
//    scarterebbe i tasti senza errori);
//  - con la telemetria del gioco (setShipStateProvider) i tasti "interruttore" guardano prima
//    com'è la nave: "accendi le armi" con le armi già accese non preme niente (vedi ship_toggle.h).
class GameController {
public:
    // Lo stato della nave adesso; nullopt = non si sa (si preme come sempre).
    using ShipStateProvider = std::function<std::optional<ScTelemetry::ShipState>()>;

    struct Result {
        bool ok = false;
        std::string spoken; // da dire a voce (vuoto = basta un suono)
        std::string shown;  // per la conversazione
        std::string brief;  // risposte brevi: una o due parole (vuoto = si ricava da spoken, o basta il suono)
    };

    struct Status {
        bool running = false;
        bool foreground = false;
        bool elevationProblem = false; // gioco amministratore, Iris no
        std::string text;
    };

    // Le zone della mappa stellare da calibrare.
    enum class Zone { SearchBox, Results, SetRoute, CancelRoute };

    GameController();

    // Il profilo: al primo avvio importa la vecchia Iris (o crea quello predefinito).
    void load();
    void reloadIfChanged(); // il file è stato salvato a mano: si rilegge
    GameProfile profile();
    std::wstring profilePath() const;
    bool saveProfile(const GameProfile& profile);
    std::string profileError(); // errore dell'ultima lettura (JSON rotto), vuoto se tutto bene

    Status status(); // ricalcolato al massimo una volta al secondo
    bool gameRunning() { return status().running; }
    // Il gioco è la finestra in primo piano ADESSO (non dalla cache di status()).
    bool gameInForeground();

    Cmd::Match match(const std::string& text);
    // say: per le frasi da dire SUBITO (es. "Calcolo la rotta..." prima di cominciare).
    Result execute(const Cmd::Match& match, const std::function<void(const std::string&)>& say, const std::atomic<bool>* cancel);
    // Dallo strumento del modello: "press:landing_gear", "set_route" + destinazione.
    // state: il verso che il modello ha capito ("on", "off"; per il carrello "down", "up"), vuoto = non detto;
    // heard: la richiesta dell'utente, per ricavare il verso se il modello non lo dice.
    Result executeById(const std::string& id, const std::string& destination, const std::function<void(const std::string&)>& say,
                       const std::atomic<bool>* cancel, const std::string& state = {}, const std::string& heard = {});
    std::string toolDefinition(); // JSON dello strumento game_command

    void setDryRun(bool on);
    // La telemetria del gioco (Assistant: ScTelemetry::Client::current()). Vuoto = senza.
    void setShipStateProvider(ShipStateProvider provider);

    // Porta il gioco in primo piano (calibrazione, lettura); altrimenti il motivo.
    bool bringToFront(std::string& error);
    // Diagnostica: porta il gioco davanti, legge una zona della mappa e dice cosa ha visto l'OCR.
    std::string readZone(Zone zone);
    // Calibrazione: la posizione del mouse relativa alla finestra del gioco (0..1).
    bool cursorRelative(double& rx, double& ry, std::string& error);
    // Salva un punto (casella di ricerca) o un rettangolo (zone) nel profilo.
    bool setZone(Zone zone, double x1, double y1, double x2, double y2);
    static std::string zoneName(Zone zone);

    std::vector<std::string> recentActions();
    std::vector<std::string> unrecognized();
    void noteUnrecognized(const std::string& text, const Cmd::Match& nearMiss);

private:
    Result executeWith(const Cmd::Match& match, const std::string& state, const std::function<void(const std::string&)>& say,
                       const std::atomic<bool>* cancel);
    Result run(const Cmd::Match& match, const std::string& state, const std::function<void(const std::string&)>& say,
               const std::atomic<bool>* cancel);
    std::optional<ScTelemetry::ShipState> shipState();
    // Il gioco in primo piano, pronto a ricevere i tasti; altrimenti il motivo.
    bool prepareGame(const GameProfile& p, HWND& window, std::string& why, std::string& brief);
    void pressLogical(const GameProfile& p, const std::string& logical, std::string& note);
    Result setRoute(const GameProfile& p, const std::string& destination, const std::atomic<bool>* cancel);
    Result cancelRoute(const GameProfile& p, const std::atomic<bool>* cancel);
    RECT zoneRect(const RECT& game, const GameProfile& p, Zone zone);
    void addAction(const std::string& text);

    std::mutex m_mutex;
    GameProfile m_profile;
    FILETIME m_profileTime{};
    std::string m_profileError;
    Status m_status;
    std::chrono::steady_clock::time_point m_statusTime{};
    HWND m_window = nullptr;
    bool m_hasLast = false;
    Cmd::Match m_last; // per "ancora"
    std::string m_lastState; // il verso dato dal modello all'ultimo comando
    std::chrono::steady_clock::time_point m_lastTime{}; // per "spegnile" (vale per qualche minuto)
    ShipStateProvider m_shipState;
    std::deque<std::string> m_actions;
    std::deque<std::string> m_unrecognized;
};
