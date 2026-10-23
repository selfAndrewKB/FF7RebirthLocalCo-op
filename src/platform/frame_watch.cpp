#include "platform/frame_watch.h"

#include "platform/every.h"
#include "platform/hitch_step.h"
#include "platform/log.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::platform::frame_watch {

namespace {

constexpr double      kAverageWeight   = 0.05;  // running average: each frame moves it 5% of the way
constexpr double      kHitchRatio      = 1.6;   // a hitch is this much over the average...
constexpr double      kHitchMarginMs   = 5.0;   // ...and at least this many ms over it
constexpr double      kSummarySeconds  = 10.0;
constexpr unsigned    kLinesPerSummary = 25;
constexpr unsigned    kNewNormalRun    = 30;    // this many hitch frames in a row reset the average
constexpr std::size_t kMaxNotes        = 6;
constexpr std::size_t kParts           = static_cast<std::size_t>(Part::Count);
constexpr const wchar_t* kPartNames[kParts] = {L"engine tick", L"overlay render", L"input", L"member tick"};

// Game thread, except g_detailed (any thread reads it).
std::array<double, kParts>                 g_partSeconds{};
std::array<double, kParts>                 g_partTotals{};  // summed over the summary's frames (the per-frame average)
std::array<const wchar_t*, kMaxNotes>      g_notes{};
std::size_t                                g_noteCount = 0;
unsigned                                   g_linesCounted = 0;  // LinesWritten() as of the last frame start
double                                     g_frameStart   = 0.0;
HitchState                                 g_hitch;              // the running average and the hitch run
double                                     g_summaryStart = 0.0;
unsigned                                   g_hitches      = 0;
unsigned                                   g_hitchLines   = 0;
double                                     g_worstMs      = 0.0;
unsigned                                   g_frames       = 0;
std::atomic<bool>                          g_detailed{false};
std::uint64_t                              g_frameIndex = 0;  // frames started since the mod loaded (never reset)
std::array<WindowReport, kMaxWindowReports> g_reports{};     // the window's reports, in the order added
std::size_t                                g_reportCount = 0;

struct JobTime
{
    const wchar_t* name  = nullptr;
    unsigned       count = 0;
    double         total = 0.0;
    double         worst = 0.0;
};
constexpr std::size_t          kMaxJobs = 12;
std::array<JobTime, kMaxJobs> g_jobs{};

void AddJobTime(const wchar_t* name, double seconds) noexcept
{
    for (JobTime& job : g_jobs)
    {
        if (job.name == nullptr || job.name == name)
        {
            job.name = name;
            ++job.count;
            job.total += seconds;
            job.worst = seconds > job.worst ? seconds : job.worst;
            return;
        }
    }
}

std::wstring JobSummary()
{
    std::wstring text;
    for (const JobTime& job : g_jobs)
    {
        if (job.name != nullptr && job.count > 0)
        {
            text += std::format(L"{}{} x{} avg {:.2f} worst {:.2f} ms", text.empty() ? L"" : L"; ", job.name, job.count,
                                job.total * 1000.0 / job.count, job.worst * 1000.0);
        }
    }
    return text.empty() ? L"none" : text;
}

void LogHitch(double frameMs, unsigned logLines)
{
    if (!g_detailed.load(std::memory_order_relaxed) || g_hitchLines >= kLinesPerSummary)
    {
        return;
    }
    ++g_hitchLines;
    std::wstring parts;
    for (std::size_t i = 0; i < kParts; ++i)
    {
        parts += std::format(L"{}{} {:.2f}", i == 0 ? L"" : L", ", kPartNames[i], g_partSeconds[i] * 1000.0);
    }
    std::wstring jobs;
    for (std::size_t i = 0; i < g_noteCount; ++i)
    {
        jobs += std::format(L"{}{}", i == 0 ? L"" : L", ", g_notes[i]);
    }
    Log(L"hitch: frame {:.1f} ms (average {:.1f}); the mod's time in it: {} ms; {} log line(s); periodic jobs: {}", frameMs,
        g_hitch.averageMs, parts, logLines, jobs.empty() ? L"none" : jobs.c_str());
}

}  // namespace

double Now() noexcept
{
    // The counter's rate is fixed for the process; read once, on the first call from any thread (a function-local
    // static initialises under the language's own lock, so no thread sees it half-set).
    static const double secondsPerCount = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 1.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * secondsPerCount;
}

Charge::Charge(Part part) noexcept : part_(part), start_(Now()) {}

Job::Job(const wchar_t* name) noexcept : name_(name), start_(Now())
{
    Note(name);
}

