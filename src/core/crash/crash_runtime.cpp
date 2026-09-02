#include "core/crash/crash_reporter.h"

#ifndef _WIN32

#include "core/crash/crash_state.h"
#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <link.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace counterstrikesharp::crash {

namespace {

std::mutex g_nameLock;
std::string g_crashDirectory;

struct SnapshotContext
{
    ModuleEntry* entries;
    int count;
};

void ReadBuildId(uintptr_t address, size_t size, char* destination)
{
    static const char kDigits[] = "0123456789abcdef";
    size_t offset = 0;

    while (offset + sizeof(ElfW(Nhdr)) <= size)
    {
        const ElfW(Nhdr)* note = (const ElfW(Nhdr)*)(address + offset);
        size_t nameSize = ((size_t)note->n_namesz + 3) & ~(size_t)3;
        size_t descriptionSize = ((size_t)note->n_descsz + 3) & ~(size_t)3;
        const char* name = (const char*)(note + 1);

        if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && strncmp(name, "GNU", 4) == 0)
        {
            const unsigned char* data = (const unsigned char*)(name + nameSize);
            size_t length = note->n_descsz > 20 ? 20 : note->n_descsz;
            for (size_t index = 0; index < length; ++index)
            {
                destination[index * 2] = kDigits[data[index] >> 4];
                destination[index * 2 + 1] = kDigits[data[index] & 0xF];
            }
            destination[length * 2] = '\0';
            return;
        }

        offset += sizeof(ElfW(Nhdr)) + nameSize + descriptionSize;
    }
}

int CollectModule(struct dl_phdr_info* info, size_t, void* data)
{
    SnapshotContext* context = (SnapshotContext*)data;
    if (context->count >= kMaxModules) return 0;

    ModuleEntry& entry = context->entries[context->count];
    memset(&entry, 0, sizeof(entry));
    entry.bias = (uintptr_t)info->dlpi_addr;

    uintptr_t low = UINTPTR_MAX;
    uintptr_t high = 0;

    for (int index = 0; index < info->dlpi_phnum; ++index)
    {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type == PT_LOAD)
        {
            uintptr_t start = entry.bias + header.p_vaddr;
            if (start < low) low = start;
            if (start + header.p_memsz > high) high = start + header.p_memsz;
        }
        else if (header.p_type == PT_NOTE)
        {
            ReadBuildId(entry.bias + header.p_vaddr, header.p_memsz, entry.buildId);
        }
    }

    if (high == 0 || low == UINTPTR_MAX) return 0;

    entry.start = low;
    entry.end = high;

    const char* name = info->dlpi_name;
    if (name == nullptr || name[0] == '\0') name = "[main-executable]";
    const char* slash = strrchr(name, '/');
    if (slash != nullptr) name = slash + 1;
    strncpy(entry.name, name, kModuleNameLen - 1);

    context->count++;
    return 0;
}

void RefreshModules()
{
    int active = g_state.activeSet.load(std::memory_order_acquire);
    int target = active == 0 ? 1 : 0;

    SnapshotContext context{ g_state.modules[target], 0 };
    dl_iterate_phdr(&CollectModule, &context);

    std::sort(g_state.modules[target], g_state.modules[target] + context.count,
              [](const ModuleEntry& left, const ModuleEntry& right) { return left.start < right.start; });

    g_state.moduleCount[target].store(context.count, std::memory_order_release);
    g_state.activeSet.store(target, std::memory_order_release);
}

void ResolveFatalError()
{
    void* address = dlsym(RTLD_DEFAULT, "Plat_FatalError");
    if (address == nullptr) return;

    g_state.fatalErrorStart = (uintptr_t)address;
    g_state.fatalErrorEnd = (uintptr_t)address + 0x400;
}

