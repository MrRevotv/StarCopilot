#pragma once

#include "ui/orb.h"

#include <windows.h>

#include <functional>

// La sfera di Iris da sola, in una finestra senza sfondo (trasparenza vera,
// pixel per pixel), sempre in primo piano anche mentre giochi. Si accende,
// gira e cambia colore come quella della pagina Iris (in ascolto, pensa,
// parla, errore). Normalmente è "bloccata": i clic la attraversano e arrivano
// al gioco, e non prende mai il focus. Si può sbloccare per qualche secondo
// per spostarla col mouse.
//
// Non compare nelle catture dello schermo (nemmeno in quella della
// traduzione della chat, che altrimenti potrebbe leggerla).
//
// LIMITE: in schermo intero ESCLUSIVO Windows non mostra finestre sopra il
// gioco. Funziona in finestra o "finestra senza bordi" (borderless).
namespace Overlay {

    void create(HINSTANCE instance);
    void destroy();

    void setVisible(bool visible);
    // ~30 volte al secondo: stato e livelli (volume RMS 0..1) per l'animazione.
    void animate(Orb::Mode mode, float micLevel, float voiceLevel, bool userSpeaking);
    void setOpacity(int alpha); // 60-255
    void setSize(int size);     // lato della finestra, in pixel a 96 DPI
    // -1, -1 = angolo in basso a destra dello schermo principale.
    void setPosition(int x, int y);

    // Si può trascinare per "seconds" secondi, poi si riblocca da sola e
    // chiama onMoved con la nuova posizione (da salvare).
    void unlockForMove(int seconds);
    void setOnMoved(std::function<void(int x, int y)> onMoved);

} // namespace Overlay
