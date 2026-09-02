#include "core/crash/crash_reporter.h"

#ifndef _WIN32

#include "core/crash/crash_state.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace counterstrikesharp::crash {

constinit State g_state;

namespace {

constexpr int kTrackedSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
constexpr int kMaxAnonRegions = 128;
constexpr int kMaxReports = 3;
constexpr int kMaxExecRegions = 512;

struct Region
{
    uintptr_t start;
    uintptr_t end;
};

struct ExecRegion
{
    uintptr_t start;
    uintptr_t end;
    char name[32];
};

Region g_anonExec[kMaxAnonRegions];
int g_anonExecCount = 0;
ExecRegion g_execRegions[kMaxExecRegions];
int g_execRegionCount = 0;
int g_execRegionsDropped = 0;
uintptr_t g_stackLow = 0;
uintptr_t g_stackHigh = 0;

char g_outBuffer[4096];
size_t g_outUsed = 0;
int g_outFd = -1;

uintptr_t g_frames[kMaxFrames];
unsigned char g_frameHow[kMaxFrames];

size_t StrLen(const char* text)
{
    size_t length = 0;
    while (text != nullptr && text[length] != '\0' && length < 65536)
        length++;
    return length;
}

void OutFlush()
{
    size_t offset = 0;
    while (offset < g_outUsed && g_outFd >= 0)
    {
        ssize_t written = write(g_outFd, g_outBuffer + offset, g_outUsed - offset);
        if (written > 0)
        {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    g_outUsed = 0;
}

void OutRaw(const char* text, size_t length)
{
    for (size_t index = 0; index < length; ++index)
    {
        if (g_outUsed == sizeof(g_outBuffer)) OutFlush();
        g_outBuffer[g_outUsed++] = text[index];
    }
}

void Out(const char* text) { OutRaw(text, StrLen(text)); }

void OutSafe(const char* text, size_t length)
{
    for (size_t index = 0; index < length && text[index] != '\0'; ++index)
    {
        char symbol = text[index];
        if (symbol == '\n' || symbol == '\r') symbol = ' ';
        if ((unsigned char)symbol < 0x20 && symbol != '\t') symbol = '?';
        OutRaw(&symbol, 1);
    }
}

void OutHex(uint64_t value, int width)
{
    static const char kDigits[] = "0123456789abcdef";
    char digits[16];
    int count = 0;

    while (value != 0 && count < 16)
    {
        digits[count++] = kDigits[value & 0xF];
        value >>= 4;
    }
    if (count == 0) digits[count++] = '0';
    while (count < width && count < 16)
        digits[count++] = '0';

    for (int index = count - 1; index >= 0; --index)
        OutRaw(&digits[index], 1);
}

void OutDec(int64_t value)
{
    char digits[24];
    int position = 24;
    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-value) : (uint64_t)value;
    if (magnitude == 0) digits[--position] = '0';
    while (magnitude != 0 && position > 0)
    {
        digits[--position] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    }
    if (negative && position > 0) digits[--position] = '-';
    OutRaw(digits + position, (size_t)(24 - position));
}

void OutPointer(uint64_t value)
{
    Out("0x");
    OutHex(value, 16);
}

bool SafeRead(uintptr_t address, void* destination, size_t size)
{
    if (size == 0 || size > 64) return false;
    if (g_state.probeWrite < 0 || g_state.probeRead < 0) return false;

    ssize_t written = write(g_state.probeWrite, (const void*)address, size);
    if (written <= 0) return false;

    ssize_t got = read(g_state.probeRead, destination, (size_t)written);
    if (got != written) return false;

    return written == (ssize_t)size;
}

uint64_t ParseHex(const char* text, int* consumed)
{
    uint64_t value = 0;
    int index = 0;
    while (text[index] != '\0')
    {
        char symbol = text[index];
        int digit;
        if (symbol >= '0' && symbol <= '9') digit = symbol - '0';
        else if (symbol >= 'a' && symbol <= 'f')
            digit = symbol - 'a' + 10;
        else if (symbol >= 'A' && symbol <= 'F')
            digit = symbol - 'A' + 10;
        else
            break;
        value = (value << 4) | (uint64_t)digit;
        index++;
    }
    *consumed = index;
    return value;
}

void ParseMapsLine(const char* line, uintptr_t stackPointer)
{
    int consumed = 0;
    uintptr_t start = (uintptr_t)ParseHex(line, &consumed);
    if (consumed == 0 || line[consumed] != '-') return;

    const char* cursor = line + consumed + 1;
    uintptr_t end = (uintptr_t)ParseHex(cursor, &consumed);
    if (consumed == 0) return;

    cursor += consumed;
    while (*cursor == ' ')
        cursor++;
    if (StrLen(cursor) < 4) return;

    bool executable = cursor[2] == 'x';
    cursor += 4;

    int fields = 0;
    while (*cursor != '\0' && fields < 3)
    {
        while (*cursor == ' ')
            cursor++;
        while (*cursor != '\0' && *cursor != ' ')
            cursor++;
        fields++;
    }
    while (*cursor == ' ')
        cursor++;

    if (stackPointer >= start && stackPointer < end)
    {
        g_stackLow = start;
        g_stackHigh = end;
    }

    if (executable && *cursor == '\0' && g_anonExecCount < kMaxAnonRegions)
    {
        g_anonExec[g_anonExecCount].start = start;
        g_anonExec[g_anonExecCount].end = end;
        g_anonExecCount++;
    }

    if (!executable) return;

    if (g_execRegionCount >= kMaxExecRegions)
    {
        g_execRegionsDropped++;
        return;
    }

    ExecRegion& region = g_execRegions[g_execRegionCount];
    region.start = start;
    region.end = end;

    const char* name = cursor;
    for (const char* scan = cursor; *scan != '\0'; ++scan)
        if (*scan == '/') name = scan + 1;

    if (*name == '\0') name = "anon-exec";

    size_t used = 0;
    while (name[used] != '\0' && used < sizeof(region.name) - 1)
    {
        region.name[used] = name[used];
        used++;
    }
    region.name[used] = '\0';

    g_execRegionCount++;
}

void ScanMaps(uintptr_t stackPointer)
{
    g_anonExecCount = 0;
    g_execRegionCount = 0;
    g_execRegionsDropped = 0;
    g_stackLow = 0;
    g_stackHigh = 0;

    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        g_stackLow = stackPointer > 0x100000 ? stackPointer - 0x100000 : 0;
        g_stackHigh = stackPointer + 0x100000;
        return;
    }

    char chunk[8192];
    char line[512];
    size_t lineLength = 0;
    ssize_t got;

    while ((got = read(fd, chunk, sizeof(chunk))) > 0)
    {
        for (ssize_t index = 0; index < got; ++index)
        {
            char symbol = chunk[index];
            if (symbol != '\n')
            {
                if (lineLength < sizeof(line) - 1) line[lineLength++] = symbol;
                continue;
            }
            line[lineLength] = '\0';
            ParseMapsLine(line, stackPointer);
            lineLength = 0;
        }
    }

    close(fd);

    if (g_stackHigh == 0)
    {
        g_stackLow = stackPointer > 0x100000 ? stackPointer - 0x100000 : 0;
        g_stackHigh = stackPointer + 0x100000;
    }
}

bool InStack(uintptr_t address) { return address >= g_stackLow && address < g_stackHigh; }

const ExecRegion* FindExecRegion(uintptr_t address)
{
    for (int index = 0; index < g_execRegionCount; ++index)
    {
        if (address >= g_execRegions[index].start && address < g_execRegions[index].end) return &g_execRegions[index];
    }
    return nullptr;
}

bool InExec(uintptr_t address) { return FindExecRegion(address) != nullptr; }

bool InAnonExec(uintptr_t address)
{
    for (int index = 0; index < g_anonExecCount; ++index)
    {
        if (address >= g_anonExec[index].start && address < g_anonExec[index].end) return true;
    }
    return false;
}

const ModuleEntry* FindModule(uintptr_t address)
{
    int set = g_state.activeSet.load(std::memory_order_acquire);
    if (set < 0 || set > 1) return nullptr;

    int count = g_state.moduleCount[set].load(std::memory_order_acquire);
    const ModuleEntry* modules = g_state.modules[set];

    int low = 0;
    int high = count - 1;
    while (low <= high)
    {
        int middle = low + (high - low) / 2;
        if (address < modules[middle].start) high = middle - 1;
        else if (address >= modules[middle].end)
            low = middle + 1;
        else
            return &modules[middle];
    }
    return nullptr;
}

const JitEntry* FindJit(uintptr_t address)
{
    int count = g_state.jitCount.load(std::memory_order_acquire);
    if (count > kMaxJitEntries) count = kMaxJitEntries;

    const JitEntry* found = nullptr;
    for (int index = 0; index < count; ++index)
    {
        const JitEntry& entry = g_state.jit[index];
        if (entry.size == 0) continue;
        if (address >= entry.start && address - entry.start < entry.size) found = &entry;
    }
    return found;
}

bool LooksLikeReturnAddress(uintptr_t address)
{
    if (address < 0x1000) return false;
    if (InStack(address)) return false;
    if (FindModule(address) == nullptr && !InExec(address)) return false;

    unsigned char window[16];
    if (!SafeRead(address - 16, window, sizeof(window))) return false;

    if (window[11] == 0xE8) return true;

    for (int length = 2; length <= 7; ++length)
    {
        int base = 16 - length;
        if (window[base] == 0xFF && ((window[base + 1] >> 3) & 7) == 2) return true;
        if (length >= 3 && window[base] >= 0x40 && window[base] <= 0x4F && window[base + 1] == 0xFF && ((window[base + 2] >> 3) & 7) == 2)
            return true;
    }

    return false;
}

int Unwind(uintptr_t instructionPointer, uintptr_t stackPointer, uintptr_t framePointer)
{
    int count = 0;
    g_frames[count] = instructionPointer;
    g_frameHow[count] = 0;
    count++;

    uintptr_t cursor = stackPointer;
    uintptr_t frame = framePointer;
    int scanned = 0;

    while (count < kMaxFrames)
    {
        while (count < kMaxFrames && frame != 0 && (frame & 7) == 0 && InStack(frame) && frame >= cursor)
        {
            uintptr_t pair[2];
            if (!SafeRead(frame, pair, sizeof(pair))) break;
            if (!LooksLikeReturnAddress(pair[1])) break;

            g_frames[count] = pair[1];
            g_frameHow[count] = 1;
            count++;
            cursor = frame + 16;

            if (pair[0] <= frame || !InStack(pair[0]))
            {
                frame = 0;
                break;
            }
            frame = pair[0];
        }

        if (count >= kMaxFrames) break;

        bool found = false;
        while (cursor + 8 <= g_stackHigh && scanned < kScanWordBudget)
        {
            uintptr_t word = 0;
            scanned++;
            if (SafeRead(cursor, &word, sizeof(word)) && LooksLikeReturnAddress(word))
            {
                g_frames[count] = word;
                g_frameHow[count] = 2;
                count++;

                uintptr_t candidate = 0;
                if (SafeRead(cursor - 8, &candidate, sizeof(candidate)) && candidate > cursor && InStack(candidate))
                {
                    frame = candidate;
                }
                cursor += 8;
                found = true;
                break;
            }
            cursor += 8;
        }

        if (!found) break;
    }

    return count;
}

const FatalSlot* NewestFatal()
{
    uint32_t cursor = g_state.fatalCursor.load(std::memory_order_acquire);
    if (cursor == 0) return nullptr;
    const FatalSlot& slot = g_state.fatal[(cursor - 1) % kFatalSlots];
    if (slot.state.load(std::memory_order_acquire) != 2) return nullptr;
    return &slot;
}

const char* const kStackOverflow = "stack_overflow";

const char* Verdict(int signalNumber, const siginfo_t* info, uintptr_t instructionPointer)
{
    if (g_state.fatalErrorStart != 0 && instructionPointer >= g_state.fatalErrorStart && instructionPointer < g_state.fatalErrorEnd)
    {
        return "engine_fatal";
    }

    const ModuleEntry* module = FindModule(instructionPointer);
    const bool inEngineCore =
        module != nullptr && (strncmp(module->name, "libtier0", 8) == 0 || strncmp(module->name, "libengine2", 10) == 0);

    const FatalSlot* fatal = NewestFatal();
    if (inEngineCore && fatal != nullptr && NowMs() - fatal->stampMs <= 2000) return "engine_fatal";

    uintptr_t faultAddress = (uintptr_t)(info != nullptr ? info->si_addr : nullptr);

    if ((signalNumber == SIGSEGV || signalNumber == SIGBUS) && g_stackLow != 0 && faultAddress + 0x10000 >= g_stackLow &&
        faultAddress <= g_stackLow + 0x1000)
    {
        return kStackOverflow;
    }

    switch (signalNumber)
    {
        case SIGSEGV:
            return faultAddress < 0x1000 ? "segv_null" : "segv_wild";
        case SIGBUS:
            return "bus_error";
        case SIGILL:
            return "illegal_instruction";
        case SIGFPE:
            return "arithmetic_fault";
        case SIGABRT:
            return "abort";
        default:
            return "unknown";
    }
}

void WriteFrameLocation(uintptr_t address)
{
    const ModuleEntry* module = FindModule(address);
    if (module != nullptr)
    {
        Out(module->name);
        Out("+0x");
        OutHex(address - module->bias, 0);
        Out(" (");
        Out(module->buildId[0] != '\0' ? module->buildId : "no-build-id");
        Out(")");
        return;
    }

    const ExecRegion* region = FindExecRegion(address);
    if (region != nullptr)
    {
        Out(region->name);
        Out("+0x");
        OutHex(address - region->start, 0);
        Out(" (jit-or-trampoline, resolve via perf map)");
        return;
    }

    Out("unmapped");
}

void SectionRun()
{
    Out("[RUN]\nrun=");
    Out(g_state.runId);
    Out(" version=");
    Out(g_state.version);
    Out("\npid=");
    OutDec(getpid());
    Out(" tid=");
    OutDec((int64_t)syscall(SYS_gettid));
    Out(" uptime_ms=");
    OutDec(NowMs() - g_state.startedMs);
    Out(" unix=");
    OutDec(g_state.startedUnix + (NowMs() - g_state.startedMs) / 1000);
    Out("\nmap=");
    Out(g_state.mapName[0] != '\0' ? g_state.mapName : "none");
    Out(" tick=");
    OutDec(g_state.tick.load(std::memory_order_relaxed));
    Out(" console_verified=");
    Out(g_state.consoleVerified.load(std::memory_order_relaxed) ? "yes" : "no");
    Out("\nperfmap=");
    Out(g_state.perfMapPath[0] != '\0' ? g_state.perfMapPath : "disabled");
    Out("\n\n");
    OutFlush();
}

void SectionFault(int signalNumber, const siginfo_t* info, const ucontext_t* context)
{
    const greg_t* registers = context->uc_mcontext.gregs;

    Out("[FAULT]\nsignal=");
    OutDec(signalNumber);
    Out(" code=");
    OutDec(info != nullptr ? info->si_code : 0);
    Out(" addr=");
    OutPointer((uint64_t)(info != nullptr ? info->si_addr : nullptr));
    Out("\nrip=");
    OutPointer((uint64_t)registers[REG_RIP]);
    Out(" at ");
    WriteFrameLocation((uintptr_t)registers[REG_RIP]);
    Out("\nrsp=");
    OutPointer((uint64_t)registers[REG_RSP]);
    Out(" rbp=");
    OutPointer((uint64_t)registers[REG_RBP]);
    Out(" stack=");
    OutPointer(g_stackLow);
    Out("..");
    OutPointer(g_stackHigh);
    Out("\nrax=");
    OutPointer((uint64_t)registers[REG_RAX]);
    Out(" rbx=");
    OutPointer((uint64_t)registers[REG_RBX]);
    Out(" rcx=");
    OutPointer((uint64_t)registers[REG_RCX]);
    Out(" rdx=");
    OutPointer((uint64_t)registers[REG_RDX]);
    Out("\nrsi=");
    OutPointer((uint64_t)registers[REG_RSI]);
    Out(" rdi=");
    OutPointer((uint64_t)registers[REG_RDI]);
    Out(" r8=");
    OutPointer((uint64_t)registers[REG_R8]);
    Out(" r9=");
    OutPointer((uint64_t)registers[REG_R9]);
    Out("\nr10=");
    OutPointer((uint64_t)registers[REG_R10]);
    Out(" r11=");
    OutPointer((uint64_t)registers[REG_R11]);
    Out(" r12=");
    OutPointer((uint64_t)registers[REG_R12]);
    Out(" r13=");
    OutPointer((uint64_t)registers[REG_R13]);
    Out("\nr14=");
    OutPointer((uint64_t)registers[REG_R14]);
    Out(" r15=");
    OutPointer((uint64_t)registers[REG_R15]);
    Out(" efl=");
    OutPointer((uint64_t)registers[REG_EFL]);
    Out(" err=");
    OutPointer((uint64_t)registers[REG_ERR]);

    unsigned char code[32];
    Out("\ncode=");
    if (SafeRead((uintptr_t)registers[REG_RIP], code, sizeof(code)))
    {
        for (size_t index = 0; index < sizeof(code); ++index)
        {
            OutHex(code[index], 2);
            Out(" ");
        }
    }
    else
    {
        Out("unreadable");
    }
    Out("\n\n");
    OutFlush();
}

void SectionConsole()
{
    Out("[CONSOLE]\n");
    uint32_t cursor = g_state.lineCursor.load(std::memory_order_acquire);

    for (uint32_t index = 0; index < (uint32_t)kLineSlots; ++index)
    {
        const LineSlot& slot = g_state.lines[(cursor + index) % kLineSlots];
        if (slot.state.load(std::memory_order_acquire) != 2) continue;

        Out("t+");
        OutDec(slot.stampMs - g_state.startedMs);
        Out(" ");
        Out(slot.source == kSourceManaged ? "managed" : "tier0");
        Out(" sev=");
        OutDec(slot.severity);
        if (slot.repeat > 0)
        {
            Out(" x");
            OutDec(slot.repeat + 1);
        }
        Out(" | ");
        OutSafe(slot.text, slot.length);
        Out("\n");
    }

    Out("\n");
    OutFlush();
}

void SectionFatal()
{
    Out("[FATAL]\n");
    const FatalSlot* fatal = NewestFatal();
    if (fatal == nullptr)
    {
        Out("none captured\n\n");
        OutFlush();
        return;
    }

    Out("t+");
    OutDec(fatal->stampMs - g_state.startedMs);
    Out(" sev=");
    OutDec(fatal->severity);
    Out(" | ");
    OutSafe(fatal->text, fatal->length);
    Out("\n\n");
    OutFlush();
}

void SectionManaged()
{
    Out("[MANAGED]\n");
    uint32_t cursor = g_state.crumbCursor.load(std::memory_order_acquire);
    uint32_t start = cursor > 24 ? cursor - 24 : 0;
    int nameCount = g_state.nameCount.load(std::memory_order_acquire);

    for (uint32_t index = start; index < cursor; ++index)
    {
        const Crumb& crumb = g_state.crumbs[index % kCrumbSlots];
        if (crumb.state.load(std::memory_order_acquire) != 2) continue;

        Out("tick=");
        OutDec(crumb.tick);
        Out(" site=");
        switch (crumb.site)
        {
            case kSiteCallback:
                Out("callback");
                break;
            case kSiteCommand:
                Out("command");
                break;
            case kSiteManaged:
                Out("managed");
                break;
            default:
                Out("none");
                break;
        }
        Out(" name=");
        if (crumb.name > 0 && (int)crumb.name <= nameCount) OutSafe(g_state.names[crumb.name - 1], kNameLen);
        else
            Out("?");

        uint32_t repeats = crumb.repeats.load(std::memory_order_acquire);
        if (repeats > 0)
        {
            Out(" x");
            OutDec((int)(repeats + 1));
        }

        Out("\n");
    }

    Out("\n[CMDS]\n");
    uint32_t commandCursor = g_state.commandCursor.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < (uint32_t)kCommandSlots; ++index)
    {
        const CommandSlot& slot = g_state.commands[(commandCursor + index) % kCommandSlots];
        if (slot.state.load(std::memory_order_acquire) != 2) continue;
        Out("tick=");
        OutDec(slot.tick);
        Out(" ");
        OutSafe(slot.text, kCommandLen);
        Out("\n");
    }

