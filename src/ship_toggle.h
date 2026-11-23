#pragma once

#include "sc_telemetry.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// I tasti "interruttore" della nave (luci, armi, scudi, carrello...) con la telemetria del gioco
// (vedi sc_telemetry.h). Prima di premere si guarda com'è la nave: "accendi le armi" con le armi
// già accese non preme niente (prima le spegneva). Dopo il tasto si guarda se è cambiato davvero.
// Senza telemetria (gioco chiuso, telemetria spenta, a piedi, al menu) si preme come sempre.
// Qui niente gioco e niente tasti: lo stato arriva da una funzione e i tasti li preme chi chiama
// (GameController), così tutto si prova senza il gioco (tests/tests.cpp).
namespace ShipToggle {

    // Il campo della telemetria che dice com'è un tasto del profilo.
    enum class Field { None, Lights, Weapons, Shields, Engines, Power, Gear, Decoupled, Cruise };

    // Com'è adesso. On = acceso, scudi alzati, carrello abbassato (fuori), modalità disaccoppiata.
    // TurningOn / TurningOff solo per il carrello: sta scendendo / sta salendo.
    enum class Value { Unknown, Off, On, TurningOn, TurningOff };

    // Dal nome del tasto nel profilo ("weapons", "landing_gear"...). None = non si controlla
    // (anche il VTOL: la telemetria lo dà sbagliato, DEPLOYED anche su navi che non ce l'hanno).
    Field fieldFor(const std::string& logicalKey);
    // Il valore nello stato; Unknown se il gioco non manda quella parte (a piedi, nave appena presa...).
    Value read(Field field, const ScTelemetry::ShipState& state);

    // Il verso voluto: +1 = On (accendi, alza gli scudi, abbassa il carrello, disaccoppiato),
    // -1 = Off (spegni, abbassa gli scudi, alza o tira su il carrello, accoppiato), 0 = non detto
    // ("armi", "carrello"). Dalla frase sentita, se non dice niente da quella del dizionario.
    int wanted(Field field, const std::string& heard, const std::string& phrase);
    // Dallo strumento del modello (game_command, "state"): "on"/"off"; "up"/"down" (per il carrello
    // down = abbassato, up = alzato; "on"/"off" sul carrello non si capiscono e valgono 0).
    int wantedFromState(Field field, const std::string& state);
    // Il verso di un comando intero (le sequenze, "prepara la nave"): quello delle sue frasi, se
    // vanno tutte d'accordo ("accendi tutto" = +1); 0 se nessuna lo dice o non sono d'accordo.
    int commandDirection(Field field, const std::vector<std::string>& phrases);
    // Una richiesta libera (quella che segue il modello) a pezzi, normalizzati: "alza gli scudi, poi
    // carrello" -> { "alza gli scudi", "carrello" } (virgole, punti, "e", "poi"...).
    std::vector<std::string> clauses(const std::string& request);
    // Il primo pezzo che parla di questo interruttore ("" se nessuno): il verso si prende solo da lì,
    // così il verbo di un'altra azione non vale per lui.
    std::string clauseAbout(Field field, const std::vector<std::string>& clauses);

    // In italiano: "armi", "carrello" (per le sequenze); "accese", "in discesa" (il valore).
    std::string name(Field field);
    std::string valueText(Field field, Value value);

    // Cosa fare prima di premere.
    struct Plan {
        bool press = true;             // false: è già come lo vuoi, niente tasto
        Value target = Value::Unknown; // dopo il tasto deve diventare così (Unknown = non si controlla)
        std::string spoken, brief;     // con press = false: "Le armi sono già accese.", "Già accese."
    };
    Plan plan(Field field, int want, Value now);

    // Dopo il tasto: arrivato, o in movimento verso target (il carrello che scende)?
    bool reached(Value target, Value now);
    // Le risposte dopo il tasto: "Armi spente." / "Ho premuto P, ma le armi risultano ancora accese."
    std::string doneText(Field field, Value now);
    std::string doneBrief(Field field, Value now);
    std::string notChangedText(Field field, Value now, const std::string& combo);
    // "le armi risultano ancora accese" (per le sequenze).
    std::string stillText(Field field, Value now);

    // Quello che serve per premere e aspettare: dal GameController; nelle prove, finto.
    struct Env {
        std::function<std::optional<ScTelemetry::ShipState>()> state; // la telemetria adesso (nullopt = non si sa)
        // Preme un tasto del profilo: la combinazione premuta ("P") o il motivo; ok = false se il tasto manca.
        std::function<std::string(const std::string& logical, bool& ok)> press;
        std::function<bool(int ms)> wait;  // aspetta; false = interrotto
        std::function<bool()> pause;       // la pausa tra due tasti di una sequenza; false = interrotto
        std::function<bool()> cancelled;   // interrotto (una richiesta nuova, "basta")
        bool dryRun = false;               // modalità prova: lo stato si guarda, i tasti no e non si aspetta
        int verifyMs = 2500;               // dopo il tasto, fin quando si aspetta che lo stato cambi
        int pollMs = 50;
        int settleMs = 1000;               // dopo l'alimentazione: tanto tempo senza cambiamenti
        int settleMaxMs = 6000;            // ma non di più
    };

    struct Outcome {
        bool used = false;        // false: la telemetria non è servita (o si è persa): vale il risultato di sempre
        bool ok = true;
        bool interrupted = false; // sequenza interrotta a metà
        std::string spoken, brief, shown;
    };

    // Dopo il tasto: aspetta (fino a verifyMs) che lo stato diventi target. used = false se la
    // telemetria si perde, se target non si sa, in prova o se si interrompe.
    Outcome afterPress(Field field, Value target, const std::string& combo, Env& env);

    // Una sequenza ("power,engines,shields,weapons") con la telemetria: i passi già a posto si saltano,
    // ogni passo si valuta appena prima di premerlo; dopo l'alimentazione si aspetta che la nave si
    // assesti (i sottosistemi possono tornare da soli); alla fine si controlla che tutto sia cambiato.
    // wantFor: il verso di ogni campo. sayText: la conferma del comando ("Accendo tutto.").
    Outcome sequence(const std::vector<std::string>& steps, const std::function<int(Field)>& wantFor, const std::string& sayText, Env& env);
    // Nella sequenza non c'è niente da premere (tutti i passi noti e già a posto)?
    bool nothingToPress(const std::vector<std::string>& steps, const std::function<int(Field)>& wantFor, const ScTelemetry::ShipState& state);
    // I passi di una sequenza: "power, engines" -> { "power", "engines" }.
    std::vector<std::string> splitSteps(const std::string& keys);

} // namespace ShipToggle
