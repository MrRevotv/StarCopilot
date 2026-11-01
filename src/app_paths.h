#pragma once

#include <string>

// Cartelle dei dati di Iris:
//  - %appdata%\Iris: impostazioni, memoria (le cose che le chiedi di ricordare), registro;
//  - i modelli stanno in %appdata%\VoiceToChat\models, CONDIVISI con Voice to
//    Chat: sono gli stessi file (6 GB), inutile scaricarli due volte.
// (La vecchia Iris in C# usava %appdata%\Iris\config: quella cartella non si tocca.)
namespace AppPaths {

    std::wstring getAppDataDir();   // %appdata%\Iris (o la cartella scelta con --cartella-dati)
    std::wstring getModelsDir();    // %appdata%\VoiceToChat\models

    // Impostazioni, memoria e registro in un'altra cartella: per le prove.
    void setDataDirOverride(const std::wstring& dir);
    bool dataDirOverridden();

    // Crea le cartelle se non esistono. false solo in caso di errore vero.
    bool ensureDirectoriesExist();

    std::wstring whisperModelPath();
    std::wstring vadModelPath();
    // fileName vuoto = il modello predefinito (Qwen2.5 7B Instruct).
    std::wstring llmModelPath(const std::string& fileName = {});
    std::wstring neuralVoiceModelPath();  // Qwen3-TTS (voce neurale)
    std::wstring neuralVoiceMmprojPath(); // il suo decodificatore audio
    std::wstring settingsPath();
    std::wstring memoryPath();      // memoria.txt: una cosa da ricordare per riga
    std::wstring logPath();
    // Chat di gioco (le funzioni di Voice to Chat, dentro Iris).
    std::wstring glossaryPath();    // glossary.txt: il gergo della community ("QT=quantum travel")
    std::wstring ocrDebugPath();    // cosa ha letto l'OCR nella chat e come l'ha divisa
    // I file di Voice to Chat, letti (mai scritti) per importarne le impostazioni al primo avvio.
    std::wstring voiceToChatSettingsPath();
    std::wstring voiceToChatGlossaryPath();

    // Percorso per le API C di whisper.cpp/llama.cpp (che vogliono char*).
    // Se contiene caratteri non ASCII usiamo il nome corto 8.3; altrimenti UTF-8.
    std::string toLibraryPath(const std::wstring& widePath);

} // namespace AppPaths