    Out("\n");
    OutFlush();
}

void SectionRawStack(uintptr_t stackPointer)
{
    Out("[RAWSTACK]\nbase=");
    OutPointer(stackPointer);
    Out("\n");

    uintptr_t limit = stackPointer + kStackDumpBytes;
    if (g_stackHigh != 0 && limit > g_stackHigh) limit = g_stackHigh;

    for (uintptr_t address = stackPointer; address + 32 <= limit; address += 32)
    {
        unsigned char bytes[32];
        if (!SafeRead(address, bytes, sizeof(bytes))) break;
        OutHex(address - stackPointer, 4);
        Out(" ");
        for (size_t index = 0; index < sizeof(bytes); ++index)
            OutHex(bytes[index], 2);
        Out("\n");
    }

    Out("\n");
    OutFlush();
}

void SectionBlame(int frameCount)
{
    Out("[BLAME]\n");

    int32_t before = g_state.jitCount.load(std::memory_order_acquire);
    PumpPerfMap(256);
    int32_t after = g_state.jitCount.load(std::memory_order_acquire);

    if (g_state.perfMapPath[0] == '\0')
    {
        Out("unresolved reason=perfmap-off\n\n");
        OutFlush();
        return;
    }

    if (g_state.perfMapFd < 0)
    {
        Out("unresolved reason=perfmap-absent path=");
        Out(g_state.perfMapPath);
        Out("\n\n");
        OutFlush();
        return;
    }

    int named = 0;
    char name[320];

    for (int index = 0; index < frameCount && named < 8; ++index)
    {
        const JitEntry* entry = FindJit(g_frames[index]);
        if (entry == nullptr) continue;

        size_t want = entry->length < sizeof(name) - 1 ? entry->length : sizeof(name) - 1;
        ssize_t got = pread(g_state.perfMapFd, name, want, (off_t)entry->offset);
        if (got <= 0) continue;

        name[got] = '\0';

        Out("#");
        OutDec(index);
        Out(" +0x");
        OutHex(g_frames[index] - entry->start, 0);
        Out(" ");
        Out(name);
        Out("\n");
        OutFlush();
        named++;
    }

    if (named == 0)
    {
        Out("unresolved reason=no-managed-frames-in-index\n");

        uintptr_t lowest = 0;
        uintptr_t highest = 0;
        int count = g_state.jitCount.load(std::memory_order_acquire);
        if (count > kMaxJitEntries) count = kMaxJitEntries;

        for (int index = 0; index < count; ++index)
        {
            const JitEntry& entry = g_state.jit[index];
            if (entry.size == 0) continue;
            if (lowest == 0 || entry.start < lowest) lowest = entry.start;
            if (entry.start + entry.size > highest) highest = entry.start + entry.size;
        }

        Out("index-range=");
        OutPointer(lowest);
        Out("..");
        OutPointer(highest);
        Out("\n");

        int shown = 0;
        for (int index = 0; index < frameCount && shown < 6; ++index)
        {
            if (FindModule(g_frames[index]) != nullptr) continue;
            if (!InExec(g_frames[index])) continue;

            Out("managed-frame #");
            OutDec(index);
            Out(" ");
            OutPointer(g_frames[index]);
            Out("\n");
            shown++;
        }

        if (shown == 0) Out("managed-frame none\n");

        for (int index = count - 5 > 0 ? count - 5 : 0; index < count; ++index)
        {
            const JitEntry& entry = g_state.jit[index];
            char sample[200];
            size_t want = entry.length < sizeof(sample) - 1 ? entry.length : sizeof(sample) - 1;
            ssize_t got = pread(g_state.perfMapFd, sample, want, (off_t)entry.offset);
            if (got <= 0) continue;
            sample[got] = '\0';

            Out("last-entry ");
            OutPointer(entry.start);
            Out(" size=0x");
            OutHex(entry.size, 0);
            Out(" ");
            Out(sample);
            Out("\n");
        }

        off_t mapSize = lseek(g_state.perfMapFd, 0, SEEK_END);
        Out("map-bytes=");
        OutDec((int)mapSize);
        Out("\n");

        if (mapSize > 0)
        {
            char tail[2048];
            off_t from = mapSize > (off_t)sizeof(tail) - 1 ? mapSize - ((off_t)sizeof(tail) - 1) : 0;
            ssize_t got = pread(g_state.perfMapFd, tail, sizeof(tail) - 1, from);
            if (got > 0)
            {
                tail[got] = '\0';
                Out("map-tail-from=");
                OutDec((int)from);
                Out("\n");
                Out(tail);
                Out("\n");
            }
        }

        OutFlush();
    }

    Out("indexed=");
    OutDec(g_state.jitCount.load(std::memory_order_acquire));
    Out(" at-crash=+");
    OutDec(after - before);
    Out(" dropped=");
    OutDec(g_state.jitDropped.load(std::memory_order_acquire));
    Out("\n\n");
    OutFlush();
}

