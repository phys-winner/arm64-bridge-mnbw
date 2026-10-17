#pragma once

#include <sys/types.h>

// Paths of the form "<something>.apk/<entry name>" are how Mono and Unity reach
// files that were never unpacked - the managed assemblies live inside the APK and
// are addressed as if the APK were a directory. The reference bridge only knew how
// to find FMOD audio by basename; this resolves any entry.
//
// Returns a readable file descriptor positioned at zero, or -1 when the path is
// not APK-relative or names no entry. The caller owns the descriptor and closes it
// like any other.
int ApkOpenEntry(const char* path, off_t* out_size);

// Unity hands Mono a root of plain "assets/bin/Data/Managed" - relative, with no
// archive in it - and expects its own file layer to resolve that against the
// package. Registering the package here makes any relative "assets/..." path
// resolve the same way.
void ApkSetPrimaryPackage(const char* apk_path);

// Copies every entry under `prefix` into `dest_dir` (flattened to basenames),
// skipping any that are already there at the right size. Returns how many entries
// the directory ended up holding, or -1 on failure.
int ApkExtractPrefix(const char* prefix, const char* dest_dir);

// Size of the entry without opening it, for stat/access. Directory prefixes count
// as entries too: Mono checks that its assembly root exists before it looks for a
// single file in it, and a zip has no records for directories.
bool ApkStatEntry(const char* path, off_t* out_size, bool* out_is_dir = nullptr);

// Visits every entry name of the primary package with its uncompressed size.
// The visit runs under the index lock, so it must not re-enter the reader.
void ApkForEachEntry(bool (*visit)(const char* name, uint32_t uncompressed, void* ud),
                     void* ud);
