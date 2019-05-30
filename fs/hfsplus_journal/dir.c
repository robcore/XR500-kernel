/*
 *  linux/fs/hfsplus/dir.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handling of directories
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/random.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

static inline void hfsplus_instantiate(struct dentry *dentry,
				       struct inode *inode, u32 cnid)
{
	dentry->d_fsdata = (void *)(unsigned long)cnid;
	d_instantiate(dentry, inode);
}

/* Find the entry inside dir named dentry->d_name */
static struct dentry *hfsplus_lookup(struct inode *dir, struct dentry *dentry,
				     struct nameidata *nd)
{
	struct inode *inode = NULL;
	struct hfs_find_data fd;
	struct super_block *sb;
	hfsplus_handle_t hfsplus_handle;
	hfsplus_cat_entry entry;
	int err;
	u32 cnid, linkid = 0;
	u16 type;

	if (hfsplus_journal_start(__FUNCTION__, dir->i_sb, &hfsplus_handle))
		return NULL;
	sb = dir->i_sb;

	dentry->d_fsdata = NULL;
	err = hfs_find_init(HFSPLUS_SB(sb)->cat_tree, &fd);
	if (err)
		return ERR_PTR(err);
	hfsplus_cat_build_key(sb, fd.search_key, dir->i_ino, &dentry->d_name);
	/*Fix the issue un-have dir mode*/
	if (dir->i_ino == 19) {
		dir->i_mode |= S_ISVTX;
	}
again:
	err = hfs_brec_read(&hfsplus_handle, &fd, &entry, sizeof(entry));
	if (err) {
		if (err == -ENOENT) {
			hfs_find_exit(&hfsplus_handle, &fd);
			/* No such entry */
			inode = NULL;
			goto out;
		}
		goto fail;
	}
	type = be16_to_cpu(entry.type);
	if (type == HFSPLUS_FOLDER) {
		if (fd.entrylength < sizeof(struct hfsplus_cat_folder)) {
			err = -EIO;
			goto fail;
		}
		cnid = be32_to_cpu(entry.folder.id);
		dentry->d_fsdata = (void *)(unsigned long)cnid;
	} else if (type == HFSPLUS_FILE) {
		if (fd.entrylength < sizeof(struct hfsplus_cat_file)) {
			err = -EIO;
			goto fail;
		}
		cnid = be32_to_cpu(entry.file.id);
		if (entry.file.user_info.fdType ==
				cpu_to_be32(HFSP_HARDLINK_TYPE) &&
				entry.file.user_info.fdCreator ==
				cpu_to_be32(HFSP_HFSPLUS_CREATOR) &&
				(entry.file.create_date ==
					HFSPLUS_I(HFSPLUS_SB(sb)->hidden_dir)->
						create_date ||
				entry.file.create_date ==
					HFSPLUS_I(sb->s_root->d_inode)->
						create_date) &&
				HFSPLUS_SB(sb)->hidden_dir) {
			struct qstr str;
			char name[32];

			if (dentry->d_fsdata) {
				/*
				 * We found a link pointing to another link,
				 * so ignore it and treat it as regular file.
				 */
				cnid = (unsigned long)dentry->d_fsdata;
				linkid = 0;
			} else {
				dentry->d_fsdata = (void *)(unsigned long)cnid;
				linkid =
					be32_to_cpu(entry.file.permissions.dev);
				str.len = sprintf(name, "iNode%d", linkid);
				str.name = name;
				hfsplus_cat_build_key(sb, fd.search_key,
					HFSPLUS_SB(sb)->hidden_dir->i_ino,
					&str);
				goto again;
			}
		} else if (!dentry->d_fsdata)
			dentry->d_fsdata = (void *)(unsigned long)cnid;
	} else {
		printk(KERN_ERR "hfs: invalid catalog entry type in lookup\n");
		err = -EIO;
		goto fail;
	}
	hfs_find_exit(&hfsplus_handle, &fd);
	inode = hfsplus_iget(dir->i_sb, cnid);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	if (S_ISREG(inode->i_mode))
		HFSPLUS_I(inode)->linkid = linkid;
out:
	d_add(dentry, inode);
	hfsplus_journal_stop(&hfsplus_handle);
	return NULL;
fail:
	hfs_find_exit(&hfsplus_handle, &fd);
	hfsplus_journal_stop(&hfsplus_handle);
	return ERR_PTR(err);
}

