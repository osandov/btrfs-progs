/*
 * Copyright (C) 2012 FUJITSU LIMITED.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_LIST_H__
#define __BTRFS_LIST_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "rbtree.h"
#include "ioctl.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/rbtree.h>
#include <btrfs/ioctl.h>
#endif /* BTRFS_FLAT_INCLUDES */

#include <time.h>

/*
 * one of these for each root we find.
 */
struct root_info {
	struct rb_node rb_node;
	struct rb_node sort_node;

	/* this root's id */
	u64 root_id;

	/* equal the offset of the root's key */
	u64 root_offset;

	/* flags of the root */
	u64 flags;

	/* the id of the root that references this one */
	u64 ref_tree;

	/* the dir id we're in from ref_tree */
	u64 dir_id;

	u64 top_id;

	/* generation when the root is created or last updated */
	u64 gen;

	/* creation generation of this root in sec*/
	u64 ogen;

	/* creation time of this root in sec*/
	time_t otime;

	u8 uuid[BTRFS_UUID_SIZE];
	u8 puuid[BTRFS_UUID_SIZE];
	u8 ruuid[BTRFS_UUID_SIZE];

	/* path from the subvol we live in to this root, including the
	 * root's name.  This is null until we do the extra lookup ioctl.
	 */
	char *path;

	/* the name of this root in the directory it lives in */
	char *name;

	char *full_path;

	int deleted;
};

int btrfs_list_find_updated_files(int fd, u64 root_id, u64 oldest_gen);
int btrfs_list_get_default_subvolume(int fd, u64 *default_id)
	__attribute__((deprecated("use btrfs_util_get_default_subvolume_fd()")));
char *btrfs_list_path_for_root(int fd, u64 root)
	__attribute__((deprecated("use btrfs_util_subvolume_path_fd()")));
int btrfs_list_get_path_rootid(int fd, u64 *treeid)
	__attribute__((deprecated("use btrfs_util_subvolume_id_fd()")));
int btrfs_get_subvol(int fd, struct root_info *the_ri)
	__attribute__((deprecated("use btrfs_util_subvolume_info_fd() and btrfs_util_subvolume_path_fd()")));
int btrfs_get_toplevel_subvol(int fd, struct root_info *the_ri)
	__attribute__((deprecated("use btrfs_util_subvolume_info_fd() and btrfs_util_subvolume_path_fd()")));

#endif
