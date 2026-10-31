#pragma once

#include "game_profile.h"

#include <windows.h>

#include <string>
#include <vector>

// Legge il testo in una zona dello schermo (OCR di Windows, offline), con la
// posizione di ogni riga e di ogni parola: serve a cliccare sul risultato giusto.
// L'immagine si prepara come faceva la vecchia Iris: grigi, contrasto stirato e
// colori invertiti (l'OCR è fatto per testo scuro su fondo chiaro, la UI di
// Star Citizen è il contrario); se legge poco si riprova senza invertire.
// Il gioco deve essere in finestra senza bordi: in schermo intero esclusivo la
// cattura esce nera.
namespace ScreenOcr {

    struct Hit {
        std::string text;
        RECT rect{};      // coordinate schermo
        bool line = false; // riga intera (true) o parola singola
        POINT center() const { return POINT{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 }; }
    };

    struct Result {
        std::vector<Hit> hits;
        std::string error;
        std::string savedImage; // immagine di controllo salvata (se attiva)
    };

    Result read(const RECT& region, const OcrSettings& settings, const std::string& debugTag);

    // Tutto il testo letto, una riga per riga (per la diagnostica).
    std::string linesText(const Result& result);

    std::wstring debugDir(); // %appdata%\Iris\debug

} // namespace ScreenOcr
