#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// La voce neurale di Iris: Qwen3-TTS 1.7B (Alibaba, licenza Apache 2.0),
// attraverso llama.cpp + mtmd, gli stessi della parte che ragiona. Molto più
// naturale delle voci di Windows; sulla scheda video genera la voce circa 4
// volte più veloce del tempo reale.
//
// Due file in %appdata%\VoiceToChat\models: il modello (1,8 GB) e il
// decodificatore audio "mmproj" (0,45 GB). La voce è quella di base del
// modello (femminile): nessuna voce di riferimento.
class NeuralVoice {
public:
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

    // Una frase -> voce. onChunk riceve i pezzi di audio appena pronti (float
    // mono a sampleRate()); se ritorna false ci si ferma. shouldStop (facoltativa)
    // si controlla a ogni fotogramma: Iris zittita smette subito di generare.
    bool speak(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
               const std::function<bool()>& shouldStop = {});

private:
    void release();

    std::mutex m_mutex;
    std::atomic<bool> m_ready{ false };
    void* m_model = nullptr;   // llama_model*
    void* m_ctx = nullptr;     // llama_context*
    void* m_mtmd = nullptr;    // mtmd_context*
    void* m_gen = nullptr;     // mtmd_helper_gen_audio*
    void* m_sampler = nullptr; // llama_sampler*
    int m_sampleRate = 24000;
    long long m_gpuBytes = 0;
};