void SectionStack(int frameCount)
{
    Out("[STACK]\n");
    for (int index = 0; index < frameCount; ++index)
    {
        Out("#");
        OutDec(index);
        Out(" ");
        OutPointer(g_frames[index]);
        Out(" ");
        WriteFrameLocation(g_frames[index]);
        Out(" ");
        switch (g_frameHow[index])
        {
            case 0:
                Out("ctx");
                break;
            case 1:
                Out("rbp");
                break;
            default:
                Out("scan?");
                break;
        }
        Out("\n");
        OutFlush();
    }
    Out("\n");
    OutFlush();
}

void SectionModules(int frameCount)
{
    Out("[MODULES]\n");
    for (int index = 0; index < frameCount; ++index)
    {
        const ModuleEntry* module = FindModule(g_frames[index]);
        if (module == nullptr) continue;

        bool duplicate = false;
        for (int earlier = 0; earlier < index; ++earlier)
        {
            if (FindModule(g_frames[earlier]) == module) duplicate = true;
        }
        if (duplicate) continue;

        Out(module->name);
        Out(" base=");
        OutPointer(module->bias);
        Out(" range=");
        OutPointer(module->start);
        Out("..");
        OutPointer(module->end);
        Out(" build-id=");
        Out(module->buildId[0] != '\0' ? module->buildId : "none");
        Out("\n");
    }
    Out("\n");
    OutFlush();
}

