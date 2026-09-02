#pragma once

#include <cstdint>

namespace counterstrikesharp::crash {

enum CrumbSite : uint16_t
{
    kSiteNone = 0,
    kSiteCallback = 1,
    kSiteCommand = 2,
    kSiteManaged = 3
};

enum LineSource : int
{
    kSourceTier0 = 0,
    kSourceManaged = 1
};

void OnEarlyLoad();
void OnPathsReady(const char* rootDirectory);
void OnAllPluginsLoaded();
void OnMapChange(const char* mapName);
void OnUnload();

bool IsInstalled();
const char* RunId();

uint16_t RegisterName(const char* name);
void Breadcrumb(uint16_t site, uint16_t name);
void PushCommand(const char* name);
void SetTick(int32_t tick);
void PushLine(int severity, int source, const char* text);

} // namespace counterstrikesharp::crash
