#include "movie_state.h"
#include "toolkit_config.h"

#include <array>
#include <cassert>

int main() {
    san9::movie_state::PlaybackState state;
    assert(state.IsFinished());
    assert(!state.IsPlaying());
    assert(state.Begin());
    assert(!state.Begin());
    assert(state.IsPlaying());
    assert(!state.IsFinished());
    state.PauseByApi(true);
    assert(state.IsPaused());
    state.PauseByWindow(true);
    state.PauseByApi(false);
    assert(state.IsPaused());
    state.PauseByWindow(false);
    assert(!state.IsPaused());
    state.RequestStop();
    assert(state.IsStopRequested());
    state.Finish();
    assert(state.IsFinished());
    assert(state.Begin());
    assert(!state.IsStopRequested());
    state.Finish();

    using namespace san9::movie_state;
    assert(!ReadyToStart(false, kPrebufferDuration, false));
    assert(!ReadyToStart(true, kPrebufferDuration - 1, false));
    assert(ReadyToStart(true, kPrebufferDuration, false));
    assert(ReadyToStart(true, 1, true));

    constexpr std::array<std::int64_t, 4> timestamps{0, 333'333, 666'666, 999'999};
    assert(LateFramesToDrop(timestamps, 0) == 0);
    assert(LateFramesToDrop(timestamps, 800'000) == 2);
    assert(LateFramesToDrop(timestamps, 20'000'000) == 3);
    assert(!PlaybackComplete(false, true, 0, 0));
    assert(!PlaybackComplete(true, false, 0, 0));
    assert(!PlaybackComplete(true, true, 1, 0));
    assert(!PlaybackComplete(true, true, 0, 1));
    assert(PlaybackComplete(true, true, 0, 0));
    assert(!AudioUnderrun(false, false, 0));
    assert(!AudioUnderrun(true, true, 0));
    assert(!AudioUnderrun(true, false, 1));
    assert(AudioUnderrun(true, false, 0));
    assert(!ReadyAfterUnderrun(kPrebufferDuration, 0, false));
    assert(!ReadyAfterUnderrun(kPrebufferDuration - 1, 1, false));
    assert(ReadyAfterUnderrun(kPrebufferDuration, 1, false));
    assert(ReadyAfterUnderrun(1, 1, true));

    using san9::toolkit_config::EdgeScrollMode;
    using san9::toolkit_config::GetEdgeScrollMode;
    using san9::toolkit_config::SetEdgeScrollMode;
    constexpr DWORD unrelatedFlags = 0xA5A5001F;
    const DWORD hoverFlags = SetEdgeScrollMode(unrelatedFlags, EdgeScrollMode::Hover);
    assert(GetEdgeScrollMode(hoverFlags) == EdgeScrollMode::Hover);
    assert((hoverFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag) ==
           (unrelatedFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag));
    const DWORD holdFlags = SetEdgeScrollMode(hoverFlags, EdgeScrollMode::HoldLeftButton);
    assert(GetEdgeScrollMode(holdFlags) == EdgeScrollMode::HoldLeftButton);
    assert((holdFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag) ==
           (unrelatedFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag));
    return 0;
}
