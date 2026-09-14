// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include "ntfs_driver.h"
#include "dvm_debug.h"

// Not defined by libntfs's device_io.h outside of the HAVE_WINDOWS_H
// branch, so BLKGETSIZE/BLKGETSIZE64/BLKSSZGET are unreachable dead code
// in every existing ntfs_device_operations::ioctl on Gekko/Wii U too
// (their #if defined(...) guards never fire). Defining them ourselves
// just means our ioctl is actually reachable if anything ever calls it -
// nothing in the mount/read/write path does.
#ifndef BLKGETSIZE
# define BLKGETSIZE   0x1260
#endif
#ifndef BLKGETSIZE64
# define BLKGETSIZE64 0x80041272
#endif
#ifndef BLKSSZGET
# define BLKSSZGET    0x1268
#endif

//-----------------------------------------------------------------------------
// devoptab_t <-> ntfs_open_r/ntfs_read_r/... (already compiled into libntfs)
//-----------------------------------------------------------------------------

static bool _ntfs_mount(devoptab_t* dotab, DvmDisc* disc, DvmPartInfo* part);
static void _ntfs_umount(void* device_data);

static const devoptab_t _ntfs_devoptab = {
	.structSize   = sizeof(ntfs_file_state),
	.open_r       = ntfs_open_r,
	.close_r      = ntfs_close_r,
	.write_r      = ntfs_write_r,
	.read_r       = ntfs_read_r,
	.seek_r       = ntfs_seek_r,
	.fstat_r      = ntfs_fstat_r,
	.stat_r       = ntfs_stat_r,
	.link_r       = ntfs_link_r,
	.unlink_r     = ntfs_unlink_r,
	.chdir_r      = ntfs_chdir_r,
	.rename_r     = ntfs_rename_r,
	.mkdir_r      = ntfs_mkdir_r,
	.dirStateSize = sizeof(ntfs_dir_state),
	.diropen_r    = ntfs_diropen_r,
	.dirreset_r   = ntfs_dirreset_r,
	.dirnext_r    = ntfs_dirnext_r,
	.dirclose_r   = ntfs_dirclose_r,
	.statvfs_r    = ntfs_statvfs_r,
	.ftruncate_r  = ntfs_ftruncate_r,
	.fsync_r      = ntfs_fsync_r,
	.lstat_r      = ntfs_stat_r, // no distinct lstat_r in this libntfs fork
	// NOTE: this libntfs fork doesn't implement rmdir_r/symlink_r/
	// readlink_r/chmod_r/utimes_r/pathconf_r at the devoptab layer at all
	// (ntfsdir.c/ntfsfile.c simply have no such functions), so those stay
	// unset here too. rmdir(2) on an ntfs: path will fail; remove(2)/
	// unlink(2) still works via ntfsUnlink(), which already special-cases
	// (empty) directories internally.
};

const DvmFsDriver g_ntfsFsDriver = {
	.fstype         = "ntfs",
	.device_data_sz = sizeof(NtfsVolume),
	.dotab_template = &_ntfs_devoptab,
	.mount          = _ntfs_mount,
	.umount         = _ntfs_umount,
};

//-----------------------------------------------------------------------------
// ntfs_device_operations <-> DvmDisc
//-----------------------------------------------------------------------------

#define DEV_IO(dev) ((NtfsDvmIo*)(dev)->d_private)

