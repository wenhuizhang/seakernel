#include <sea/types.h>
#include <sea/fs/fs.h>
#include <sea/fs/inode.h>
#include <sea/kernel.h>
#include <sea/cpu/atomic.h>
#include <sea/mm/kmalloc.h>
#include <sea/errno.h>
static uint32_t fsids = 0;

static struct hash_table fsdrivershash;
static struct llist fsdriverslist;

static struct llist fslist;
extern struct filesystem *devfs;

void fs_fsm_init()
{
	ll_create(&fsdriverslist);
	ll_create(&fslist);
	hash_table_create(&fsdrivershash, 0, HASH_TYPE_CHAIN);
	hash_table_resize(&fsdrivershash, HASH_RESIZE_MODE_IGNORE,10);
	hash_table_specify_function(&fsdrivershash, HASH_FUNCTION_BYTE_SUM);
}

struct inode *fs_read_root_inode(struct filesystem *fs)
{
	struct inode *node = vfs_icache_get(fs, fs->root_inode_id);
	assert(node);
	return node;
}

int fs_filesystem_init_mount(struct filesystem *fs, char *node, char *type, int opts)
{
	strncpy(fs->type, type, 128);
	fs->opts = opts;
	if(!strcmp(fs->type, "devfs"))
		return 0;
	int err;
	struct inode *i = fs_path_resolve_inode(node, 0, &err);
	if(!i)
		return err;
	fs->dev = i->phys_dev;
	vfs_icache_put(i);
	if(!strcmp(fs->type, "tmpfs"))
		return ramfs_mount(fs);
	struct fsdriver *fd = 0;

	if(type) {
		if(hash_table_get_entry(&fsdrivershash, type, 1, strlen(type), (void **)&fd) == -ENOENT)
			return -EINVAL;
		fs->driver = fd;
		return fd->mount(fs);
	} else {
		rwlock_acquire(&fsdriverslist.rwl, RWL_READER);
		struct llistnode *ln;
		ll_for_each_entry(&fsdriverslist, ln, struct fsdriver *, fd) {
			if(!fd->mount(fs)) {
				fs->driver = fd;
				rwlock_release(&fsdriverslist.rwl, RWL_READER);
				return 0;
			}
		}
		rwlock_release(&fsdriverslist.rwl, RWL_READER);
	}
	return -EINVAL;
}

void fs_unmount_all()
{
	struct llistnode *ln, *next;
	struct filesystem *fs;
	rwlock_acquire(&fslist.rwl, RWL_WRITER);
	ll_for_each_entry_safe(&fslist, ln, next, struct filesystem *, fs) {
		if(fs != devfs)
			fs_umount(fs);
	}
	rwlock_release(&fslist.rwl, RWL_WRITER);
}

int fs_mount(struct inode *pt, struct filesystem *fs)
{
	if(!strcmp(fs->type, "devfs")) {
		fs = devfs;
	} else {
		fs->listnode = ll_insert(&fslist, fs);
	}
	vfs_inode_mount(pt, fs);
	return 0;
}

int fs_umount(struct filesystem *fs)
{
	assert(fs);
	if(fs->driver && fs->driver->umount)
		fs->driver->umount(fs);
	vfs_inode_umount(fs->point);
	return 0;
}

struct filesystem *fs_filesystem_create()
{
	struct filesystem *fs = kmalloc(sizeof(struct filesystem));
	fs->id = add_atomic(&fsids, 1)-1;
	return fs;
}

void fs_filesystem_destroy(struct filesystem *fs)
{
	kfree(fs);
}

int fs_filesystem_register(struct fsdriver *fd)
{
	fd->ln = ll_insert(&fsdriverslist, fd);
	return hash_table_set_entry(&fsdrivershash, (void *)fd->name, 1, strlen(fd->name), fd);
}

int fs_filesystem_unregister(struct fsdriver *fd)
{
	assert(fd->ln);
	ll_remove(&fsdriverslist, fd->ln);
	fd->ln=0;
	return hash_table_delete_entry(&fsdrivershash, (void *)fd->name, 1, strlen(fd->name));
}