int OpenReportTarget()
{
    if (g_state.reportFd >= 0) return g_state.reportFd;
    if (g_state.fallbackPath[0] == '\0') return -1;
    return open(g_state.fallbackPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void StderrLine(const char* verdict)
{
    char line[kPathLen + 128];
    size_t used = 0;
    const char* prefix = "CSSHARP-CRASH run=";
    for (const char* cursor = prefix; *cursor != '\0'; ++cursor)
        line[used++] = *cursor;
    for (const char* cursor = g_state.runId; *cursor != '\0' && used < sizeof(line) - 2; ++cursor)
        line[used++] = *cursor;
    const char* middle = " verdict=";
    for (const char* cursor = middle; *cursor != '\0'; ++cursor)
        line[used++] = *cursor;
    for (const char* cursor = verdict; *cursor != '\0' && used < sizeof(line) - 2; ++cursor)
        line[used++] = *cursor;
    const char* tail = " file=";
    for (const char* cursor = tail; *cursor != '\0'; ++cursor)
        line[used++] = *cursor;
    const char* path = g_state.finalPath[0] != '\0' ? g_state.finalPath : g_state.fallbackPath;
    for (const char* cursor = path; *cursor != '\0' && used < sizeof(line) - 2; ++cursor)
        line[used++] = *cursor;
    line[used++] = '\n';

    ssize_t ignored = write(STDERR_FILENO, line, used);
    (void)ignored;
}

void RestoreDefault(int signalNumber)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signalNumber, &action, nullptr);
}