static s64 _ntfs_dev_readbytes(struct ntfs_device* dev, s64 offset, s64 count, void* buf)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (!io || !io->disc) {
		errno = EBADF;
		return -1;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if (!count) {
		return 0;
	}

	DvmDisc* disc = io->disc;
	unsigned sec_sz = io->sector_sz;

	sec_t sec_start = io->start_sector + (sec_t)(offset / sec_sz);
	uint32_t buf_off = (uint32_t)(offset % sec_sz);
	sec_t sec_count = 1;
	if (buf_off + count > sec_sz) {
		sec_count = (sec_t)((buf_off + count + sec_sz - 1) / sec_sz);
	}

	// Sector-aligned: read straight into the caller's buffer.
	if (buf_off == 0 && (count % sec_sz) == 0) {
		if (!disc->vt->read_sectors(disc, buf, sec_start, sec_count, false)) {
			errno = EIO;
			return -1;
		}
		return count;
	}

	// Unaligned: bounce through a full-sector buffer and copy out the
	// slice that was actually asked for.
	void* bounce = aligned_alloc(LIBDVM_BUFFER_ALIGN, (size_t)sec_count * sec_sz);
	if (!bounce) {
		errno = ENOMEM;
		return -1;
	}

	if (!disc->vt->read_sectors(disc, bounce, sec_start, sec_count, true)) {
		free(bounce);
		errno = EIO;
		return -1;
	}

	memcpy(buf, (uint8_t*)bounce + buf_off, count);
	free(bounce);
	return count;
}

static s64 _ntfs_dev_writebytes(struct ntfs_device* dev, s64 offset, s64 count, const void* buf)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (!io || !io->disc) {
		errno = EBADF;
		return -1;
	}
	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}
	if (offset < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	if (!count) {
		return 0;
	}

	DvmDisc* disc = io->disc;
	unsigned sec_sz = io->sector_sz;

	sec_t sec_start = io->start_sector + (sec_t)(offset / sec_sz);
	uint32_t buf_off = (uint32_t)(offset % sec_sz);
	sec_t sec_count = 1;
	if (buf_off + count > sec_sz) {
		sec_count = (sec_t)((buf_off + count + sec_sz - 1) / sec_sz);
	}

	// Sector-aligned: write straight from the caller's buffer.
	if (buf_off == 0 && (count % sec_sz) == 0) {
		if (!disc->vt->write_sectors(disc, buf, sec_start, sec_count, false)) {
			errno = EIO;
			return -1;
		}
		NDevSetDirty(dev);
		return count;
	}

	// Unaligned: read-modify-write, but only the boundary sector(s) that
	// actually overlap data we're not fully replacing.
	uint8_t* bounce = (uint8_t*)aligned_alloc(LIBDVM_BUFFER_ALIGN, (size_t)sec_count * sec_sz);
	if (!bounce) {
		errno = ENOMEM;
		return -1;
	}

	if (buf_off != 0) {
		if (!disc->vt->read_sectors(disc, bounce, sec_start, 1, true)) {
			free(bounce);
			errno = EIO;
			return -1;
		}
	}
	if ((buf_off + count) % sec_sz != 0) {
		if (!disc->vt->read_sectors(disc, bounce + (size_t)(sec_count - 1) * sec_sz, sec_start + sec_count - 1, 1, true)) {
			free(bounce);
			errno = EIO;
			return -1;
		}
	}

	memcpy(bounce + buf_off, buf, count);

	if (!disc->vt->write_sectors(disc, bounce, sec_start, sec_count, false)) {
		free(bounce);
		errno = EIO;
		return -1;
	}

	free(bounce);
	NDevSetDirty(dev);
	return count;
}

static int _ntfs_dev_open(struct ntfs_device* dev, int flags)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (!io || !io->disc) {
		errno = EBADF;
		return -1;
	}
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}

	io->pos = 0;

	if (!(io->disc->features & FEATURE_MEDIUM_CANWRITE)) {
		NDevSetReadOnly(dev);
	}

	NDevSetBlock(dev);
	NDevSetOpen(dev);
	return 0;
}

static int _ntfs_dev_close(struct ntfs_device* dev)
{
	if (!NDevOpen(dev)) {
		errno = EIO;
		return -1;
	}

	NDevClearOpen(dev);
	NDevClearBlock(dev);
	NDevClearDirty(dev);

	// Deliberately does NOT free dev->d_private: unlike libntfs's own
	// gekko_fd (heap-allocated per mount in ntfsMount()), our NtfsDvmIo is
	// embedded inside the enclosing NtfsVolume, which _ntfs_umount() owns.
	return 0;
}

