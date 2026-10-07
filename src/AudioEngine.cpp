#include "AudioEngine.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <cstdio>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace mrefiner {

namespace {

struct ComInit {
    HRESULT hr;
    ComInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) CoUninitialize(); }
};

struct ThreadCom {
    HRESULT hr;
    ThreadCom() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ThreadCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T* detach() { T* t = p; p = nullptr; return t; }
    explicit operator bool() const { return p != nullptr; }
};

void throwIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(what) + " failed (HRESULT 0x" +
            [hr]{ char b[16]; std::snprintf(b, sizeof(b), "%08lX", (unsigned long)hr); return std::string(b); }() + ")");
    }
}

std::wstring propVariantToWString(const PROPVARIANT& pv) {
    return pv.pwszVal ? std::wstring(pv.pwszVal) : std::wstring();
}

std::vector<DeviceInfo> enumerate(EDataFlow flow) {
    ThreadCom com;

    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    throwIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(enumerator.put())),
                  "CoCreateInstance MMDeviceEnumerator");

    ComPtr<IMMDeviceCollection> col;
    throwIfFailed(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, col.put()),
                  "EnumAudioEndpoints");

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        col->Item(i, dev.put());

        LPWSTR id = nullptr;
        dev->GetId(&id);

        ComPtr<IPropertyStore> ps;
        dev->OpenPropertyStore(STGM_READ, ps.put());
        PROPVARIANT pv; PropVariantInit(&pv);
        ps->GetValue(PKEY_Device_FriendlyName, &pv);

        DeviceInfo di;
        di.id = id ? id : L"";
        di.friendlyName = propVariantToWString(pv);
        PropVariantClear(&pv);
        if (id) CoTaskMemFree(id);

        out.push_back(std::move(di));
    }
    return out;
}

} // namespace

struct AudioEngine::Impl {
    ComInit com;

    ComPtr<IAudioClient>        captureClient;
    ComPtr<IAudioCaptureClient> captureReader;
    HANDLE                      captureEvent = nullptr;

    ComPtr<IAudioClient>        renderClient;
    ComPtr<IAudioRenderClient>  renderWriter;
    HANDLE                      renderEvent = nullptr;

    UINT32 captureBufferFrames = 0;
    UINT32 renderBufferFrames = 0;

    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    WORD formatTag = 0;

    std::mutex               ringMutex;
    std::vector<float>       ring;
    size_t                   ringCapacity = 0;

    void ringPush(const float* data, size_t count) {
        std::lock_guard<std::mutex> lk(ringMutex);
        if (ring.size() + count > ringCapacity) {
            // Overflow — drop oldest to catch up (still better than blocking)
            const size_t drop = ring.size() + count - ringCapacity;
            ring.erase(ring.begin(), ring.begin() + std::min(drop, ring.size()));
        }
        ring.insert(ring.end(), data, data + count);
    }

