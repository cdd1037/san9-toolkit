#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* g_stage = L"start";

HRESULT Probe(const std::filesystem::path& path) {
    g_stage = L"RIFF";
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    std::uint8_t header[12]{};
    DWORD bytesRead = 0;
    const bool read = ReadFile(file, header, sizeof(header), &bytesRead, nullptr) != FALSE;
    CloseHandle(file);
    if (!read || bytesRead != sizeof(header) || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "AVI ", 4) != 0) return MF_E_INVALID_FILE_FORMAT;

    g_stage = L"source reader";
    ComPtr<IMFByteStream> stream;
    HRESULT result = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                  MF_FILEFLAGS_NONE, path.c_str(), &stream);
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(result)) result = stream.As(&attributes);
    if (SUCCEEDED(result)) result = attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/avi");
    ComPtr<IMFAttributes> readerAttributes;
    if (SUCCEEDED(result)) result = MFCreateAttributes(&readerAttributes, 1);
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromByteStream(stream.Get(), readerAttributes.Get(), &reader);
    }

    const DWORD videoStream = 0;
    const DWORD audioStream = 1;
    g_stage = L"native video";
    ComPtr<IMFMediaType> video;
    GUID major{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (SUCCEEDED(result)) result = reader->GetNativeMediaType(videoStream, 0, &video);
    if (SUCCEEDED(result)) result = video->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (SUCCEEDED(result)) result = video->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (SUCCEEDED(result) && (major != MFMediaType_Video || subtype != MFVideoFormat_WMV3)) {
        result = MF_E_INVALIDMEDIATYPE;
    }
    if (SUCCEEDED(result)) result = MFGetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (SUCCEEDED(result)) {
        result = MFGetAttributeRatio(video.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
    }
    if (SUCCEEDED(result) && (width != 640 || height != 480 || numerator != 30 || denominator != 1)) {
        result = MF_E_INVALIDMEDIATYPE;
    }

    g_stage = L"native audio";
    ComPtr<IMFMediaType> audio;
    UINT32 sampleRate = 0;
    UINT32 channels = 0;
    if (SUCCEEDED(result)) result = reader->GetNativeMediaType(audioStream, 0, &audio);
    if (SUCCEEDED(result)) result = audio->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (SUCCEEDED(result)) result = audio->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (SUCCEEDED(result) && (major != MFMediaType_Audio || subtype != MFAudioFormat_MP3)) {
        result = MF_E_INVALIDMEDIATYPE;
    }
    if (SUCCEEDED(result)) result = audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    if (SUCCEEDED(result)) result = audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (SUCCEEDED(result) && (sampleRate != 44'100 || channels != 2)) result = MF_E_INVALIDMEDIATYPE;

    g_stage = L"RGB32 output";
    ComPtr<IMFMediaType> decodedVideo;
    if (SUCCEEDED(result)) result = MFCreateMediaType(&decodedVideo);
    if (SUCCEEDED(result)) result = decodedVideo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = decodedVideo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(videoStream, nullptr, decodedVideo.Get());
    g_stage = L"PCM output";
    ComPtr<IMFMediaType> decodedAudio;
    if (SUCCEEDED(result)) result = MFCreateMediaType(&decodedAudio);
    if (SUCCEEDED(result)) result = decodedAudio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result)) result = decodedAudio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(result)) result = decodedAudio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(result)) result = decodedAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44'100);
    if (SUCCEEDED(result)) result = decodedAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(result)) result = decodedAudio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    if (SUCCEEDED(result)) result = decodedAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 176'400);
    if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(audioStream, nullptr, decodedAudio.Get());

    g_stage = L"full decode";
    std::uint64_t videoSamples = 0;
    std::uint64_t audioBytes = 0;
    bool videoEnded = false;
    bool audioEnded = false;
    while (SUCCEEDED(result) && (!videoEnded || !audioEnded)) {
        DWORD actualStream = 0;
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        g_stage = L"ReadSample";
        result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
                                    &actualStream, &flags, nullptr, &sample);
        if (FAILED(result)) {
            std::fwprintf(stderr, L"decode stopped after %llu video samples and %llu audio bytes\n",
                          videoSamples, audioBytes);
            break;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            if (actualStream == videoStream) videoEnded = true;
            if (actualStream == audioStream) audioEnded = true;
            if (actualStream != videoStream && actualStream != audioStream) {
                videoEnded = true;
                audioEnded = true;
            }
            continue;
        }
        if (!sample) continue;
        if (actualStream == videoStream) {
            g_stage = L"video buffer";
            ComPtr<IMFMediaBuffer> buffer;
            result = sample->ConvertToContiguousBuffer(&buffer);
            ComPtr<IMF2DBuffer> buffer2d;
            if (SUCCEEDED(result)) result = buffer.As(&buffer2d);
            DWORD length = 0;
            if (SUCCEEDED(result)) result = buffer->GetCurrentLength(&length);
            if (SUCCEEDED(result) && length < width * height * 4) result = E_UNEXPECTED;
            ++videoSamples;
        } else if (actualStream == audioStream) {
            g_stage = L"audio buffer";
            ComPtr<IMFMediaBuffer> buffer;
            result = sample->ConvertToContiguousBuffer(&buffer);
            DWORD length = 0;
            if (SUCCEEDED(result)) result = buffer->GetCurrentLength(&length);
            audioBytes += length;
        }
    }
    if (SUCCEEDED(result) && (videoSamples < 3 || audioBytes == 0)) {
        std::fwprintf(stderr, L"decoded %llu video samples and %llu audio bytes\n",
                      videoSamples, audioBytes);
        result = MF_E_INVALID_FILE_FORMAT;
    }
    return result;
}

} // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) return 2;
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) return 3;
    result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) return 4;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(arguments[1])) {
        if (!entry.is_regular_file()) continue;
        result = Probe(entry.path());
        if (FAILED(result)) {
            std::fwprintf(stderr, L"%s (%s): 0x%08X\n", entry.path().filename().c_str(),
                          g_stage,
                          static_cast<unsigned int>(result));
            break;
        }
        ++count;
    }
    MFShutdown();
    if (uninitialize) CoUninitialize();
    if (FAILED(result) || count == 0) return 1;
    std::wprintf(L"validated %zu movie files\n", count);
    return 0;
}