void InvokePrevious(int signalNumber, siginfo_t* info, void* context)
{
    if (signalNumber < 0 || signalNumber >= kSignalSlots) return;

    const struct sigaction& previous = g_state.previous[signalNumber];

    if ((previous.sa_flags & SA_SIGINFO) != 0)
    {
        if (previous.sa_sigaction != nullptr)
        {
            previous.sa_sigaction(signalNumber, info, context);
            return;
        }
        RestoreDefault(signalNumber);
        if (signalNumber == SIGABRT) raise(signalNumber);
        return;
    }

    if (previous.sa_handler == SIG_IGN) return;

    if (previous.sa_handler == SIG_DFL || previous.sa_handler == nullptr)
    {
        RestoreDefault(signalNumber);
        if (signalNumber == SIGABRT) raise(signalNumber);
        return;
    }

    previous.sa_handler(signalNumber);
}

bool InManagedCode(uintptr_t faultIp)
{
    const ModuleEntry* module = FindModule(faultIp);
    if (module == nullptr) return InExec(faultIp);

    size_t length = StrLen(module->name);
    if (length < 4) return false;

    const char* tail = module->name + (length - 4);
    return tail[0] == '.' && tail[1] == 'd' && tail[2] == 'l' && tail[3] == 'l';
}

bool RuntimeOwnsFault(int signalNumber, const siginfo_t* info, uintptr_t faultIp)
{
    if (signalNumber != SIGSEGV || info == nullptr) return false;
    if ((uintptr_t)info->si_addr >= 0x1000) return false;

    return InManagedCode(faultIp);
}