    size_t ringPop(float* out, size_t maxCount) {
        std::lock_guard<std::mutex> lk(ringMutex);
        const size_t n = std::min(maxCount, ring.size());
        std::copy(ring.begin(), ring.begin() + n, out);
        ring.erase(ring.begin(), ring.begin() + n);
        return n;
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    throwIfFailed(impl_->com.hr, "CoInitializeEx");
}

AudioEngine::~AudioEngine() {
    stop();
}

std::vector<DeviceInfo> AudioEngine::listInputDevices()  { return enumerate(eCapture); }
std::vector<DeviceInfo> AudioEngine::listOutputDevices() { return enumerate(eRender); }

void AudioEngine::addFilter(std::shared_ptr<IAudioFilter> f) {
    std::lock_guard<std::mutex> lk(filtersMutex_);
    filters_.push_back(std::move(f));
}

void AudioEngine::start(const std::wstring& inputId, const std::wstring& outputId) {
    if (running_.load()) throw std::runtime_error("Already running");

    auto& I = *impl_;

    ComPtr<IMMDeviceEnumerator> enumerator;
    throwIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(enumerator.put())),
                  "CoCreateInstance MMDeviceEnumerator");

    ComPtr<IMMDevice> inputDevice;
    ComPtr<IMMDevice> outputDevice;
    throwIfFailed(enumerator->GetDevice(inputId.c_str(), inputDevice.put()),  "GetDevice(input)");
    throwIfFailed(enumerator->GetDevice(outputId.c_str(), outputDevice.put()), "GetDevice(output)");

    throwIfFailed(inputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(I.captureClient.put())),
                  "Activate capture");
    throwIfFailed(outputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(I.renderClient.put())),
                  "Activate render");

    // Use the capture endpoint's mix format for the whole pipeline.
    WAVEFORMATEX* mix = nullptr;
    throwIfFailed(I.captureClient->GetMixFormat(&mix), "GetMixFormat capture");

    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        if (ext->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            CoTaskMemFree(mix);
            throw std::runtime_error("Capture mix format is not IEEE float");
        }
    } else if (mix->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        CoTaskMemFree(mix);
        throw std::runtime_error("Capture mix format is not IEEE float");
    }

    I.sampleRate = mix->nSamplesPerSec;
    I.channels = mix->nChannels;
    I.bitsPerSample = mix->wBitsPerSample;
    I.formatTag = mix->wFormatTag;

    sampleRate_ = I.sampleRate;
    channels_ = I.channels;

    // 3 ms buffer target; WASAPI shared mode rounds to the engine period (typically 10 ms).
    const REFERENCE_TIME hnsBuffer = 3 * 10000;

    HRESULT hr = I.captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBuffer, 0, mix, nullptr);
    if (FAILED(hr)) { CoTaskMemFree(mix); throw std::runtime_error("Initialize capture failed"); }

    // Shared mode needs each endpoint's own mix format, and there is no resampler,
    // so the two sample rates must match.
    WAVEFORMATEX* outMix = nullptr;
    throwIfFailed(I.renderClient->GetMixFormat(&outMix), "GetMixFormat render");
    if (outMix->nSamplesPerSec != mix->nSamplesPerSec) {
        CoTaskMemFree(mix); CoTaskMemFree(outMix);
        throw std::runtime_error("Capture and render devices have different sample rates. "
                                 "Set both to the same rate (e.g. 48000 Hz) in Windows Sound settings.");
    }

    hr = I.renderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBuffer, 0, outMix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { CoTaskMemFree(outMix); throw std::runtime_error("Initialize render failed"); }

    if (outMix->nChannels != I.channels) {
        CoTaskMemFree(outMix);
        throw std::runtime_error("Capture and render channel counts differ; not yet supported.");
    }
    CoTaskMemFree(outMix);

    I.captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    I.renderEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    throwIfFailed(I.captureClient->SetEventHandle(I.captureEvent), "SetEventHandle capture");
    throwIfFailed(I.renderClient->SetEventHandle(I.renderEvent),   "SetEventHandle render");

    throwIfFailed(I.captureClient->GetBufferSize(&I.captureBufferFrames), "GetBufferSize capture");
    throwIfFailed(I.renderClient->GetBufferSize(&I.renderBufferFrames),   "GetBufferSize render");

    throwIfFailed(I.captureClient->GetService(__uuidof(IAudioCaptureClient),
                        reinterpret_cast<void**>(I.captureReader.put())),
                  "GetService capture reader");
    throwIfFailed(I.renderClient->GetService(__uuidof(IAudioRenderClient),
                        reinterpret_cast<void**>(I.renderWriter.put())),
                  "GetService render writer");

    // Ring buffer: hold at most ~50 ms of samples. Enough headroom for jitter,
    // little enough to keep total pipeline latency down.
    I.ringCapacity = static_cast<size_t>(I.sampleRate * I.channels * 50 / 1000);
    I.ring.clear();
    I.ring.reserve(I.ringCapacity);

    {
        std::lock_guard<std::mutex> lk(filtersMutex_);
        for (auto& f : filters_) f->initialize(I.sampleRate, I.channels);
    }

    // Pre-fill with silence so playback doesn't underrun before the first captured packet.
    {
        BYTE* p = nullptr;
        if (SUCCEEDED(I.renderWriter->GetBuffer(I.renderBufferFrames, &p))) {
            I.renderWriter->ReleaseBuffer(I.renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    running_.store(true, std::memory_order_release);

    throwIfFailed(I.captureClient->Start(), "captureClient->Start");
    throwIfFailed(I.renderClient->Start(),  "renderClient->Start");

    captureThread_ = std::thread(&AudioEngine::captureThreadMain, this);
    renderThread_  = std::thread(&AudioEngine::renderThreadMain,  this);
}

void AudioEngine::stop() {
    if (!running_.exchange(false)) return;

    if (impl_->captureEvent) SetEvent(impl_->captureEvent);
    if (impl_->renderEvent)  SetEvent(impl_->renderEvent);

    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable())  renderThread_.join();

    teardown();
}

void AudioEngine::teardown() {
    auto& I = *impl_;
    if (I.captureClient) I.captureClient->Stop();
    if (I.renderClient)  I.renderClient->Stop();
    I.captureReader.reset();
    I.renderWriter.reset();
    I.captureClient.reset();
    I.renderClient.reset();
    if (I.captureEvent) { CloseHandle(I.captureEvent); I.captureEvent = nullptr; }
    if (I.renderEvent)  { CloseHandle(I.renderEvent);  I.renderEvent  = nullptr; }
    I.ring.clear();
}

void AudioEngine::captureThreadMain() {
    ThreadCom com;
    // Boost thread priority for audio ("Pro Audio" MMCSS characteristics).
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto& I = *impl_;

    while (running_.load(std::memory_order_acquire)) {
        DWORD w = WaitForSingleObject(I.captureEvent, 200);
        if (w != WAIT_OBJECT_0) continue;

        UINT32 packetFrames = 0;
        I.captureReader->GetNextPacketSize(&packetFrames);
        while (packetFrames > 0 && running_.load(std::memory_order_acquire)) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags = 0;
            HRESULT hr = I.captureReader->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr) || numFrames == 0) break;

            const int totalSamples = static_cast<int>(numFrames) * I.channels;

            std::vector<float> work(totalSamples);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                std::memcpy(work.data(), data, totalSamples * sizeof(float));
            } // else: work stays zero

            {
                std::lock_guard<std::mutex> lk(filtersMutex_);
                for (auto& f : filters_) {
                    if (f->enabled())
                        f->process(work.data(), static_cast<int>(numFrames), I.channels);
                }
            }

            I.ringPush(work.data(), totalSamples);
            I.captureReader->ReleaseBuffer(numFrames);
            I.captureReader->GetNextPacketSize(&packetFrames);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioEngine::renderThreadMain() {
    ThreadCom com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto& I = *impl_;

    while (running_.load(std::memory_order_acquire)) {
        DWORD w = WaitForSingleObject(I.renderEvent, 200);
        if (w != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(I.renderClient->GetCurrentPadding(&padding))) continue;
        const UINT32 available = I.renderBufferFrames - padding;
        if (available == 0) continue;

        BYTE* buf = nullptr;
        HRESULT hr = I.renderWriter->GetBuffer(available, &buf);
        if (FAILED(hr) || !buf) continue;

        const size_t needSamples = static_cast<size_t>(available) * I.channels;
        const size_t got = I.ringPop(reinterpret_cast<float*>(buf), needSamples);

        if (got < needSamples) {
            std::memset(buf + got * sizeof(float), 0, (needSamples - got) * sizeof(float));
        }
        I.renderWriter->ReleaseBuffer(available, 0);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace mrefiner
