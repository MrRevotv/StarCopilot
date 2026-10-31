#pragma once

#include <memory>
#include <string>
#include <vector>

// Effetti sulla voce di Iris, per farla sembrare una macchina APPOSTA (un'IA di
// bordo, un androide, un robot), qualunque sia il motore (neurale o Windows).
// DSP puro su campioni float mono: si chiama a pezzi e lo stato resta tra un
// pezzo e l'altro (riverbero, modulazioni), quindi va bene anche in streaming.
class VoiceEffect {
public:
    struct PresetInfo {
        const char* id;    // nelle impostazioni ("nessuno", "ia", ...)
        const char* label; // nella finestra
    };
    static const std::vector<PresetInfo>& presets(); // il primo è "nessuno"
    static bool isValid(const std::string& id);

    explicit VoiceEffect(int sampleRate);
    ~VoiceEffect();
    VoiceEffect(const VoiceEffect&) = delete;
    VoiceEffect& operator=(const VoiceEffect&) = delete;

    void setPreset(const std::string& id); // azzera lo stato; un nome sconosciuto vale "nessuno"
    const std::string& preset() const;
    bool active() const; // false = "nessuno": process non cambia niente

    void process(float* samples, size_t count);
    // La coda del riverbero dopo una frase, da suonare in fondo (vuota se non serve).
    std::vector<float> tail();
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