void Handle(int signalNumber, siginfo_t* info, void* contextPointer)
{
    int savedErrno = errno;

    const ucontext_t* context = (const ucontext_t*)contextPointer;
    uintptr_t instructionPointer = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
    uintptr_t stackPointer = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
    uintptr_t framePointer = (uintptr_t)context->uc_mcontext.gregs[REG_RBP];

    bool expected = false;

    if (g_state.handlerBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        ScanMaps(stackPointer);

        if (RuntimeOwnsFault(signalNumber, info, instructionPointer))
        {
            g_state.handlerBusy.store(false, std::memory_order_release);
            errno = savedErrno;
            InvokePrevious(signalNumber, info, contextPointer);
            return;
        }

        if (g_state.reportsWritten.fetch_add(1, std::memory_order_acq_rel) >= kMaxReports)
        {
            g_state.handlerBusy.store(false, std::memory_order_release);
            errno = savedErrno;
            InvokePrevious(signalNumber, info, contextPointer);
            return;
        }

        const char* verdict = Verdict(signalNumber, info, instructionPointer);

        g_outFd = OpenReportTarget();
        g_outUsed = 0;

        if (g_outFd >= 0)
        {
            Out("[VERDICT]\n");
            Out(verdict);
            Out("\n\n");
            OutFlush();

            SectionRun();
            SectionFault(signalNumber, info, context);
            SectionConsole();
            SectionFatal();
            SectionManaged();
            SectionRawStack(stackPointer);

            int frameCount = 0;
            if (verdict == kStackOverflow)
            {
                Out("[STACK]\nskipped: stack overflow\n\n");
                OutFlush();
            }
            else
            {
                frameCount = Unwind(instructionPointer, stackPointer, framePointer);
                SectionStack(frameCount);
                SectionBlame(frameCount);
                SectionModules(frameCount);
            }

            Out("[END]\n");
            OutFlush();

            off_t size = lseek(g_outFd, 0, SEEK_CUR);
            if (g_outFd == g_state.reportFd && size > 0) ftruncate(g_outFd, size);
            if (g_outFd == g_state.reportFd && g_state.finalPath[0] != '\0') rename(g_state.reportPath, g_state.finalPath);
            if (g_outFd == g_state.reportFd) g_state.reportFd = -1;
        }

        StderrLine(verdict);
        g_state.handlerBusy.store(false, std::memory_order_release);
    }
    else
    {
        int fd = g_state.reportFd >= 0 ? g_state.reportFd : STDERR_FILENO;
        const char* marker = "\n[REENTERED] handler faulted while writing the report\n";
        ssize_t ignored = write(fd, marker, StrLen(marker));
        (void)ignored;
    }

    errno = savedErrno;
    InvokePrevious(signalNumber, info, contextPointer);
}

