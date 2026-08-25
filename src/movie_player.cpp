#include "movie_player.h"
#include "movie_state.h"
#include "movie_trace.h"

#include "cursor_lock.h"
#include "d3d11_presenter.h"
#include "import_hook.h"
#include "status_overlay.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace san9::movie_player {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond =
    movie_state::kHundredNanosecondsPerSecond;
constexpr LONGLONG kDecodedQueueDuration = 15'000'000;
constexpr std::size_t kDecodedVideoFrames = 45;
constexpr std::size_t kPresentationVideoFrames = 12;
constexpr UINT32 kMaximumSubmittedAudioBuffers = 48;
constexpr UINT32 kExpectedWidth = 640;
constexpr UINT32 kExpectedHeight = 480;
constexpr UINT32 kExpectedFrameRate = 30;
constexpr UINT32 kExpectedSampleRate = 44'100;
constexpr UINT32 kExpectedChannels = 2;
constexpr std::size_t kMaximumMessagesPerPump = 32;
constexpr std::uintptr_t kPostMovieShowWindowReturnRva = 0x161DA;

using PlayFunction = int(__cdecl*)(HWND, const char*, int, int);
using NoArgumentFunction = void(__cdecl*)();
using QueryFunction = int(__cdecl*)();
using SetVolumeFunction = void(__cdecl*)(int);
using NotifyFunction = void(__cdecl*)(HWND, UINT, WPARAM, LPARAM);
using SetMessageLoopFunction = void(__cdecl*)(void(__cdecl*)());
using ShowWindowFunction = BOOL(WINAPI*)(HWND, int);

PlayFunction g_oldPlay = nullptr;
QueryFunction g_oldCheckFinish = nullptr;
QueryFunction g_oldIsPlaying = nullptr;
SetVolumeFunction g_oldSetVolume = nullptr;
NoArgumentFunction g_oldPause = nullptr;
NoArgumentFunction g_oldRestart = nullptr;
NoArgumentFunction g_oldExit = nullptr;
NotifyFunction g_oldNotify = nullptr;
SetMessageLoopFunction g_oldSetMessageLoop = nullptr;
ShowWindowFunction g_oldShowWindow = nullptr;

movie_state::PlaybackState g_state;
std::atomic_int g_volumeHundredthsDb{0};
std::atomic_bool g_audioRebuffering{false};
HWND g_owner = nullptr;
IXAudio2SourceVoice* g_activeVoice = nullptr;
HHOOK g_messageHook = nullptr;
std::atomic_bool g_preservePostMovieMaximized{false};
std::atomic<HWND> g_postMovieWindow{nullptr};

struct VideoFrame {
    LONGLONG timestamp = 0;
    std::vector<std::uint8_t> pixels;
};

struct AudioPacket {
    LONGLONG timestamp = 0;
    LONGLONG duration = 0;
    std::vector<std::uint8_t> bytes;
};

struct DecoderQueue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<VideoFrame> video;
    std::deque<std::unique_ptr<AudioPacket>> audio;
    LONGLONG audioDuration = 0;
    LONGLONG lastVideoTimestamp = -1;
    LONGLONG lastAudioTimestamp = -1;
    std::uint64_t decodedVideoFrames = 0;
    std::uint64_t decodedAudioPackets = 0;
    bool initialized = false;
    bool videoEnded = false;
    bool audioEnded = false;
    bool stop = false;
    HRESULT error = S_OK;
    const wchar_t* errorStage = L"decoder initialization";
};

class VoiceCallback final : public IXAudio2VoiceCallback {
public:
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        delete static_cast<AudioPacket*>(context);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
        error_.store(error);
    }

    HRESULT Error() const { return error_.load(); }

private:
    std::atomic<HRESULT> error_{S_OK};
};

class PlaybackWatchdog final {
public:
    PlaybackWatchdog() : thread_([this] { Run(); }) {}

    ~PlaybackWatchdog() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    PlaybackWatchdog(const PlaybackWatchdog&) = delete;
    PlaybackWatchdog& operator=(const PlaybackWatchdog&) = delete;

