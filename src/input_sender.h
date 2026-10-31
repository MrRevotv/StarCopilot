#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Scrive il testo nella chat del gioco che ha il focus, come una tastiera
// vera (SendInput con scan code: molte chat di gioco ignorano l'input "Unicode").
// - Usa il layout di tastiera della finestra del gioco (italiano, inglese...),
//   compresi i caratteri con AltGr (@ # [ ] { } sulla tastiera italiana).
// - Prima di scrivere aspetta che Shift/Ctrl/Alt siano rilasciati, altrimenti
//   le lettere uscirebbero modificate (MAIUSCOLE, scorciatoie...).
// - I caratteri che la tastiera non ha (ñ, ü, ß... su tastiera italiana) si
//   sostituiscono con la lettera semplice, oppure si mandano come Unicode.
namespace InputSender {

    enum class Unmappable { Transliterate, Unicode };

    struct Options {
        bool openChat = true;
        int openKeyVk = VK_RETURN;
        bool autoSend = true;
        int sendKeyVk = VK_RETURN;
        int delayAfterOpenMs = 180;  // l'animazione di apertura della chat
        int delayBeforeSendMs = 90;
        int interCharDelayMs = 12;   // alcuni giochi perdono caratteri se si scrive troppo veloce
        Unmappable unmappable = Unmappable::Transliterate;
    };

    // Un tasto da premere: scan code con i modificatori, o un carattere Unicode.
    struct Stroke {
        bool unicode = false;
        wchar_t ch = 0;
        WORD scan = 0;
        bool extended = false;
        bool shift = false, ctrl = false, alt = false; // ctrl+alt = AltGr
    };

    // Come scrivere text con il layout dato. dropped riceve i caratteri che non
    // si possono scrivere in nessun modo (con Transliterate e senza equivalente).
    std::vector<Stroke> planText(const std::wstring& text, HKL layout, Unmappable mode, std::wstring* dropped = nullptr);

    // Aspetta che Shift, Ctrl, Alt e Win siano rilasciati (al massimo timeoutMs).
    bool waitForModifiersReleased(int timeoutMs);

    void pressKey(int virtualKey);

    // Apre la chat (se richiesto), scrive, invia (se richiesto). Il layout è
    // quello della finestra in primo piano.
    void sendChatMessage(const std::string& utf8Text, const Options& options);

    // Layout di tastiera della finestra in primo piano.
    HKL foregroundLayout();

} // namespace InputSender
