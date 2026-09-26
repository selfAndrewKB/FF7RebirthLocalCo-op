#include "platform/frame_watch.h"

#include "platform/log.h"

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

double g_secondsPerCount = 0.0;

double Now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (g_secondsPerCount == 0.0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        g_secondsPerCount = 1.0 / static_cast<double>(frequency.QuadPart);
    }
    return static_cast<double>(counter.QuadPart) * g_secondsPerCount;
}

// Game thread, except the log-line counter.
std::array<double, kParts>                 g_partSeconds{};
std::array<double, kParts>                 g_partTotals{};  // summed over the summary's frames (the per-frame average)
std::array<const wchar_t*, kMaxNotes>      g_notes{};
std::size_t                                g_noteCount = 0;
std::atomic<unsigned>                      g_logLines{0};
double                                     g_frameStart   = 0.0;
double                                     g_averageMs    = 0.0;
double                                     g_summaryStart = 0.0;
unsigned                                   g_hitches      = 0;
unsigned                                   g_hitchLines   = 0;
double                                     g_worstMs      = 0.0;
unsigned                                   g_frames       = 0;
unsigned                                   g_hitchRun     = 0;  // hitch frames in a row
std::atomic<bool>                          g_detailed{false};

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
        g_averageMs, parts, logLines, jobs.empty() ? L"none" : jobs.c_str());
}

}  // namespace

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
    Log(L"perf log {}: hitch lines and the periodic timing lines", on ? L"on" : L"off");
}

bool Detailed() noexcept
{
    return g_detailed.load(std::memory_order_relaxed);
}

void CountLogLine() noexcept
{
    g_logLines.fetch_add(1, std::memory_order_relaxed);
}

void OnFrameStart() noexcept
{
    const double now      = Now();
    const unsigned lines  = g_logLines.exchange(0, std::memory_order_relaxed);
    if (g_frameStart != 0.0)
    {
        const double frameMs = (now - g_frameStart) * 1000.0;
        ++g_frames;
        if (g_averageMs == 0.0)
        {
            g_averageMs = frameMs;
        }
        else if (frameMs > g_averageMs * kHitchRatio && frameMs > g_averageMs + kHitchMarginMs &&
                 ++g_hitchRun <= kNewNormalRun)
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
            // A hitch does not move the average (it would hide the next one).
        }
        else if (g_hitchRun > kNewNormalRun)
        {
            g_averageMs = frameMs;  // long "hitches" are the new normal (a load screen ended, say)
            g_hitchRun  = 0;
        }
        else
        {
            g_hitchRun = 0;
            g_averageMs += (frameMs - g_averageMs) * kAverageWeight;
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
        try
        {
            std::wstring perFrame;
            for (std::size_t i = 0; i < kParts; ++i)
            {
                perFrame += std::format(L"{}{} {:.3f}", i == 0 ? L"" : L", ", kPartNames[i],
                                        g_frames > 0 ? g_partTotals[i] * 1000.0 / g_frames : 0.0);
            }
            Log(L"hitches: {} in the last {:.0f} s ({} frames, average {:.1f} ms), worst {:.1f} ms; the mod's hooks per frame "
                L"(ms): {}; the mod's periodic jobs: {}",
                g_hitches, now - g_summaryStart, g_frames, g_averageMs, g_worstMs, perFrame, JobSummary());
        }
        catch (...)
        {
        }
        g_jobs = {};
        g_partTotals.fill(0.0);
        g_summaryStart = now;
        g_hitches = g_hitchLines = g_frames = 0;
        g_worstMs = 0.0;
        g_logLines.store(0, std::memory_order_relaxed);  // the summary's own line is not the next frame's
    }
}

}  // namespace lss::platform::frame_watch