    void SetActivity(const wchar_t* activity) { activity_.store(activity); }
    const wchar_t* ExchangeActivity(const wchar_t* activity) {
        return activity_.exchange(activity);
    }

private:
    void Run() {
        std::unique_lock lock(mutex_);
        while (!changed_.wait_for(lock, std::chrono::seconds(1),
                                  [this] { return stopped_; })) {
            const wchar_t* activity = activity_.load();
            lock.unlock();
            std::wstring details = L"activity=";
            details.append(activity);
            movie_trace::Record(L"watchdog", details);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<const wchar_t*> activity_{L"setup"};
    bool stopped_ = false;
    std::thread thread_;
};

std::atomic<PlaybackWatchdog*> g_playbackWatchdog{nullptr};

class WatchdogBinding final {
public:
    explicit WatchdogBinding(PlaybackWatchdog& watchdog) {
        g_playbackWatchdog.store(&watchdog);
    }
    ~WatchdogBinding() { g_playbackWatchdog.store(nullptr); }

    WatchdogBinding(const WatchdogBinding&) = delete;
    WatchdogBinding& operator=(const WatchdogBinding&) = delete;
};

void TraceFailure(const wchar_t* stage, HRESULT error) {
    wchar_t message[256]{};
    swprintf_s(message, L"San9Toolkit movie: %s failed (HRESULT 0x%08X).\n",
               stage, static_cast<unsigned int>(error));
    OutputDebugStringW(message);
    wchar_t details[384]{};
    swprintf_s(details, L"stage=\"%s\" hresult=0x%08X", stage,
               static_cast<unsigned int>(error));
    movie_trace::Record(L"failure", details);
}

BOOL WINAPI ModernShowWindow(HWND window, int command) {
    const auto* module = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* returnAddress = reinterpret_cast<const unsigned char*>(_ReturnAddress());
    const bool postMovieNormalization =
        module && returnAddress == module + kPostMovieShowWindowReturnRva;
    if (postMovieNormalization && command == SW_SHOWNORMAL &&
        window == g_postMovieWindow.load() &&
        g_preservePostMovieMaximized.exchange(false)) {
        g_postMovieWindow.store(nullptr);
        return g_oldShowWindow(window, SW_MAXIMIZE);
    }
    return g_oldShowWindow(window, command);
}

HRESULT ValidateAviSignature(const wchar_t* path) {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::uint8_t header[12]{};
    DWORD read = 0;
    const bool validRead = ReadFile(file, header, sizeof(header), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!validRead) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (read != sizeof(header) || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "AVI ", 4) != 0) {
        return MF_E_INVALID_FILE_FORMAT;
    }
    return S_OK;
}

HRESULT ValidateNativeStreams(IMFSourceReader* reader, DWORD& videoStream, DWORD& audioStream) {
    ComPtr<IMFMediaType> video;
    ComPtr<IMFMediaType> audio;
    GUID major{};
    GUID subtype{};
    for (DWORD stream = 0; stream < 32; ++stream) {
        ComPtr<IMFMediaType> candidate;
        const HRESULT candidateResult = reader->GetNativeMediaType(stream, 0, &candidate);
        if (candidateResult == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(candidateResult)) continue;
        GUID candidateMajor{};
        if (FAILED(candidate->GetGUID(MF_MT_MAJOR_TYPE, &candidateMajor))) continue;
        if (candidateMajor == MFMediaType_Video && !video) {
            videoStream = stream;
            video = candidate;
        } else if (candidateMajor == MFMediaType_Audio && !audio) {
            audioStream = stream;
            audio = candidate;
        }
    }
    if (!video || !audio) return MF_E_INVALIDMEDIATYPE;
    HRESULT result = S_OK;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (FAILED(result) || FAILED(video->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(video->GetGUID(MF_MT_SUBTYPE, &subtype)) || major != MFMediaType_Video ||
        subtype != MFVideoFormat_WMV3 ||
        FAILED(MFGetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        FAILED(MFGetAttributeRatio(video.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) ||
        width != kExpectedWidth || height != kExpectedHeight ||
        numerator != kExpectedFrameRate || denominator != 1) {
        return MF_E_INVALIDMEDIATYPE;
    }

    UINT32 sampleRate = 0;
    UINT32 channels = 0;
    if (FAILED(result) || FAILED(audio->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(audio->GetGUID(MF_MT_SUBTYPE, &subtype)) || major != MFMediaType_Audio ||
        subtype != MFAudioFormat_MP3 ||
        FAILED(audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate)) ||
        FAILED(audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
        sampleRate != kExpectedSampleRate || channels != kExpectedChannels) {
        return MF_E_INVALIDMEDIATYPE;
    }
    return S_OK;
}

HRESULT ConfigureOutputs(IMFSourceReader* reader, DWORD videoStream, DWORD audioStream) {
    HRESULT result = reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(result)) {
        result = reader->SetStreamSelection(videoStream, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = reader->SetStreamSelection(audioStream, TRUE);
    }
    ComPtr<IMFMediaType> video;
    if (SUCCEEDED(result)) {
        result = MFCreateMediaType(&video);
    }
    if (SUCCEEDED(result)) {
        result = video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = video->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(videoStream, nullptr, video.Get());
    }
    ComPtr<IMFMediaType> audio;
    if (SUCCEEDED(result)) {
        result = MFCreateMediaType(&audio);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kExpectedSampleRate);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kExpectedChannels);
    }
    if (SUCCEEDED(result)) {
        result = audio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                  kExpectedChannels * sizeof(std::int16_t));
    }
    if (SUCCEEDED(result)) {
        result = audio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                  kExpectedSampleRate * kExpectedChannels *
                                      sizeof(std::int16_t));
    }
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(audioStream, nullptr, audio.Get());
    }
    return result;
}

