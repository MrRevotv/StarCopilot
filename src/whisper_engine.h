#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Wrapper su whisper.cpp: carica il modello una volta e trascrive audio
// (float32, mono, 16 kHz). Thread-safe: una trascrizione alla volta.
class WhisperEngine {
public:
    WhisperEngine() = default;
    ~WhisperEngine();
    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // vadModelPath: il rilevatore di voce (Silero); vuoto = senza (trascrive tutto l'audio).
    // useGpu = false: sul processore anche se c'è la scheda video (per lasciarla al gioco).
    bool init(const std::string& modelPath, const std::string& vadModelPath, int threads, bool useGpu = true);
    bool isReady() const { return m_ready.load(); }
    // Libera il modello (e la memoria della scheda video), per ricaricarlo in un altro modo.
    void unload();

    bool onGpu() const { return m_onGpu; }
    // Memoria della scheda video occupata, misurata al caricamento (0 sul processore).
    long long gpuBytes() const { return m_gpuBytes; }

    // languageCode: lingua parlata (es. "it"). hint: nomi propri e gergo da
    // aiutare a riconoscere (non compaiono nel testo). Ritorna il testo già
    // ripulito, o stringa vuota se non c'era parlato vero (silenzio, rumore,
    // o una delle frasi che whisper inventa sul silenzio).
    std::string transcribe(const std::vector<float>& samples, const std::string& languageCode, const std::string& hint);

private:
    std::mutex m_mutex;
    std::atomic<bool> m_ready{ false };
    void* m_ctx = nullptr; // whisper_context*
    std::string m_vadPath;
    int m_threads = 4;
    bool m_onGpu = false;
    long long m_gpuBytes = 0;
};
