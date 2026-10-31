#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NeuralVoice;

// La voce di Iris: le voci di Windows (Elsa, Cosimo...), senza librerie in più.
// - Le frasi si mettono in coda: mentre una suona, la successiva si prepara.
// - stop() la zittisce subito (la chiami mentre parla, o dici "basta").
// - level() è il volume di quello che sta dicendo, per l'animazione.
// - Suoni brevi (il "ding" quando la chiami, il timer) generati qui.
class TtsEngine {
public:
    enum class Chime { Wake, Timer, Error, Confirm };

    TtsEngine() = default;
    ~TtsEngine();
    TtsEngine(const TtsEngine&) = delete;
    TtsEngine& operator=(const TtsEngine&) = delete;

    bool start();    // apre l'uscita audio e il thread della voce
    void shutdown();

    // Le voci installate ("Microsoft Elsa", "Microsoft Cosimo"...).
    static std::vector<std::string> voices();

    // Sintesi senza suonare: campioni a 24 kHz (per le prove: "Iris" detta da Elsa
    // passa da Silero e whisper come se arrivasse dal microfono). Va chiamata da
    // un thread con COM multi-threaded (o senza COM).
    static std::vector<float> synthesize(const std::string& text, const std::string& voiceName);

    // Le uscite audio (cuffie, casse...) e quella predefinita di Windows adesso.
    static std::vector<std::string> outputDevices();
    static std::string defaultOutputName();
    // Dove parla Iris: "" = l'uscita predefinita di Windows. Si riapre subito.
    void setOutputDevice(const std::string& name);
    // Riapre l'uscita (la predefinita di Windows è cambiata, cuffie ricollegate...).
    void reopenOutput();

    void setVoice(const std::string& namePart);
    void setRate(double rate); // 0.5 - 2.0 (solo voci di Windows)
    // Effetto sulla voce ("nessuno", "ia", "androide", "robot": vedi VoiceEffect), per ogni motore.
    void setEffect(const std::string& id);

    // La voce neurale (Qwen3-TTS): se c'è ed è accesa, parla lei; se una frase
    // non le riesce, la dice la voce di Windows.
    void setNeuralVoice(NeuralVoice* voice) { m_neural.store(voice); }
    void setUseNeural(bool on) { m_useNeural = on; }
    bool usingNeural() const;

    void speak(const std::string& text);
    void chime(Chime kind);
    void stop();

    // In coda, in preparazione o in riproduzione.
    bool isSpeaking() const;
    float level() const { return m_level.load(); }

    // Chiamata dal thread audio di miniaudio: non usarla direttamente.
    void onPlayback(float* out, unsigned int frames);

    static constexpr int kSampleRate = 24000;

private:
    void synthLoop();
    void enqueueSamples(std::vector<float> samples, unsigned long long generation);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_texts;
    std::string m_voice = "Elsa";
    double m_rate = 1.0;
    bool m_voiceChanged = true;
    std::string m_effect = "nessuno";
    std::atomic<bool> m_synthesizing{ false };
    std::atomic<bool> m_running{ false };
    std::thread m_thread;
    std::atomic<unsigned long long> m_generation{ 0 }; // cambia a ogni stop(): quello preparato prima si butta
    std::atomic<NeuralVoice*> m_neural{ nullptr };
    std::atomic<bool> m_useNeural{ false };

    mutable std::mutex m_audioMutex;
    std::deque<float> m_samples; // da suonare (24 kHz mono)
    std::atomic<float> m_level{ 0.0f };
    bool openOutput(); // con m_deviceMutex preso
    void closeOutput();

    std::mutex m_deviceMutex;
    std::string m_outputName;
    void* m_context = nullptr; // ma_context*
    void* m_device = nullptr;  // ma_device*
};