Job::~Job()
{
    AddJobTime(name_, Now() - start_);
}

Charge::~Charge()
{
    g_partSeconds[static_cast<std::size_t>(part_)] += Now() - start_;
}

void Note(const wchar_t* job) noexcept
{
    for (std::size_t i = 0; i < g_noteCount; ++i)
    {
        if (g_notes[i] == job)
        {
            return;
        }
    }
    if (g_noteCount < kMaxNotes)
    {
        g_notes[g_noteCount++] = job;
    }
}

void SetDetailed(bool on) noexcept
{
    g_detailed.store(on, std::memory_order_relaxed);
    Log(L"perf log {}: the 10 s hitch summary, hitch lines and the periodic timing lines", on ? L"on" : L"off");
}

bool Detailed() noexcept
{
    return g_detailed.load(std::memory_order_relaxed);
}

bool AddWindowReport(WindowReport report) noexcept
{
    for (std::size_t i = 0; i < g_reportCount; ++i)
    {
        if (g_reports[i] == report)
        {
            return true;
        }
    }
    if (report == nullptr || g_reportCount == kMaxWindowReports)
    {
        return false;
    }
    g_reports[g_reportCount++] = report;
    return true;
}

std::uint64_t FrameIndex() noexcept
{
    return g_frameIndex;
}

void OnFrameStart() noexcept
{
    ++g_frameIndex;
    const double now      = Now();
    const unsigned written = LinesWritten();
    const unsigned lines   = written - g_linesCounted;  // this frame's
    g_linesCounted         = written;
    if (g_frameStart != 0.0)
    {
        const double frameMs = (now - g_frameStart) * 1000.0;
        ++g_frames;
        // A hitch does not move the average (it would hide the next one); long "hitches" are the new normal (a
        // load screen ended, say): platform/hitch_step.h.
        if (HitchStep(g_hitch, frameMs, kHitchRatio, kHitchMarginMs, kNewNormalRun, kAverageWeight) == FrameKind::Hitch)
        {
            ++g_hitches;
            g_worstMs = frameMs > g_worstMs ? frameMs : g_worstMs;
            try
            {
                LogHitch(frameMs, lines);
            }
            catch (...)
            {
            }
        }
    }
    if (g_frameStart != 0.0)  // the frames the summary counts
    {
        for (std::size_t i = 0; i < kParts; ++i)
        {
            g_partTotals[i] += g_partSeconds[i];
        }
    }
    g_frameStart = now;
    g_partSeconds.fill(0.0);
    g_noteCount = 0;

    if (g_summaryStart == 0.0)
    {
        g_summaryStart = now;
    }
    else if (now - g_summaryStart >= kSummarySeconds)
    {
        const std::uint32_t frames  = g_frames;
        const double        seconds = now - g_summaryStart;
        const bool          log     = Detailed();
        try
        {
            if (log)  // counted either way; written only with the perf log on
            {
                std::wstring perFrame;
                for (std::size_t i = 0; i < kParts; ++i)
                {
                    perFrame += std::format(L"{}{} {:.3f}", i == 0 ? L"" : L", ", kPartNames[i],
                                            g_frames > 0 ? g_partTotals[i] * 1000.0 / g_frames : 0.0);
                }
                Log(L"hitches: {} in the last {:.0f} s ({} frames, average {:.1f} ms), worst {:.1f} ms; the mod's hooks per "
                    L"frame (ms): {}; the mod's periodic jobs: {}",
                    g_hitches, now - g_summaryStart, g_frames, g_hitch.averageMs, g_worstMs, perFrame, JobSummary());
            }
        }
        catch (...)
        {
        }
        g_jobs = {};
        g_partTotals.fill(0.0);
        g_summaryStart = now;
        g_hitches = g_hitchLines = g_frames = 0;
        g_worstMs = 0.0;
        for (std::size_t i = 0; i < g_reportCount; ++i)
        {
            g_reports[i](frames, seconds, log);  // the modules' reports of the window, after its own summary
        }
        g_linesCounted = LinesWritten();  // the summary's and reports' lines are not the next frame's
    }
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"perf-log",
                       .shape = control::Shape::OnOff,
                       .help  = L"each hitch's own line, the 10 s hitch summary and the periodic timing lines (off at "
                                L"start; perf_log.txt turns it on at launch)",
                       .set   = [](const control::Value& v) {
                           SetDetailed(std::get<bool>(v));
                           return control::Outcome{true, std::get<bool>(v) ? L"perf log on" : L"perf log off"};
                       },
                       .get = [] { return control::Value{Detailed()}; }});
}

#endif

}  // namespace lss::platform::frame_watch
