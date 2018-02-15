/*
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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <libgen.h>
#include <limits.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <linux/magic.h>

#include <btrfsutil.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "qgroup.h"

#include "ctree.h"
#include "commands.h"
#include "utils.h"
#include "btrfs-list.h"
#include "utils.h"
#include "help.h"

static int wait_for_subvolume_cleaning(int fd, size_t count, uint64_t *ids,
				       int sleep_interval)
{
	size_t i;
	enum btrfs_util_error err;

	while (1) {
		int clean = 1;

		for (i = 0; i < count; i++) {
			if (!ids[i])
				continue;
			err = btrfs_util_subvolume_info_fd(fd, ids[i], NULL);
			if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
				printf("Subvolume id %" PRIu64 " is gone\n",
				       ids[i]);
				ids[i] = 0;
			} else if (err) {
				error_btrfs_util(err);
				return -errno;
			} else {
				clean = 0;
			}
		}
		if (clean)
			break;
		sleep(sleep_interval);
	}

	return 0;
}

static int qgroup_inherit_add_group(struct btrfs_util_qgroup_inherit **inherit,
				    const char *arg)
{
	enum btrfs_util_error err;
	u64 qgroupid;

	if (!*inherit) {
		err = btrfs_util_create_qgroup_inherit(0, inherit);
		if (err) {
			error_btrfs_util(err);
			return -1;
		}
	}

	qgroupid = parse_qgroupid(optarg);
	if (qgroupid == 0) {
		error("invalid qgroup specification, qgroupid must not be 0");
		return -1;
	}

	err = btrfs_util_qgroup_inherit_add_group(inherit, qgroupid);
	if (err) {
		error_btrfs_util(err);
		return -1;
	}

	return 0;
}

static const char * const subvolume_cmd_group_usage[] = {
	"btrfs subvolume <command> <args>",
	NULL
};

static const char * const cmd_subvol_create_usage[] = {
	"btrfs subvolume create [-i <qgroupid>] [<dest>/]<name>",
	"Create a subvolume",
	"Create a subvolume <name> in <dest>.  If <dest> is not given",
	"subvolume <name> will be created in the current directory.",
	"",
	"-i <qgroupid>  add the newly created subvolume to a qgroup. This",
	"               option can be given multiple times.",
	NULL
};

static int cmd_subvol_create(int argc, char **argv)
{
	struct btrfs_util_qgroup_inherit *inherit = NULL;
	enum btrfs_util_error err;
	int retval = 1;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "i:");
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			if (qgroup_inherit_add_group(&inherit, optarg) == -1)
				goto out;
			break;
		default:
			usage(cmd_subvol_create_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_create_usage);

	printf("Create subvolume '%s'\n", argv[optind]);

	err = btrfs_util_create_subvolume(argv[optind], 0, NULL, inherit);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	retval = 0;
out:
	if (inherit)
		btrfs_util_destroy_qgroup_inherit(inherit);

	return retval;
}

static int wait_for_commit(int fd)
{
	enum btrfs_util_error err;
	uint64_t transid;

	err = btrfs_util_start_sync_fd(fd, &transid);
	if (err)
		return -1;

	err = btrfs_util_wait_sync_fd(fd, transid);
	if (err)
		return -1;

	return 0;
}

static const char * const cmd_subvol_delete_usage[] = {
	"btrfs subvolume delete [options] <subvolume> [<subvolume>...]",
	"Delete subvolume(s)",
	"Delete subvolumes from the filesystem. The corresponding directory",
	"is removed instantly but the data blocks are removed later.",
	"The deletion does not involve full commit by default due to",
	"performance reasons (as a consequence, the subvolume may appear again",
	"after a crash). Use one of the --commit options to wait until the",
	"operation is safely stored on the media.",
	"",
	"-c|--commit-after      wait for transaction commit at the end of the operation",
	"-C|--commit-each       wait for transaction commit after deleting each subvolume",
	"-R|--recursive         delete subvolumes beneath each subvolume recursively",
	"-v|--verbose           verbose output of operations",
	NULL
};

static int cmd_subvol_delete(int argc, char **argv)
{
	int res, ret = 0;
	int cnt;
	int fd = -1;
	char	*dname, *vname, *cpath;
	char	*dupdname = NULL;
	char	*dupvname = NULL;
	char	*path;
	DIR	*dirstream = NULL;
	int verbose = 0;
	int commit_mode = 0;
	int flags = 0;
	u8 fsid[BTRFS_FSID_SIZE];
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = { NULL, };
	enum { COMMIT_AFTER = 1, COMMIT_EACH = 2 };
	enum btrfs_util_error err;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{"commit-after", no_argument, NULL, 'c'},
			{"commit-each", no_argument, NULL, 'C'},
			{"recursive", no_argument, NULL, 'R'},
			{"verbose", no_argument, NULL, 'v'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "cCRv", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			commit_mode = COMMIT_AFTER;
			break;
		case 'C':
			commit_mode = COMMIT_EACH;
			break;
		case 'R':
			flags |= BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage(cmd_subvol_delete_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_subvol_delete_usage);

	if (verbose > 0) {
		printf("Transaction commit: %s\n",
			!commit_mode ? "none (default)" :
			commit_mode == COMMIT_AFTER ? "at the end" : "after each");
	}

	cnt = optind;

again:
	path = argv[cnt];

	err = btrfs_util_is_subvolume(path);
	if (err) {
		error_btrfs_util(err);
		ret = 1;
		goto out;
	}

	cpath = realpath(path, NULL);
	if (!cpath) {
		ret = errno;
		error("cannot find real path for '%s': %m", path);
		goto out;
	}
	dupdname = strdup(cpath);
	dname = dirname(dupdname);
	dupvname = strdup(cpath);
	vname = basename(dupvname);
	free(cpath);

	fd = btrfs_open_dir(dname, &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	printf("Delete subvolume (%s): '%s/%s'\n",
		commit_mode == COMMIT_EACH || (commit_mode == COMMIT_AFTER && cnt + 1 == argc)
		? "commit" : "no-commit", dname, vname);

	err = btrfs_util_delete_subvolume_fd(fd, vname, flags);
	if (err) {
		error_btrfs_util(err);
		ret = 1;
		goto out;
	}

	if (commit_mode == COMMIT_EACH) {
		res = wait_for_commit(fd);
		if (res < 0) {
			error("unable to wait for commit after '%s': %m", path);
			ret = 1;
		}
	} else if (commit_mode == COMMIT_AFTER) {
		res = get_fsid(dname, fsid, 0);
		if (res < 0) {
			errno = -res;
			error("unable to get fsid for '%s': %m", path);
			error(
			"delete succeeded but commit may not be done in the end");
			ret = 1;
			goto out;
		}

		if (add_seen_fsid(fsid, seen_fsid_hash, fd, dirstream) == 0) {
			if (verbose > 0) {
				uuid_unparse(fsid, uuidbuf);
				printf("  new fs is found for '%s', fsid: %s\n",
						path, uuidbuf);
			}
			/*
			 * This is the first time a subvolume on this
			 * filesystem is deleted, keep fd in order to issue
			 * SYNC ioctl in the end
			 */
			goto keep_fd;
		}
	}

