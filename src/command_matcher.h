#pragma once

#include "game_profile.h"

#include <map>
#include <string>
#include <vector>

// Dalla frase detta al comando di gioco, senza modello: veloce (meno di un
// millisecondo), deterministico e ispezionabile (dice sempre il punteggio).
// L'algoritmo della vecchia Iris (frasi con "slot", distanza di Levenshtein +
// insieme di parole), con in più: accenti ignorati, correzioni anche di più
// parole ("a nulla rotta" -> "annulla rotta"), nomi dei luoghi trovati anche
// storpiati. Funzioni pure: provate in tests/tests.cpp.
namespace Cmd {

    struct Match {
        bool found = false;
        GameCommand command;                       // copia: il profilo può essere ricaricato nel frattempo
        double score = 0.0;
        std::map<std::string, std::string> slots; // es. destination = "Stazione Orbitale Seraphim"
        std::string phrase;                        // la frase del dizionario che ha vinto
    };

    // Minuscolo, senza accenti né punteggiatura, spazi singoli ("Modalità VTOL!" -> "modalita vtol").
    std::string normalize(const std::string& text);
    // Somiglianza 0..1: lettere (Levenshtein) e parole in comune (l'ordine conta poco).
    double similarity(const std::string& a, const std::string& b);
    // Applica le correzioni del profilo (parole intere, anche più di una).
    std::string applyCorrections(const std::string& normalized, const std::map<std::string, std::string>& corrections);

    // Il comando migliore per la frase; found = false sotto la soglia del profilo
    // (in "best" resta comunque il candidato più vicino, per il registro).
    Match match(const GameProfile& profile, const std::string& spoken);

    // Il nome vero di un luogo detto a voce ("sera a fino" -> "Stazione Orbitale Seraphim").
    // Vuoto se nessuno somiglia abbastanza.
    std::string resolveAlias(const GameProfile& profile, const std::string& slot, const std::string& spoken, double* score = nullptr);

    // Il testo letto dall'OCR più simile a quello cercato (indice, o -1 sotto la soglia).
    int bestText(const std::vector<std::string>& candidates, const std::string& target, double threshold, double* score = nullptr);

} // namespace Cmd