HRESULT CopyVideoFrame(IMFSample* sample, LONGLONG timestamp, VideoFrame& frame) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IMF2DBuffer> buffer2d;
    result = buffer.As(&buffer2d);
    if (FAILED(result)) {
        return result;
    }
    BYTE* scanline = nullptr;
    LONG pitch = 0;
    result = buffer2d->Lock2D(&scanline, &pitch);
    if (FAILED(result)) {
        return result;
    }
    frame.timestamp = timestamp;
    frame.pixels.resize(static_cast<std::size_t>(kExpectedWidth) * kExpectedHeight * 4);
    for (UINT32 y = 0; y < kExpectedHeight; ++y) {
        std::memcpy(frame.pixels.data() + static_cast<std::size_t>(y) * kExpectedWidth * 4,
                    scanline + static_cast<ptrdiff_t>(y) * pitch,
                    static_cast<std::size_t>(kExpectedWidth) * 4);
    }
    buffer2d->Unlock2D();
    return S_OK;
}

HRESULT DecodeAudioPacket(IMFSample* sample, LONGLONG timestamp,
                          std::unique_ptr<AudioPacket>& packet) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(result)) {
        return result;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    result = buffer->Lock(&data, nullptr, &length);
    if (FAILED(result)) {
        return result;
    }
    packet = std::make_unique<AudioPacket>();
    packet->timestamp = timestamp;
    packet->bytes.assign(data, data + length);
    buffer->Unlock();
    if (FAILED(sample->GetSampleDuration(&packet->duration)) || packet->duration <= 0) {
        packet->duration = static_cast<LONGLONG>(length) * kHundredNanosecondsPerSecond /
                           (kExpectedSampleRate * kExpectedChannels * sizeof(std::int16_t));
    }
    return S_OK;
}

HRESULT SubmitAudioPacket(std::unique_ptr<AudioPacket> packet,
                          IXAudio2SourceVoice* voice) {
    XAUDIO2_BUFFER xaudioBuffer{};
    xaudioBuffer.AudioBytes = static_cast<UINT32>(packet->bytes.size());
    xaudioBuffer.pAudioData = packet->bytes.data();
    xaudioBuffer.pContext = packet.get();
    const HRESULT result = voice->SubmitSourceBuffer(&xaudioBuffer);
    if (SUCCEEDED(result)) {
        packet.release();
    }
    return result;
}

void ApplyPauseState(IXAudio2SourceVoice* voice) {
    if (!voice) {
        return;
    }
    PlaybackWatchdog* watchdog = g_playbackWatchdog.load();
    const wchar_t* previousActivity =
        watchdog ? watchdog->ExchangeActivity(L"xaudio_pause_state_change") : nullptr;
    if (g_state.IsPaused() || g_audioRebuffering.load()) {
        voice->Stop(0);
    } else {
        voice->Start(0);
    }
    if (watchdog) watchdog->SetActivity(previousActivity);
}

void PumpMessages(PlaybackWatchdog& watchdog) {
    watchdog.SetActivity(L"windows_message_queue");
    MSG message{};
    for (std::size_t processed = 0; processed < kMaximumMessagesPerPump; ++processed) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
        const bool skip = message.hwnd == g_owner &&
                          (message.message == WM_LBUTTONUP ||
                           (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE));
        if (skip) {
            movie_trace::Record(L"stop_requested", L"source=input");
            g_state.RequestStop();
            continue;
        }
        TranslateMessage(&message);
        watchdog.SetActivity(L"window_message_dispatch");
        DispatchMessageW(&message);
        watchdog.SetActivity(L"windows_message_queue");
    }
    watchdog.SetActivity(L"playback_loop");
}

LRESULT CALLBACK MovieGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && wParam == PM_REMOVE) {
        auto* message = reinterpret_cast<MSG*>(lParam);
        if (message && message->hwnd == g_owner &&
            (message->message == WM_LBUTTONUP ||
             (message->message == WM_KEYDOWN && message->wParam == VK_ESCAPE))) {
            movie_trace::Record(L"stop_requested", L"source=input_hook");
            g_state.RequestStop();
            message->message = WM_NULL;
        }
    }
    return CallNextHookEx(g_messageHook, code, wParam, lParam);
}

