// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <sys/lock.h>
#include <sys/iosupport.h>
#include <dvm.h>

// These come from libntfs's *private* source/ headers, not its public
// include/ntfs.h - it doesn't expose a low-level "give me a device-backed
// volume" API of its own, only the DISC_INTERFACE-flavoured ntfsMount().
// So this driver reaches past that and drives ntfs_device_alloc() /
// ntfs_device_mount() / ntfsInitVolume() directly, the same way ntfs.c
// itself does. Build systems consuming this file need the libntfs
// checkout's source/ directory on the include path in addition to its
// include/ directory - see the accompanying README for the exact flag.
#include <ntfsinternal.h>
#include <ntfsfile.h>
#include <ntfsdir.h>
#include <device.h>
#include <volume.h>

// Backing store for a single mounted ntfs_device. Handed to
// ntfs_device_alloc() as its private data (dev->d_private) - see
// ntfs_driver.c's ntfs_device_dvm_io_ops. Deliberately does no caching of
// its own (unlike libntfs's own gekko_io.c, which layers its own
// NTFS_CACHE on top of the raw DISC_INTERFACE): every read/write goes
// straight to DvmDisc's read_sectors()/write_sectors(), so caching (if
// any) is provided exactly once, by whatever the caller wrapped the disc
// in via dvmDiscCacheCreate() - not duplicated here.
typedef struct NtfsDvmIo {
	DvmDisc* disc;
	sec_t    start_sector; // first sector of this partition, disc-relative
	sec_t    num_sectors;  // partition length, from DvmPartInfo
	uint16_t sector_sz;    // cached from disc->sector_sz
	uint64_t pos;          // current byte offset - only ever touched by
	                        // the non-p{read,write} read()/write()/seek()
	                        // device ops; libntfs itself almost always
	                        // uses ntfs_pread()/ntfs_pwrite() instead
} NtfsDvmIo;

// One mounted NTFS volume's worth of state.
//
// vd MUST be the first member: ntfsGetVolume()/ntfsGetDevice() (used by
// every ntfs_open_r/ntfs_stat_r/etc. devoptab callback already compiled
// into libntfs) resolve a path to its devoptab_t via GetDeviceOpTab(),
// confirm dotab->open_r matches this driver's devoptab template, and then
// cast dotab->deviceData straight to (ntfs_vd*). dvmMountPartition() sets
// dotab->deviceData to this struct's address, so that cast is only valid
// if &vol->vd == vol - i.e. vd is at offset 0.
typedef struct NtfsVolume {
	ntfs_vd   vd;
	DvmDisc*  disc;
	NtfsDvmIo io;
} NtfsVolume;