static int hfsplus_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	int len, err;
	char strbuf[HFSPLUS_MAX_STRLEN + 1];
	hfsplus_cat_entry entry;
	struct hfs_find_data fd;
	struct hfsplus_readdir_data *rd;
	hfsplus_handle_t hfsplus_handle;
	u16 type;

	if (filp->f_pos >= inode->i_size)
		return 0;

	if ((err = hfsplus_journal_start(__FUNCTION__, sb, &hfsplus_handle)))
		return err;
	err = hfs_find_init(HFSPLUS_SB(sb)->cat_tree, &fd);
	if (err)
		return err;
	hfsplus_cat_build_key(sb, fd.search_key, inode->i_ino, NULL);
	err = hfs_brec_find(&hfsplus_handle, &fd);
	if (err)
		goto out;

	switch ((u32)filp->f_pos) {
	case 0:
		/* This is completely artificial... */
		if (filldir(dirent, ".", 1, 0, inode->i_ino, DT_DIR))
			goto out;
		filp->f_pos++;
		/* fall through */
	case 1:
		if (fd.entrylength > sizeof(entry) || fd.entrylength < 0) {
			err = -EIO;
			goto out;
		}

		hfs_bnode_read(fd.bnode, &entry, fd.entryoffset,
			fd.entrylength);
		if (be16_to_cpu(entry.type) != HFSPLUS_FOLDER_THREAD) {
			printk(KERN_ERR "hfs: bad catalog folder thread\n");
			err = -EIO;
			goto out;
		}
		if (fd.entrylength < HFSPLUS_MIN_THREAD_SZ) {
			printk(KERN_ERR "hfs: truncated catalog thread\n");
			err = -EIO;
			goto out;
		}
		if (filldir(dirent, "..", 2, 1,
			    be32_to_cpu(entry.thread.parentID), DT_DIR))
			goto out;
		filp->f_pos++;
		/* fall through */
	default:
		if (filp->f_pos >= inode->i_size)
			goto out;
		err = hfs_brec_goto(&hfsplus_handle, &fd, filp->f_pos - 1);
		if (err)
			goto out;
	}

	for (;;) {
		if (be32_to_cpu(fd.key->cat.parent) != inode->i_ino) {
			printk(KERN_ERR "hfs: walked past end of dir\n");
			err = -EIO;
			goto out;
		}

		if (fd.entrylength > sizeof(entry) || fd.entrylength < 0) {
			err = -EIO;
			goto out;
		}

		hfs_bnode_read(fd.bnode, &entry, fd.entryoffset,
			fd.entrylength);
		type = be16_to_cpu(entry.type);
		len = HFSPLUS_MAX_STRLEN;
		err = hfsplus_uni2asc(sb, &fd.key->cat.name, strbuf, &len);
		if (err)
			goto out;
		if (type == HFSPLUS_FOLDER) {
			if (fd.entrylength <
					sizeof(struct hfsplus_cat_folder)) {
				printk(KERN_ERR "hfs: small dir entry\n");
				err = -EIO;
				goto out;
			}
			if (HFSPLUS_SB(sb)->hidden_dir &&
			    HFSPLUS_SB(sb)->hidden_dir->i_ino ==
					be32_to_cpu(entry.folder.id))
				goto next;
			if (filldir(dirent, strbuf, len, filp->f_pos,
				    be32_to_cpu(entry.folder.id), DT_DIR))
				break;
		} else if (type == HFSPLUS_FILE) {
			if (fd.entrylength < sizeof(struct hfsplus_cat_file)) {
				printk(KERN_ERR "hfs: small file entry\n");
				err = -EIO;
				goto out;
			}
			if (filldir(dirent, strbuf, len, filp->f_pos,
				    be32_to_cpu(entry.file.id), DT_REG))
				break;
		} else {
			printk(KERN_ERR "hfs: bad catalog entry type\n");
			err = -EIO;
			goto out;
		}
next:
		filp->f_pos++;
		if (filp->f_pos >= inode->i_size)
			goto out;
		err = hfs_brec_goto(&hfsplus_handle, &fd, 1);
		if (err)
			goto out;
	}
	rd = filp->private_data;
	if (!rd) {
		rd = kmalloc(sizeof(struct hfsplus_readdir_data), GFP_KERNEL);
		if (!rd) {
			err = -ENOMEM;
			goto out;
		}
		filp->private_data = rd;
		rd->file = filp;
		list_add(&rd->list, &HFSPLUS_I(inode)->open_dir_list);
	}
	memcpy(&rd->key, fd.key, sizeof(struct hfsplus_cat_key));
