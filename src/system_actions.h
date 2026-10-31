#pragma once

#include <string>
#include <vector>

// Azioni sul PC: aprire programmi e siti, volume, tasti multimediali.
// Le funzioni che usano COM vanno chiamate da un thread con COM inizializzato.
namespace SystemActions {

    struct App {
        std::string name;        // come compare nel menu Start ("Blocco note", "Spotify")
        std::wstring launchPath; // "shell:AppsFolder\..." da aprire
    };

    // Tutti i programmi del menu Start, anche quelli dello Store (tenuti in memoria qualche minuto).
    std::vector<App> listApps();

    // Il programma che somiglia di più a quello che hai detto ("spotifai" -> Spotify).
    // -1 se nessuno somiglia abbastanza. Pura: provata in tests/tests.cpp.
    int bestAppMatch(const std::vector<std::string>& names, const std::string& query, double* score = nullptr);

    // Apre il programma; out = il nome trovato, oppure il motivo per cui non è stato aperto.
    bool launchApp(const std::string& query, std::string& out);
    // Apre un sito nel browser predefinito ("youtube.com" -> https://youtube.com).
    bool openUrl(const std::string& url, std::string& out);

    // action: "set" (level 0-100), "up", "down", "mute", "unmute". Ritorna come è ora il volume.
    bool volume(const std::string& action, int level, std::string& out);
    // action: "play_pause", "next", "previous", "stop".
    bool mediaKey(const std::string& action, std::string& out);

} // namespace SystemActions