out:
	close_file_or_dir(fd, dirstream);
keep_fd:
	fd = -1;
	dirstream = NULL;
	free(dupdname);
	free(dupvname);
	dupdname = NULL;
	dupvname = NULL;
	cnt++;
	if (cnt < argc)
		goto again;

	if (commit_mode == COMMIT_AFTER) {
		int slot;

		/*
		 * Traverse seen_fsid_hash and issue SYNC ioctl on each
		 * filesystem
		 */
		for (slot = 0; slot < SEEN_FSID_HASH_SIZE; slot++) {
			struct seen_fsid *seen = seen_fsid_hash[slot];

			while (seen) {
				res = wait_for_commit(seen->fd);
				if (res < 0) {
					uuid_unparse(seen->fsid, uuidbuf);
					error(
			"unable to do final sync after deletion: %m, fsid: %s",
						uuidbuf);
					ret = 1;
				} else if (verbose > 0) {
					uuid_unparse(seen->fsid, uuidbuf);
					printf("final sync is done for fsid: %s\n",
						uuidbuf);
				}
				seen = seen->next;
			}
		}
		/* fd will also be closed in free_seen_fsid */
		free_seen_fsid(seen_fsid_hash);
	}

	return ret;
}

enum btrfs_list_layout {
	BTRFS_LIST_LAYOUT_DEFAULT = 0,
	BTRFS_LIST_LAYOUT_TABLE,
	BTRFS_LIST_LAYOUT_RAW
};

enum btrfs_list_column_enum {
	BTRFS_LIST_OBJECTID,
	BTRFS_LIST_GENERATION,
	BTRFS_LIST_OGENERATION,
	BTRFS_LIST_PARENT,
	BTRFS_LIST_TOP_LEVEL,
	BTRFS_LIST_OTIME,
	BTRFS_LIST_PUUID,
	BTRFS_LIST_RUUID,
	BTRFS_LIST_UUID,
	BTRFS_LIST_PATH,
	BTRFS_LIST_ALL,
};

enum btrfs_list_filter_enum {
	BTRFS_LIST_FILTER_ROOTID,
	BTRFS_LIST_FILTER_SNAPSHOT_ONLY,
	BTRFS_LIST_FILTER_FLAGS,
	BTRFS_LIST_FILTER_GEN,
	BTRFS_LIST_FILTER_GEN_EQUAL	=	BTRFS_LIST_FILTER_GEN,
	BTRFS_LIST_FILTER_GEN_LESS,
	BTRFS_LIST_FILTER_GEN_MORE,
	BTRFS_LIST_FILTER_CGEN,
	BTRFS_LIST_FILTER_CGEN_EQUAL	=	BTRFS_LIST_FILTER_CGEN,
	BTRFS_LIST_FILTER_CGEN_LESS,
	BTRFS_LIST_FILTER_CGEN_MORE,
	BTRFS_LIST_FILTER_TOPID_EQUAL,
	BTRFS_LIST_FILTER_FULL_PATH,
	BTRFS_LIST_FILTER_BY_PARENT,
	BTRFS_LIST_FILTER_DELETED,
	BTRFS_LIST_FILTER_MAX,
};

enum btrfs_list_comp_enum {
	BTRFS_LIST_COMP_ROOTID,
	BTRFS_LIST_COMP_OGEN,
	BTRFS_LIST_COMP_GEN,
	BTRFS_LIST_COMP_PATH,
	BTRFS_LIST_COMP_MAX,
};

#define BTRFS_LIST_NFILTERS_INCREASE	(2 * BTRFS_LIST_FILTER_MAX)
#define BTRFS_LIST_NCOMPS_INCREASE	(2 * BTRFS_LIST_COMP_MAX)

struct listed_subvol {
	struct btrfs_util_subvolume_info info;
	char *path;
};

struct subvol_list {
	size_t num;
	struct listed_subvol subvols[];
};

typedef int (*btrfs_list_filter_func)(struct listed_subvol *, uint64_t);
typedef int (*btrfs_list_comp_func)(const struct listed_subvol *,
				    const struct listed_subvol *,
				    int);

struct btrfs_list_filter {
	btrfs_list_filter_func filter_func;
	u64 data;
};

struct btrfs_list_comparer {
	btrfs_list_comp_func comp_func;
	int is_descending;
};

struct btrfs_list_filter_set {
	int total;
	int nfilters;
	int only_deleted;
	struct btrfs_list_filter filters[0];
};

struct btrfs_list_comparer_set {
	int total;
	int ncomps;
	struct btrfs_list_comparer comps[0];
};

static struct {
	char	*name;
	char	*column_name;
	int	need_print;
} btrfs_list_columns[] = {
	{
		.name		= "ID",
		.column_name	= "ID",
		.need_print	= 0,
	},
	{
		.name		= "gen",
		.column_name	= "Gen",
		.need_print	= 0,
	},
	{
		.name		= "cgen",
		.column_name	= "CGen",
		.need_print	= 0,
	},
	{
		.name		= "parent",
		.column_name	= "Parent",
		.need_print	= 0,
	},
	{
		.name		= "top level",
		.column_name	= "Top Level",
		.need_print	= 0,
	},
	{
		.name		= "otime",
		.column_name	= "OTime",
		.need_print	= 0,
	},
	{
		.name		= "parent_uuid",
		.column_name	= "Parent UUID",
		.need_print	= 0,
	},
	{
		.name		= "received_uuid",
		.column_name	= "Received UUID",
		.need_print	= 0,
	},
	{
		.name		= "uuid",
		.column_name	= "UUID",
		.need_print	= 0,
	},
	{
		.name		= "path",
		.column_name	= "Path",
		.need_print	= 0,
	},
	{
		.name		= NULL,
		.column_name	= NULL,
		.need_print	= 0,
	},
};

static btrfs_list_filter_func all_filter_funcs[];
static btrfs_list_comp_func all_comp_funcs[];

