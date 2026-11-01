#pragma once

#include <windows.h>

// Scelta di una zona dello schermo: si "fotografa" lo schermo (tutti i
// monitor), lo si mostra scurito e l'utente trascina un rettangolo; dentro il
// rettangolo l'immagine torna chiara. Esc o tasto destro annullano.
// Con `rotatable` il rettangolo si può anche girare un poco (l'HUD di Star
// Citizen segue la curva del casco): rotellina del mouse o frecce ← →, e si
// conferma con Invio.
// Bloccante: ritorna quando l'utente ha finito.
namespace RegionSelector {

    struct Options {
        const wchar_t* title = L"Trascina un rettangolo intorno alla chat del gioco";
        const wchar_t* hint = L"Comprendi i nomi dei giocatori. Esc o tasto destro per annullare.";
        bool rotatable = false;
        double angle = 0.0; // rotazione di partenza, in gradi (+ = senso orario)
        RECT initial{};     // zona di partenza (con rotatable): la si può solo girare, senza ridisegnarla
    };

    // out: il rettangolo dritto, in coordinate dello schermo (pixel veri); angle
    // (se non nullo): la rotazione scelta attorno al suo centro. false =
    // annullato o zona troppo piccola.
    bool selectRegion(HINSTANCE instance, RECT& out, const Options& options = Options{}, double* angle = nullptr);

} // namespace RegionSelector
