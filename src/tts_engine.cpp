#include "tts_engine.h"
#include "log.h"
#include "neural_voice.h"
#include "text_utils.h"
#include "voice_effect.h"

// Stesse opzioni dell'implementazione (audio_capture.cpp): lo stesso miniaudio.
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace winrt::Windows::Media::SpeechSynthesis;
using namespace winrt::Windows::Storage::Streams;

namespace {
    constexpr double kPi = 3.14159265358979323846;

    void playbackCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        static_cast<TtsEngine*>(device->pUserData)->onPlayback(static_cast<float*>(output), frames);
    }

    // WAV (PCM 16 bit) -> campioni float mono a 24 kHz.
    std::vector<float> decodeWav(const uint8_t* data, size_t size) {
        std::vector<float> out;
        if (size < 44 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return out;
        int channels = 1, rate = 16000, bits = 16;
        const uint8_t* pcm = nullptr;
        size_t pcmBytes = 0;
        size_t pos = 12;
        while (pos + 8 <= size) {
            uint32_t chunk = 0;
            std::memcpy(&chunk, data + pos + 4, 4);
            if (std::memcmp(data + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= size) {
                uint16_t ch = 0, bps = 0;
                uint32_t sr = 0;
                std::memcpy(&ch, data + pos + 10, 2);
                std::memcpy(&sr, data + pos + 12, 4);
                std::memcpy(&bps, data + pos + 22, 2);
                channels = std::max<int>(1, ch);
                rate = (int)sr;
                bits = bps;
            } else if (std::memcmp(data + pos, "data", 4) == 0) {
                pcm = data + pos + 8;
                pcmBytes = std::min<size_t>(chunk, size - pos - 8);
                break;
            }
            pos += 8 + chunk + (chunk & 1);
        }
        if (!pcm || bits != 16 || rate <= 0) return out;
        const size_t frames = pcmBytes / (2 * channels);
        std::vector<float> mono(frames);
        for (size_t i = 0; i < frames; ++i) {
            int sum = 0;
            for (int c = 0; c < channels; ++c) {
                int16_t v;
                std::memcpy(&v, pcm + (i * channels + c) * 2, 2);
                sum += v;
            }
            mono[i] = (float)sum / (32768.0f * channels);
        }
        if (rate == TtsEngine::kSampleRate) return mono;
        // Ricampionamento lineare: per la voce basta e avanza.
        const double ratio = (double)rate / TtsEngine::kSampleRate;
        const size_t outFrames = (size_t)(frames / ratio);
        out.resize(outFrames);
        for (size_t i = 0; i < outFrames; ++i) {
            double src = i * ratio;
            size_t a = (size_t)src;
            double t = src - a;
            float s0 = mono[std::min(a, frames - 1)], s1 = mono[std::min(a + 1, frames - 1)];
            out[i] = (float)(s0 + (s1 - s0) * t);
        }
        return out;
    }

    // Una nota con attacco e coda morbidi.
    void addTone(std::vector<float>& out, double freq, double seconds, double volume) {
        const size_t n = (size_t)(seconds * TtsEngine::kSampleRate);
        const size_t start = out.size();
        out.resize(start + n);
        for (size_t i = 0; i < n; ++i) {
            double t = (double)i / TtsEngine::kSampleRate;
            double env = std::min(1.0, t / 0.008) * std::exp(-t * 7.0);
            out[start + i] += (float)(volume * env * (std::sin(2 * kPi * freq * t) + 0.25 * std::sin(4 * kPi * freq * t)));
        }
    }

    void addSilence(std::vector<float>& out, double seconds) {
        out.resize(out.size() + (size_t)(seconds * TtsEngine::kSampleRate), 0.0f);
    }
}

TtsEngine::~TtsEngine() {
    shutdown();
}

bool TtsEngine::openOutput() {
    auto* context = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, context) != MA_SUCCESS) {
        delete context;
        Log::error("Voce: audio non disponibile");
        return false;
    }
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = kSampleRate;
    config.dataCallback = playbackCallback;
    config.pUserData = this;
    config.periodSizeInMilliseconds = 20;
    // L'uscita scelta, per nome; se non c'è più, quella predefinita di Windows.
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (!m_outputName.empty() && ma_context_get_devices(context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (m_outputName == infos[i].name) { config.playback.pDeviceID = &infos[i].id; break; }
        }
        if (!config.playback.pDeviceID) Log::warn("Voce: l'uscita \"" + m_outputName + "\" non c'è, uso quella predefinita");
    }
    auto* device = new ma_device();
    if (ma_device_init(context, &config, device) != MA_SUCCESS || ma_device_start(device) != MA_SUCCESS) {
        Log::error("Voce: uscita audio non disponibile");
        ma_device_uninit(device);
        delete device;
        ma_context_uninit(context);
        delete context;
        return false;
    }
    Log::info("Voce: uscita " + std::string(device->playback.name));
    m_context = context;
    m_device = device;
    return true;
}

