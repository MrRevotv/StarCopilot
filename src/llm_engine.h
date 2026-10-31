#pragma once

#include "translation_prompt.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Il "cervello" di Iris: llama.cpp con un modello da chat (Qwen2.5 7B Instruct).
// - Sulla scheda video se c'è (con un limite di memoria, sulla scheda vanno gli
//   strati che ci stanno e il resto sul processore). Come in Voice to Chat.
// - La risposta arriva un pezzo alla volta (onPiece), così la voce parte prima.
// - Riuso del lavoro già fatto: della conversazione si rielabora solo la parte
//   nuova (le istruzioni e i messaggi di prima restano nella memoria del modello).
// - Lo STESSO modello traduce anche per la chat di gioco (le funzioni di Voice
//   to Chat), con due "canali" suoi, ognuno con la sua memoria: voce e chat letta
//   dallo schermo. Così Iris e le traduzioni non si rubano il lavoro già fatto,
//   e il modello sta in memoria una volta sola (4,7 GB risparmiati).
// - Thread-safe: una richiesta alla volta. Interrompibile con "cancel".
class LlmEngine {
public:
    enum class Result { Ok, Cancelled, Error, TooLong };
    enum class Channel { Voice = 0, Screen = 1 };

    struct Sampling {
        float temperature = 0.4f;
        float topP = 0.9f;
        int topK = 40;
        float repeatPenalty = 1.05f;
        int maxTokens = 700;
    };

    LlmEngine() = default;
    ~LlmEngine();
    LlmEngine(const LlmEngine&) = delete;
    LlmEngine& operator=(const LlmEngine&) = delete;

    // gpuBudget: byte della scheda video che può usare; -1 = senza limite, 0 = niente scheda.
    bool init(const std::string& modelPath, int threads, long long gpuBudget = -1);
    bool isReady() const { return m_ready.load(); }
    void unload();

    int gpuLayers() const { return m_gpuLayers; }
    int totalLayers() const { return m_totalLayers; }
    long long gpuBytes() const { return m_gpuBytes; }
    int contextSize() const { return m_nCtx; }
    // Il nome scritto nel file del modello ("Qwen2.5 7B Instruct").
    std::string modelName() const { return m_modelName; }
    // Qwen3 e simili "ragionano" prima di rispondere: per la voce si salta.
    bool isQwen3() const { return m_qwen3; }

    int countTokens(const std::string& text);

    // onPiece riceve ogni pezzo di testo generato; ritorna false per fermarsi lì.
    // out: tutto il testo generato.
    Result generate(const std::string& prompt, const Sampling& sampling, const std::function<bool(const std::string&)>& onPiece,
                    std::string& out, const std::atomic<bool>* cancel = nullptr);

    // Traduzione per la chat di gioco (come Voice to Chat): out riceve la
    // traduzione ripulita (vedi TranslationPrompt::cleanOutput).
    Result translate(const TranslationRequest& request, std::string& out, Channel channel = Channel::Voice,
                     const std::atomic<bool>* cancel = nullptr);

private:
    struct Slot {
        void* ctx = nullptr; // llama_context*
        std::string cachedPrefix;
        int prefixTokens = 0;
    };

    bool load(const std::string& modelPath, int threads, void* gpu, int gpuLayers);
    bool evalTokens(const std::vector<int>& tokens, const std::atomic<bool>* cancel);
    bool evalTokensIn(void* ctx, const std::vector<int>& tokens, const std::atomic<bool>* cancel);
    void release();

    std::mutex m_mutex;
    std::atomic<bool> m_ready{ false };
    void* m_model = nullptr; // llama_model*
    void* m_ctx = nullptr;   // llama_context*
    Slot m_slots[2];         // traduzioni: voce, schermo
    void* m_greedy = nullptr; // llama_sampler*: per tradurre sempre la parola più probabile
    std::vector<int> m_cached; // i token che sono adesso nella memoria del modello
    int m_nCtx = 0;
    int m_nBatch = 0;
    int m_gpuLayers = 0;
    int m_totalLayers = 0;
    long long m_gpuBytes = 0;
    std::string m_modelName;
    bool m_qwen3 = false;
};
