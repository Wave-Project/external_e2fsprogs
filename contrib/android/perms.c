#ifndef _GNU_SOURCE
# define _GNU_SOURCE //asprintf
#endif
#include "perms.h"
#include "support/nls-enable.h"
#include <time.h>
#include <sys/stat.h>

#ifndef XATTR_SELINUX_SUFFIX
# define XATTR_SELINUX_SUFFIX  "selinux"
#endif
#ifndef XATTR_CAPS_SUFFIX
# define XATTR_CAPS_SUFFIX     "capability"
#endif

struct inode_params {
	ext2_filsys fs;
	char *path;
	char *filename;
	char *src_dir;
	char *target_out;
	char *mountpoint;
	fs_config_f fs_config_func;
	struct selabel_handle *sehnd;
	time_t fixed_time;
};

static errcode_t ino_add_xattr(ext2_filsys fs, ext2_ino_t ino, const char *name,
			       const void *value, int value_len)
{
	errcode_t retval, close_retval;
	struct ext2_xattr_handle *xhandle;

	retval = ext2fs_xattrs_open(fs, ino, &xhandle);
	if (retval) {
		com_err(__func__, retval, _("while opening inode %u"), ino);
		return retval;
	}
	retval = ext2fs_xattrs_read(xhandle);
	if (retval) {
		com_err(__func__, retval,
			_("while reading xattrs of inode %u"), ino);
		goto xattrs_close;
	}
	retval = ext2fs_xattr_set(xhandle, name, value, value_len);
	if (retval) {
		com_err(__func__, retval,
			_("while setting xattrs of inode %u"), ino);
		goto xattrs_close;
	}
	retval = ext2fs_xattrs_write(xhandle);
	if (retval) {
		com_err(__func__, retval,
			_("while writting xattrs of inode %u"), ino);
		goto xattrs_close;
	}
xattrs_close:
	close_retval = ext2fs_xattrs_close(&xhandle);
	if (close_retval) {
		com_err(__func__, close_retval,
			_("while closing xattrs of inode %u"), ino);
		return retval ? retval : close_retval;
	}
	return retval;
}

static errcode_t set_selinux_xattr(ext2_filsys fs, ext2_ino_t ino,
				   struct inode_params *params)
{
	errcode_t retval;
	char *secontext = NULL;
	struct ext2_inode inode;

	if (params->sehnd == NULL)
		return 0;

	retval = ext2fs_read_inode(fs, ino, &inode);
	if (retval) {
		com_err(__func__, retval,
			_("while reading inode %u"), ino);
		return retval;
	}

	retval = selabel_lookup(params->sehnd, &secontext, params->filename,
				inode.i_mode);
	if (retval < 0) {
		com_err(__func__, retval,
			_("searching for label \"%s\""), params->filename);
		return retval;
	}

	retval = ino_add_xattr(fs, ino,  "security." XATTR_SELINUX_SUFFIX,
			       secontext, strlen(secontext) + 1);

	freecon(secontext);
	return retval;
}

static errcode_t set_perms_and_caps(ext2_filsys fs, ext2_ino_t ino,
				    struct inode_params *params)
{
	errcode_t retval;
	uint64_t capabilities = 0;
	struct ext2_inode inode;
	struct vfs_cap_data cap_data;
	unsigned int uid = 0, gid = 0, imode = 0;

	retval = ext2fs_read_inode(fs, ino, &inode);
	if (retval) {
		com_err(__func__, retval, _("while reading inode %u"), ino);
		return retval;
	}

	/* Permissions */
	if (params->fs_config_func != NULL) {
		params->fs_config_func(params->filename, S_ISDIR(inode.i_mode),
				       params->target_out, &uid, &gid, &imode,
				       &capabilities);
		inode.i_uid = uid & 0xffff;
		inode.i_gid = gid & 0xffff;
		inode.i_mode = (inode.i_mode & S_IFMT) | (imode & 0xffff);
		retval = ext2fs_write_inode(fs, ino, &inode);
		if (retval) {
			com_err(__func__, retval,
				_("while writting inode %u"), ino);
			return retval;
		}
	}

	/* Capabilities */
	if (!capabilities)
		return 0;
	memset(&cap_data, 0, sizeof(cap_data));
	cap_data.magic_etc = VFS_CAP_REVISION | VFS_CAP_FLAGS_EFFECTIVE;
	cap_data.data[0].permitted = (uint32_t) (capabilities & 0xffffffff);
	cap_data.data[1].permitted = (uint32_t) (capabilities >> 32);
	return ino_add_xattr(fs, ino,  "security." XATTR_CAPS_SUFFIX,
			     &cap_data, sizeof(cap_data));
}

static errcode_t set_timestamp(ext2_filsys fs, ext2_ino_t ino,
			       struct inode_params *params)
{
	errcode_t retval;
	struct ext2_inode inode;
	struct stat stat;
	char *src_filename = NULL;

	retval = ext2fs_read_inode(fs, ino, &inode);
	if (retval) {
		com_err(__func__, retval,
			_("while reading inode %u"), ino);
		return retval;
	}

