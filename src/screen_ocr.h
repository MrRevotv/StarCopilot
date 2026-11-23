#pragma once

#include "game_profile.h"
#include "screen_capture.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Legge il testo in una zona dello schermo (OCR di Windows, offline), con la
// posizione di ogni riga e di ogni parola: serve a cliccare sul risultato giusto.
// L'immagine si prepara come faceva la vecchia Iris: grigi, contrasto stirato e
// colori invertiti (l'OCR è fatto per testo scuro su fondo chiaro, la UI di
// Star Citizen è il contrario); se legge poco si riprova senza invertire.
// Per i campi dove si sa cosa cercare (rotta sulla mappa) ci sono più
// preparazioni: il testo bianco sul vetro chiaro della ricerca, con il solo
// contrasto stirato, si perde (testo 240, vetro 210: dopo lo stiramento sono quasi uguali).
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
        std::string variant;    // la preparazione che ha dato questa lettura ("invertita", "contrasto locale"...)
    };

    Result read(const RECT& region, const OcrSettings& settings, const std::string& debugTag);

    // Come read, ma prova più preparazioni della stessa cattura (invertita, contrasto locale,
    // normale, solo il testo chiaro) finché accept() dice che la lettura va bene; se nessuna va
    // bene, quella con più lettere. Un motore OCR solo per tutte le prove.
    Result readUntil(const RECT& region, const OcrSettings& settings, const std::string& debugTag,
                     const std::function<bool(const Result&)>& accept);

    // Lo stesso su un'immagine già catturata (region = dove stava sullo schermo): per le prove
    // e per rileggere le immagini salvate. accept vuoto = la lettura semplice di read().
    Result readImage(const ScreenCapture::Image& shot, const RECT& region, const OcrSettings& settings, const std::string& debugTag,
                     const std::function<bool(const Result&)>& accept);

    // Tutto il testo letto, una riga per riga (per la diagnostica).
    std::string linesText(const Result& result);

    // Una cattura veloce in grigi, per vedere se una zona cambia (il testo che compare in un campo).
    struct Shot {
        std::vector<uint8_t> gray;
        int width = 0, height = 0;
        bool empty() const { return gray.empty(); }
    };
    Shot grab(const RECT& region);
    Shot grayOf(const ScreenCapture::Image& image);
    // Quanta parte della zona è testo chiaro comparso o sparito tra le due catture (0..1).
    // Il cursore che lampeggia resta sotto lo 0,2%; una lettera scritta nel campo della mappa
    // vale circa lo 0,3%, un nome intero dall'1% in su. Catture di misura diversa: 1.
    double textChange(const Shot& before, const Shot& after);

    std::wstring debugDir(); // %appdata%\Iris\debug

} // namespace ScreenOcr
