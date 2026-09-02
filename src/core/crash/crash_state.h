#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <ctime>

namespace counterstrikesharp::crash {

constexpr int kMaxModules = 320;
constexpr int kModuleNameLen = 128;
constexpr int kMaxNames = 1024;
constexpr int kNameLen = 64;
constexpr int kLineSlots = 128;
constexpr int kLineLen = 512;
constexpr int kFatalSlots = 8;
constexpr int kFatalLen = 2048;
constexpr int kCrumbSlots = 256;
constexpr int kCommandSlots = 32;
constexpr int kCommandLen = 72;
constexpr int kMaxFrames = 40;
constexpr int kScanWordBudget = 4096;
constexpr int kStackDumpBytes = 8192;
constexpr int kPathLen = 512;
constexpr int kSignalSlots = 64;

struct ModuleEntry
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t bias;
    char buildId[41];
    char name[kModuleNameLen];
};

struct LineSlot
{
    std::atomic<uint32_t> state{ 0 };
    uint32_t hash;
    uint32_t repeat;
    int severity;
    int source;
    int64_t stampMs;
    uint16_t length;
    char text[kLineLen];
};

struct FatalSlot
{
    std::atomic<uint32_t> state{ 0 };
    int severity;
    int64_t stampMs;
    uint16_t length;
    char text[kFatalLen];
};

struct Crumb
{
    std::atomic<uint32_t> state{ 0 };
    int32_t tick;
    uint16_t site;
    uint16_t name;
};

struct CommandSlot
{
    std::atomic<uint32_t> state{ 0 };
    int32_t tick;
    char text[kCommandLen];
};

struct State
{
    std::atomic<bool> installed{ false };
    std::atomic<bool> handlerBusy{ false };
    std::atomic<bool> consoleVerified{ false };
    std::atomic<bool> cleanExit{ false };
    std::atomic<uint32_t> lineCursor{ 0 };
    std::atomic<uint32_t> fatalCursor{ 0 };
    std::atomic<uint32_t> crumbCursor{ 0 };
    std::atomic<uint32_t> commandCursor{ 0 };
    std::atomic<int32_t> tick{ 0 };
    std::atomic<int32_t> nameCount{ 0 };
    std::atomic<int32_t> moduleCount[2]{};
    std::atomic<int32_t> activeSet{ 0 };

    int reportFd;
    int probeRead;
    int probeWrite;
    int64_t startedMs;
    int64_t startedUnix;

    uintptr_t fatalErrorStart;
    uintptr_t fatalErrorEnd;

    char runId[17];
    char installNote[192];
    char reportPath[kPathLen];
    char finalPath[kPathLen];
    char fallbackPath[kPathLen];
    char perfMapPath[kPathLen];
    char mapName[64];
    char version[64];

    ModuleEntry modules[2][kMaxModules];
    char names[kMaxNames][kNameLen];
    LineSlot lines[kLineSlots];
    FatalSlot fatal[kFatalSlots];
    Crumb crumbs[kCrumbSlots];
    CommandSlot commands[kCommandSlots];

    struct sigaction previous[kSignalSlots];
};

extern State g_state;

inline int64_t NowMs()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

} // namespace counterstrikesharp::crash
