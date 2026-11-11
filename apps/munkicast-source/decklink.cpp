// SPDX-License-Identifier: GPL-3.0-or-later
#include "decklink.hpp"

#include <DeckLinkAPI.h>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <vector>
extern "C" {
#include <libswscale/swscale.h>
}

struct Decklink::State : IDeckLinkInputCallback {
    void* library = nullptr;
    IDeckLinkInput* input = nullptr;
    SwsContext* converter = nullptr;
    std::atomic<ULONG> refs{1};
    Video video;
    Audio audio;

    ~State() override {
        if (input) {
            input->StopStreams();
            input->SetCallback(nullptr);
            input->DisableVideoInput();
            input->DisableAudioInput();
            input->Release();
        }
        sws_freeContext(converter);
        if (library)
            dlclose(library);
    }

    HRESULT QueryInterface(REFIID id, LPVOID* out) override {
        if (!out)
            return E_POINTER;
        constexpr REFIID unknown{0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
        if (std::memcmp(&id, &IID_IDeckLinkInputCallback, sizeof(id)) != 0 &&
            std::memcmp(&id, &unknown, sizeof(id)) != 0) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = this;
        AddRef();
        return S_OK;
    }

    ULONG AddRef() override { return ++refs; }

    ULONG Release() override { return --refs; }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*,
                                    BMDDetectedVideoInputFormatFlags) override {
        return S_OK;
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame,
                                   IDeckLinkAudioInputPacket* packet) override {
        try {
            if (frame &&
                !(frame->GetFlags() & static_cast<BMDFrameFlags>(bmdFrameHasNoInputSource))) {
                const auto w = frame->GetWidth(), h = frame->GetHeight(),
                           stride = frame->GetRowBytes();
                void* data = nullptr;
                if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || stride < w * 2 || stride > 65536 ||
                    frame->GetBytes(&data) != S_OK || !data)
                    return E_FAIL;
                converter = sws_getCachedContext(
                    converter, static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_UYVY422,
                    static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_RGBA, SWS_BILINEAR,
                    nullptr, nullptr, nullptr);
                if (!converter)
                    return E_FAIL;
                const auto* matrix = sws_getCoefficients(h >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
                if (sws_setColorspaceDetails(converter, matrix, 0, matrix, 1, 0, 1 << 16, 1 << 16) <
                    0)
                    return E_FAIL;
                QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
                const std::uint8_t* src[4] = {static_cast<const std::uint8_t*>(data), nullptr,
                                              nullptr, nullptr};
                int ss[4] = {static_cast<int>(stride), 0, 0, 0};
                std::uint8_t* dst[4] = {image.bits(), nullptr, nullptr, nullptr};
                int ds[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
                if (sws_scale(converter, src, ss, 0, static_cast<int>(h), dst, ds) == h)
                    video(std::move(image));
            } else if (frame)
                video({});
            if (packet) {
                void* data = nullptr;
                const auto count = packet->GetSampleFrameCount();
                if (count > 0 && count <= 48000 && packet->GetBytes(&data) == S_OK && data) {
                    const auto* samples = static_cast<const std::int32_t*>(data);
                    std::vector<float> pcm(static_cast<std::size_t>(count) * 2);
                    for (std::size_t i = 0; i < pcm.size(); ++i)
                        pcm[i] = static_cast<float>(samples[i]) / 2147483648.0f;
                    audio(pcm);
                }
            }
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
};

Decklink::Decklink(std::unique_ptr<State> state) : state_(std::move(state)) {}

Decklink::~Decklink() = default;

std::unique_ptr<Decklink> Decklink::open(int index, const std::string& mode, Video video,
                                         Audio audio, std::string& error) {
    auto state = std::make_unique<State>();
    auto fail = [&](const char* why) -> std::unique_ptr<Decklink> {
        error = why;
        return {};
    };
    if (index < 0 || index > 63)
        return fail("invalid Blackmagic device index");
    state->library = dlopen("libDeckLinkAPI.so", RTLD_NOW | RTLD_LOCAL);
    if (!state->library)
        return fail("Blackmagic Desktop Video driver is not installed");
    auto factory = reinterpret_cast<IDeckLinkIterator* (*)()>(
        dlsym(state->library, "CreateDeckLinkIteratorInstance_0004"));
    if (!factory)
        return fail("Blackmagic driver API is incompatible");
    IDeckLinkIterator* iterator = factory();
    if (!iterator)
        return fail("no Blackmagic capture devices");
    IDeckLink* device = nullptr;
    for (int i = 0; i <= index; ++i) {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (iterator->Next(&device) != S_OK)
            break;
    }
    iterator->Release();
    if (!device)
        return fail("Blackmagic device index is unavailable");
    const auto queried =
        device->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&state->input));
    device->Release();
    if (queried != S_OK)
        return fail("selected Blackmagic device has no capture input");
    IDeckLinkDisplayModeIterator* modes = nullptr;
    if (state->input->GetDisplayModeIterator(&modes) != S_OK)
        return fail("cannot enumerate Blackmagic modes");
    BMDDisplayMode selected = bmdModeUnknown;
    IDeckLinkDisplayMode* candidate = nullptr;
    while (modes->Next(&candidate) == S_OK) {
        const char* name = nullptr;
        if (candidate->GetName(&name) == S_OK) {
            if (mode == name && candidate->GetFieldDominance() == bmdProgressiveFrame)
                selected = candidate->GetDisplayMode();
            std::free(const_cast<char*>(name));
        }
        candidate->Release();
    }
    modes->Release();
    if (selected == bmdModeUnknown)
        return fail("requested progressive Blackmagic mode is unavailable; use its exact display "
                    "mode name");
    state->video = std::move(video);
    state->audio = std::move(audio);
    if (state->input->SetCallback(state.get()) != S_OK ||
        state->input->EnableVideoInput(selected, bmdFormat8BitYUV, bmdVideoInputFlagDefault) !=
            S_OK ||
        state->input->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger,
                                       2) != S_OK ||
        state->input->StartStreams() != S_OK)
        return fail("cannot start Blackmagic capture in requested mode");
    return std::unique_ptr<Decklink>(new Decklink(std::move(state)));
}