static void btrfs_list_setup_print_column(enum btrfs_list_column_enum column)
{
	int i;

	ASSERT(0 <= column && column <= BTRFS_LIST_ALL);

	if (column < BTRFS_LIST_ALL) {
		btrfs_list_columns[column].need_print = 1;
		return;
	}

	for (i = 0; i < BTRFS_LIST_ALL; i++)
		btrfs_list_columns[i].need_print = 1;
}

static int comp_entry_with_rootid(const struct listed_subvol *entry1,
				  const struct listed_subvol *entry2,
				  int is_descending)
{
	int ret;

	if (entry1->info.id > entry2->info.id)
		ret = 1;
	else if (entry1->info.id < entry2->info.id)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_gen(const struct listed_subvol *entry1,
			       const struct listed_subvol *entry2,
			       int is_descending)
{
	int ret;

	if (entry1->info.generation > entry2->info.generation)
		ret = 1;
	else if (entry1->info.generation < entry2->info.generation)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_ogen(const struct listed_subvol *entry1,
				const struct listed_subvol *entry2,
				int is_descending)
{
	int ret;

	if (entry1->info.otransid > entry2->info.otransid)
		ret = 1;
	else if (entry1->info.otransid < entry2->info.otransid)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_path(const struct listed_subvol *entry1,
				const struct listed_subvol *entry2,
				int is_descending)
{
	int ret;

	if (strcmp(entry1->path, entry2->path) > 0)
		ret = 1;
	else if (strcmp(entry1->path, entry2->path) < 0)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static btrfs_list_comp_func all_comp_funcs[] = {
	[BTRFS_LIST_COMP_ROOTID]	= comp_entry_with_rootid,
	[BTRFS_LIST_COMP_OGEN]		= comp_entry_with_ogen,
	[BTRFS_LIST_COMP_GEN]		= comp_entry_with_gen,
	[BTRFS_LIST_COMP_PATH]		= comp_entry_with_path,
};

static char *all_sort_items[] = {
	[BTRFS_LIST_COMP_ROOTID]	= "rootid",
	[BTRFS_LIST_COMP_OGEN]		= "ogen",
	[BTRFS_LIST_COMP_GEN]		= "gen",
	[BTRFS_LIST_COMP_PATH]		= "path",
	[BTRFS_LIST_COMP_MAX]		= NULL,
};

static int btrfs_list_get_sort_item(char *sort_name)
{
	int i;

	for (i = 0; i < BTRFS_LIST_COMP_MAX; i++) {
		if (strcmp(sort_name, all_sort_items[i]) == 0)
			return i;
	}
	return -1;
}

static struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void)
{
	struct btrfs_list_comparer_set *set;
	int size;

	size = sizeof(struct btrfs_list_comparer_set) +
	       BTRFS_LIST_NCOMPS_INCREASE * sizeof(struct btrfs_list_comparer);
	set = calloc(1, size);
	if (!set) {
		fprintf(stderr, "memory allocation failed\n");
		exit(1);
	}

	set->total = BTRFS_LIST_NCOMPS_INCREASE;

	return set;
}

static int btrfs_list_setup_comparer(struct btrfs_list_comparer_set **comp_set,
				     enum btrfs_list_comp_enum comparer,
				     int is_descending)
{
	struct btrfs_list_comparer_set *set = *comp_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(comparer < BTRFS_LIST_COMP_MAX);
	ASSERT(set->ncomps <= set->total);

	if (set->ncomps == set->total) {
		void *tmp;

		size = set->total + BTRFS_LIST_NCOMPS_INCREASE;
		size = sizeof(*set) + size * sizeof(struct btrfs_list_comparer);
		tmp = set;
		set = realloc(set, size);
		if (!set) {
			fprintf(stderr, "memory allocation failed\n");
			free(tmp);
			exit(1);
		}

		memset(&set->comps[set->total], 0,
		       BTRFS_LIST_NCOMPS_INCREASE *
		       sizeof(struct btrfs_list_comparer));
		set->total += BTRFS_LIST_NCOMPS_INCREASE;
		*comp_set = set;
	}

	ASSERT(set->comps[set->ncomps].comp_func == NULL);

	set->comps[set->ncomps].comp_func = all_comp_funcs[comparer];
	set->comps[set->ncomps].is_descending = is_descending;
	set->ncomps++;
	return 0;
}

static int subvol_comp(const void *entry1, const void *entry2, void *arg)
{
	const struct btrfs_list_comparer_set * const set = arg;
	int rootid_compared = 0;
	int ret;
	int i;

	for (i = 0; set && i < set->ncomps; i++) {
		if (!set->comps[i].comp_func)
			break;

		ret = set->comps[i].comp_func(entry1, entry2,
					      set->comps[i].is_descending);
		if (ret)
			return ret;

		if (set->comps[i].comp_func == comp_entry_with_rootid)
			rootid_compared = 1;
	}

	if (!rootid_compared)
		return comp_entry_with_rootid(entry1, entry2, 0);

	return 0;
}

static void sort_subvols(struct btrfs_list_comparer_set *comp_set,
			 struct subvol_list *subvols)
{
	qsort_r(subvols->subvols, subvols->num, sizeof(subvols->subvols[0]),
		subvol_comp, comp_set);
}

static int filter_by_rootid(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.id == data;
}

static int filter_snapshot(struct listed_subvol *subvol, uint64_t data)
{
	return !uuid_is_null(subvol->info.parent_uuid);
}

static int filter_flags(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.flags & data;
}

static int filter_gen_more(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.generation >= data;
}

static int filter_gen_less(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.generation <= data;
}

static int filter_gen_equal(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.generation == data;
}

static int filter_cgen_more(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.otransid >= data;
}

static int filter_cgen_less(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.otransid <= data;
}

static int filter_cgen_equal(struct listed_subvol *subvol, uint64_t data)
{
	return subvol->info.otransid == data;
}

static int filter_topid_equal(struct listed_subvol *subvol, uint64_t data)
{
	/* See the comment in print_subvolume_column() about top level. */
	return subvol->info.parent_id == data;
}

static int filter_full_path(struct listed_subvol *subvol, uint64_t data)
{
	/*
	 * This implements the same behavior as before the conversion to
	 * libbtrfsutil, which is mostly nonsensical.
	 */
	if (subvol->info.parent_id != data) {
		char *tmp;
		int ret;

		ret = asprintf(&tmp, "<FS_TREE>/%s", subvol->path);
		if (ret == -1) {
			error("out of memory");
			exit(1);
		}

		free(subvol->path);
		subvol->path = tmp;
	}
	return 1;
}

static int filter_by_parent(struct listed_subvol *subvol, uint64_t data)
{
	return !uuid_compare(subvol->info.parent_uuid,
			     (u8 *)(unsigned long)data);
}

static btrfs_list_filter_func all_filter_funcs[] = {
	[BTRFS_LIST_FILTER_ROOTID]		= filter_by_rootid,
	[BTRFS_LIST_FILTER_SNAPSHOT_ONLY]	= filter_snapshot,
	[BTRFS_LIST_FILTER_FLAGS]		= filter_flags,
	[BTRFS_LIST_FILTER_GEN_MORE]		= filter_gen_more,
	[BTRFS_LIST_FILTER_GEN_LESS]		= filter_gen_less,
	[BTRFS_LIST_FILTER_GEN_EQUAL]           = filter_gen_equal,
	[BTRFS_LIST_FILTER_CGEN_MORE]		= filter_cgen_more,
	[BTRFS_LIST_FILTER_CGEN_LESS]		= filter_cgen_less,
	[BTRFS_LIST_FILTER_CGEN_EQUAL]          = filter_cgen_equal,
	[BTRFS_LIST_FILTER_TOPID_EQUAL]		= filter_topid_equal,
	[BTRFS_LIST_FILTER_FULL_PATH]		= filter_full_path,
	[BTRFS_LIST_FILTER_BY_PARENT]		= filter_by_parent,
};

static struct btrfs_list_filter_set *btrfs_list_alloc_filter_set(void)
{
	struct btrfs_list_filter_set *set;
	int size;

	size = sizeof(struct btrfs_list_filter_set) +
	       BTRFS_LIST_NFILTERS_INCREASE * sizeof(struct btrfs_list_filter);
	set = calloc(1, size);
	if (!set) {
		fprintf(stderr, "memory allocation failed\n");
		exit(1);
	}

	set->total = BTRFS_LIST_NFILTERS_INCREASE;

	return set;
}

/*
 * Setup list filters. Exit if there's not enough memory, as we can't continue
 * without the structures set up properly.
 */
static void btrfs_list_setup_filter(struct btrfs_list_filter_set **filter_set,
				    enum btrfs_list_filter_enum filter,
				    u64 data)
{
	struct btrfs_list_filter_set *set = *filter_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(filter < BTRFS_LIST_FILTER_MAX);
	ASSERT(set->nfilters <= set->total);

	if (set->nfilters == set->total) {
		void *tmp;

		size = set->total + BTRFS_LIST_NFILTERS_INCREASE;
		size = sizeof(*set) + size * sizeof(struct btrfs_list_filter);
		tmp = set;
		set = realloc(set, size);
		if (!set) {
			fprintf(stderr, "memory allocation failed\n");
			free(tmp);
			exit(1);
		}

		memset(&set->filters[set->total], 0,
		       BTRFS_LIST_NFILTERS_INCREASE *
		       sizeof(struct btrfs_list_filter));
		set->total += BTRFS_LIST_NFILTERS_INCREASE;
		*filter_set = set;
	}

	ASSERT(set->filters[set->nfilters].filter_func == NULL);

	if (filter == BTRFS_LIST_FILTER_DELETED) {
		set->only_deleted = 1;
	} else {
		set->filters[set->nfilters].filter_func = all_filter_funcs[filter];
		set->filters[set->nfilters].data = data;
		set->nfilters++;
	}
}

static int filters_match(struct listed_subvol *subvol,
			 struct btrfs_list_filter_set *set)
{
	int i, ret;

	if (!set)
		return 1;

	for (i = 0; i < set->nfilters; i++) {
		if (!set->filters[i].filter_func)
			break;
		ret = set->filters[i].filter_func(subvol, set->filters[i].data);
		if (!ret)
			return 0;
	}
	return 1;
}

static void print_subvolume_column(struct listed_subvol *subvol,
				   enum btrfs_list_column_enum column)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	ASSERT(0 <= column && column < BTRFS_LIST_ALL);

	switch (column) {
	case BTRFS_LIST_OBJECTID:
		printf("%" PRIu64, subvol->info.id);
		break;
	case BTRFS_LIST_GENERATION:
		printf("%" PRIu64, subvol->info.generation);
		break;
	case BTRFS_LIST_OGENERATION:
		printf("%" PRIu64, subvol->info.otransid);
		break;
	case BTRFS_LIST_PARENT:
	/*
	 * Top level used to mean something else, but since 4f5ebb3ef553
	 * ("Btrfs-progs: fix to make list specified directory's subvolumes
	 * work") it was always set to the parent ID. See
	 * https://www.spinics.net/lists/linux-btrfs/msg69820.html.
	 */
	case BTRFS_LIST_TOP_LEVEL:
		printf("%" PRIu64, subvol->info.parent_id);
		break;
	case BTRFS_LIST_OTIME:
		if (subvol->info.otime.tv_sec) {
			struct tm tm;

			localtime_r(&subvol->info.otime.tv_sec, &tm);
			strftime(tstr, sizeof(tstr), "%Y-%m-%d %X", &tm);
		} else
			strcpy(tstr, "-");
		printf("%s", tstr);
		break;
	case BTRFS_LIST_UUID:
		if (uuid_is_null(subvol->info.uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subvol->info.uuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_PUUID:
		if (uuid_is_null(subvol->info.parent_uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subvol->info.parent_uuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_RUUID:
		if (uuid_is_null(subvol->info.received_uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subvol->info.received_uuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_PATH:
		printf("%s", subvol->path);
		break;
	default:
		break;
	}
}

static void print_one_subvol_info_raw(struct listed_subvol *subvol,
				      const char *raw_prefix)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		if (raw_prefix)
			printf("%s",raw_prefix);

		print_subvolume_column(subvol, i);
	}
	printf("\n");
}

static void print_one_subvol_info_table(struct listed_subvol *subvol)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		print_subvolume_column(subvol, i);

		if (i != BTRFS_LIST_PATH)
			printf("\t");

		if (i == BTRFS_LIST_TOP_LEVEL)
			printf("\t");
	}
	printf("\n");
}

static void print_one_subvol_info_default(struct listed_subvol *subvol)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		printf("%s ", btrfs_list_columns[i].name);
		print_subvolume_column(subvol, i);

		if (i != BTRFS_LIST_PATH)
			printf(" ");
	}
	printf("\n");
}

static void print_all_subvol_info_tab_head(void)
{
	int i;
	int len;
	char barrier[20];

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (btrfs_list_columns[i].need_print)
			printf("%s\t", btrfs_list_columns[i].name);

		if (i == BTRFS_LIST_ALL-1)
			printf("\n");
	}

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		memset(barrier, 0, sizeof(barrier));

		if (btrfs_list_columns[i].need_print) {
			len = strlen(btrfs_list_columns[i].name);
			while (len--)
				strcat(barrier, "-");

			printf("%s\t", barrier);
		}
		if (i == BTRFS_LIST_ALL-1)
			printf("\n");
	}
}

