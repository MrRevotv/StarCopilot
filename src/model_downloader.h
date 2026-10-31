#pragma once

#include <atomic>
#include <functional>
#include <string>

// Scarica i modelli con WinHTTP (già in Windows, nessuna libreria in più).
// - Si scarica in "nome.part": il file vero compare solo a download finito
//   e controllato. Un download interrotto non lascia mai un modello rotto.
// - Se c'è già un .part, si riprende da dove si era fermato.
// - Controlla la risposta del server (200/206) e che i byte arrivati siano
//   quelli annunciati; riprova da solo se la connessione cade.
namespace ModelDownloader {

    using ProgressCallback = std::function<void(long long downloaded, long long total)>;

    bool downloadFile(const std::wstring& url, const std::wstring& destPath, const ProgressCallback& onProgress,
                      const std::atomic<bool>* cancel, std::string& error);

    // Il file comincia come un modello vero? "ggml" (whisper) o "GGUF" (llama).
    // Serve a riconoscere un file rovinato da una versione precedente.
    enum class Kind { Whisper, Gguf };
    bool looksValid(const std::wstring& path, Kind kind);

} // namespace ModelDownloader