HRESULT OpenReader(const wchar_t* path, ComPtr<IMFSourceReader>& reader,
                   DWORD& videoStream, DWORD& audioStream, const wchar_t*& stage) {
    ComPtr<IMFByteStream> byteStream;
    stage = L"byte stream creation";
    HRESULT result = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                  MF_FILEFLAGS_NONE, path, &byteStream);
    ComPtr<IMFAttributes> streamAttributes;
    if (SUCCEEDED(result)) result = byteStream.As(&streamAttributes);
    if (SUCCEEDED(result)) {
        result = streamAttributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/avi");
    }
    ComPtr<IMFAttributes> readerAttributes;
    if (SUCCEEDED(result)) result = MFCreateAttributes(&readerAttributes, 1);
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    stage = L"AVI source reader creation";
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromByteStream(byteStream.Get(), readerAttributes.Get(),
                                                    &reader);
    }
    if (SUCCEEDED(result)) {
        stage = L"native stream validation";
        result = ValidateNativeStreams(reader.Get(), videoStream, audioStream);
    }
    if (SUCCEEDED(result)) {
        stage = L"decoder output configuration";
        result = ConfigureOutputs(reader.Get(), videoStream, audioStream);
    }
    return result;
}

void DecodeIntoQueue(std::wstring path, DecoderQueue& queue) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    HRESULT result = comResult == RPC_E_CHANGED_MODE ? S_OK : comResult;
    const wchar_t* stage = L"decoder COM initialization";
    ComPtr<IMFSourceReader> reader;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    if (SUCCEEDED(result)) {
        result = OpenReader(path.c_str(), reader, videoStream, audioStream, stage);
    }
    {
        std::lock_guard lock(queue.mutex);
        queue.initialized = true;
        queue.error = result;
        queue.errorStage = stage;
    }
    queue.changed.notify_all();
    {
        wchar_t details[256]{};
        swprintf_s(details, L"video_stream=%lu audio_stream=%lu hresult=0x%08X",
                   videoStream, audioStream, static_cast<unsigned int>(result));
        movie_trace::Record(L"decoder_initialized", details);
    }

    movie_state::DecodeStream preferred = movie_state::DecodeStream::Audio;
    while (SUCCEEDED(result)) {
        movie_state::DecodeStream requested = movie_state::DecodeStream::None;
        {
            std::unique_lock lock(queue.mutex);
            queue.changed.wait(lock, [&queue] {
                return queue.stop || movie_state::SelectDecodeStream(
                                         queue.videoEnded, queue.video.size(),
                                         kDecodedVideoFrames, queue.audioEnded,
                                         queue.audioDuration, kDecodedQueueDuration,
                                         movie_state::DecodeStream::Audio) !=
                                         movie_state::DecodeStream::None;
            });
            if (queue.stop) break;
            requested = movie_state::SelectDecodeStream(
                queue.videoEnded, queue.video.size(), kDecodedVideoFrames,
                queue.audioEnded, queue.audioDuration, kDecodedQueueDuration, preferred);
        }
        preferred = requested == movie_state::DecodeStream::Audio
                        ? movie_state::DecodeStream::Video
                        : movie_state::DecodeStream::Audio;

        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        stage = L"sample decoding";
        const DWORD requestedStream = requested == movie_state::DecodeStream::Audio
                                          ? audioStream
                                          : videoStream;
        result = reader->ReadSample(requestedStream, 0,
                                    &actualStream, &flags, &timestamp, &sample);
        if (FAILED(result)) break;
        if (actualStream != requestedStream) {
            result = MF_E_INVALIDSTREAMNUMBER;
            stage = L"unexpected decoded stream";
            break;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            result = MF_E_TRANSFORM_STREAM_CHANGE;
            stage = L"unexpected media type change";
            break;
        }

        bool changed = false;
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            {
                std::lock_guard lock(queue.mutex);
                if (requested == movie_state::DecodeStream::Video) queue.videoEnded = true;
                if (requested == movie_state::DecodeStream::Audio) queue.audioEnded = true;
            }
            movie_trace::Record(requested == movie_state::DecodeStream::Video
                                    ? L"decoder_video_eos"
                                    : L"decoder_audio_eos");
            changed = true;
        } else if (sample && actualStream == videoStream) {
            VideoFrame frame;
            result = CopyVideoFrame(sample.Get(), timestamp, frame);
            if (SUCCEEDED(result)) {
                std::lock_guard lock(queue.mutex);
                queue.lastVideoTimestamp = timestamp;
                ++queue.decodedVideoFrames;
                queue.video.push_back(std::move(frame));
                changed = true;
            }
        } else if (sample && actualStream == audioStream) {
            std::unique_ptr<AudioPacket> packet;
            result = DecodeAudioPacket(sample.Get(), timestamp, packet);
            if (SUCCEEDED(result)) {
                std::lock_guard lock(queue.mutex);
                queue.lastAudioTimestamp = timestamp;
                ++queue.decodedAudioPackets;
                queue.audioDuration += packet->duration;
                queue.audio.push_back(std::move(packet));
                changed = true;
            }
        }
        if (changed) queue.changed.notify_all();
        {
            std::lock_guard lock(queue.mutex);
            if (queue.videoEnded && queue.audioEnded) break;
        }
    }

    {
        std::lock_guard lock(queue.mutex);
        if (FAILED(result)) {
            queue.error = result;
            queue.errorStage = stage;
        }
    }
    queue.changed.notify_all();
    {
        wchar_t details[384]{};
        bool stopped = false;
        {
            std::lock_guard lock(queue.mutex);
            stopped = queue.stop;
        }
        swprintf_s(details, L"stopped=%d stage=\"%s\" hresult=0x%08X",
                   stopped ? 1 : 0, stage, static_cast<unsigned int>(result));
        movie_trace::Record(L"decoder_exit", details);
    }
    if (uninitializeCom) CoUninitialize();
}