bool CoreClrAlreadyMapped()
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    char chunk[8192];
    char tail[32];
    size_t tailLength = 0;
    bool found = false;
    ssize_t got;

    memset(tail, 0, sizeof(tail));

    while (!found && (got = read(fd, chunk, sizeof(chunk))) > 0)
    {
        for (ssize_t index = 0; index < got; ++index)
        {
            if (tailLength == sizeof(tail) - 1)
            {
                memmove(tail, tail + 1, tailLength - 1);
                tailLength--;
            }
            tail[tailLength++] = chunk[index];
            tail[tailLength] = '\0';
            if (strstr(tail, "libcoreclr.so") != nullptr)
            {
                found = true;
                break;
            }
        }
    }

    close(fd);
    return found;
}

void NoteInstall(const char* text)
{
    size_t index = 0;
    while (text[index] != '\0' && index < sizeof(g_state.installNote) - 1)
    {
        g_state.installNote[index] = text[index];
        index++;
    }
    g_state.installNote[index] = '\0';
}

void MakeRunId()
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    uint64_t seed = (uint64_t)now.tv_sec << 20;
    seed ^= (uint64_t)now.tv_nsec;
    seed ^= ((uint64_t)getpid()) << 48;
    seed ^= (uint64_t)(uintptr_t)&g_state;

    static const char kDigits[] = "0123456789abcdef";
    for (int index = 0; index < 16; ++index)
    {
        g_state.runId[15 - index] = kDigits[(seed >> (index * 4)) & 0xF];
    }
    g_state.runId[16] = '\0';
}

