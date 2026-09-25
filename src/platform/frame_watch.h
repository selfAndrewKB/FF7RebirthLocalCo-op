#pragma once

// Frame hitches, and what the mod did in them.
//
// Once per frame (the engine-tick callback, on the game thread) the time since the last frame is
// compared with a running average. A frame well over it is a hitch: it is logged with the mod's own
// time in that frame, by hook, the log lines written, and the periodic jobs that ran (a HUD root
// scan, a timing log, the status line...). So a hitch the mod causes names its cause, and one it
// does not shows the mod idle. Every 10 s a summary line gives the hitch count and the worst.
// Solo and split are both watched, so they can be compared.

namespace lss::platform::frame_watch {

enum class Part
{
    EngineTick,     // the mod's engine-tick callback
    OverlayRender,  // the mod's work around the overlay render (the HUD passes)
    InputTick,      // the button views after the input Tick
    MemberTick,     // player 2's member's move-state tick (engine work the mod adds)
    Count,
};

/// Adds the time between construction and destruction to `part` for this frame. Game thread.
class Charge
{
public:
    explicit Charge(Part part) noexcept;
    ~Charge();
    Charge(const Charge&)            = delete;
    Charge& operator=(const Charge&) = delete;

private:
    Part   part_;
    double start_;
};

/// A periodic job ran this frame. `job` must be a string literal. Game thread.
void Note(const wchar_t* job) noexcept;

/// A periodic job, noted and timed from construction to destruction. The summary lists each job's
/// count, average and worst time. `name` must be a string literal. Game thread.
class Job
{
public:
    explicit Job(const wchar_t* name) noexcept;
    ~Job();
    Job(const Job&)            = delete;
    Job& operator=(const Job&) = delete;

private:
    const wchar_t* name_;
    double         start_;
};

/// The `perf-log` switch (off at start): each hitch's own line, the modules' periodic timing lines
/// and player 2's 10 s status block. The 10 s hitch summary is written either way.
void SetDetailed(bool on) noexcept;
bool Detailed() noexcept;

/// A log line was written (any thread).
void CountLogLine() noexcept;

/// Once per frame, at the start of the engine tick (game thread): ends the last frame, logs it if it
/// was a hitch, and every 10 s the summary.
void OnFrameStart() noexcept;

}  // namespace lss::platform::frame_watch
