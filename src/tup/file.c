#define _ATFILE_SOURCE
#include "file.h"
#include "access_event.h"
#include "debug.h"
#include "db.h"
#include "fileio.h"
#include "config.h"
#include "entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct dfd_info {
	tupid_t dt;
	int dfd;
};

static struct file_entry *new_entry(const char *filename, tupid_t dt);
static void del_entry(struct file_entry *fent);
static void check_unlink_list(const struct pel_group *pg,
			      struct file_entry_head *u_head);
static void handle_unlink(struct file_info *info);
static int update_write_info(FILE *f, tupid_t cmdid, struct file_info *info,
			     int *warnings, struct tup_entry_head *entryhead);
static int update_read_info(FILE *f, tupid_t cmdid, struct file_info *info,
			    struct tup_entry_head *entryhead);
static int add_parser_files_locked(struct file_info *finfo,
				   struct tupid_entries *root);

int init_file_info(struct file_info *info)
{
	LIST_INIT(&info->read_list);
	LIST_INIT(&info->write_list);
	LIST_INIT(&info->unlink_list);
	LIST_INIT(&info->var_list);
	LIST_INIT(&info->mapping_list);
	LIST_INIT(&info->tmpdir_list);
	pthread_mutex_init(&info->lock, NULL);
	info->server_fail = 0;
	return 0;
}

void finfo_lock(struct file_info *info)
{
	pthread_mutex_lock(&info->lock);
}

void finfo_unlock(struct file_info *info)
{
	pthread_mutex_unlock(&info->lock);
}

int handle_file(enum access_type at, const char *filename, const char *file2,
		struct file_info *info, tupid_t dt)
{
	DEBUGP("received file '%s' in mode %i\n", filename, at);
	int rc;

	finfo_lock(info);
	if(at == ACCESS_RENAME) {
		rc = handle_rename(filename, file2, info);
	} else {
		rc = handle_open_file(at, filename, info, dt);
	}
	finfo_unlock(info);

	return rc;
}

int handle_open_file(enum access_type at, const char *filename,
		     struct file_info *info, tupid_t dt)
{
	struct file_entry *fent;
	int rc = 0;

	fent = new_entry(filename, dt);
	if(!fent) {
		return -1;
	}

	switch(at) {
		case ACCESS_READ:
			LIST_INSERT_HEAD(&info->read_list, fent, list);
			break;
		case ACCESS_WRITE:
			check_unlink_list(&fent->pg, &info->unlink_list);
			LIST_INSERT_HEAD(&info->write_list, fent, list);
			break;
		case ACCESS_UNLINK:
			LIST_INSERT_HEAD(&info->unlink_list, fent, list);
			break;
		case ACCESS_VAR:
			LIST_INSERT_HEAD(&info->var_list, fent, list);
			break;
		default:
			fprintf(stderr, "Invalid event type: %i\n", at);
			rc = -1;
			break;
	}

	return rc;
}

int write_files(FILE *f, tupid_t cmdid, struct file_info *info, int *warnings,
		int check_only)
{
	struct tup_entry_head *entrylist;
	struct tmpdir *tmpdir;
	int tmpdir_bork = 0;
	int rc1 = 0, rc2;

	finfo_lock(info);
	handle_unlink(info);

	if(!check_only) {
		LIST_FOREACH(tmpdir, &info->tmpdir_list, list) {
			fprintf(f, "tup error: Directory '%s' was created, but not subsequently removed. Only temporary directories can be created by commands.\n", tmpdir->dirname);
			tmpdir_bork = 1;
		}
		if(tmpdir_bork) {
			finfo_unlock(info);
			return -1;
		}

		entrylist = tup_entry_get_list();
		rc1 = update_write_info(f, cmdid, info, warnings, entrylist);
		tup_entry_release_list();
	}

	entrylist = tup_entry_get_list();
	rc2 = update_read_info(f, cmdid, info, entrylist);
	tup_entry_release_list();
	finfo_unlock(info);

	if(rc1 == 0 && rc2 == 0)
		return 0;
	return -1;
}

int add_parser_files(struct file_info *finfo, struct tupid_entries *root)
{
	int rc;
	finfo_lock(finfo);
	rc = add_parser_files_locked(finfo, root);
	finfo_unlock(finfo);
	return rc;
}

static int add_node_to_list(tupid_t dt, struct pel_group *pg,
			    struct tup_entry_head *head)
{
	tupid_t new_dt;
	struct path_element *pel = NULL;
	struct tup_entry *tent;

	new_dt = find_dir_tupid_dt_pg(dt, pg, &pel, 1);
	if(new_dt < 0)
		return -1;
	if(new_dt == 0) {
		return 0;
	}
	if(pel == NULL) {
		/* This can happen for the '.' entry */
		return 0;
	}

