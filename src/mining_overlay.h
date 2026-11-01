#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// A schermo, sopra il gioco: che minerale o oggetto c'è nella signature letta
// (es. "Copper (Rame) × 4"), oppure "sconosciuta", oppure l'inserimento
// manuale in corso. Riquadro sempre in primo piano, che non prende mai il
// focus e lascia passare i clic; sfondo e scritte hanno opacità separate
// (sfondo a 0 = solo le scritte). Di solito sta appena sotto la zona letta (o
// sopra, se sotto non c'è spazio); si può spostare e resta dove lo lasci.
// Sparisce da solo dopo qualche secondo.
//
// Il contorno della zona letta (facoltativo) è disegnato FUORI dalla zona e
// non compare nelle catture dello schermo: non finisce mai nella lettura
// (in Mining Signature Calculator il bordo ciano veniva letto dentro l'immagine).
//
// LIMITE: in schermo intero ESCLUSIVO Windows non mostra finestre sopra il
// gioco: serve la finestra senza bordi (come per tutta Iris).
namespace MiningOverlay {

    enum class Size { Small, Normal, Big };

    struct Line {
        std::wstring text;
        COLORREF color = RGB(235, 237, 240);
        Size size = Size::Normal;
    };

    void create(HINSTANCE instance);
    void destroy();

    // region: la zona letta (per mettersi accanto); hasRegion=false = in alto al centro.
    // seconds: dopo quanti secondi sparisce (0 = resta finché non si chiama hide).
    void show(const RECT& region, bool hasRegion, const std::vector<Line>& lines, int seconds);
    void hide();

    // 0-255 ciascuna: sfondo (0 = niente sfondo) e scritte.
    void setOpacity(int backgroundAlpha, int textAlpha);
    // -1, -1 = accanto alla zona letta.
    void setPosition(int x, int y);
    void unlockForMove(int seconds);
    void setOnMoved(std::function<void(int x, int y)> onMoved);

    // Il contorno della zona letta.
    void setBorder(const RECT& region, bool visible);

} // namespace MiningOverlay
