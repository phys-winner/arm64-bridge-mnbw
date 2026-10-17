// Host-side Ogg Vorbis decode for the guest FMOD sound effects.
#pragma once

#include <cstdint>

struct DynarmicHost;

// Enumerates the packaged .ogg sound effects, queues them for the decode pool
// and starts the worker threads. Call once after the game data is prepared;
// later calls are no-ops. debug.warband.audio_host_decode=0 disables it.
void WarbandAudioPrefetchStart();

// Stops the worker pool and drops cached PCM. Optional: the pool also winds
// down when the process exits.
void WarbandAudioPrefetchStop();