static s64 _ntfs_dev_seek(struct ntfs_device* dev, s64 offset, int whence)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (!io) {
		errno = EBADF;
		return -1;
	}

	uint64_t len = (uint64_t)io->num_sectors * io->sector_sz;
	int64_t newpos;
	switch (whence) {
		case SEEK_SET: newpos = offset; break;
		case SEEK_CUR: newpos = (int64_t)io->pos + offset; break;
		case SEEK_END: newpos = (int64_t)len + offset; break;
		default: errno = EINVAL; return -1;
	}

	if (newpos < 0) newpos = 0;
	if ((uint64_t)newpos > len) newpos = (int64_t)len;

	io->pos = (uint64_t)newpos;
	return (s64)io->pos;
}

static s64 _ntfs_dev_read(struct ntfs_device* dev, void* buf, s64 count)
{
	NtfsDvmIo* io = DEV_IO(dev);
	s64 n = _ntfs_dev_readbytes(dev, (s64)io->pos, count, buf);
	if (n > 0) io->pos += (uint64_t)n;
	return n;
}

static s64 _ntfs_dev_write(struct ntfs_device* dev, const void* buf, s64 count)
{
	NtfsDvmIo* io = DEV_IO(dev);
	s64 n = _ntfs_dev_writebytes(dev, (s64)io->pos, count, buf);
	if (n > 0) io->pos += (uint64_t)n;
	return n;
}

static s64 _ntfs_dev_pread(struct ntfs_device* dev, void* buf, s64 count, s64 offset)
{
	return _ntfs_dev_readbytes(dev, offset, count, buf);
}

static s64 _ntfs_dev_pwrite(struct ntfs_device* dev, const void* buf, s64 count, s64 offset)
{
	return _ntfs_dev_writebytes(dev, offset, count, buf);
}

