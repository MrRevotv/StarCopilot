#pragma once

#include <windows.h>

// Scelta di una zona dello schermo: si "fotografa" lo schermo (tutti i
// monitor), lo si mostra scurito e l'utente trascina un rettangolo; dentro il
// rettangolo l'immagine torna chiara. Esc o tasto destro annullano.
// Bloccante: ritorna quando l'utente ha finito.
namespace RegionSelector {

    // out: coordinate dello schermo (pixel veri). false = annullato o zona troppo piccola.
    bool selectRegion(HINSTANCE instance, RECT& out);

} // namespace RegionSelector
