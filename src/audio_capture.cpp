#include "audio_capture.h"
#include "log.h"
#include "speech_utils.h"

#include <windows.h>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr size_t kPreRollSamples = AudioCapture::kSampleRate * 4 / 10; // 0,4 s
    constexpr size_t kMaxRecordingSamples = (size_t)AudioCapture::kSampleRate * 120; // 2 minuti al massimo

    struct CallbackData {
        AudioCapture* self;
        int slot;
    };

    void dataCallback(ma_device* device, void*, const void* input, ma_uint32 frameCount) {
        auto* data = static_cast<CallbackData*>(device->pUserData);
        data->self->onAudioData(data->slot, static_cast<const float*>(input), frameCount);
    }
}

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    close();
}

std::vector<AudioDeviceInfo> AudioCapture::listCaptureDevices() {
    std::vector<AudioDeviceInfo> result;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return result;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, nullptr, nullptr, &infos, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) result.push_back({ (int)i, infos[i].name });
    }
    ma_context_uninit(&context);
    return result;
}

std::string AudioCapture::defaultCaptureName() {
    std::string name;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return name;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, nullptr, nullptr, &infos, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (infos[i].isDefault) { name = infos[i].name; break; }
        }
    }
    ma_context_uninit(&context);
    return name;
}

long long AudioCapture::msSinceLastData() const {
    if (m_openCount.load() == 0) return 0;
    const unsigned long long last = m_lastDataTick.load();
    return last == 0 ? 0 : (long long)(GetTickCount64() - last);
}

void AudioCapture::close() {
    // I device si fermano prima di toccare i buffer: dopo ma_device_uninit la
    // callback non viene più chiamata.
    for (void* d : m_devices) {
        ma_device_uninit(static_cast<ma_device*>(d));
        delete static_cast<ma_device*>(d);
    }
    m_devices.clear();
    for (void* u : m_userData) delete static_cast<CallbackData*>(u);
    m_userData.clear();
    if (m_context) {
        ma_context_uninit(static_cast<ma_context*>(m_context));
        delete static_cast<ma_context*>(m_context);
        m_context = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slots.clear();
    m_recording = false;
    m_openCount = 0;
}

bool AudioCapture::open(const std::vector<std::string>& deviceNames) {
    close();

    auto* context = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, context) != MA_SUCCESS) {
        delete context;
        Log::error("Audio: impossibile inizializzare miniaudio");
        return false;
    }
    m_context = context;

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    ma_context_get_devices(context, nullptr, nullptr, &infos, &count);

    // Per nome: se un microfono salvato non c'è più (scollegato), si salta.
    std::vector<int> indices;
    for (const std::string& name : deviceNames) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (name == infos[i].name) { indices.push_back((int)i); break; }
        }
    }
    if (indices.empty()) {
        if (!deviceNames.empty()) Log::warn("Audio: i microfoni scelti non ci sono, uso quello predefinito");
        indices.push_back(-1); // predefinito di Windows
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots.assign(indices.size(), DeviceSlot{});
        for (auto& s : m_slots) s.preRoll.assign(kPreRollSamples, 0.0f);
    }

    int opened = 0;
    for (size_t slot = 0; slot < indices.size(); ++slot) {
        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.sampleRate = kSampleRate;
        config.dataCallback = dataCallback;
        config.periodSizeInMilliseconds = 20;
        if (indices[slot] >= 0) config.capture.pDeviceID = &infos[indices[slot]].id;

        auto* data = new CallbackData{ this, (int)slot };
        config.pUserData = data;
        auto* device = new ma_device();
        if (ma_device_init(context, &config, device) != MA_SUCCESS || ma_device_start(device) != MA_SUCCESS) {
            Log::warn("Audio: microfono non aperto: " + std::string(indices[slot] >= 0 ? infos[indices[slot]].name : "predefinito"));
            ma_device_uninit(device);
            delete device;
            delete data;
            continue;
        }
        m_devices.push_back(device);
        m_userData.push_back(data);
        opened++;
        Log::info("Audio: aperto " + std::string(device->capture.name));
    }
    m_openCount = opened;
    m_lastDataTick.store(GetTickCount64()); // il primo audio arriva tra poco: si conta da adesso
    return opened > 0;
}

void AudioCapture::onAudioData(int slot, const float* input, unsigned int frameCount) {
    if (!input || frameCount == 0) return;
    m_lastDataTick.store(GetTickCount64());
    const float gain = m_gain.load();

    double sum = 0.0;
    for (unsigned int i = 0; i < frameCount; ++i) {
        float s = input[i] * gain;
        sum += (double)s * s;
    }
    m_currentLevel.store((float)std::sqrt(sum / frameCount));

    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot < 0 || (size_t)slot >= m_slots.size()) return;
    if (slot == 0 && m_streaming.load()) {
        constexpr size_t kMaxStream = (size_t)kSampleRate * 30;
        if (m_stream.size() + frameCount > kMaxStream) m_stream.erase(m_stream.begin(), m_stream.begin() + std::min(m_stream.size(), (size_t)frameCount + kSampleRate));
        for (unsigned int i = 0; i < frameCount; ++i) m_stream.push_back(std::clamp(input[i] * gain, -1.0f, 1.0f));
    }
    DeviceSlot& d = m_slots[slot];
    const bool recording = m_recording.load();
    for (unsigned int i = 0; i < frameCount; ++i) {
        float s = std::clamp(input[i] * gain, -1.0f, 1.0f);
        if (recording) {
            if (d.recording.size() < kMaxRecordingSamples) d.recording.push_back(s);
        } else {
            d.preRoll[d.preRollPos] = s;
            d.preRollPos = (d.preRollPos + 1) % d.preRoll.size();
            if (d.preRollPos == 0) d.preRollFull = true;
        }
    }
}

void AudioCapture::startRecording() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (DeviceSlot& d : m_slots) {
        d.recording.clear();
        d.recording.reserve((size_t)kSampleRate * 10);
        // Il pre-ascolto, in ordine cronologico, apre la registrazione.
        if (d.preRollFull) d.recording.insert(d.recording.end(), d.preRoll.begin() + d.preRollPos, d.preRoll.end());
        d.recording.insert(d.recording.end(), d.preRoll.begin(), d.preRoll.begin() + d.preRollPos);
        d.preRollPos = 0;
        d.preRollFull = false;
    }
    m_recording = true;
}

void AudioCapture::setStreaming(bool on) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_streaming = on;
    m_stream.clear();
}

std::vector<float> AudioCapture::takeStream() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<float> out;
    out.swap(m_stream);
    m_stream.reserve(kSampleRate);
    return out;
}

std::vector<float> AudioCapture::stopRecording() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recording = false;
    // Il microfono che ha sentito più voce.
    size_t best = 0;
    double bestEnergy = -1.0;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        double e = SpeechUtils::rms(m_slots[i].recording) * std::sqrt((double)m_slots[i].recording.size());
        if (e > bestEnergy) { bestEnergy = e; best = i; }
    }
    std::vector<float> result;
    if (best < m_slots.size()) result.swap(m_slots[best].recording);
    for (DeviceSlot& d : m_slots) d.recording.clear();
    return result;
}
