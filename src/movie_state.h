#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace san9::movie_state {

constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;
constexpr std::int64_t kPrebufferDuration = 5'000'000;
constexpr std::int64_t kLateFrameThreshold = kHundredNanosecondsPerSecond / 30;

enum class DecodeStream {
    None,
    Video,
    Audio,
};

inline DecodeStream SelectDecodeStream(bool videoEnded, std::size_t queuedVideoFrames,
                                       std::size_t maximumVideoFrames, bool audioEnded,
                                       std::int64_t bufferedAudio,
                                       std::int64_t maximumBufferedAudio,
                                       DecodeStream preferred) {
    const bool canDecodeVideo = !videoEnded && queuedVideoFrames < maximumVideoFrames;
    const bool canDecodeAudio = !audioEnded && bufferedAudio < maximumBufferedAudio;
    if (canDecodeVideo && canDecodeAudio) {
        return preferred == DecodeStream::Video ? DecodeStream::Video
                                                : DecodeStream::Audio;
    }
    if (canDecodeAudio) {
        return DecodeStream::Audio;
    }
    if (canDecodeVideo) {
        return DecodeStream::Video;
    }
    return DecodeStream::None;
}

class PlaybackState {
public:
    bool Begin() {
        bool expected = false;
        if (!playing_.compare_exchange_strong(expected, true)) {
            return false;
        }
        finished_.store(false);
        stopRequested_.store(false);
        apiPaused_.store(false);
        windowPaused_.store(false);
        return true;
    }

    void Finish() {
        playing_.store(false);
        finished_.store(true);
    }

    void RequestStop() { stopRequested_.store(true); }
    void PauseByApi(bool paused) { apiPaused_.store(paused); }
    void PauseByWindow(bool paused) { windowPaused_.store(paused); }
    bool IsPlaying() const { return playing_.load(); }
    bool IsFinished() const { return finished_.load(); }
    bool IsStopRequested() const { return stopRequested_.load(); }
    bool IsPaused() const { return apiPaused_.load() || windowPaused_.load(); }

private:
    std::atomic_bool playing_{false};
    std::atomic_bool finished_{true};
    std::atomic_bool stopRequested_{false};
    std::atomic_bool apiPaused_{false};
    std::atomic_bool windowPaused_{false};
};

inline bool ReadyToStart(bool hasVideoFrame, std::int64_t queuedAudio,
                         bool audioEnded) {
    return hasVideoFrame && queuedAudio > 0 &&
           (queuedAudio >= kPrebufferDuration || audioEnded);
}

inline std::size_t LateFramesToDrop(std::span<const std::int64_t> timestamps,
                                    std::int64_t clock) {
    std::size_t count = 0;
    while (count + 1 < timestamps.size() &&
           timestamps[count] + kLateFrameThreshold < clock) {
        ++count;
    }
    return count;
}

inline bool PlaybackComplete(bool videoEnded, bool audioEnded,
                             std::size_t queuedVideoFrames,
                             std::uint32_t queuedAudioBuffers) {
    return videoEnded && audioEnded && queuedVideoFrames == 0 && queuedAudioBuffers == 0;
}

inline bool AudioUnderrun(bool started, bool audioEnded,
                          std::uint32_t queuedAudioBuffers) {
    return started && !audioEnded && queuedAudioBuffers == 0;
}

inline bool ReadyAfterUnderrun(std::int64_t bufferedAudio,
                               std::uint32_t queuedAudioBuffers,
                               bool audioEnded) {
    return queuedAudioBuffers > 0 &&
           (bufferedAudio >= kPrebufferDuration || audioEnded);
}

} // namespace san9::movie_state