HRESULT RunPlayback(HWND owner, const wchar_t* path) {
    movie_trace::Record(L"playback_setup_begin");
    PlaybackWatchdog watchdog;
    WatchdogBinding watchdogBinding(watchdog);
    HRESULT result = ValidateAviSignature(path);
    const wchar_t* stage = L"AVI signature validation";
    if (FAILED(result)) {
        TraceFailure(stage, result);
        return result;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        TraceFailure(L"COM initialization", comResult);
        return comResult;
    }
    result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
        TraceFailure(L"Media Foundation startup", result);
        if (uninitializeCom) CoUninitialize();
        return result;
    }

    DecoderQueue decoder;
    std::thread decoderThread(&DecodeIntoQueue, std::wstring(path), std::ref(decoder));
    watchdog.SetActivity(L"waiting_for_decoder_initialization");
    while (!g_state.IsStopRequested()) {
        PumpMessages(watchdog);
        std::unique_lock lock(decoder.mutex);
        if (decoder.changed.wait_for(lock, std::chrono::milliseconds(5),
                                     [&decoder] { return decoder.initialized; })) {
            result = decoder.error;
            stage = decoder.errorStage;
            break;
        }
    }
    const bool cancelledBeforeSetup = g_state.IsStopRequested();

    ComPtr<IXAudio2> xaudio;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    IXAudio2SourceVoice* sourceVoice = nullptr;
    VoiceCallback voiceCallback;
    watchdog.SetActivity(L"creating_audio_and_video_devices");
    if (SUCCEEDED(result) && !cancelledBeforeSetup) {
        stage = L"XAudio2 creation";
        result = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    }
    if (SUCCEEDED(result) && !cancelledBeforeSetup) {
        result = xaudio->CreateMasteringVoice(&masteringVoice, kExpectedChannels,
                                               kExpectedSampleRate);
    }
    WAVEFORMATEX waveFormat{};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = kExpectedChannels;
    waveFormat.nSamplesPerSec = kExpectedSampleRate;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
    if (SUCCEEDED(result) && !cancelledBeforeSetup) {
        result = xaudio->CreateSourceVoice(&sourceVoice, &waveFormat, 0,
                                           XAUDIO2_DEFAULT_FREQ_RATIO, &voiceCallback);
    }
    if (SUCCEEDED(result) && !cancelledBeforeSetup) {
        sourceVoice->SetVolume(std::pow(10.0F, g_volumeHundredthsDb.load() / 2000.0F));
        if (!d3d11_presenter::BeginMovie(owner, kExpectedWidth, kExpectedHeight)) {
            result = E_FAIL;
            stage = L"D3D11 movie mode";
        }
    }
    if (SUCCEEDED(result) && !cancelledBeforeSetup) {
        movie_trace::Record(L"playback_devices_ready");
    }

    std::deque<VideoFrame> frames;
    LONGLONG audioQueued = 0;
    LONGLONG audioStartTimestamp = -1;
    bool videoEnded = false;
    bool audioEnded = false;
    bool started = false;
    bool audioRunning = false;
    LONGLONG lastClock = -1;
    LONGLONG lastPresentedTimestamp = -1;
    std::uint64_t droppedVideoFrames = 0;
    auto nextSnapshot = std::chrono::steady_clock::now();
    watchdog.SetActivity(L"playback_loop");
    while (SUCCEEDED(result) && !cancelledBeforeSetup && !g_state.IsStopRequested()) {
        PumpMessages(watchdog);

        XAUDIO2_VOICE_STATE voiceState{};
        sourceVoice->GetState(&voiceState);
        std::deque<std::unique_ptr<AudioPacket>> audioToSubmit;
        std::size_t decoderVideoQueue = 0;
        std::size_t decoderAudioQueue = 0;
        LONGLONG decoderAudioDuration = 0;
        LONGLONG lastDecodedVideoTimestamp = -1;
        LONGLONG lastDecodedAudioTimestamp = -1;
        std::uint64_t decodedVideoFrames = 0;
        std::uint64_t decodedAudioPackets = 0;
        watchdog.SetActivity(L"decoder_queue_lock");
        {
            std::lock_guard lock(decoder.mutex);
            while (!decoder.video.empty() && frames.size() < kPresentationVideoFrames) {
                frames.push_back(std::move(decoder.video.front()));
                decoder.video.pop_front();
            }
            UINT32 availableSlots = voiceState.BuffersQueued < kMaximumSubmittedAudioBuffers
                                        ? kMaximumSubmittedAudioBuffers - voiceState.BuffersQueued
                                        : 0;
            while (availableSlots-- > 0 && !decoder.audio.empty()) {
                decoder.audioDuration -= decoder.audio.front()->duration;
                audioToSubmit.push_back(std::move(decoder.audio.front()));
                decoder.audio.pop_front();
            }
            videoEnded = decoder.videoEnded;
            audioEnded = decoder.audioEnded;
            decoderVideoQueue = decoder.video.size();
            decoderAudioQueue = decoder.audio.size();
            decoderAudioDuration = decoder.audioDuration;
            lastDecodedVideoTimestamp = decoder.lastVideoTimestamp;
            lastDecodedAudioTimestamp = decoder.lastAudioTimestamp;
            decodedVideoFrames = decoder.decodedVideoFrames;
            decodedAudioPackets = decoder.decodedAudioPackets;
            if (FAILED(decoder.error)) {
                result = decoder.error;
                stage = decoder.errorStage;
            }
        }
        watchdog.SetActivity(L"playback_loop");
        decoder.changed.notify_all();
        if (FAILED(result)) break;

        for (auto& packet : audioToSubmit) {
            watchdog.SetActivity(L"audio_buffer_submission");
            if (audioStartTimestamp < 0) audioStartTimestamp = packet->timestamp;
            audioQueued += packet->duration;
            result = SubmitAudioPacket(std::move(packet), sourceVoice);
            if (FAILED(result)) {
                stage = L"XAudio2 buffer submission";
                break;
            }
        }
        watchdog.SetActivity(L"playback_loop");
        if (FAILED(result)) break;

        if (!started && !g_state.IsPaused() &&
            movie_state::ReadyToStart(!frames.empty(), audioQueued, audioEnded)) {
            result = sourceVoice->Start(0);
            if (SUCCEEDED(result)) {
                started = true;
                audioRunning = true;
                g_audioRebuffering.store(false);
                g_activeVoice = sourceVoice;
                movie_trace::Record(L"playback_started");
            }
        }
        if (!started && videoEnded && audioEnded &&
            (frames.empty() || audioQueued == 0)) {
            result = MF_E_END_OF_STREAM;
            stage = L"media prebuffer";
            break;
        }
        if (!started && std::chrono::steady_clock::now() >= nextSnapshot) {
            wchar_t details[768]{};
            swprintf_s(
                details,
                L"started=0 paused=%d video_eos=%d audio_eos=%d present_queue=%zu "
                L"decoder_video_queue=%zu decoder_audio_queue=%zu decoder_audio_duration=%lld "
                L"decoded_video=%llu decoded_audio=%llu last_decoded_video=%lld "
                L"last_decoded_audio=%lld audio_buffers=%u",
                g_state.IsPaused() ? 1 : 0, videoEnded ? 1 : 0, audioEnded ? 1 : 0,
                frames.size(), decoderVideoQueue, decoderAudioQueue,
                decoderAudioDuration,
                static_cast<unsigned long long>(decodedVideoFrames),
                static_cast<unsigned long long>(decodedAudioPackets),
                lastDecodedVideoTimestamp, lastDecodedAudioTimestamp,
                voiceState.BuffersQueued);
            movie_trace::Record(L"snapshot", details);
            nextSnapshot = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (started) {
            sourceVoice->GetState(&voiceState);
            const LONGLONG played = static_cast<LONGLONG>(voiceState.SamplesPlayed) *
                                   kHundredNanosecondsPerSecond / kExpectedSampleRate;
            const LONGLONG buffered = std::max<LONGLONG>(0, audioQueued - played);
            if (audioRunning &&
                movie_state::AudioUnderrun(started, audioEnded, voiceState.BuffersQueued)) {
                sourceVoice->Stop(0);
                audioRunning = false;
                g_audioRebuffering.store(true);
                OutputDebugStringW(L"San9Toolkit movie: audio underrun; rebuffering.\n");
                movie_trace::Record(L"audio_underrun");
            }
            if (!audioRunning && !g_state.IsPaused() && movie_state::ReadyAfterUnderrun(
                                     buffered, voiceState.BuffersQueued, audioEnded)) {
                result = sourceVoice->Start(0);
                if (FAILED(result)) {
                    stage = L"XAudio2 restart after rebuffer";
                    break;
                }
                audioRunning = true;
                g_audioRebuffering.store(false);
                movie_trace::Record(L"audio_restarted");
            }
            lastClock = audioStartTimestamp + played;
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSnapshot) {
                const LONGLONG nextVideoTimestamp =
                    frames.empty() ? -1 : frames.front().timestamp;
                wchar_t details[1024]{};
                swprintf_s(
                    details,
                    L"started=%d paused=%d audio_running=%d video_eos=%d audio_eos=%d "
                    L"clock=%lld last_presented=%lld next_video=%lld present_queue=%zu "
                    L"decoder_video_queue=%zu decoder_audio_queue=%zu decoder_audio_duration=%lld "
                    L"decoded_video=%llu decoded_audio=%llu last_decoded_video=%lld "
                    L"last_decoded_audio=%lld audio_buffers=%u samples_played=%llu dropped_video=%llu",
                    started ? 1 : 0, g_state.IsPaused() ? 1 : 0,
                    audioRunning ? 1 : 0, videoEnded ? 1 : 0, audioEnded ? 1 : 0,
                    lastClock, lastPresentedTimestamp, nextVideoTimestamp, frames.size(),
                    decoderVideoQueue, decoderAudioQueue, decoderAudioDuration,
                    static_cast<unsigned long long>(decodedVideoFrames),
                    static_cast<unsigned long long>(decodedAudioPackets),
                    lastDecodedVideoTimestamp, lastDecodedAudioTimestamp,
                    voiceState.BuffersQueued,
                    static_cast<unsigned long long>(voiceState.SamplesPlayed),
                    static_cast<unsigned long long>(droppedVideoFrames));
                movie_trace::Record(L"snapshot", details);
                nextSnapshot = now + std::chrono::seconds(1);
            }
            if (g_state.IsPaused() || !audioRunning) {
                Sleep(5);
                continue;
            }
            const LONGLONG clock = lastClock;
            std::vector<std::int64_t> timestamps;
            timestamps.reserve(frames.size());
            for (const VideoFrame& frame : frames) timestamps.push_back(frame.timestamp);
            std::size_t dropCount = movie_state::LateFramesToDrop(timestamps, clock);
            droppedVideoFrames += dropCount;
            while (dropCount-- > 0) frames.pop_front();
            if (!frames.empty() &&
                frames.front().timestamp <= clock + movie_state::kLateFrameThreshold / 2) {
                const VideoFrame& frame = frames.front();
                watchdog.SetActivity(L"video_frame_presentation");
                if (!d3d11_presenter::PresentMovieFrame(frame.pixels.data(), kExpectedWidth * 4)) {
                    result = E_FAIL;
                    stage = L"movie frame presentation";
                    break;
                }
                watchdog.SetActivity(L"playback_loop");
                lastPresentedTimestamp = frame.timestamp;
                frames.pop_front();
            }
            if (movie_state::PlaybackComplete(videoEnded, audioEnded, frames.size(),
                                              voiceState.BuffersQueued)) {
                movie_trace::Record(L"playback_complete");
                break;
            }
        }
        if (FAILED(voiceCallback.Error())) {
            result = voiceCallback.Error();
            stage = L"XAudio2 voice";
            break;
        }
        Sleep(1);
    }

    g_activeVoice = nullptr;
    g_audioRebuffering.store(false);
    watchdog.SetActivity(L"decoder_thread_shutdown");
    {
        std::lock_guard lock(decoder.mutex);
        decoder.stop = true;
    }
    decoder.changed.notify_all();
    decoderThread.join();
    watchdog.SetActivity(L"playback_cleanup");
    if (sourceVoice) {
        sourceVoice->Stop(0);
        sourceVoice->FlushSourceBuffers();
        sourceVoice->DestroyVoice();
    }
    if (masteringVoice) masteringVoice->DestroyVoice();
    d3d11_presenter::EndMovie();
    MFShutdown();
    if (uninitializeCom) CoUninitialize();
    if (FAILED(result)) TraceFailure(stage, result);
    return result;
}

