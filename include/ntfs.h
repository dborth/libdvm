// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <dvm.h>

#ifdef __cplusplus
extern "C" {
#endif

// ntfs filesystem driver
extern const DvmFsDriver g_ntfsFsDriver;

static inline bool ntfsUnmountVolume(const char* name) {
	return dvmUnmountVolume(name);
}

#ifdef __cplusplus
}
#endif