static void print_all_subvol_info(struct subvol_list *subvols,
				  enum btrfs_list_layout layout,
				  const char *raw_prefix)
{
	size_t i;

	if (layout == BTRFS_LIST_LAYOUT_TABLE)
		print_all_subvol_info_tab_head();

	for (i = 0; i < subvols->num; i++) {
		struct listed_subvol *subvol = &subvols->subvols[i];

		switch (layout) {
		case BTRFS_LIST_LAYOUT_DEFAULT:
			print_one_subvol_info_default(subvol);
			break;
		case BTRFS_LIST_LAYOUT_TABLE:
			print_one_subvol_info_table(subvol);
			break;
		case BTRFS_LIST_LAYOUT_RAW:
			print_one_subvol_info_raw(subvol, raw_prefix);
			break;
		}
	}
}

static void free_subvol_list(struct subvol_list *subvols)
{
	size_t i;

	if (subvols) {
		for (i = 0; i < subvols->num; i++)
			free(subvols->subvols[i].path);
		free(subvols);
	}
}

static struct subvol_list *btrfs_list_deleted_subvols(int fd,
						      struct btrfs_list_filter_set *filter_set)
{
	struct subvol_list *subvols = NULL;
	uint64_t *ids = NULL;
	size_t i, n;
	enum btrfs_util_error err;
	int ret = -1;