int __cdecl ModernPlay(HWND owner, const char* path, int, int) {
    if (!owner || !path || !g_state.Begin()) {
        return 0;
    }
    const bool wasMaximized = IsZoomed(owner) != FALSE;
    g_preservePostMovieMaximized.store(false);
    g_postMovieWindow.store(nullptr);
    g_owner = owner;
    g_state.PauseByWindow(IsIconic(owner) != FALSE || GetForegroundWindow() != owner);
    cursor_lock::Suspend();
    status_overlay::Hide();
    g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, &MovieGetMessageHook, nullptr,
                                      GetCurrentThreadId());

    std::wstring widePath;
    const int required = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1,
                                              nullptr, 0);
    HRESULT result = E_INVALIDARG;
    if (required > 0) {
        widePath.resize(static_cast<std::size_t>(required));
        MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1,
                            widePath.data(), required);
        widePath.resize(static_cast<std::size_t>(required - 1));
        movie_trace::BeginPlayback(std::filesystem::path(widePath).filename().wstring());
        result = RunPlayback(owner, widePath.c_str());
    } else {
        movie_trace::BeginPlayback(L"invalid-path-encoding");
        TraceFailure(L"movie path conversion", result);
    }

    if (g_messageHook) {
        UnhookWindowsHookEx(g_messageHook);
        g_messageHook = nullptr;
    }

    cursor_lock::Resume();
    g_owner = nullptr;
    g_state.Finish();
    wchar_t endDetails[128]{};
    swprintf_s(endDetails, L"result=0x%08X stopped=%d",
               static_cast<unsigned int>(result),
               g_state.IsStopRequested() ? 1 : 0);
    movie_trace::EndPlayback(endDetails);
    if (FAILED(result)) {
        status_overlay::ShowMessage(owner, L"影片无法播放，已跳过");
    }
    g_postMovieWindow.store(owner);
    g_preservePostMovieMaximized.store(wasMaximized);
    return SUCCEEDED(result) ? 1 : 0;
}

