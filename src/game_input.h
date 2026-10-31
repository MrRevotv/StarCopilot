#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Tasti e mouse verso il gioco, come una tastiera e un mouse veri.
// Star Citizen legge i tasti con DirectInput/RawInput: i "virtual key" li
// ignora, servono gli SCANCODE (la posizione fisica del tasto). Per il testo
// nei campi del gioco (ricerca della mappa) si digitano scancode uno per uno.
namespace GameInput {

    // La coda unica dei tasti verso il gioco: chi preme (comandi della nave,
    // messaggi in chat) la tiene finché ha finito, così un messaggio e un comando
    // non si mescolano mai (una "N" del carrello dentro una frase, o le lettere di
    // una frase prese come comandi).
    std::recursive_mutex& inputLock();

    // "F2" -> scancode (0xE0xx = tasti estesi), "0x3C" accettato. 0 = sconosciuto.
    unsigned short scanCode(const std::string& keyName);
    // "LAlt+N" -> {0x38, 0x31}. Vuoto se un tasto non esiste.
    std::vector<unsigned short> parseCombo(const std::string& combo);
    // I nomi dei tasti accettati (per l'aiuto).
    std::vector<std::string> keyNames();

    // Tutte le funzioni qui sotto: con dryRun non premono niente.
    void pressCombo(const std::string& combo, int holdMs, int gapMs, bool dryRun);
    void holdCombo(const std::string& combo, int ms, int gapMs, bool dryRun);
    // Digita lettere, cifre, spazio, - . , con gli scancode (accenti tolti: "à" -> "a").
    void typeScanCodes(const std::string& text, int perCharMs, bool dryRun);
    // Digita con caratteri Unicode (per i campi che lo accettano).
    void typeUnicode(const std::string& text, int perCharMs, bool dryRun);

    // Mouse in coordinate schermo assolute (anche su più monitor).
    void moveMouse(int x, int y, bool smooth, int steps, int stepDelayMs, bool dryRun);
    void click(int x, int y, bool smooth, int steps, int stepDelayMs, int settleMs, bool dryRun);
    POINT cursorPosition();

    // La finestra del gioco: per nome del processo ("StarCitizen" = StarCitizen.exe), se no per titolo.
    HWND findGameWindow(const std::string& processName, const std::string& windowTitle);
    bool isForeground(HWND window);
    // Porta il gioco in primo piano (anche quando Windows non vorrebbe).
    bool focus(HWND window);
    // L'area interna della finestra, in coordinate schermo.
    bool clientRect(HWND window, RECT& out);

    // Iris è amministratore? Il gioco lo è? (se il gioco sì e Iris no, Windows
    // scarta TUTTI i tasti inviati, senza dare errori).
    bool selfElevated();
    bool windowElevated(HWND window);

} // namespace GameInput