out:
	hfs_find_exit(&hfsplus_handle, &fd);
	hfsplus_journal_stop(&hfsplus_handle);
	return err;
}

static int hfsplus_dir_release(struct inode *inode, struct file *file)
{
	struct hfsplus_readdir_data *rd = file->private_data;
	if (rd) {
		mutex_lock(&inode->i_mutex);
		list_del(&rd->list);
		mutex_unlock(&inode->i_mutex);
		kfree(rd);
	}
	return 0;
}

static int hfsplus_link(struct dentry *src_dentry, struct inode *dst_dir,
			struct dentry *dst_dentry)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(dst_dir->i_sb);
	struct inode *inode = src_dentry->d_inode;
	struct inode *src_dir = src_dentry->d_parent->d_inode;
	hfsplus_handle_t hfsplus_handle;
	struct qstr str;
	char name[32];
	u32 cnid, id;
	int res;

	if (HFSPLUS_IS_RSRC(inode))
		return -EPERM;
	if (!S_ISREG(inode->i_mode))
		return -EPERM;

	if ((res = hfsplus_journal_start(__FUNCTION__, dst_dir->i_sb, &hfsplus_handle)))
		return res;
	
	mutex_lock(&sbi->vh_mutex);
	if (inode->i_ino == (u32)(unsigned long)src_dentry->d_fsdata) {
		for (;;) {
			get_random_bytes(&id, sizeof(cnid));
			id &= 0x3fffffff;
			str.name = name;
			str.len = sprintf(name, "iNode%d", id);
			res = hfsplus_rename_cat(&hfsplus_handle, inode->i_ino,
						 src_dir, &src_dentry->d_name,
						 sbi->hidden_dir, &str);
			if (!res)
				break;
			if (res != -EEXIST) {
				//hfsplus_journal_stop(&hfsplus_handle);
				goto out;
			}
		}
		HFSPLUS_I(inode)->linkid = id;
		cnid = sbi->next_cnid++;
		src_dentry->d_fsdata = (void *)(unsigned long)cnid;
		res = hfsplus_create_cat(&hfsplus_handle, cnid, src_dir, &src_dentry->d_name, inode);
		if (res) {
			/* panic? */
			//hfsplus_journal_stop(&hfsplus_handle);
			goto out;
		}
		sbi->file_count++;
	}
	cnid = sbi->next_cnid++;
	res = hfsplus_create_cat(&hfsplus_handle, cnid, dst_dir, &dst_dentry->d_name, inode);
	if (res) {
		//hfsplus_journal_stop(&hfsplus_handle);
		goto out;
	}

	inc_nlink(inode);
	hfsplus_instantiate(dst_dentry, inode, cnid);
	ihold(inode);
	inode->i_ctime = CURRENT_TIME_SEC;
	res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
	sbi->file_count++;
	dst_dir->i_sb->s_dirt = 1;
out:
	hfsplus_journal_stop(&hfsplus_handle);
	mutex_unlock(&sbi->vh_mutex);
	return res;
}