int __cdecl ModernCheckFinish() { return g_state.IsFinished() ? 1 : 0; }
int __cdecl ModernIsPlaying() { return g_state.IsPlaying() ? 1 : 0; }
void __cdecl ModernSetVolume(int volume) {
    volume = std::clamp(volume, -10'000, 0);
    g_volumeHundredthsDb.store(volume);
    if (g_activeVoice) g_activeVoice->SetVolume(std::pow(10.0F, volume / 2000.0F));
}
void __cdecl ModernPause() {
    g_state.PauseByApi(true);
    movie_trace::Record(L"pause_changed", L"source=api paused=1");
    ApplyPauseState(g_activeVoice);
}
void __cdecl ModernRestart() {
    g_state.PauseByApi(false);
    movie_trace::Record(L"pause_changed", L"source=api paused=0");
    ApplyPauseState(g_activeVoice);
}
void __cdecl ModernExit() {
    movie_trace::Record(L"stop_requested", L"source=api");
    g_state.RequestStop();
}
void __cdecl ModernNotify(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    HandleWindowMessage(window, message, wParam, lParam);
}
void __cdecl ModernSetMessageLoop(void(__cdecl*)()) {}

} // namespace

bool Install(const std::filesystem::path& configPath) {
    movie_trace::Initialize(configPath);
    return import_hook::Install("user32.dll", "ShowWindow", &ModernShowWindow,
                                g_oldShowWindow) &&
           import_hook::Install("koeimpeg.dll", "MPEGMoviePlay", &ModernPlay, g_oldPlay) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieCheckFinish", &ModernCheckFinish,
                                g_oldCheckFinish) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieIsPlaying", &ModernIsPlaying,
                                g_oldIsPlaying) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieSetSoundVolume", &ModernSetVolume,
                                g_oldSetVolume) &&
           import_hook::Install("koeimpeg.dll", "MPEGMoviePause", &ModernPause, g_oldPause) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieReStart", &ModernRestart, g_oldRestart) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieExit", &ModernExit, g_oldExit) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieNotifyOwnerMessage", &ModernNotify,
                                g_oldNotify) &&
           import_hook::Install("koeimpeg.dll", "MPEGMovieSetSan9MsgLoop", &ModernSetMessageLoop,
                                g_oldSetMessageLoop);
}

bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM) {
    if (!g_state.IsPlaying() || window != g_owner) return false;
    if (message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY) {
        movie_trace::Record(L"stop_requested", L"source=window");
        g_state.RequestStop();
        return false;
    }
    if (message == WM_ACTIVATE) {
        g_state.PauseByWindow(LOWORD(wParam) == WA_INACTIVE);
        movie_trace::Record(L"pause_changed", LOWORD(wParam) == WA_INACTIVE
                                                   ? L"source=activate paused=1"
                                                   : L"source=activate paused=0");
        ApplyPauseState(g_activeVoice);
    } else if (message == WM_ACTIVATEAPP) {
        g_state.PauseByWindow(wParam == FALSE);
        movie_trace::Record(L"pause_changed", wParam == FALSE
                                                   ? L"source=activate_app paused=1"
                                                   : L"source=activate_app paused=0");
        ApplyPauseState(g_activeVoice);
    } else if (message == WM_SIZE) {
        g_state.PauseByWindow(wParam == SIZE_MINIMIZED);
        movie_trace::Record(L"pause_changed", wParam == SIZE_MINIMIZED
                                                   ? L"source=minimize paused=1"
                                                   : L"source=minimize paused=0");
        ApplyPauseState(g_activeVoice);
    }
    if (message == WM_PAINT || message == WM_SIZE) {
        d3d11_presenter::PresentCurrentFrame();
    }
    return false;
}

void Shutdown() {
    g_state.RequestStop();
}

} // namespace san9::movie_player