	err = btrfs_util_deleted_subvolumes_fd(fd, &ids, &n);
	if (err) {
		error_btrfs_util(err);
		return NULL;
	}

	subvols = malloc(sizeof(*subvols) + n * sizeof(subvols->subvols[0]));
	if (!subvols) {
		error("out of memory");
		goto out;
	}

	subvols->num = 0;
	for (i = 0; i < n; i++) {
		struct listed_subvol *subvol = &subvols->subvols[subvols->num];

		err = btrfs_util_subvolume_info_fd(fd, ids[i], &subvol->info);
		if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
			/*
			 * The subvolume might have been cleaned up since it was
			 * returned.
			 */
			continue;
		} else if (err) {
			error_btrfs_util(err);
			goto out;
		}

		subvol->path = strdup("DELETED");
		if (!subvol->path)
			goto out;

		if (!filters_match(subvol, filter_set)) {
			free(subvol->path);
			continue;
		}

		subvols->num++;
	}

	ret = 0;
out:
	if (ret) {
		free_subvol_list(subvols);
		subvols = NULL;
	}
	free(ids);
	return subvols;
}

static struct subvol_list *btrfs_list_subvols(int fd,
					      struct btrfs_list_filter_set *filter_set)
{
	struct subvol_list *subvols;
	size_t capacity = 0;
	struct btrfs_util_subvolume_iterator *iter;
	enum btrfs_util_error err;
	int ret = -1;

	subvols = malloc(sizeof(*subvols));
	if (!subvols) {
		error("out of memory");
		return NULL;
	}
	subvols->num = 0;

	err = btrfs_util_create_subvolume_iterator_fd(fd,
						      BTRFS_FS_TREE_OBJECTID, 0,
						      &iter);
	if (err) {
		iter = NULL;
		error_btrfs_util(err);
		goto out;
	}

	for (;;) {
		struct listed_subvol subvol;

		err = btrfs_util_subvolume_iterator_next_info(iter,
							      &subvol.path,
							      &subvol.info);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			break;
		} else if (err) {
			error_btrfs_util(err);
			goto out;
		}

		if (!filters_match(&subvol, filter_set)) {
			free(subvol.path);
			continue;
		}

		if (subvols->num >= capacity) {
			struct subvol_list *new_subvols;
			size_t new_capacity = max_t(size_t, 1, capacity * 2);

			new_subvols = realloc(subvols,
					      sizeof(*new_subvols) +
					      new_capacity *
					      sizeof(new_subvols->subvols[0]));
			if (!new_subvols) {
				error("out of memory");
				goto out;
			}

			subvols = new_subvols;
			capacity = new_capacity;
		}

		subvols->subvols[subvols->num] = subvol;
		subvols->num++;
	}

	ret = 0;
out:
	if (iter)
		btrfs_util_destroy_subvolume_iterator(iter);
	if (ret)
		free_subvol_list(subvols);
	return subvols;
}

static int btrfs_list_subvols_print(int fd,
				    struct btrfs_list_filter_set *filter_set,
				    struct btrfs_list_comparer_set *comp_set,
				    enum btrfs_list_layout layout,
				    int full_path, const char *raw_prefix)
{
	struct subvol_list *subvols;

	/*
	 * full_path hasn't done anything since 4f5ebb3ef553 ("Btrfs-progs: fix
	 * to make list specified directory's subvolumes work"). See
	 * https://www.spinics.net/lists/linux-btrfs/msg69820.html
	 */

	if (filter_set->only_deleted)
		subvols = btrfs_list_deleted_subvols(fd, filter_set);
	else
		subvols = btrfs_list_subvols(fd, filter_set);
	if (!subvols)
		return -1;

	sort_subvols(comp_set, subvols);

	print_all_subvol_info(subvols, layout, raw_prefix);

	free_subvol_list(subvols);

	return 0;
}

static int btrfs_list_parse_sort_string(char *opt_arg,
					struct btrfs_list_comparer_set **comps)
{
	int order;
	int flag;
	char *p;
	char **ptr_argv;
	int what_to_sort;

	while ((p = strtok(opt_arg, ",")) != NULL) {
		flag = 0;
		ptr_argv = all_sort_items;

		while (*ptr_argv) {
			if (strcmp(*ptr_argv, p) == 0) {
				flag = 1;
				break;
			} else {
				p++;
				if (strcmp(*ptr_argv, p) == 0) {
					flag = 1;
					p--;
					break;
				}
				p--;
			}
			ptr_argv++;
		}

		if (flag == 0)
			return -1;

		else {
			if (*p == '+') {
				order = 0;
				p++;
			} else if (*p == '-') {
				order = 1;
				p++;
			} else
				order = 0;

			what_to_sort = btrfs_list_get_sort_item(p);
			btrfs_list_setup_comparer(comps, what_to_sort, order);
		}
		opt_arg = NULL;
	}

