#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AudioDeviceInfo {
    int index;        // posizione nell'elenco di adesso (cambia se colleghi/scolleghi)
    std::string name; // nome della periferica: è QUESTO che si salva nelle impostazioni
};

// Registra da uno o più microfoni insieme (miniaudio), già in float32 mono
// 16 kHz come vuole whisper. (Da Voice to Chat, con in più l'ascolto continuo.)
// - Pre-ascolto: tiene sempre in memoria l'ultimo 0,4 s, che viene aggiunto
//   all'inizio della registrazione. Chi preme il tasto e parla subito non
//   perde la prima sillaba.
// - Con più microfoni non si mischia (due microfoni sfasati suonano "metallici"):
//   per ogni frase si usa quello che ha sentito meglio la voce.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    static std::vector<AudioDeviceInfo> listCaptureDevices();
    // Il nome del microfono predefinito di Windows adesso (vuoto se non c'è).
    static std::string defaultCaptureName();

    // Apre i microfoni con questi nomi (vuoto = quello predefinito di
    // Windows). Ritorna true se almeno uno si è aperto. Si può richiamare per
    // cambiare microfono; nel frattempo la registrazione in corso si annulla.
    bool open(const std::vector<std::string>& deviceNames);
    void close();
    bool isOpen() const { return m_openCount.load() > 0; }
    int openDeviceCount() const { return m_openCount.load(); }

    void startRecording();
    // Ritorna l'audio registrato (con il pre-ascolto davanti) e smette.
    std::vector<float> stopRecording();
    bool isRecording() const { return m_recording.load(); }

    void setGain(float gain) { m_gain.store(gain); }
    float getGain() const { return m_gain.load(); }

    // Livello istantaneo (RMS dopo il gain), aggiornato sempre: il misuratore.
    float getCurrentLevel() const { return m_currentLevel.load(); }
    // Da quanti millisecondi non arriva audio (microfono staccato?). 0 se chiuso.
    long long msSinceLastData() const;

    // Ascolto continuo (per la parola "Iris"): l'audio del primo microfono si
    // accumula finché takeStream() non lo prende (al massimo gli ultimi 30 s).
    void setStreaming(bool on);
    std::vector<float> takeStream();

    static constexpr int kSampleRate = 16000;

    // Chiamata dal thread audio di miniaudio: non usarla direttamente.
    void onAudioData(int slot, const float* input, unsigned int frameCount);

private:
    struct DeviceSlot {
        std::vector<float> recording;
        std::vector<float> preRoll; // anello circolare
        size_t preRollPos = 0;
        bool preRollFull = false;
    };

    std::mutex m_mutex;          // protegge m_slots e i buffer
    void* m_context = nullptr;   // ma_context*
    std::vector<void*> m_devices;
    std::vector<void*> m_userData;
    std::vector<DeviceSlot> m_slots;
    std::atomic<bool> m_recording{ false };
    std::atomic<int> m_openCount{ 0 };
    std::atomic<float> m_gain{ 1.0f };
    std::atomic<float> m_currentLevel{ 0.0f };
    std::atomic<bool> m_streaming{ false };
    std::atomic<unsigned long long> m_lastDataTick{ 0 };
    std::vector<float> m_stream; // protetto da m_mutex
};