	if(tup_db_select_tent_part(new_dt, pel->path, pel->len, &tent) < 0)
		return -1;
	if(!tent) {
		if(tup_db_node_insert_tent(new_dt, pel->path, pel->len, TUP_NODE_GHOST, -1, &tent) < 0) {
			fprintf(stderr, "Error: Node '%.*s' doesn't exist in directory %lli, and no luck creating a ghost node there.\n", pel->len, pel->path, new_dt);
			return -1;
		}
	}
	free(pel);

	tup_entry_list_add(tent, head);

	return 0;
}

static int add_parser_files_locked(struct file_info *finfo,
				   struct tupid_entries *root)
{
	struct file_entry *r;
	struct mapping *map;
	struct tup_entry_head *entrylist;
	struct tup_entry *tent;
	int map_bork = 0;

	entrylist = tup_entry_get_list();
	while(!LIST_EMPTY(&finfo->read_list)) {
		r = LIST_FIRST(&finfo->read_list);
		if(r->dt > 0) {
			if(add_node_to_list(r->dt, &r->pg, entrylist) < 0)
				return -1;
		}
		del_entry(r);
	}
	while(!LIST_EMPTY(&finfo->var_list)) {
		r = LIST_FIRST(&finfo->var_list);

		if(add_node_to_list(VAR_DT, &r->pg, entrylist) < 0)
			return -1;
		del_entry(r);
	}
	LIST_FOREACH(tent, entrylist, list) {
		if(strcmp(tent->name.s, ".gitignore") != 0)
			if(tupid_tree_add_dup(root, tent->tnode.tupid) < 0)
				return -1;
	}
	tup_entry_release_list();

	/* TODO: write_list not needed here? */
	while(!LIST_EMPTY(&finfo->write_list)) {
		r = LIST_FIRST(&finfo->write_list);
		del_entry(r);
	}

	while(!LIST_EMPTY(&finfo->mapping_list)) {
		map = LIST_FIRST(&finfo->mapping_list);

		if(gimme_tent(map->realname, &tent) < 0)
			return -1;
		if(!tent || strcmp(tent->name.s, ".gitignore") != 0) {
			fprintf(stderr, "tup error: Writing to file '%s' while parsing is not allowed. Only a .gitignore file may be created during the parsing stage.\n", map->realname);
			map_bork = 1;
		} else {
			if(renameat(tup_top_fd(), map->tmpname, tup_top_fd(), map->realname) < 0) {
				perror("renameat");
				return -1;
			}
		}
		del_map(map);
	}
	if(map_bork)
		return -1;
	return 0;
}

static int file_set_mtime(struct tup_entry *tent, int dfd, const char *file)
{
	struct stat buf;
	if(fstatat(dfd, file, &buf, AT_SYMLINK_NOFOLLOW) < 0) {
		fprintf(stderr, "tup error: file_set_mtime() fstatat failed.\n");
		perror(file);
		return -1;
	}
	if(tup_db_set_mtime(tent, buf.MTIME) < 0)
		return -1;
	return 0;
}

static struct file_entry *new_entry(const char *filename, tupid_t dt)
{
	struct file_entry *fent;

	fent = malloc(sizeof *fent);
	if(!fent) {
		perror("malloc");
		return NULL;
	}

	fent->filename = strdup(filename);
	if(!fent->filename) {
		perror("strdup");
		free(fent);
		return NULL;
	}

	if(get_path_elements(fent->filename, &fent->pg) < 0) {
		free(fent->filename);
		free(fent);
		return NULL;
	}
	fent->dt = dt;
	return fent;
}

static void del_entry(struct file_entry *fent)
{
	LIST_REMOVE(fent, list);
	del_pel_group(&fent->pg);
	free(fent->filename);
	free(fent);
}

int handle_rename(const char *from, const char *to, struct file_info *info)
{
	struct file_entry *fent;
	struct pel_group pg_from;
	struct pel_group pg_to;

	if(get_path_elements(from, &pg_from) < 0)
		return -1;
	if(get_path_elements(to, &pg_to) < 0)
		return -1;

	LIST_FOREACH(fent, &info->write_list, list) {
		if(pg_eq(&fent->pg, &pg_from)) {
			del_pel_group(&fent->pg);
			free(fent->filename);

			fent->filename = strdup(to);
			if(!fent->filename) {
				perror("strdup");
				return -1;
			}
			if(get_path_elements(fent->filename, &fent->pg) < 0)
				return -1;
		}
	}
	LIST_FOREACH(fent, &info->read_list, list) {
		if(pg_eq(&fent->pg, &pg_from)) {
			del_pel_group(&fent->pg);
			free(fent->filename);

			fent->filename = strdup(to);
			if(!fent->filename) {
				perror("strdup");
				return -1;
			}
			if(get_path_elements(fent->filename, &fent->pg) < 0)
				return -1;
		}
	}

	check_unlink_list(&pg_to, &info->unlink_list);
	del_pel_group(&pg_to);
	del_pel_group(&pg_from);
	return 0;
}

void del_map(struct mapping *map)
{
	LIST_REMOVE(map, list);
	free(map->tmpname);
	free(map->realname);
	free(map);
}