	return 0;
}

static int btrfs_list_parse_filter_string(char *opt_arg,
					  struct btrfs_list_filter_set **filters,
					  enum btrfs_list_filter_enum type)
{

	u64 arg;

	switch (*(opt_arg++)) {
	case '+':
		arg = arg_strtou64(opt_arg);
		type += 2;

		btrfs_list_setup_filter(filters, type, arg);
		break;
	case '-':
		arg = arg_strtou64(opt_arg);
		type += 1;

		btrfs_list_setup_filter(filters, type, arg);
		break;
	default:
		opt_arg--;
		arg = arg_strtou64(opt_arg);

		btrfs_list_setup_filter(filters, type, arg);
		break;
	}

	return 0;
}

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [options] <path>",
	"List subvolumes and snapshots in the filesystem.",
	"",
	"Path filtering:",
	"-o           print only subvolumes below specified path",
	"-a           print all the subvolumes in the filesystem and",
	"             distinguish absolute and relative path with respect",
	"             to the given <path>",
	"",
	"Field selection:",
	"-p           print parent ID",
	"-c           print the ogeneration of the subvolume",
	"-g           print the generation of the subvolume",
	"-u           print the uuid of subvolumes (and snapshots)",
	"-q           print the parent uuid of the snapshots",
	"-R           print the uuid of the received snapshots",
	"",
	"Type filtering:",
	"-s           list only snapshots",
	"-r           list readonly subvolumes (including snapshots)",
	"-d           list deleted subvolumes that are not yet cleaned",
	"",
	"Other:",
	"-t           print the result as a table",
	"",
	"Sorting:",
	"-G [+|-]value",
	"             filter the subvolumes by generation",
	"             (+value: >= value; -value: <= value; value: = value)",
	"-C [+|-]value",
	"             filter the subvolumes by ogeneration",
	"             (+value: >= value; -value: <= value; value: = value)",
	"--sort=gen,ogen,rootid,path",
	"             list the subvolume in order of gen, ogen, rootid or path",
	"             you also can add '+' or '-' in front of each items.",
	"             (+:ascending, -:descending, ascending default)",
	NULL,
};

static int cmd_subvol_list(int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	int ret = -1, uerr = 0;
	char *subvol;
	int is_list_all = 0;
	int is_only_in_path = 0;
	DIR *dirstream = NULL;
	enum btrfs_list_layout layout = BTRFS_LIST_LAYOUT_DEFAULT;

	filter_set = btrfs_list_alloc_filter_set();
	comparer_set = btrfs_list_alloc_comparer_set();

	optind = 0;
	while(1) {
		int c;
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, 'S'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv,
				    "acdgopqsurRG:C:t", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'p':
			btrfs_list_setup_print_column(BTRFS_LIST_PARENT);
			break;
		case 'a':
			is_list_all = 1;
			break;
		case 'c':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			break;
		case 'd':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_DELETED,
						0);
			break;
		case 'g':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			break;
		case 'o':
			is_only_in_path = 1;
			break;
		case 't':
			layout = BTRFS_LIST_LAYOUT_TABLE;
			break;
		case 's':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_SNAPSHOT_ONLY,
						0);
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			btrfs_list_setup_print_column(BTRFS_LIST_OTIME);
			break;
		case 'u':
			btrfs_list_setup_print_column(BTRFS_LIST_UUID);
			break;
		case 'q':
			btrfs_list_setup_print_column(BTRFS_LIST_PUUID);
			break;
		case 'R':
			btrfs_list_setup_print_column(BTRFS_LIST_RUUID);
			break;
		case 'r':
			flags |= BTRFS_ROOT_SUBVOL_RDONLY;
			break;
		case 'G':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_GEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		case 'C':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_CGEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;
		case 'S':
			ret = btrfs_list_parse_sort_string(optarg,
							   &comparer_set);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		default:
			uerr = 1;
			goto out;
		}
	}

	if (check_argc_exact(argc - optind, 1)) {
		uerr = 1;
		goto out;
	}

	subvol = argv[optind];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0) {
		ret = -1;
		error("can't access '%s'", subvol);
		goto out;
	}

	if (flags)
		btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_FLAGS,
					flags);

	ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret)
		goto out;

	if (is_list_all)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_FULL_PATH,
					top_id);
	else if (is_only_in_path)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_TOPID_EQUAL,
					top_id);

	/* by default we shall print the following columns*/
	btrfs_list_setup_print_column(BTRFS_LIST_OBJECTID);
	btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
	btrfs_list_setup_print_column(BTRFS_LIST_TOP_LEVEL);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
			layout, !is_list_all && !is_only_in_path, NULL);

out:
	close_file_or_dir(fd, dirstream);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd_subvol_list_usage);
	return !!ret;
}

static const char * const cmd_subvol_snapshot_usage[] = {
	"btrfs subvolume snapshot [-r|-R] [-i <qgroupid>] <source> <dest>|[<dest>/]<name>",
	"Create a snapshot of the subvolume",
	"Create a writable/readonly snapshot of the subvolume <source> with",
	"the name <name> in the <dest> directory.  If only <dest> is given,",
	"the subvolume will be named the basename of <source>.",
	"",
	"-r             create a readonly snapshot",
	"-R             recursively snapshot subvolumes beneath the source. This",
	"               option cannot be combined with -r.",
	"-i <qgroupid>  add the newly created snapshot to a qgroup. This",
	"               option can be given multiple times.",
	NULL
};