	if (params->fixed_time == -1) {
		/* replace mountpoint from filename with src_dir */
		if (asprintf(&src_filename, "%s/%s", params->src_dir,
					params->filename + strlen(params->mountpoint)) < 0)
			return -ENOMEM;
		retval = lstat(src_filename, &stat);
		if (retval < 0) {
			com_err(__func__, retval,
				_("while lstat file %s"), src_filename);
			goto end;
		}
		inode.i_atime = inode.i_ctime = inode.i_mtime = stat.st_mtime;
	} else {
		inode.i_atime = inode.i_ctime = inode.i_mtime = params->fixed_time;
	}

	retval = ext2fs_write_inode(fs, ino, &inode);
	if (retval) {
		com_err(__func__, retval,
			_("while writting inode %u"), ino);
		goto end;
	}

end:
	free(src_filename);
	return retval;
}

static int is_dir(ext2_filsys fs, ext2_ino_t ino)
{
	struct ext2_inode inode;

	if (ext2fs_read_inode(fs, ino, &inode))
		return 0;
	return S_ISDIR(inode.i_mode);
}

static errcode_t androidify_inode(ext2_filsys fs, ext2_ino_t ino,
				  struct inode_params *params)
{
	errcode_t retval;

	retval = set_timestamp(fs, ino, params);
	if (retval)
		return retval;

	retval = set_selinux_xattr(fs, ino, params);
	if (retval)
		return retval;

	return set_perms_and_caps(fs, ino, params);
}

static int walk_dir(ext2_ino_t dir EXT2FS_ATTR((unused)),
		    int flags EXT2FS_ATTR((unused)),
		    struct ext2_dir_entry *de,
		    int offset EXT2FS_ATTR((unused)),
		    int blocksize EXT2FS_ATTR((unused)),
		    char *buf EXT2FS_ATTR((unused)), void *priv_data)
{
	__u16 name_len;
	errcode_t retval;
	struct inode_params *params = (struct inode_params *)priv_data;

	name_len = de->name_len & 0xff;
	if (!strncmp(de->name, ".", name_len)
	    || (!strncmp(de->name, "..", name_len)))
		return 0;

	if (asprintf(&params->filename, "%s/%.*s", params->path, name_len,
		     de->name) < 0)
		return -ENOMEM;

	if (!strncmp(de->name, "lost+found", 10)) {
		retval = set_selinux_xattr(params->fs, de->inode, params);
		if (retval)
			goto end;
	} else {
		retval = androidify_inode(params->fs, de->inode, params);
		if (retval)
			goto end;
		if (is_dir(params->fs, de->inode)) {
			char *cur_path = params->path;
			char *cur_filename = params->filename;
			params->path = params->filename;
			ext2fs_dir_iterate2(params->fs, de->inode, 0, NULL,
					    walk_dir, params);
			params->path = cur_path;
			params->filename = cur_filename;
		}
	}

end:
	free(params->filename);
	return retval;
}

errcode_t __android_configure_fs(ext2_filsys fs, char *src_dir,
				 char *target_out,
				 char *mountpoint,
				 fs_config_f fs_config_func,
				 struct selabel_handle *sehnd,
				 time_t fixed_time)
{
	errcode_t retval;
	struct inode_params params = {
		.fs = fs,
		.src_dir = src_dir,
		.target_out = target_out,
		.fs_config_func = fs_config_func,
		.sehnd = sehnd,
		.fixed_time = fixed_time,
		.path = mountpoint,
		.filename = mountpoint,
		.mountpoint = mountpoint,
	};

	/* walk_dir will add the "/". Don't add it twice. */
	if (strlen(mountpoint) == 1 && mountpoint[0] == '/')
		params.path = "";

	retval = set_selinux_xattr(fs, EXT2_ROOT_INO, &params);
	if (retval)
		return retval;
	retval = set_timestamp(fs, EXT2_ROOT_INO, &params);
	if (retval)
		return retval;

	return ext2fs_dir_iterate2(fs, EXT2_ROOT_INO, 0, NULL, walk_dir,
				   &params);
}

errcode_t android_configure_fs(ext2_filsys fs, char *src_dir, char *target_out,
			       char *mountpoint,
			       char *file_contexts,
			       char *fs_config_file, time_t fixed_time)
{
	errcode_t retval;
	fs_config_f fs_config_func = NULL;
	struct selabel_handle *sehnd = NULL;

	/* Retrieve file contexts */
	if (file_contexts) {
		struct selinux_opt seopts[] = { { SELABEL_OPT_PATH, "" } };
		seopts[0].value = file_contexts;
		sehnd = selabel_open(SELABEL_CTX_FILE, seopts, 1);
		if (!sehnd) {
			com_err(__func__, -EINVAL,
				_("while opening file contexts \"%s\""),
				seopts[0].value);
			return -EINVAL;
		}
	}

	/* Load the FS config */
	if (fs_config_file) {
		retval = load_canned_fs_config(fs_config_file);
		if (retval < 0) {
			com_err(__func__, retval,
				_("while loading fs_config \"%s\""),
				fs_config_file);
			return retval;
		}
		fs_config_func = canned_fs_config;
	} else if (mountpoint)
		fs_config_func = fs_config;

	return __android_configure_fs(fs, src_dir, target_out, mountpoint,
				      fs_config_func, sehnd, fixed_time);
}
