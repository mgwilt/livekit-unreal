#include "LiveKitWindowsBridgeInternal.h"

#if WITH_LIVEKIT_LINUX
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

using namespace LiveKitWindowsBridgeInternal;

bool FLiveKitWindowsBridge::FImplementation::StartRawMicrophone(
    LKUB_Room* Candidate, FLiveKitError& OutError)
{
    if (bCaptureRunning.load(std::memory_order_acquire)) return true;
    StopRawMicrophone();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        OutError = MakeLiveKitError(TEXT("microphone_device_failed"), TEXT("Cannot initialize the recording device."));
        return false;
    }
    bCaptureAudioInitialized = true;
    SDL_AudioSpec Format{};
    Format.format = SDL_AUDIO_S16;
    Format.channels = 1;
    Format.freq = 48000;
    CaptureStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &Format, nullptr, nullptr);
    if (!CaptureStream)
    {
        StopRawMicrophone();
        OutError = MakeLiveKitError(TEXT("microphone_device_failed"), TEXT("Cannot open the default recording device."));
        return false;
    }
    LKUB_Result Result{};
    if (lkub_room_start_raw_microphone(Candidate, 48000, 1, &Result) != LKUB_STATUS_OK)
    {
        StopRawMicrophone();
        OutError = AdapterError(TEXT("microphone_publish_failed"), Result);
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(CaptureStream))
    {
        StopRawMicrophone();
        lkub_room_stop_raw_microphone(Candidate, &Result);
        OutError = MakeLiveKitError(TEXT("microphone_device_failed"), TEXT("Cannot start the recording device."));
        return false;
    }
    bCaptureFailed.store(false, std::memory_order_release);
    bCaptureRunning.store(true, std::memory_order_release);
    try
    {
        CaptureThread = std::thread([this, Candidate]()
        {
            constexpr int32 FrameSamples = 480;
            constexpr int32 FrameBytes = FrameSamples * sizeof(int16);
            int16 Samples[FrameSamples];
            int32 Filled = 0;
            while (bCaptureRunning.load(std::memory_order_acquire))
            {
                const int32 Available = SDL_GetAudioStreamAvailable(CaptureStream);
                if (Available < 0) break;
                if (Available > FrameBytes * 20)
                {
                    // Discard old input after a stall. Never retain more than 200 ms.
                    SDL_ClearAudioStream(CaptureStream);
                    Filled = 0;
                }
                else if (Available >= FrameBytes - Filled)
                {
                    const int32 Read = SDL_GetAudioStreamData(CaptureStream,
                        reinterpret_cast<uint8*>(Samples) + Filled, FrameBytes - Filled);
                    if (Read < 0) break;
                    if (Read == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    Filled += Read;
                    if (Filled == FrameBytes)
                    {
                        LKUB_Result PushResult{};
                        if (lkub_room_push_microphone_pcm(Candidate, Samples, FrameSamples, &PushResult) != LKUB_STATUS_OK)
                            break;
                        Filled = 0;
                    }
                }
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (bCaptureRunning.exchange(false, std::memory_order_acq_rel))
                bCaptureFailed.store(true, std::memory_order_release);
        });
    }
    catch (...)
    {
        StopRawMicrophone();
        lkub_room_stop_raw_microphone(Candidate, &Result);
        OutError = MakeLiveKitError(TEXT("microphone_worker_failed"), TEXT("Cannot start the recording worker."));
        return false;
    }
    return true;
}

void FLiveKitWindowsBridge::FImplementation::StopRawMicrophone()
{
    bCaptureRunning.store(false, std::memory_order_release);
    if (CaptureThread.joinable()) CaptureThread.join();
    if (CaptureStream) SDL_DestroyAudioStream(CaptureStream);
    CaptureStream = nullptr;
    if (bCaptureAudioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    bCaptureAudioInitialized = false;
    bCaptureFailed.store(false, std::memory_order_release);
}
#endif