static int _ntfs_dev_sync(struct ntfs_device* dev)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}

	NDevClearDirty(dev);
	NDevClearSync(dev);

	if (!io->disc->vt->flush(io->disc)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int _ntfs_dev_stat(struct ntfs_device* dev, struct stat* buf)
{
	NtfsDvmIo* io = DEV_IO(dev);
	if (!buf) {
		return 0;
	}

	memset(buf, 0, sizeof(*buf));
	mode_t mode = S_IFBLK | S_IRUSR | S_IRGRP | S_IROTH |
	              (NDevReadOnly(dev) ? 0 : (S_IWUSR | S_IWGRP | S_IWOTH));

	buf->st_dev     = io->disc->io_type;
	buf->st_rdev    = io->disc->io_type;
	buf->st_mode    = mode;
	buf->st_blksize = io->sector_sz;
	buf->st_blocks  = io->num_sectors;
	return 0;
}

static int _ntfs_dev_ioctl(struct ntfs_device* dev, int request, void* argp)
{
	NtfsDvmIo* io = DEV_IO(dev);
	switch (request) {
		case BLKGETSIZE:
			*(uint32_t*)argp = io->num_sectors;
			return 0;

		case BLKGETSIZE64:
			*(uint64_t*)argp = (uint64_t)io->num_sectors * io->sector_sz;
			return 0;

		case BLKSSZGET:
			*(int*)argp = io->sector_sz;
			return 0;

		default:
			errno = EOPNOTSUPP;
			return -1;
	}
}

static struct ntfs_device_operations ntfs_device_dvm_io_ops = {
	.open   = _ntfs_dev_open,
	.close  = _ntfs_dev_close,
	.seek   = _ntfs_dev_seek,
	.read   = _ntfs_dev_read,
	.write  = _ntfs_dev_write,
	.pread  = _ntfs_dev_pread,
	.pwrite = _ntfs_dev_pwrite,
	.sync   = _ntfs_dev_sync,
	.stat   = _ntfs_dev_stat,
	.ioctl  = _ntfs_dev_ioctl,
};

//-----------------------------------------------------------------------------
// mount / umount
//-----------------------------------------------------------------------------

bool _ntfs_mount(devoptab_t* dotab, DvmDisc* disc, DvmPartInfo* part)
{
	NtfsVolume* vol = (NtfsVolume*)dotab->deviceData;
	ntfs_vd* vd = &vol->vd;

	vol->disc          = disc;
	vol->io.disc       = disc;
	vol->io.start_sector = part->start_sector;
	vol->io.num_sectors  = part->num_sectors;
	vol->io.sector_sz    = disc->sector_sz;
	vol->io.pos          = 0;

	vd->id    = disc->io_type;
	vd->atime = ATIME_DISABLED;

	vd->dev = ntfs_device_alloc(dotab->name, 0, &ntfs_device_dvm_io_ops, &vol->io);
	if (!vd->dev) {
		return false;
	}

	ntfs_mount_flags mflags = NTFS_MNT_NONE;
	if (!(disc->features & FEATURE_MEDIUM_CANWRITE)) {
		mflags |= NTFS_MNT_RDONLY;
	} else {
		mflags |= NTFS_MNT_EXCLUSIVE;
	}
	// dvmMountPartition()/DvmFsDriver have no flags path down to us (unlike
	// ntfsMount()'s own NTFS_RECOVER flag), so default to recovering a
	// dirty $LogFile the way a real OS mounting the volume would, rather
	// than failing the mount outright on an unclean shutdown.
	mflags |= NTFS_MNT_RECOVER;

	vd->vol = ntfs_device_mount(vd->dev, mflags);
	if (!vd->vol) {
		ntfs_device_free(vd->dev);
		return false;
	}

	vd->showHiddenFiles = false;
	vd->showSystemFiles = false;

	if (ntfsInitVolume(vd)) {
		ntfs_umount(vd->vol, TRUE);
		return false;
	}

	dvmDiscAddUser(disc);
	return true;
}

void _ntfs_umount(void* device_data)
{
	NtfsVolume* vol = (NtfsVolume*)device_data;
	ntfs_vd* vd = &vol->vd;

	ntfsDeinitVolume(vd);
	ntfs_umount(vd->vol, TRUE);
	dvmDiscRemoveUser(vol->disc);
}

//-----------------------------------------------------------------------------
// Real locking (overrides the weak no-op stubs in libntfs's lock.c)
//-----------------------------------------------------------------------------
// With USE_LWP_LOCK undefined (the Wii U build), libntfs's lock.c defines
// _NTFS_lock_init/_NTFS_lock/_NTFS_unlock/_NTFS_lock_deinit as weak symbols
// that do nothing - see lock.c/lock.h in the libntfs checkout. That makes
// ntfsLock()/ntfsUnlock() (used throughout ntfsfile.c/ntfsdir.c to guard
// vd->cwd_ni and the open-file/open-dir lists) complete no-ops, unlike
// FatVolume/Ext4Volume elsewhere in this same libdvm, which get real
// locking via newlib's _LOCK_T. These strong definitions close that gap
// the same way, by stashing a heap-allocated _LOCK_T's address inside the
// mutex_t (a plain int - see lock.h) libntfs actually stores in ntfs_vd.
// This only helps if the linker picks these over the weak ones, which
// requires this translation unit to actually be linked in.

_Static_assert(sizeof(_LOCK_T*) <= sizeof(int),
	"can't fit a _LOCK_T pointer inside libntfs's mutex_t (int) on this target");

void _NTFS_lock_init(int* mutex, int unused)
{
	(void)unused;
	_LOCK_T* lock = (_LOCK_T*)malloc(sizeof(_LOCK_T));
	if (!lock) {
		*mutex = 0;
		return;
	}
	__lock_init(*lock);
	*mutex = (int)(intptr_t)lock;
}

void _NTFS_lock_deinit(int* mutex)
{
	_LOCK_T* lock = (_LOCK_T*)(intptr_t)*mutex;
	if (lock) {
		__lock_close(*lock);
		free(lock);
	}
	*mutex = 0;
}

void _NTFS_lock(int* mutex)
{
	_LOCK_T* lock = (_LOCK_T*)(intptr_t)*mutex;
	if (lock) {
		__lock_acquire(*lock);
	}
}

void _NTFS_unlock(int* mutex)
{
	_LOCK_T* lock = (_LOCK_T*)(intptr_t)*mutex;
	if (lock) {
		__lock_release(*lock);
	}
}