static int hfsplus_unlink(struct inode *dir, struct dentry *dentry)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(dir->i_sb);
	struct inode *inode = dentry->d_inode;
	struct qstr str;
	char name[32];
	u32 cnid;
	int res;
	hfsplus_handle_t hfsplus_handle;

	if (HFSPLUS_IS_RSRC(inode))
		return -EPERM;

	if ((res = hfsplus_journal_start(__FUNCTION__, dir->i_sb, &hfsplus_handle)))
		return res;
	
	mutex_lock(&sbi->vh_mutex);
	cnid = (u32)(unsigned long)dentry->d_fsdata;
	if (inode->i_ino == cnid &&
	    atomic_read(&HFSPLUS_I(inode)->opencnt)) {
		str.name = name;
		str.len = sprintf(name, "temp%lu", inode->i_ino);
		res = hfsplus_rename_cat(&hfsplus_handle, inode->i_ino,
					 dir, &dentry->d_name,
					 sbi->hidden_dir, &str);
		if (!res) {
			inode->i_flags |= S_DEAD;
			drop_nlink(inode);
		}
		//hfsplus_journal_stop(&hfsplus_handle);
		goto out;
	}
	res = hfsplus_delete_cat(&hfsplus_handle, cnid, dir, &dentry->d_name);
	if (res) {
		//hfsplus_journal_stop(&hfsplus_handle);
		goto out;
	}

	if (inode->i_nlink > 0)
		drop_nlink(inode);
	if (inode->i_ino == cnid)
		clear_nlink(inode);
	if (!inode->i_nlink) {
		if (inode->i_ino != cnid) {
			sbi->file_count--;
			if (!atomic_read(&HFSPLUS_I(inode)->opencnt)) {
				res = hfsplus_delete_cat(&hfsplus_handle, inode->i_ino,
							 sbi->hidden_dir,
							 NULL);
				if (!res)
					hfsplus_delete_inode(&hfsplus_handle, inode);
			} else
				inode->i_flags |= S_DEAD;
		} else
			hfsplus_delete_inode(&hfsplus_handle, inode);
	} else
		sbi->file_count--;
	inode->i_ctime = CURRENT_TIME_SEC;
	res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
out:
	hfsplus_journal_stop(&hfsplus_handle);
	mutex_unlock(&sbi->vh_mutex);
	return res;
}

static int hfsplus_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(dir->i_sb);
	struct inode *inode = dentry->d_inode;
	hfsplus_handle_t hfsplus_handle;
	int res;
	
	if ((res = hfsplus_journal_start(__FUNCTION__, dir->i_sb, &hfsplus_handle)))
		return res;
	if (inode->i_size != 2){
		hfsplus_journal_stop(&hfsplus_handle);
		return -ENOTEMPTY;
	}

	mutex_lock(&sbi->vh_mutex);
	res = hfsplus_delete_cat(&hfsplus_handle, inode->i_ino, dir, &dentry->d_name);
	if (res)
		goto out;
	clear_nlink(inode);
	inode->i_ctime = CURRENT_TIME_SEC;
	hfsplus_delete_inode(&hfsplus_handle, inode);
	res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
out:
	hfsplus_journal_stop(&hfsplus_handle);
	mutex_unlock(&sbi->vh_mutex);
	return res;
}

static int hfsplus_symlink(struct inode *dir, struct dentry *dentry,
			   const char *symname)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(dir->i_sb);
	struct inode *inode;
	int res = -ENOSPC;
	hfsplus_handle_t hfsplus_handle;
	
	if ((res = hfsplus_journal_start(__FUNCTION__, dir->i_sb, &hfsplus_handle)))
		return res;

	mutex_lock(&sbi->vh_mutex);
	inode = hfsplus_new_inode(&hfsplus_handle, dir->i_sb, S_IFLNK | S_IRWXUGO);
	if (!inode){
		hfsplus_journal_stop(&hfsplus_handle);
		goto out;
	}

	res = page_symlink(inode, symname, strlen(symname) + 1);
	if (res)
		goto out_err;

	res = hfsplus_create_cat(&hfsplus_handle, inode->i_ino, dir, &dentry->d_name, inode);
	if (res)
		goto out_err;

	hfsplus_instantiate(dentry, inode, inode->i_ino);
	res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
	hfsplus_journal_stop(&hfsplus_handle);
	goto out;