static int cmd_subvol_snapshot(int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, retval;
	int	fd = -1, fddst = -1;
	char	*dupname = NULL;
	char	*dupdir = NULL;
	char	*newname;
	char	*dstdir;
	enum btrfs_util_error err;
	struct btrfs_util_qgroup_inherit *inherit = NULL;
	DIR *dirstream1 = NULL, *dirstream2 = NULL;
	int flags = 0;

	retval = 1;	/* failure */
	while (1) {
		int c = getopt(argc, argv, "i:rR");
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			if (qgroup_inherit_add_group(&inherit, optarg) == -1)
				goto out;
			break;
		case 'r':
			flags |= BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY;
			break;
		case 'R':
			flags |= BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE;
			break;
		default:
			usage(cmd_subvol_snapshot_usage);
		}
	}

	if ((flags & BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY) &&
	    (flags & BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE)) {
		error("-r and -R cannot be combined");
		return 1;
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_subvol_snapshot_usage);

	subvol = argv[optind];
	dst = argv[optind + 1];

	err = btrfs_util_is_subvolume(subvol);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	res = test_isdir(dst);
	if (res < 0 && res != -ENOENT) {
		errno = -res;
		error("cannot access %s: %m", dst);
		goto out;
	}
	if (res == 0) {
		error("'%s' exists and it is not a directory", dst);
		goto out;
	}

	if (res > 0) {
		dupname = strdup(subvol);
		newname = basename(dupname);
		dstdir = dst;
	} else {
		dupname = strdup(dst);
		newname = basename(dupname);
		dupdir = strdup(dst);
		dstdir = dirname(dupdir);
	}

	fddst = btrfs_open_dir(dstdir, &dirstream1, 1);
	if (fddst < 0)
		goto out;

	fd = btrfs_open_dir(subvol, &dirstream2, 1);
	if (fd < 0)
		goto out;

	if (flags & BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY) {
		printf("Create a readonly snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	} else {
		printf("Create a snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	}

	err = btrfs_util_create_snapshot_fd2(fd, fddst, newname, flags, NULL,
					     inherit);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	retval = 0;	/* success */

out:
	close_file_or_dir(fddst, dirstream1);
	close_file_or_dir(fd, dirstream2);
	if (inherit)
		btrfs_util_destroy_qgroup_inherit(inherit);
	free(dupname);
	free(dupdir);

	return retval;
}

static const char * const cmd_subvol_get_default_usage[] = {
	"btrfs subvolume get-default <path>",
	"Get the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_get_default(int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
	uint64_t default_id;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;
	struct btrfs_util_subvolume_info subvol;
	char *path;

	clean_args_no_options(argc, argv, cmd_subvol_get_default_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_get_default_usage);

	fd = btrfs_open_dir(argv[1], &dirstream, 1);
	if (fd < 0)
		return 1;

	err = btrfs_util_get_default_subvolume_fd(fd, &default_id);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	/* no need to resolve roots if FS_TREE is default */
	if (default_id == BTRFS_FS_TREE_OBJECTID) {
		printf("ID 5 (FS_TREE)\n");
		ret = 0;
		goto out;
	}

	err = btrfs_util_subvolume_info_fd(fd, default_id, &subvol);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	err = btrfs_util_subvolume_path_fd(fd, default_id, &path);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	printf("ID %" PRIu64 " gen %" PRIu64 " top level %" PRIu64 " path %s\n",
	       subvol.id, subvol.generation, subvol.parent_id, path);

	free(path);

	ret = 0;
out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_subvol_set_default_usage[] = {
	"btrfs subvolume set-default <subvolume>\n"
	"btrfs subvolume set-default <subvolid> <path>",
	"Set the default subvolume of the filesystem mounted as default.",
	"The subvolume can be specified by its path,",
	"or the pair of subvolume id and path to the filesystem.",
	NULL
};

static int cmd_subvol_set_default(int argc, char **argv)
{
	u64 objectid;
	char *path;
	enum btrfs_util_error err;

	clean_args_no_options(argc, argv, cmd_subvol_set_default_usage);

	if (check_argc_min(argc - optind, 1) ||
			check_argc_max(argc - optind, 2))
		usage(cmd_subvol_set_default_usage);

	if (argc - optind == 1) {
		/* path to the subvolume is specified */
		objectid = 0;
		path = argv[optind];
	} else {
		/* subvol id and path to the filesystem are specified */
		objectid = arg_strtou64(argv[optind]);
		path = argv[optind + 1];
	}

	err = btrfs_util_set_default_subvolume(path, objectid);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}
	return 0;
}

static const char * const cmd_subvol_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

static int cmd_subvol_find_new(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;

	clean_args_no_options(argc, argv, cmd_subvol_find_new_usage);

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_subvol_find_new_usage);

	subvol = argv[optind];
	last_gen = arg_strtou64(argv[optind + 1]);

	err = btrfs_util_is_subvolume(subvol);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}

	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0)
		return 1;

	err = btrfs_util_sync_fd(fd);
	if (err) {
		error_btrfs_util(err);
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	ret = btrfs_list_find_updated_files(fd, 0, last_gen);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}

static const char * const cmd_subvol_show_usage[] = {
	"btrfs subvolume show [options] <subvol-path>|<mnt>",
	"Show more information about the subvolume",
	"-r|--rootid   rootid of the subvolume",
	"-u|--uuid     uuid of the subvolume",
	"",
	"If no option is specified, <subvol-path> will be shown, otherwise",
	"the rootid or uuid are resolved relative to the <mnt> path.",
	NULL
};

