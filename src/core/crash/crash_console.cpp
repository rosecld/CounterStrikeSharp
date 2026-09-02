#include "core/crash/crash_reporter.h"

#include "tier0/platform.h"

#ifndef _WIN32

#include "core/crash/crash_state.h"
#include "core/log.h"

#include <cstring>
#include <dlfcn.h>

#include "tier0/dbg.h"
#include "tier0/logging.h"

namespace counterstrikesharp::crash {

namespace {

class CrashConsoleListener : public ILoggingListener
{
  public:
    void Log(const LoggingContext_t* pContext, const tchar* pMessage) override
    {
        if (m_detached) return;
        if (pMessage == nullptr) return;

        PushLine(pContext != nullptr ? (int)pContext->m_Severity : 2, kSourceTier0, pMessage);
    }

    volatile bool m_detached = false;
};

CrashConsoleListener g_listener;
bool g_attached = false;

using RegisterListenerFn = void (*)(ILoggingListener*);
using EnableBackdoorFn = void (*)(bool);

} // namespace

void ConsoleAttach()
{
    if (g_attached) return;

    auto backdoor = (RegisterListenerFn)dlsym(RTLD_DEFAULT, "LoggingSystem_RegisterBackdoorLoggingListener");
    auto enable = (EnableBackdoorFn)dlsym(RTLD_DEFAULT, "LoggingSystem_EnableBackdoorLoggingListeners");
    auto plain = (RegisterListenerFn)dlsym(RTLD_DEFAULT, "LoggingSystem_RegisterLoggingListener");

    if (backdoor != nullptr && enable != nullptr)
    {
        backdoor(&g_listener);
        enable(true);
        g_attached = true;
    }
    else if (plain != nullptr)
    {
        plain(&g_listener);
        g_attached = true;
        CSSHARP_CORE_WARN("Crash reporter: backdoor logging listener is unavailable, falling back to the regular one");
    }
    else
    {
        CSSHARP_CORE_ERROR("Crash reporter: tier0 logging listener API is missing, the console section will stay empty");
        return;
    }

    const uint32_t before = g_state.lineCursor.load(std::memory_order_acquire);
    Msg("[CSSharp] crash reporter console probe %s\n", g_state.runId);
    const uint32_t after = g_state.lineCursor.load(std::memory_order_acquire);

    bool seen = false;
    for (uint32_t index = before; index < after; ++index)
    {
        const LineSlot& slot = g_state.lines[index % kLineSlots];
        if (slot.state.load(std::memory_order_acquire) == 2 && strstr(slot.text, g_state.runId) != nullptr) seen = true;
    }

    g_state.consoleVerified.store(seen, std::memory_order_release);

    if (seen)
    {
        CSSHARP_CORE_INFO("Crash reporter: console capture verified, engine output will be attached to crash reports");
    }
    else
    {
        CSSHARP_CORE_ERROR("Crash reporter: console probe was not received. Either all 16 tier0 listener slots are taken or the "
                           "logging system is bypassed on this build. Crash reports will have no engine console text");
    }
}

void ConsoleDetach()
{
    g_listener.m_detached = true;

    auto unregister = (RegisterListenerFn)dlsym(RTLD_DEFAULT, "LoggingSystem_UnregisterLoggingListener");
    if (unregister != nullptr) unregister(&g_listener);

    g_attached = false;
    g_state.consoleVerified.store(false, std::memory_order_release);
}

} // namespace counterstrikesharp::crash

#endif

DLL_EXPORT void CrashReportManagedLog(int level, const char* message)
{
    counterstrikesharp::crash::PushLine(level, counterstrikesharp::crash::kSourceManaged, message);
}

DLL_EXPORT unsigned short CrashReportRegisterName(const char* name) { return counterstrikesharp::crash::RegisterName(name); }

DLL_EXPORT void CrashReportBreadcrumb(unsigned short site, unsigned short name) { counterstrikesharp::crash::Breadcrumb(site, name); }
