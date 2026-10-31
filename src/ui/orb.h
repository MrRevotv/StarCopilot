#pragma once

#include <windows.h>

namespace Gdiplus {
    class Graphics;
}

// La "sfera" di Iris, stile J.A.R.V.I.S.: anelli che girano e un alone che
// respira, con colori e movimento che cambiano con lo stato:
//  - in attesa: azzurro tenue, respira piano;
//  - ti ascolta: si accende e segue la tua voce;
//  - pensa: gli anelli girano veloci, ambra;
//  - parla: pulsa con la sua voce;
//  - errore: rosso.
// La stessa sfera sta nella pagina Iris e nella finestra fluttuante (overlay.h).
namespace Orb {

    enum class Mode { Starting, Idle, Listening, Thinking, Speaking, Error };

    // Lo stato dell'animazione (energia livellata, rotazione, colore che sfuma).
    struct Animation {
        Mode mode = Mode::Starting;
        float energy = 0.0f;
        float spin = 0.0f;
        float tint[3] = { 30, 170, 220 };
        unsigned long long lastTick = 0;
    };

    // Un passo dell'animazione (~30 al secondo): livelli = volume (0..1, RMS) di chi parla.
    void step(Animation& a, Mode mode, float micLevel, float voiceLevel, bool userSpeaking);

    // La disegna al centro di un'area w x h (su sfondo trasparente o colorato, a scelta di chi
    // chiama). radius: il raggio del nucleo rispetto al lato corto; l'alone arriva a circa 2,2 volte.
    void draw(Gdiplus::Graphics& g, float w, float h, const Animation& a, float radius = 0.30f);

    // La sfera dentro una finestra (pagina Iris).
    HWND create(HWND parent, int id);
    void update(HWND orb, Mode mode, float micLevel, float voiceLevel, bool userSpeaking);

} // namespace Orb
