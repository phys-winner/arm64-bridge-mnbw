#pragma once
#include <cstdint>
#include <string>

class ThunkManager;
class DynarmicHost;

void RegisterOpenSLESThunks(ThunkManager& manager, DynarmicHost& host);
uint32_t GetOpenSLESInterfacePointer(const std::string& name);
void PumpOpenSLESCallbacks(DynarmicHost& host);
void PumpAudioWorkers(DynarmicHost& host);
void StartAudioThread(DynarmicHost* host);
void StopAudioThread();
void PauseAudioPlayback();
void ResumeAudioPlayback();
bool HasPendingAudioCallbacks();