void CopyInto(char* destination, size_t capacity, const std::string& source)
{
    size_t length = source.size() < capacity - 1 ? source.size() : capacity - 1;
    memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

std::string ReadCommandLine()
{
    std::ifstream stream("/proc/self/cmdline", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    for (char& symbol : raw)
    {
        if (symbol == '\0') symbol = ' ';
    }
    return raw;
}

void WriteManifest()
{
    if (g_crashDirectory.empty()) return;

    std::ofstream stream(g_crashDirectory + "/run-" + g_state.runId + ".txt", std::ios::trunc);
    if (!stream.is_open()) return;

    stream << "[RUN]\nrun=" << g_state.runId << " version=" << g_state.version << " pid=" << getpid() << "\n";
    stream << "map=" << (g_state.mapName[0] != '\0' ? g_state.mapName : "none") << " started_unix=" << g_state.startedUnix << "\n";
    stream << "installed=" << (g_state.installed.load() ? "yes" : "no") << " note=" << g_state.installNote << "\n";
    stream << "\n[CMDLINE]\n" << ReadCommandLine() << "\n";

    stream << "\n[MODULES]\n";
    int active = g_state.activeSet.load(std::memory_order_acquire);
    int count = g_state.moduleCount[active].load(std::memory_order_acquire);
    for (int index = 0; index < count; ++index)
    {
        const ModuleEntry& entry = g_state.modules[active][index];
        stream << entry.name << " base=0x" << std::hex << entry.bias << " range=0x" << entry.start << "..0x" << entry.end << std::dec
               << " build-id=" << (entry.buildId[0] != '\0' ? entry.buildId : "none") << "\n";
    }

    stream << "\n[END]\n";
}

void AppendRunLog(const char* event)
{
    if (g_crashDirectory.empty()) return;

    std::ofstream stream(g_crashDirectory + "/runs.jsonl", std::ios::app);
    if (!stream.is_open()) return;

    stream << "{\"run\":\"" << g_state.runId << "\",\"event\":\"" << event << "\",\"unix\":" << (int64_t)time(nullptr)
           << ",\"version\":\"" << g_state.version << "\",\"pid\":" << getpid() << "}\n";
}

void SweepAndRotate()
{
    namespace fs = std::filesystem;
    std::error_code error;

    fs::directory_iterator iterator(g_crashDirectory, error);
    if (error) return;

    std::vector<fs::directory_entry> reports;
    for (const auto& entry : iterator)
    {
        std::error_code inner;
        if (!entry.is_regular_file(inner)) continue;

        const std::string name = entry.path().filename().string();

        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0)
        {
            char head[16] = { 0 };
            std::ifstream probe(entry.path(), std::ios::binary);
            probe.read(head, sizeof(head) - 1);

            if (strncmp(head, "[VERDICT]", 9) != 0)
            {
                fs::remove(entry.path(), inner);
                CSSHARP_CORE_WARN("Crash reporter: a previous run died without reaching the handler (stack overflow, SIGKILL or an "
                                  "out-of-memory kill): {}",
                                  name);
                continue;
            }

            fs::rename(entry.path(), entry.path().string() + "ial-truncated", inner);
            CSSHARP_CORE_WARN("Crash reporter: a previous report stops mid-way, keeping it as truncated: {}", name);
            continue;
        }

        if (name.rfind("crash-", 0) == 0) reports.push_back(entry);
    }

    std::sort(reports.begin(), reports.end(),
              [](const fs::directory_entry& left, const fs::directory_entry& right)
              {
                  std::error_code inner;
                  return fs::last_write_time(left, inner) < fs::last_write_time(right, inner);
              });

    uintmax_t total = 0;
    for (const auto& entry : reports)
    {
        std::error_code inner;
        total += fs::file_size(entry, inner);
    }

    const size_t kPinnedOldest = 3;
    const size_t kMaxFiles = 30;
    const uintmax_t kMaxBytes = 8u * 1024u * 1024u;

    size_t remaining = reports.size();
    size_t index = kPinnedOldest;
    while (index < reports.size() && (remaining > kMaxFiles || total > kMaxBytes))
    {
        std::error_code inner;
        uintmax_t size = fs::file_size(reports[index], inner);
        if (fs::remove(reports[index], inner))
        {
            total -= size;
            remaining--;
        }
        index++;
    }

    if (!reports.empty())
    {
        CSSHARP_CORE_INFO("Crash reporter: {} report(s) kept in {}", reports.size(), g_crashDirectory);
    }
}

void PrepareReportFile()
{
    const std::string base = g_crashDirectory + "/crash-" + std::to_string(g_state.startedUnix) + "-" + g_state.runId + ".txt";

    CopyInto(g_state.finalPath, kPathLen, base);
    CopyInto(g_state.reportPath, kPathLen, base + ".part");
    CopyInto(g_state.fallbackPath, kPathLen, g_crashDirectory + "/crash-fallback-" + g_state.runId + ".txt");

    int fd = open(g_state.reportPath, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        CSSHARP_CORE_ERROR("Crash reporter: could not preallocate '{}', reports will use the fallback path", g_state.reportPath);
        return;
    }

    if (posix_fallocate(fd, 0, 128 * 1024) != 0)
    {
        CSSHARP_CORE_WARN("Crash reporter: could not reserve space for the report file, writing may fail on a full disk");
    }

    g_state.reportFd = fd;
}

bool Disabled()
{
    const char* toggle = getenv("CSSHARP_CRASH_REPORTER");
    return toggle != nullptr && toggle[0] == '0';
}

uint32_t HashText(const char* text)
{
    uint32_t hash = 2166136261u;
    for (const char* cursor = text; *cursor != '\0'; ++cursor)
    {
        hash ^= (uint32_t)(unsigned char)*cursor;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

void ConsoleAttach();
void ConsoleDetach();

void OnEarlyLoad()
{
    CopyInto(g_state.version, sizeof(g_state.version), std::string("v" SEMVER " @ " GITHUB_SHA));

    if (Disabled())
    {
        CSSHARP_CORE_WARN("Crash reporter disabled by CSSHARP_CRASH_REPORTER=0");
        return;
    }

    RefreshModules();
    ResolveFatalError();

    const char* perfMap = getenv("CSSHARP_CRASH_PERFMAP");
    if (perfMap != nullptr && perfMap[0] == '1')
    {
        setenv("DOTNET_PerfMapEnabled", "1", 0);
        CopyInto(g_state.perfMapPath, kPathLen, "/tmp/perf-" + std::to_string(getpid()) + ".map");
    }

    if (g_state.installed.load())
    {
        CSSHARP_CORE_INFO("Crash reporter armed, run id {} ({})", g_state.runId, g_state.installNote);
    }
    else
    {
        CSSHARP_CORE_WARN("Crash reporter is NOT armed: {}", g_state.installNote);
    }

    ConsoleAttach();
}

void OnPathsReady(const char* rootDirectory)
{
    if (rootDirectory == nullptr || Disabled()) return;

    std::error_code error;
    g_crashDirectory = std::string(rootDirectory) + "/logs/crashes";
    std::filesystem::create_directories(g_crashDirectory, error);

    if (error)
    {
        CSSHARP_CORE_ERROR("Crash reporter: could not create '{}', reports will be lost", g_crashDirectory);
        g_crashDirectory.clear();
        return;
    }

    SweepAndRotate();
    PrepareReportFile();
    WriteManifest();
    AppendRunLog("start");

    CSSHARP_CORE_INFO("Crash reporter: run {} will write to {}", g_state.runId, g_state.finalPath);
}

void OnAllPluginsLoaded()
{
    if (Disabled()) return;

    RefreshModules();
    WriteManifest();

    struct sigaction current;
    if (sigaction(SIGSEGV, nullptr, &current) != 0) return;

    void* owner = (current.sa_flags & SA_SIGINFO) != 0 ? (void*)current.sa_sigaction : (void*)current.sa_handler;
    if (owner == nullptr || owner == (void*)SIG_DFL || owner == (void*)SIG_IGN)
    {
        CSSHARP_CORE_WARN("Crash reporter: nothing owns SIGSEGV, the .NET runtime has not installed its handler");
        return;
    }

    Dl_info info;
    if (dladdr(owner, &info) != 0 && info.dli_fname != nullptr)
    {
        CSSHARP_CORE_INFO("Crash reporter: SIGSEGV is currently owned by {} (we are behind it in the chain)", info.dli_fname);
    }
    else
    {
        CSSHARP_CORE_INFO("Crash reporter: SIGSEGV is owned by an unnamed handler at {}", owner);
    }
}

void OnMapChange(const char* mapName)
{
    if (Disabled()) return;

    if (mapName != nullptr) CopyInto(g_state.mapName, sizeof(g_state.mapName), std::string(mapName));

    RefreshModules();
    WriteManifest();

    CSSHARP_CORE_INFO("Crash reporter: run {} still armed, map {}", g_state.runId, g_state.mapName);
}

void OnUnload()
{
    if (Disabled()) return;

    ConsoleDetach();
    g_state.cleanExit.store(true, std::memory_order_release);
    AppendRunLog("clean-exit");

    if (g_state.reportFd >= 0)
    {
        int fd = g_state.reportFd;
        g_state.reportFd = -1;
        close(fd);
        unlink(g_state.reportPath);
    }
}

uint16_t RegisterName(const char* name)
{
    if (name == nullptr || name[0] == '\0') return 0;

    std::lock_guard<std::mutex> guard(g_nameLock);

    int count = g_state.nameCount.load(std::memory_order_acquire);
    for (int index = 0; index < count; ++index)
    {
        if (strncmp(g_state.names[index], name, kNameLen - 1) == 0) return (uint16_t)(index + 1);
    }

    if (count >= kMaxNames) return 0;

    strncpy(g_state.names[count], name, kNameLen - 1);
    g_state.names[count][kNameLen - 1] = '\0';
    g_state.nameCount.store(count + 1, std::memory_order_release);

    return (uint16_t)(count + 1);
}

void Breadcrumb(uint16_t site, uint16_t name)
{
    uint32_t index = g_state.crumbCursor.fetch_add(1, std::memory_order_acq_rel);
    Crumb& crumb = g_state.crumbs[index % kCrumbSlots];

    crumb.state.store(1, std::memory_order_release);
    crumb.site = site;
    crumb.name = name;
    crumb.tick = g_state.tick.load(std::memory_order_relaxed);
    crumb.state.store(2, std::memory_order_release);
}

void PushCommand(const char* name)
{
    if (name == nullptr || name[0] == '\0') return;

    uint32_t index = g_state.commandCursor.fetch_add(1, std::memory_order_acq_rel);
    CommandSlot& slot = g_state.commands[index % kCommandSlots];

    slot.state.store(1, std::memory_order_release);
    strncpy(slot.text, name, kCommandLen - 1);
    slot.text[kCommandLen - 1] = '\0';
    slot.tick = g_state.tick.load(std::memory_order_relaxed);
    slot.state.store(2, std::memory_order_release);
}

void SetTick(int32_t tick) { g_state.tick.store(tick, std::memory_order_relaxed); }

void PushLine(int severity, int source, const char* text)
{
    if (text == nullptr || text[0] == '\0') return;

    const int64_t stamp = NowMs();
    const uint32_t hash = HashText(text);

    uint32_t cursor = g_state.lineCursor.load(std::memory_order_acquire);
    if (cursor > 0)
    {
        LineSlot& last = g_state.lines[(cursor - 1) % kLineSlots];
        if (last.state.load(std::memory_order_acquire) == 2 && last.hash == hash && last.source == source)
        {
            last.repeat++;
            last.stampMs = stamp;
            return;
        }
    }

    uint32_t index = g_state.lineCursor.fetch_add(1, std::memory_order_acq_rel);
    LineSlot& slot = g_state.lines[index % kLineSlots];

    slot.state.store(1, std::memory_order_release);
    slot.hash = hash;
    slot.repeat = 0;
    slot.severity = severity;
    slot.source = source;
    slot.stampMs = stamp;
    strncpy(slot.text, text, kLineLen - 1);
    slot.text[kLineLen - 1] = '\0';
    slot.length = (uint16_t)strnlen(slot.text, kLineLen - 1);
    slot.state.store(2, std::memory_order_release);

    if (severity < 4) return;

    uint32_t fatalIndex = g_state.fatalCursor.fetch_add(1, std::memory_order_acq_rel);
    FatalSlot& fatal = g_state.fatal[fatalIndex % kFatalSlots];

    fatal.state.store(1, std::memory_order_release);
    fatal.severity = severity;
    fatal.stampMs = stamp;
    strncpy(fatal.text, text, kFatalLen - 1);
    fatal.text[kFatalLen - 1] = '\0';
    fatal.length = (uint16_t)strnlen(fatal.text, kFatalLen - 1);
    fatal.state.store(2, std::memory_order_release);
}

} // namespace counterstrikesharp::crash

#else

namespace counterstrikesharp::crash {

void OnEarlyLoad() {}
void OnPathsReady(const char*) {}
void OnAllPluginsLoaded() {}
void OnMapChange(const char*) {}
void OnUnload() {}
bool IsInstalled() { return false; }
const char* RunId() { return "windows"; }
uint16_t RegisterName(const char*) { return 0; }
void Breadcrumb(uint16_t, uint16_t) {}
void PushCommand(const char*) {}
void SetTick(int32_t) {}
void PushLine(int, int, const char*) {}

} // namespace counterstrikesharp::crash

#endif