static int cmd_subvol_show(int argc, char **argv)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];
	char *fullpath = NULL;
	int fd = -1;
	int ret = 1;
	DIR *dirstream1 = NULL;
	int by_rootid = 0;
	int by_uuid = 0;
	u64 rootid_arg = 0;
	u8 uuid_arg[BTRFS_UUID_SIZE];
	struct btrfs_util_subvolume_iterator *iter;
	struct btrfs_util_subvolume_info subvol;
	char *subvol_path = NULL;
	enum btrfs_util_error err;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "rootid", required_argument, NULL, 'r'},
			{ "uuid", required_argument, NULL, 'u'},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "r:u:", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'r':
			rootid_arg = arg_strtou64(optarg);
			by_rootid = 1;
			break;
		case 'u':
			uuid_parse(optarg, uuid_arg);
			by_uuid = 1;
			break;
		default:
			usage(cmd_subvol_show_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_show_usage);

	if (by_rootid && by_uuid) {
		error(
		"options --rootid and --uuid cannot be used at the same time");
		usage(cmd_subvol_show_usage);
	}

	fullpath = realpath(argv[optind], NULL);
	if (!fullpath) {
		error("cannot find real path for '%s': %m", argv[optind]);
		goto out;
	}

	fd = open_file_or_dir(fullpath, &dirstream1);
	if (fd < 0) {
		error("can't access '%s'", fullpath);
		goto out;
	}

	if (by_uuid) {
		err = btrfs_util_create_subvolume_iterator_fd(fd,
							      BTRFS_FS_TREE_OBJECTID,
							      0, &iter);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		for (;;) {
			err = btrfs_util_subvolume_iterator_next_info(iter,
								      &subvol_path,
								      &subvol);
			if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
				uuid_unparse(uuid_arg, uuidparse);
				error("can't find uuid '%s' on '%s'", uuidparse,
				      fullpath);
				btrfs_util_destroy_subvolume_iterator(iter);
				goto out;
			} else if (err) {
				error_btrfs_util(err);
				btrfs_util_destroy_subvolume_iterator(iter);
				goto out;
			}

			if (uuid_compare(subvol.uuid, uuid_arg) == 0)
				break;

			free(subvol_path);
		}
		btrfs_util_destroy_subvolume_iterator(iter);
	} else {
		/*
		 * If !by_rootid, rootid_arg = 0, which means find the
		 * subvolume ID of the fd and use that.
		 */
		err = btrfs_util_subvolume_info_fd(fd, rootid_arg, &subvol);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		err = btrfs_util_subvolume_path_fd(fd, subvol.id, &subvol_path);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

	}

	/* print the info */
	printf("%s\n", subvol.id == BTRFS_FS_TREE_OBJECTID ? "/" : subvol_path);
	printf("\tName: \t\t\t%s\n",
	       (subvol.id == BTRFS_FS_TREE_OBJECTID ? "<FS_TREE>" :
		basename(subvol_path)));

	if (uuid_is_null(subvol.uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.uuid, uuidparse);
	printf("\tUUID: \t\t\t%s\n", uuidparse);

	if (uuid_is_null(subvol.parent_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.parent_uuid, uuidparse);
	printf("\tParent UUID: \t\t%s\n", uuidparse);

	if (uuid_is_null(subvol.received_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.received_uuid, uuidparse);
	printf("\tReceived UUID: \t\t%s\n", uuidparse);

	if (subvol.otime.tv_sec) {
		struct tm tm;

		localtime_r(&subvol.otime.tv_sec, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else
		strcpy(tstr, "-");
	printf("\tCreation time: \t\t%s\n", tstr);

	printf("\tSubvolume ID: \t\t%" PRIu64 "\n", subvol.id);
	printf("\tGeneration: \t\t%" PRIu64 "\n", subvol.generation);
	printf("\tGen at creation: \t%" PRIu64 "\n", subvol.otransid);
	printf("\tParent ID: \t\t%" PRIu64 "\n", subvol.parent_id);
	printf("\tTop level ID: \t\t%" PRIu64 "\n", subvol.parent_id);

	if (subvol.flags & BTRFS_ROOT_SUBVOL_RDONLY)
		printf("\tFlags: \t\t\treadonly\n");
	else
		printf("\tFlags: \t\t\t-\n");

	/* print the snapshots of the given subvol if any*/
	printf("\tSnapshot(s):\n");

	err = btrfs_util_create_subvolume_iterator_fd(fd,
						      BTRFS_FS_TREE_OBJECTID, 0,
						      &iter);

	for (;;) {
		struct btrfs_util_subvolume_info subvol2;
		char *path;

		err = btrfs_util_subvolume_iterator_next_info(iter, &path, &subvol2);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			break;
		} else if (err) {
			error_btrfs_util(err);
			btrfs_util_destroy_subvolume_iterator(iter);
			goto out;
		}

		if (uuid_compare(subvol2.parent_uuid, subvol.uuid) == 0)
			printf("\t\t\t\t%s\n", path);

		free(path);
	}
	btrfs_util_destroy_subvolume_iterator(iter);

	ret = 0;
out:
	free(subvol_path);
	close_file_or_dir(fd, dirstream1);
	free(fullpath);
	return !!ret;
}

static const char * const cmd_subvol_sync_usage[] = {
	"btrfs subvolume sync <path> [<subvol-id>...]",
	"Wait until given subvolume(s) are completely removed from the filesystem.",
	"Wait until given subvolume(s) are completely removed from the filesystem",
	"after deletion.",
	"If no subvolume id is given, wait until all current deletion requests",
	"are completed, but do not wait for subvolumes deleted meanwhile.",
	"The status of subvolume ids is checked periodically.",
	"",
	"-s <N>       sleep N seconds between checks (default: 1)",
	NULL
};

static int cmd_subvol_sync(int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
	DIR *dirstream = NULL;
	uint64_t *ids = NULL;
	size_t id_count, i;
	int sleep_interval = 1;
	enum btrfs_util_error err;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "s:");

		if (c < 0)
			break;

		switch (c) {
		case 's':
			sleep_interval = atoi(optarg);
			if (sleep_interval < 1) {
				error("invalid sleep interval %s", optarg);
				ret = 1;
				goto out;
			}
			break;
		default:
			usage(cmd_subvol_sync_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_subvol_sync_usage);

	fd = btrfs_open_dir(argv[optind], &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}
	optind++;

	id_count = argc - optind;
	if (!id_count) {
		err = btrfs_util_deleted_subvolumes_fd(fd, &ids, &id_count);
		if (err) {
			error_btrfs_util(err);
			ret = 1;
			goto out;
		}
		if (id_count == 0) {
			ret = 0;
			goto out;
		}
	} else {
		ids = malloc(id_count * sizeof(uint64_t));
		if (!ids) {
			error("not enough memory");
			ret = 1;
			goto out;
		}

		for (i = 0; i < id_count; i++) {
			u64 id;
			const char *arg;

			arg = argv[optind + i];
			errno = 0;
			id = strtoull(arg, NULL, 10);
			if (errno) {
				error("unrecognized subvolume id %s", arg);
				ret = 1;
				goto out;
			}
			if (id < BTRFS_FIRST_FREE_OBJECTID ||
			    id > BTRFS_LAST_FREE_OBJECTID) {
				error("subvolume id %s out of range", arg);
				ret = 1;
				goto out;
			}
			ids[i] = id;
		}
	}

	ret = wait_for_subvolume_cleaning(fd, id_count, ids, sleep_interval);

out:
	free(ids);
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

static const char subvolume_cmd_group_info[] =
"manage subvolumes: create, delete, list, etc";

const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, subvolume_cmd_group_info, {
		{ "create", cmd_subvol_create, cmd_subvol_create_usage, NULL, 0 },
		{ "delete", cmd_subvol_delete, cmd_subvol_delete_usage, NULL, 0 },
		{ "list", cmd_subvol_list, cmd_subvol_list_usage, NULL, 0 },
		{ "snapshot", cmd_subvol_snapshot, cmd_subvol_snapshot_usage,
			NULL, 0 },
		{ "get-default", cmd_subvol_get_default,
			cmd_subvol_get_default_usage, NULL, 0 },
		{ "set-default", cmd_subvol_set_default,
			cmd_subvol_set_default_usage, NULL, 0 },
		{ "find-new", cmd_subvol_find_new, cmd_subvol_find_new_usage,
			NULL, 0 },
		{ "show", cmd_subvol_show, cmd_subvol_show_usage, NULL, 0 },
		{ "sync", cmd_subvol_sync, cmd_subvol_sync_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_subvolume(int argc, char **argv)
{
	return handle_command_group(&subvolume_cmd_group, argc, argv);
}
