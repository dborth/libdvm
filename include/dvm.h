// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/iosupport.h>

#if defined(__gamecube__) || defined(__wii__)
# include <ogc/disc_io.h>
#elif (defined(NDS) || defined(__NDS__)) && defined(ARM9)
# if __has_include(<calico/dev/disc_io.h>)
#  include <calico/dev/disc_io.h>
# elif __has_include(<nds/disc_io.h>)
#  include <nds/disc_io.h>
# else
#  error "Missing DS platform library"
# endif
#elif defined(__WIIU__)
// Cafe OS has no native raw-disc concept of its own - libmocha's
// DISC_INTERFACE is the de facto standard abstraction homebrew uses for
// it (same role disc_io.h plays for GC/Wii), and is already
// struct-for-struct identical to what the rest of this header expects.
# include <mocha/disc_interface.h>
#elif __has_include(<disc_io.h>)
# include <disc_io.h>
#else
# error "Unsupported platform"
#endif

// Disc I/O buffer alignment (should match CPU cache line size, or
// whatever the platform's IPC layer requires - eg. Wii U's IOSU raw
// storage calls require 64-byte-aligned buffers). Every supported
// platform gets a correct value just from including this header - no
// consumer needs to pass -DLIBDVM_BUFFER_ALIGN itself. A build system
// that still wants to override this (eg. this library's own CMakeLists.txt,
// for platforms it already supports) can do so with a -D flag, which
// takes precedence over the default below.
#ifndef LIBDVM_BUFFER_ALIGN
# if defined(__WIIU__)
#  define LIBDVM_BUFFER_ALIGN 0x40
# elif defined(__gamecube__) || defined(__wii__) || ((defined(NDS) || defined(__NDS__)) && defined(ARM9))
#  define LIBDVM_BUFFER_ALIGN 32
# else
#  define LIBDVM_BUFFER_ALIGN 4
# endif
#endif

#define DVM_IDENT_FSTYPE (1U<<0)

typedef struct DvmDisc DvmDisc;
typedef struct DvmDiscIface DvmDiscIface;
typedef struct DvmFsDriver DvmFsDriver;
typedef struct DvmPartInfo DvmPartInfo;

struct DvmDisc {
	const DvmDiscIface* vt;
	uint32_t io_type;
	uint16_t features;
	uint16_t num_users;
	sec_t num_sectors;
	uint16_t sector_sz;
	uint16_t block_sz;
};

struct DvmDiscIface {
	void (*destroy)(DvmDisc* self);
	bool (*read_sectors)(DvmDisc* self, void* buffer, sec_t first_sector, sec_t num_sectors, bool is_partial);
	bool (*write_sectors)(DvmDisc* self, const void* buffer, sec_t first_sector, sec_t num_sectors, bool is_partial);
	bool (*flush)(DvmDisc* self);
};

struct DvmFsDriver {
	const char* fstype;
	const devoptab_t* dotab_template;
	size_t device_data_sz;

	bool (*mount)(devoptab_t* dotab, DvmDisc* disc, DvmPartInfo* part);
	void (*umount)(void* device_data);
};

struct DvmPartInfo {
	uint16_t index;
	uint16_t type;
	const char* fstype;
	sec_t start_sector;
	sec_t num_sectors;
};

#ifdef __cplusplus
extern "C" {
#endif

// Initialization
bool dvmInitDefault(void);
bool dvmInit(bool set_app_cwdir, unsigned cache_pages, unsigned sectors_per_page);
void dvmDeinit(void);

// Disc and cache management
DvmDisc* dvmDiscCreate(DISC_INTERFACE* iface);
DvmDisc* dvmDiscCacheCreate(DvmDisc* inner_disc, unsigned cache_pages, unsigned sectors_per_page);
void dvmDiscAddUser(DvmDisc* disc);
void dvmDiscRemoveUser(DvmDisc* disc);

static inline bool dvmDiscReadSectors(DvmDisc* disc, void* buffer, sec_t first_sector, sec_t num_sectors)
{
	return disc->vt->read_sectors(disc, buffer, first_sector, num_sectors, false);
}

static inline bool dvmDiscWriteSectors(DvmDisc* disc, const void* buffer, sec_t first_sector, sec_t num_sectors)
{
	return disc->vt->write_sectors(disc, buffer, first_sector, num_sectors, false);
}

static inline bool dvmDiscFlush(DvmDisc* disc)
{
	return disc->vt->flush(disc);
}

// Forces a genuine, uncached disc-level read to check whether the
// underlying media is still physically present, even when disc is
// wrapped in a cache created via dvmDiscCacheCreate() - whose ordinary
// read/write path can be served entirely from memory (eg. a root
// directory sector that's still cache-resident) and never touch hardware
// again after mount, so it can't detect a real removal on its own. Safe
// to call concurrently with normal file I/O against the same disc - for
// a cache-wrapped disc this takes the same lock the cache's own
// read/write path uses. scratch_buffer must be at least disc->sector_sz
// bytes; aligning it to LIBDVM_BUFFER_ALIGN avoids an extra bounce
// allocation on platforms whose DISC_INTERFACE needs aligned buffers
// (eg. Wii U).
bool dvmDiscProbePresence(DvmDisc* disc, void* scratch_buffer);

// Volume management
bool dvmRegisterFsDriver(const DvmFsDriver* fsdrv);
bool dvmMountPartition(const char* name, DvmDisc* disc, DvmPartInfo* part);
bool dvmMountVolume(const char* name, DvmDisc* disc, sec_t start_sector, const char* fstype);
bool dvmUnmountVolume(const char* name);

// Partition table and filesystem probing
unsigned dvmReadPartitionTable(DvmDisc* disc, DvmPartInfo* out, unsigned max_partitions, unsigned flags);
unsigned dvmProbeMountDisc(const char* basename, DvmDisc* disc);
unsigned dvmProbeMountDiscIface(const char* basename, DISC_INTERFACE* iface, unsigned cache_pages, unsigned sectors_per_page);

#ifdef __cplusplus
}
#endif