void TtsEngine::closeOutput() {
    if (m_device) {
        ma_device_uninit(static_cast<ma_device*>(m_device));
        delete static_cast<ma_device*>(m_device);
        m_device = nullptr;
    }
    if (m_context) {
        ma_context_uninit(static_cast<ma_context*>(m_context));
        delete static_cast<ma_context*>(m_context);
        m_context = nullptr;
    }
}

bool TtsEngine::start() {
    if (m_running) return true;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        openOutput(); // anche senza uscita la voce si prepara: magari le cuffie arrivano dopo
    }
    m_running = true;
    m_thread = std::thread(&TtsEngine::synthLoop, this);
    return true;
}

void TtsEngine::shutdown() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    closeOutput();
}

void TtsEngine::setOutputDevice(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    if (name == m_outputName && m_device) return;
    m_outputName = name;
    if (!m_running) return;
    closeOutput();
    openOutput();
}

void TtsEngine::reopenOutput() {
    std::lock_guard<std::mutex> lock(m_deviceMutex);
    if (!m_running) return;
    closeOutput();
    openOutput();
}

std::vector<std::string> TtsEngine::outputDevices() {
    std::vector<std::string> names;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return names;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) names.push_back(infos[i].name);
    }
    ma_context_uninit(&context);
    return names;
}

std::string TtsEngine::defaultOutputName() {
    std::string name;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return name;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (infos[i].isDefault) { name = infos[i].name; break; }
        }
    }
    ma_context_uninit(&context);
    return name;
}

std::vector<std::string> TtsEngine::voices() {
    std::vector<std::string> names;
    try {
        for (const auto& v : SpeechSynthesizer::AllVoices()) {
            names.push_back(TextUtils::wideToUtf8(std::wstring(v.DisplayName())) + " (" + TextUtils::wideToUtf8(std::wstring(v.Language())) + ")");
        }
    } catch (...) {
    }
    return names;
}

std::vector<float> TtsEngine::synthesize(const std::string& text, const std::string& voiceName) {
    try {
        SpeechSynthesizer synth;
        const std::string wanted = TextUtils::toLower(voiceName);
        for (const auto& v : SpeechSynthesizer::AllVoices()) {
            if (TextUtils::toLower(TextUtils::wideToUtf8(std::wstring(v.DisplayName()))).find(wanted) != std::string::npos) {
                synth.Voice(v);
                break;
            }
        }
        SpeechSynthesisStream stream = synth.SynthesizeTextToStreamAsync(winrt::hstring(TextUtils::utf8ToWide(text))).get();
        const uint32_t size = (uint32_t)stream.Size();
        Buffer buffer(size);
        IBuffer data = stream.ReadAsync(buffer, size, InputStreamOptions::None).get();
        return decodeWav(data.data(), data.Length());
    } catch (...) {
        return {};
    }
}

bool TtsEngine::usingNeural() const {
    NeuralVoice* neural = m_neural.load();
    return m_useNeural && neural && neural->isReady();
}

void TtsEngine::setVoice(const std::string& namePart) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (namePart == m_voice) return;
    m_voice = namePart;
    m_voiceChanged = true;
}

void TtsEngine::setRate(double rate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (rate == m_rate) return;
    m_rate = rate;
    m_voiceChanged = true;
}

void TtsEngine::setEffect(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_effect = id;
}

void TtsEngine::speak(const std::string& text) {
    if (TextUtils::trim(text).empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_texts.push_back(text);
    }
    m_cv.notify_all();
}

void TtsEngine::chime(Chime kind) {
    std::vector<float> s;
    switch (kind) {
    case Chime::Wake: // due note che salgono: "ti ascolto"
        addTone(s, 880.0, 0.12, 0.18);
        addTone(s, 1318.5, 0.25, 0.16);
        break;
    case Chime::Timer:
        for (int i = 0; i < 3; ++i) {
            addTone(s, 1046.5, 0.18, 0.22);
            addSilence(s, 0.08);
        }
        break;
    case Chime::Error:
        addTone(s, 330.0, 0.25, 0.18);
        break;
    case Chime::Confirm: // un "tic" breve: comando di gioco eseguito
        addTone(s, 1567.98, 0.06, 0.14);
        break;
    }
    enqueueSamples(std::move(s), m_generation.load());
}

void TtsEngine::stop() {
    m_generation++;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_texts.clear();
    }
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_samples.clear();
}

bool TtsEngine::isSpeaking() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_texts.empty()) return true;
    }
    if (m_synthesizing) return true;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return !m_samples.empty();
}

void TtsEngine::enqueueSamples(std::vector<float> samples, unsigned long long generation) {
    if (samples.empty() || generation != m_generation.load()) return;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_samples.insert(m_samples.end(), samples.begin(), samples.end());
}

