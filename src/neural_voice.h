#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// La voce neurale di Iris: Qwen3-TTS 1.7B (Alibaba, licenza Apache 2.0),
// attraverso llama.cpp + mtmd, gli stessi della parte che ragiona. Molto più
// naturale delle voci di Windows; sulla scheda video genera la voce più veloce
// del tempo reale.
//
// Due file in %appdata%\VoiceToChat\models: il modello (1,8 GB) e il
// decodificatore audio "mmproj" (0,45 GB).
//
// La voce è SEMPRE la stessa: il modello "Base" è fatto per imitare una voce e,
// senza un esempio, si inventa un parlante a ogni frase (e a volte lo cambia a
// metà). Per questo ogni frase parte dallo stesso "timbro" (x-vector: il
// codificatore di parlante del mmproj lo ricava da qualche secondo di voce di
// riferimento) e con semi fissi (stesso testo = stesso audio). Il riferimento:
//  - una delle voci predefinite: la prima volta il modello la genera da sé con
//    un seme fisso (niente da scaricare) e la salva in %appdata%\Iris\voci;
//  - oppure un tuo file WAV (5-15 s di parlato pulito, una persona sola).
class NeuralVoice {
public:
    struct Preset {
        const char* id;    // nelle impostazioni ("aurora", ...)
        const char* label; // nella finestra
        uint32_t seed;     // il seme con cui il modello la genera
    };
    static const std::vector<Preset>& presets(); // la prima è la predefinita
    static bool isPreset(const std::string& id);
    static constexpr const char* kFileVoice = "file"; // "usa un tuo file WAV"

    // Campionamento (di base quello consigliato da Qwen3-TTS). Solo per le prove.
    struct Sampling {
        float temperature = 0.9f;
        int topK = 50;
        float topP = 1.0f;
        float minP = 0.0f;
        float repeatPenalty = 1.05f;
        uint32_t seed = 20251122;
        int firstChunk = 0; // fotogrammi del primo pezzo di audio (0 = blocchi da 72 della libreria; 12 = prima, ma fa un clic)
    };

    NeuralVoice() = default;
    ~NeuralVoice();
    NeuralVoice(const NeuralVoice&) = delete;
    NeuralVoice& operator=(const NeuralVoice&) = delete;

    bool load(const std::string& modelPath, const std::string& mmprojPath, int threads);
    bool isReady() const { return m_ready.load(); }
    void unload();

    // Memoria della scheda video occupata, misurata al caricamento.
    long long gpuBytes() const { return m_gpuBytes; }
    int sampleRate() const { return m_sampleRate; }

    // Quale voce: l'id di una predefinita, oppure kFileVoice e il percorso del WAV.
    // Non blocca: la voce si prepara alla frase successiva (o con prepare()).
    void setVoice(const std::string& id, const std::string& wavPath = {});
    // Prepara subito la voce scelta (la prima volta che si usa una predefinita la
    // genera e la salva: qualche secondo) e scalda la scheda video.
    bool prepare();
    // La voce in uso, a parole (per il registro).
    std::string voiceName() const;

    // Una frase -> voce. onChunk riceve i pezzi di audio appena pronti (float
    // mono a sampleRate()); se ritorna false ci si ferma. shouldStop (facoltativa)
    // si controlla a ogni fotogramma: Iris zittita smette subito di generare.
    // false = non riuscita (anche: nessun audio). attempt > 0 = un altro tentativo
    // con un altro seme.
    bool speak(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
               const std::function<bool()>& shouldStop = {}, int attempt = 0);

    // Il "timbro" di un audio (x-vector del codificatore di parlante del mmproj):
    // due audio della stessa voce danno vettori simili (vedi VoiceMeasure::cosine).
    bool speakerEmbedding(const float* pcm, size_t count, int rate, std::vector<float>& out);

    // --- Per le prove ---
    void setSampling(const Sampling& s);
    // Usa direttamente questo audio come riferimento (vuoto = nessuno: la voce a caso di prima).
    bool setReferenceAudio(const std::vector<float>& pcm, int rate);
    // Genera il riferimento di una voce predefinita (senza salvarlo).
    bool makePresetReference(const Preset& preset, std::vector<float>& out);
    // Il file in cui si salva il riferimento di una voce predefinita.
    static std::wstring presetPath(const std::string& id);

    // Un file WAV (PCM 16/24/32 bit o float, mono o stereo) -> campioni mono.
    static bool readWav(const std::wstring& path, std::vector<float>& mono, int& rate, std::string* error = nullptr);
    static bool writeWav(const std::wstring& path, const std::vector<float>& mono, int rate);
    // Un riferimento usabile: senza i silenzi in testa e in coda, al massimo 15 s, volume
    // pareggiato. false (e il motivo) se è troppo corto o muto.
    static bool cleanReference(std::vector<float>& mono, int rate, std::string* error = nullptr);

private:
    void release();
    bool prepareLocked();
    bool useReferenceLocked(const std::vector<float>& pcm, int rate);
    bool generateLocked(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
                        const std::function<bool()>& shouldStop, uint32_t seed, bool withReference, int* framesOut);
    void buildSampler(uint32_t seed);

    mutable std::mutex m_mutex;
    std::atomic<bool> m_ready{ false };
    void* m_model = nullptr;   // llama_model*
    void* m_ctx = nullptr;     // llama_context*
    void* m_mtmd = nullptr;    // mtmd_context*
    void* m_gen = nullptr;     // mtmd_helper_gen_audio*
    void* m_sampler = nullptr; // llama_sampler*
    void* m_reference = nullptr; // mtmd_bitmap* della voce di riferimento (nullptr = nessuna)
    int m_sampleRate = 24000;
    long long m_gpuBytes = 0;
    Sampling m_sampling;
    uint32_t m_samplerSeed = 0;
    std::vector<int32_t> m_suppressed; // codici speciali del modello che non devono uscire (tranne la fine)
    std::string m_activeVoice;         // la voce preparata ("aurora", "file:C:\...")
    std::string m_activeName;

    mutable std::mutex m_wantMutex; // la voce scelta: si cambia senza aspettare la frase in corso
    std::string m_wantedId;
    std::string m_wantedFile;
    bool m_wantChanged = false; // setVoice dopo setReferenceAudio: si torna alla voce scelta
};

// Misure oggettive su una voce (per le prove: non possiamo ascoltare).
namespace VoiceMeasure {
    // Frequenza fondamentale (F0, Hz) ogni 10 ms, metodo YIN; 0 = niente voce.
    std::vector<float> pitch(const float* samples, size_t count, int rate);
    // Centroide spettrale (Hz), mediana sui pezzi con voce: più alto = voce più chiara.
    double centroid(const float* samples, size_t count, int rate);
    // Mediana dei valori > 0 (0 se non ce ne sono).
    double median(std::vector<float> values);
    // Somiglianza tra due vettori: 1 = uguali, 0 = niente in comune.
    double cosine(const std::vector<float>& a, const std::vector<float>& b);
    // Ricampionamento lineare (la voce neurale è a 24 kHz, whisper vuole 16 kHz...).
    std::vector<float> resample(const float* samples, size_t count, int fromRate, int toRate);
}
