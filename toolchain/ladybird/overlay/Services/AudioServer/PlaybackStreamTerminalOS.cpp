/*
 * Ladybird on Terminal OS: sound out through the kernel's one output stream
 * (SYS_SOUND: 48 kHz, 16-bit stereo; a write waits while the card's queue is
 * full, so the writing thread keeps time by itself).
 *
 * A thread asks Ladybird for 20 ms of audio at a time, turns the float
 * samples into 16-bit ones and queues them. Suspended, it waits instead.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Vector.h>
#include <AudioServer/PlatformAudio.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadedPromise.h>
#include <LibThreading/Thread.h>
#include <terminalos/syscall.h>
#include <unistd.h>

namespace Audio {

static constexpr u32 sample_rate = 48000;
static constexpr size_t frames_per_request = sample_rate / 50;    // 20 ms

static long sound(long op, long a = 0, long b = 0)
{
    return tos_syscall(TOS_SOUND, op, a, b, 0);
}

class PlaybackStreamTerminalOS final : public PlaybackStream {
public:
    struct State : public AtomicRefCounted<State> {
        AudioDataRequestCallback callback;
        Atomic<bool> playing { false };
        Atomic<bool> exit { false };
        Atomic<bool> discard { false };
        Atomic<bool> data_available { true };
        Atomic<u32> volume_permille { 1000 };

        void run()
        {
            Vector<float> samples;
            Vector<i16> frames;
            samples.resize(frames_per_request * 2);
            frames.resize(frames_per_request * 2);
            bool open = true;                               // (opened by create_platform_playback_stream)
            int idle_ms = 0;
            while (!exit) {
                if (discard.exchange(false) && open) {
                    sound(TOS_SOUND_CLOSE);                 // drops what the card has queued
                    open = sound(TOS_SOUND_OPEN) >= 0;
                }
                if (!playing || !data_available) {
                    // Paused (or out of data) a while: give the card back, so
                    // other programs (the video player) can play meanwhile.
                    if (open && (idle_ms += 5) >= 500 && sound(TOS_SOUND_QUEUED) <= 0) {
                        sound(TOS_SOUND_CLOSE);
                        open = false;
                    }
                    usleep(5000);
                    continue;
                }
                idle_ms = 0;
                if (!open && !(open = sound(TOS_SOUND_OPEN) >= 0)) {
                    usleep(100000);                         // another program is playing
                    continue;
                }
                long queued = sound(TOS_SOUND_QUEUED);
                auto starts = MonotonicTime::now() + AK::Duration::from_microseconds(max(queued, 0l) * 1'000'000 / sample_rate);
                auto filled = callback(samples.span(), starts);
                size_t count = filled.size() / 2;
                if (count == 0) {
                    data_available = false;                 // an underrun: wait for notify_data_available()
                    continue;
                }
                float gain = volume_permille.load() / 1000.0f;
                for (size_t i = 0; i < count * 2; ++i) {
                    float v = filled[i] * gain;
                    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                    frames[i] = static_cast<i16>(v * 32767.0f);
                }
                sound(TOS_SOUND_WRITE, reinterpret_cast<long>(frames.data()), static_cast<long>(count));
            }
            if (open)
                sound(TOS_SOUND_CLOSE);
        }
    };

    explicit PlaybackStreamTerminalOS(NonnullRefPtr<State> state)
        : m_state(move(state))
    {
    }

    ~PlaybackStreamTerminalOS() override { m_state->exit = true; }

    SampleSpecification sample_specification() const override
    {
        return SampleSpecification { sample_rate, ChannelMap::stereo() };
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> resume() override
    {
        m_state->data_available = true;
        m_state->playing = true;
        return resolved();
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override
    {
        m_state->playing = false;
        for (int i = 0; i < 200 && sound(TOS_SOUND_QUEUED) > 0; ++i)   // (at most 2 s; 0 or an error once closed)
            usleep(10000);
        return resolved();
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override
    {
        m_state->playing = false;
        m_state->discard = true;
        return resolved();
    }

    void notify_data_available() override { m_state->data_available = true; }

    NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double volume) override
    {
        m_state->volume_permille = static_cast<u32>(clamp(volume, 0.0, 1.0) * 1000);
        return resolved();
    }

private:
    static NonnullRefPtr<Core::ThreadedPromise<void>> resolved()
    {
        auto promise = Core::ThreadedPromise<void>::create();
        promise->resolve();
        return promise;
    }

    NonnullRefPtr<State> m_state;
};

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState initial_state, u32, PlaybackStream::AudioDataRequestCallback data_request_callback)
{
    if (long r = sound(TOS_SOUND_OPEN); r < 0)
        return PlaybackStream::CreatePromise::rejected(Error::from_string_literal("No sound output (no sound card, or another program is playing)"));

    auto state = adopt_ref(*new PlaybackStreamTerminalOS::State);
    state->callback = move(data_request_callback);
    state->playing = initial_state == OutputState::Playing;
    auto thread = Threading::Thread::construct("Sound output"sv, [state] {
        state->run();
        return 0;
    });
    thread->start();
    thread->detach();

    auto promise = PlaybackStream::CreatePromise::construct();
    promise->resolve(adopt_ref(*new PlaybackStreamTerminalOS(state)));
    return promise;
}

}
