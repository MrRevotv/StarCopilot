#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// Riquadro semi-trasparente con la traduzione della chat letta dallo schermo.
// Di solito sta appena SOTTO la zona letta (o sopra, se sotto non c'è
// spazio): mai sopra la zona stessa. Si vede nelle catture dello schermo
// (Discord, OBS), tranne quando copre la zona letta: lì il programma
// rileggerebbe e ritradurrebbe la propria traduzione. Se l'utente lo sposta,
// resta dove l'ha messo.
namespace OcrOverlay {

    struct Line {
        std::wstring prefix;  // "[SQUADRA] Trime98: " (colorato)
        std::wstring text;
        bool pending = false; // traduzione in corso: testo originale, più chiaro
    };

    void create(HINSTANCE instance);
    void destroy();

    // region: la zona letta, in coordinate dello schermo.
    void show(const RECT& region, const std::vector<Line>& lines);
    void hide();

    void setOpacity(int alpha); // 60-255
    // -1, -1 = sotto la zona letta.
    void setPosition(int x, int y);

    void unlockForMove(int seconds);
    void setOnMoved(std::function<void(int x, int y)> onMoved);

} // namespace OcrOverlay