void MakeFallbackPath()
{
    const char* prefix = "counterstrikesharp-crash-";
    size_t used = 0;
    while (prefix[used] != '\0')
    {
        g_state.fallbackPath[used] = prefix[used];
        used++;
    }
    for (int index = 0; index < 16; ++index)
        g_state.fallbackPath[used++] = g_state.runId[index];
    const char* suffix = ".txt";
    for (int index = 0; index < 4; ++index)
        g_state.fallbackPath[used++] = suffix[index];
    g_state.fallbackPath[used] = '\0';
}

__attribute__((constructor)) void EarlyInit()
{
    g_state.reportFd = -1;
    g_state.probeRead = -1;
    g_state.probeWrite = -1;
    g_state.startedMs = NowMs();
    g_state.startedUnix = (int64_t)time(nullptr);
    g_state.activeSet.store(0, std::memory_order_release);

    MakeRunId();
    MakeFallbackPath();

    NoteInstall("early init done, waiting for the .NET runtime before arming");
}

void ArmHandler()
{
    if (g_state.installed.load(std::memory_order_acquire)) return;

    const char* toggle = getenv("CSSHARP_CRASH_REPORTER");
    if (toggle != nullptr && toggle[0] == '0')
    {
        NoteInstall("disabled by CSSHARP_CRASH_REPORTER=0");
        return;
    }

    if (!CoreClrAlreadyMapped())
    {
        NoteInstall("refused: the .NET runtime is not mapped yet, arming now would leave us behind it in the chain");
        return;
    }

    int probe[2];
    if (pipe2(probe, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        NoteInstall("refused: could not create the memory probe pipe");
        return;
    }

    g_state.probeRead = probe[0];
    g_state.probeWrite = probe[1];

    for (int signalNumber : kTrackedSignals)
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = &Handle;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigemptyset(&action.sa_mask);

        if (sigaction(signalNumber, &action, &g_state.previous[signalNumber]) != 0)
        {
            NoteInstall("partially installed: sigaction failed for one of the signals");
        }
    }

    g_state.installed.store(true, std::memory_order_release);
    NoteInstall("armed after the .NET runtime, faults outside known modules are handed straight back to it");
}

} // namespace

void Arm() { ArmHandler(); }

bool IsInstalled() { return g_state.installed.load(std::memory_order_acquire); }

const char* RunId() { return g_state.runId; }

} // namespace counterstrikesharp::crash

#endif