static void check_unlink_list(const struct pel_group *pg,
			      struct file_entry_head *u_head)
{
	struct file_entry *fent, *tmp;

	LIST_FOREACH_SAFE(fent, u_head, list, tmp) {
		if(pg_eq(&fent->pg, pg)) {
			del_entry(fent);
		}
	}
}

static void handle_unlink(struct file_info *info)
{
	struct file_entry *u, *fent, *tmp;

	while(!LIST_EMPTY(&info->unlink_list)) {
		u = LIST_FIRST(&info->unlink_list);

		LIST_FOREACH_SAFE(fent, &info->write_list, list, tmp) {
			if(pg_eq(&fent->pg, &u->pg)) {
				del_entry(fent);
			}
		}
		LIST_FOREACH_SAFE(fent, &info->read_list, list, tmp) {
			if(pg_eq(&fent->pg, &u->pg)) {
				del_entry(fent);
			}
		}

		del_entry(u);
	}
}

static int update_write_info(FILE *f, tupid_t cmdid, struct file_info *info,
			     int *warnings, struct tup_entry_head *entryhead)
{
	struct file_entry *w;
	struct file_entry *r;
	struct file_entry *tmp;
	struct tup_entry *tent;
	int write_bork = 0;

	while(!LIST_EMPTY(&info->write_list)) {
		tupid_t newdt;
		struct path_element *pel = NULL;

		w = LIST_FIRST(&info->write_list);
		if(w->dt < 0) {
			goto out_skip;
		}

		/* Remove duplicate write entries */
		LIST_FOREACH_SAFE(r, &info->write_list, list, tmp) {
			if(r != w && pg_eq(&w->pg, &r->pg)) {
				del_entry(r);
			}
		}

		if(w->pg.pg_flags & PG_HIDDEN) {
			fprintf(f, "tup warning: Writing to hidden file '%s'\n", w->filename);
			(*warnings)++;
			goto out_skip;
		}

		newdt = find_dir_tupid_dt_pg(w->dt, &w->pg, &pel, 0);
		if(newdt <= 0) {
			fprintf(f, "tup error: File '%s' was written to, but is not in .tup/db. You probably should specify it as an output\n", w->filename);
			return -1;
		}
		if(!pel) {
			fprintf(f, "[31mtup internal error: find_dir_tupid_dt_pg() in write_files() didn't get a final pel pointer.[0m\n");
			return -1;
		}

		if(tup_db_select_tent_part(newdt, pel->path, pel->len, &tent) < 0)
			return -1;
		free(pel);
		if(!tent) {
			fprintf(f, "tup error: File '%s' was written to, but is not in .tup/db. You probably should specify it as an output\n", w->filename);
			write_bork = 1;
		} else {
			struct mapping *map;
			tup_entry_list_add(tent, entryhead);

			LIST_FOREACH(map, &info->mapping_list, list) {
				if(strcmp(map->realname, w->filename) == 0) {
					map->tent = tent;
				}
			}
		}

out_skip:
		del_entry(w);
	}

	if(write_bork) {
		while(!LIST_EMPTY(&info->mapping_list)) {
			struct mapping *map;

			map = LIST_FIRST(&info->mapping_list);
			unlink(map->tmpname);
			del_map(map);
		}
		return -1;
	}

	if(tup_db_check_actual_outputs(f, cmdid, entryhead) < 0)
		return -1;

	while(!LIST_EMPTY(&info->mapping_list)) {
		struct mapping *map;

		map = LIST_FIRST(&info->mapping_list);

		/* TODO: strcmp only here for win32 support */
		if(strcmp(map->tmpname, map->realname) != 0) {
			if(renameat(tup_top_fd(), map->tmpname, tup_top_fd(), map->realname) < 0) {
				perror(map->realname);
				fprintf(f, "tup error: Unable to rename temporary file '%s' to destination '%s'\n", map->tmpname, map->realname);
				write_bork = 1;
			}
		}
		if(map->tent) {
			/* tent may not be set (in the case of hidden files) */
			if(file_set_mtime(map->tent, tup_top_fd(), map->realname) < 0)
				return -1;
		}
		del_map(map);
	}

	if(write_bork)
		return -1;

	return 0;
}

static int update_read_info(FILE *f, tupid_t cmdid, struct file_info *info,
			    struct tup_entry_head *entryhead)
{
	struct file_entry *r;

	while(!LIST_EMPTY(&info->read_list)) {
		r = LIST_FIRST(&info->read_list);
		if(r->dt > 0) {
			if(add_node_to_list(r->dt, &r->pg, entryhead) < 0)
				return -1;
		}
		del_entry(r);
	}

	while(!LIST_EMPTY(&info->var_list)) {
		r = LIST_FIRST(&info->var_list);

		if(add_node_to_list(VAR_DT, &r->pg, entryhead) < 0)
			return -1;
		del_entry(r);
	}

	if(tup_db_check_actual_inputs(f, cmdid, entryhead) < 0)
		return -1;
	return 0;
}