void TtsEngine::onPlayback(float* out, unsigned int frames) {
    double sum = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        const unsigned int n = (unsigned int)std::min<size_t>(frames, m_samples.size());
        for (unsigned int i = 0; i < n; ++i) {
            out[i] = m_samples.front();
            m_samples.pop_front();
            sum += (double)out[i] * out[i];
        }
        for (unsigned int i = n; i < frames; ++i) out[i] = 0.0f;
    }
    // Il livello scende piano: l'animazione non sfarfalla tra una sillaba e l'altra.
    const float now = (float)std::sqrt(sum / std::max(1u, frames));
    const float prev = m_level.load();
    m_level.store(now > prev ? now : prev * 0.85f + now * 0.15f);
}

void TtsEngine::synthLoop() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    SpeechSynthesizer synth = nullptr;
    try {
        synth = SpeechSynthesizer();
    } catch (...) {
        Log::error("Voce: le voci di Windows non sono disponibili");
    }
    VoiceEffect effect(kSampleRate);

    while (true) {
        std::string text;
        std::string voiceName;
        std::string effectId;
        double rate = 1.0;
        bool voiceChanged = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return !m_running || !m_texts.empty(); });
            if (!m_running) break;
            text = m_texts.front();
            m_texts.pop_front();
            voiceChanged = m_voiceChanged;
            m_voiceChanged = false;
            voiceName = m_voice;
            rate = m_rate;
            effectId = m_effect;
            m_synthesizing = true;
        }
        const unsigned long long generation = m_generation.load();
        if (effectId != effect.preset()) {
            effect.setPreset(effectId);
            Log::info("Effetto sulla voce: " + effect.preset());
        }

        // Prima la voce neurale, a pezzi man mano che si genera.
        NeuralVoice* neural = m_neural.load();
        if (m_useNeural && neural && neural->isReady()) {
            const int rate = neural->sampleRate();
            const bool ok = neural->speak(text, [&](const float* pcm, size_t n) {
                if (generation != m_generation.load()) return false; // zittita nel frattempo
                std::vector<float> chunk(pcm, pcm + n);
                if (rate != kSampleRate && rate > 0) {
                    const double ratio = (double)rate / kSampleRate;
                    std::vector<float> res((size_t)(n / ratio));
                    for (size_t i = 0; i < res.size(); ++i) {
                        double src = i * ratio;
                        size_t a = (size_t)src;
                        float s0 = pcm[std::min(a, n - 1)], s1 = pcm[std::min(a + 1, n - 1)];
                        res[i] = (float)(s0 + (s1 - s0) * (src - a));
                    }
                    chunk.swap(res);
                }
                effect.process(chunk.data(), chunk.size());
                enqueueSamples(std::move(chunk), generation);
                return true;
            }, [&]() { return generation != m_generation.load() || !m_running; });
            if (ok) {
                enqueueSamples(effect.tail(), generation);
                m_synthesizing = false;
                continue;
            }
            Log::warn("Voce neurale non riuscita: questa frase la dice la voce di Windows");
        }

        if (!synth) {
            m_synthesizing = false;
            continue;
        }
        try {
            if (voiceChanged) {
                // La voce che contiene il nome scelto; se no una italiana; se no quella di Windows.
                VoiceInformation chosen = nullptr, italian = nullptr;
                const std::string wanted = TextUtils::toLower(voiceName);
                for (const auto& v : SpeechSynthesizer::AllVoices()) {
                    const std::string name = TextUtils::toLower(TextUtils::wideToUtf8(std::wstring(v.DisplayName())));
                    if (!chosen && !wanted.empty() && name.find(wanted) != std::string::npos) chosen = v;
                    if (!italian && std::wstring(v.Language()).rfind(L"it", 0) == 0) italian = v;
                }
                if (!chosen) chosen = italian;
                if (chosen) {
                    synth.Voice(chosen);
                    Log::info("Voce: " + TextUtils::wideToUtf8(std::wstring(chosen.DisplayName())));
                }
                synth.Options().SpeakingRate(std::clamp(rate, 0.5, 2.0));
                try {
                    synth.Options().AppendedSilence(SpeechAppendedSilence::Min); // meno pausa tra una frase e l'altra
                } catch (...) {
                }
            }
            SpeechSynthesisStream stream = synth.SynthesizeTextToStreamAsync(winrt::hstring(TextUtils::utf8ToWide(text))).get();
            const uint32_t size = (uint32_t)stream.Size();
            Buffer buffer(size);
            IBuffer data = stream.ReadAsync(buffer, size, InputStreamOptions::None).get();
            std::vector<float> samples = decodeWav(data.data(), data.Length());
            effect.process(samples.data(), samples.size());
            enqueueSamples(std::move(samples), generation);
            enqueueSamples(effect.tail(), generation);
        } catch (const winrt::hresult_error& e) {
            Log::error("Voce: sintesi non riuscita: " + TextUtils::wideToUtf8(std::wstring(e.message())));
        } catch (...) {
            Log::error("Voce: sintesi non riuscita");
        }
        m_synthesizing = false;
    }
}