out_err:
	clear_nlink(inode);
	hfsplus_delete_inode(&hfsplus_handle, inode);
	iput(inode);
	hfsplus_journal_stop(&hfsplus_handle);
out:
	mutex_unlock(&sbi->vh_mutex);
	return res;
}

static int hfsplus_mknod(struct inode *dir, struct dentry *dentry,
			 umode_t mode, dev_t rdev)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(dir->i_sb);
	struct inode *inode;
	hfsplus_handle_t hfsplus_handle;
	int res = -ENOSPC;

	if ((res = hfsplus_journal_start(__FUNCTION__, dir->i_sb, &hfsplus_handle)))
		return res;
	mutex_lock(&sbi->vh_mutex);
	inode = hfsplus_new_inode(&hfsplus_handle, dir->i_sb, mode);
	if (!inode)
		goto out;

	if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISFIFO(mode) || S_ISSOCK(mode))
		init_special_inode(inode, mode, rdev);

	res = hfsplus_create_cat(&hfsplus_handle, inode->i_ino, dir, &dentry->d_name, inode);
	if (res) {
		clear_nlink(inode);
		hfsplus_delete_inode(&hfsplus_handle, inode);
		iput(inode);
		goto out;
	}

	hfsplus_instantiate(dentry, inode, inode->i_ino);
	res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
out:
	hfsplus_journal_stop(&hfsplus_handle);
	mutex_unlock(&sbi->vh_mutex);
	return res;
}

static int hfsplus_create(struct inode *dir, struct dentry *dentry, umode_t mode,
			  struct nameidata *nd)
{
	return hfsplus_mknod(dir, dentry, mode, 0);
}

static int hfsplus_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	/*Fix the issue incorrect hard links ...*/
	if (dir->i_ino == 19) 
		return 0;
	return hfsplus_mknod(dir, dentry, mode | S_IFDIR, 0);
}

static int hfsplus_rename(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
{
	int res;
	hfsplus_handle_t hfsplus_handle;

	/* Unlink destination if it already exists */
	if (new_dentry->d_inode) {
		if (S_ISDIR(new_dentry->d_inode->i_mode))
			res = hfsplus_rmdir(new_dir, new_dentry);
		else
			res = hfsplus_unlink(new_dir, new_dentry);
		if (res)
			return res;
	}
	
	if ((res = hfsplus_journal_start(__FUNCTION__, old_dir->i_sb, &hfsplus_handle)))
		return res;

	res = hfsplus_rename_cat(&hfsplus_handle,(u32)(unsigned long)old_dentry->d_fsdata,
				 old_dir, &old_dentry->d_name,
				 new_dir, &new_dentry->d_name);
	if (!res)
		new_dentry->d_fsdata = old_dentry->d_fsdata;
	hfsplus_journal_stop(&hfsplus_handle);
	return res;
}

const struct inode_operations hfsplus_dir_inode_operations = {
	.lookup		= hfsplus_lookup,
	.create		= hfsplus_create,
	.link		= hfsplus_link,
	.unlink		= hfsplus_unlink,
	.mkdir		= hfsplus_mkdir,
	.rmdir		= hfsplus_rmdir,
	.symlink	= hfsplus_symlink,
	.mknod		= hfsplus_mknod,
	.rename		= hfsplus_rename,
};

const struct file_operations hfsplus_dir_operations = {
	.fsync		= hfsplus_file_fsync,
	.read		= generic_read_dir,
	.readdir	= hfsplus_readdir,
	.unlocked_ioctl = hfsplus_ioctl,
	.llseek		= generic_file_llseek,
	.release	= hfsplus_dir_release,
};
