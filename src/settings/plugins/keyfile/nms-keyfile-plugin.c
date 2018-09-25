/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service - keyfile plugin
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 - 2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-keyfile-plugin.h"

#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <glib/gstdio.h>

#include "nm-utils/c-list-util.h"
#include "nm-utils/nm-c-list.h"
#include "nm-utils/nm-io-utils.h"

#include "nm-connection.h"
#include "nm-setting.h"
#include "nm-setting-connection.h"
#include "nm-utils.h"
#include "nm-config.h"
#include "nm-core-internal.h"
#include "nm-keyfile-internal.h"

#include "systemd/nm-sd-utils-shared.h"

#include "settings/nm-settings-plugin.h"

#include "nms-keyfile-storage.h"
#include "nms-keyfile-writer.h"
#include "nms-keyfile-reader.h"
#include "nms-keyfile-utils.h"

/*****************************************************************************/

typedef enum {
	COMMIT_CHANGES_FLAG_NONE                   = 0,

	/* note that "allow" flags are only hints. If the plugin has
	 * priv->dirname_etc unset, it must use priv->dirname_run (regardless
	 * of allow-storage-type-run). See _storage_type_select_for_commit(). */
	COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_RUN = (1LL << 0),

	COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_ETC = (1LL << 1),

} CommitChangesFlags;

typedef struct {
	CList crld_lst;
	char *full_filename;
	const char *filename;

	/* the profile loaded from the file. Note that this profile is only relevant
	 * during _do_reload_all(). The winning profile at the end of reload will
	 * be referenced as connection_exported, the connection field here will be
	 * cleared. */
	NMConnection *connection;

	/* the following fields are only required during _do_reload_all() for comparing
	 * which profile is the most relevant one (in case multple files provide a profile
	 * with the same UUID). */
	struct timespec stat_mtime;
	dev_t stat_dev;
	ino_t stat_ino;
	NMSKeyfileStorageType storage_type:3;
	guint storage_priority:15;
} ConnReloadData;

typedef struct _NMSKeyfileConnReloadHead {
	CList crld_lst_head;

	char *loaded_path_etc;
	char *loaded_path_run;
} ConnReloadHead;

typedef struct {

	/* there can/could be multiple read-only directories. For example, one
	 * could set dirname_libs to
	 *   - /usr/lib/NetworkManager/profiles/
	 *   - /etc/NetworkManager/system-connections
	 * and leave dirname_etc unset. In this case, there would be multiple
	 * read-only directories.
	 *
	 * Directories that come later have higher priority and shadow profiles
	 * from earlier directories.
	 */
	char **dirname_libs;
	char *dirname_etc;
	char *dirname_run;

	struct {
		CList lst_head;
		GHashTable *idx;

		// XXX: do we need filename-idx? Also, it's not properly maintained.
		GHashTable *filename_idx;
	} storages;

	NMConfig *config;

	bool initialized:1;
} NMSKeyfilePluginPrivate;

struct _NMSKeyfilePlugin {
	NMSettingsPlugin parent;
	NMSKeyfilePluginPrivate _priv;
};

struct _NMSKeyfilePluginClass {
	NMSettingsPluginClass parent;
};

G_DEFINE_TYPE (NMSKeyfilePlugin, nms_keyfile_plugin, NM_TYPE_SETTINGS_PLUGIN)

#define NMS_KEYFILE_PLUGIN_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMSKeyfilePlugin, NMS_IS_KEYFILE_PLUGIN, NMSettingsPlugin)

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME      "keyfile"
#define _NMLOG_DOMAIN           LOGD_SETTINGS
#define _NMLOG(level, ...) \
    nm_log ((level), _NMLOG_DOMAIN, NULL, NULL, \
            "%s" _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
            _NMLOG_PREFIX_NAME": " \
            _NM_UTILS_MACRO_REST (__VA_ARGS__))

/*****************************************************************************/

static void _storage_reload_data_head_clear (NMSKeyfileStorage *storage);

/*****************************************************************************/

static NMSKeyfileStorageType
_storage_type_select_for_commit (NMSKeyfileStorageType current_storage_type,
                                 CommitChangesFlags commit_flags,
                                 gboolean has_dirname_etc)
{
	if (current_storage_type == NMS_KEYFILE_STORAGE_TYPE_LIB) {
		/* of course, we cannot persist anything to read-only directory. Fallback
		 * to persistent read-write directory. */
		current_storage_type = NMS_KEYFILE_STORAGE_TYPE_ETC;
	}

	nm_assert (NM_IN_SET (current_storage_type, NMS_KEYFILE_STORAGE_TYPE_ETC, NMS_KEYFILE_STORAGE_TYPE_RUN));

	if (!has_dirname_etc) {
		/* we don't have a /etc directory. Nothing we can do, but use the
		 * /run directory (which always exists).
		 *
		 * This is regardless of @change_flags. */
		return NMS_KEYFILE_STORAGE_TYPE_RUN;
	}

	if (   current_storage_type == NMS_KEYFILE_STORAGE_TYPE_ETC
	    && !NM_FLAGS_HAS (commit_flags, COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_ETC)) {
		/* /etc is not allowed. We cannot do but use /run. */
		return NMS_KEYFILE_STORAGE_TYPE_RUN;
	}

	if (   current_storage_type == NMS_KEYFILE_STORAGE_TYPE_RUN
	    && !NM_FLAGS_HAS (commit_flags, COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_RUN)
	    && NM_FLAGS_HAS (commit_flags, COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_ETC)) {
		/* this is the only case where we "upgrade" from /run to /etc. */
		return NMS_KEYFILE_STORAGE_TYPE_ETC;
	}

	return current_storage_type;
}

/*****************************************************************************/

static gboolean
_ignore_filename (NMSKeyfileStorageType storage_type,
                  const char *filename)
{
	/* for backward-compatibility, we don't require an extension for
	 * files under "/etc/...". */
	return nm_keyfile_utils_ignore_filename (filename,
	                                         (storage_type != NMS_KEYFILE_STORAGE_TYPE_ETC));
}

/*****************************************************************************/

static const char *
_get_plugin_dir (NMSKeyfilePluginPrivate *priv)
{
	/* the plugin dir is only needed to generate connection.uuid value via
	 * nm_keyfile_read_ensure_uuid(). This is either the configured /etc
	 * directory, of the compile-time default (in case the /etc directory
	 * is disabled). */
	return priv->dirname_etc ?: NM_KEYFILE_PATH_NAME_ETC_DEFAULT;
}

static gboolean
_path_detect_storage_type (const char *full_filename,
                           const char *const*dirname_libs,
                           const char *dirname_etc,
                           const char *dirname_run,
                           NMSKeyfileStorageType *out_storage_type,
                           const char **out_dirname,
                           const char **out_filename,
                           GError **error)
{
	NMSKeyfileStorageType storage_type;
	const char *filename = NULL;
	const char *dirname = NULL;
	guint i;

	if (full_filename[0] != '/') {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
		                            "filename is not an absolute path");
		return FALSE;
	}

	if (   dirname_run
	    && (filename = nm_utils_file_is_in_path (full_filename, dirname_run))) {
		storage_type = NMS_KEYFILE_STORAGE_TYPE_RUN;
		dirname = dirname_run;
	} else if (   dirname_etc
	           && (filename = nm_utils_file_is_in_path (full_filename, dirname_etc))) {
		storage_type = NMS_KEYFILE_STORAGE_TYPE_ETC;
		dirname = dirname_etc;
	} else {
		for (i = 0; dirname_libs && dirname_libs[i]; i++) {
			if ((filename = nm_utils_file_is_in_path (full_filename, dirname_libs[i]))) {
				storage_type = NMS_KEYFILE_STORAGE_TYPE_LIB;
				dirname = dirname_libs[i];
				break;
			}
		}
		if (!dirname) {
			nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
			                            "filename is not inside a keyfile directory");
			return FALSE;
		}
	}

	if (_ignore_filename (storage_type, filename)) {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
		                            "filename is not a valid keyfile");
		return FALSE;
	}

	NM_SET_OUT (out_storage_type, storage_type);
	NM_SET_OUT (out_dirname, dirname);
	NM_SET_OUT (out_filename, filename);
	return TRUE;
}

/*****************************************************************************/

static NMConnection *
_read_from_file (const char *full_filename,
                 const char *plugin_dir,
                 struct stat *out_stat,
                 GError **error)
{
	NMConnection *connection;

	g_return_val_if_fail (full_filename && full_filename[0] == '/', NULL);

	connection = nms_keyfile_reader_from_file (full_filename, plugin_dir, out_stat, error);
	if (!connection)
		return NULL;

	nm_assert (nm_connection_get_uuid (connection));
	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (nm_utils_is_uuid (nm_connection_get_uuid (connection)));
	return connection;
}

/*****************************************************************************/

static void
_conn_reload_data_destroy (ConnReloadData *storage_data)
{
	c_list_unlink_stale (&storage_data->crld_lst);
	nm_g_object_unref (storage_data->connection);
	g_free (storage_data->full_filename);
	g_slice_free (ConnReloadData, storage_data);
}

static ConnReloadData *
_conn_reload_data_new (guint storage_priority,
                       NMSKeyfileStorageType storage_type,
                       char *full_filename_take,
                       NMConnection *connection_take,
                       const struct stat *st)
{
	ConnReloadData *storage_data;

	storage_data = g_slice_new (ConnReloadData);
	*storage_data = (ConnReloadData) {
		.storage_type     = storage_type,
		.storage_priority = storage_priority,
		.full_filename    = full_filename_take,
		.filename         = strrchr (full_filename_take, '/') + 1,
		.connection       = connection_take,
	};
	if (st) {
		storage_data->stat_mtime = st->st_mtim;
		storage_data->stat_dev = st->st_dev;
		storage_data->stat_ino = st->st_ino;
	}

	nm_assert (storage_data->storage_type     == storage_type);
	nm_assert (storage_data->storage_priority == storage_priority);
	nm_assert (storage_data->full_filename);
	nm_assert (storage_data->full_filename[0] == '/');
	nm_assert (storage_data->filename);
	nm_assert (storage_data->filename[0]);
	nm_assert (!strchr (storage_data->filename, '/'));

	return storage_data;
}

static void
_conn_reload_data_destroy_all (CList *crld_lst_head)
{
	ConnReloadData *storage_data;

	while ((storage_data = c_list_first_entry (crld_lst_head, ConnReloadData, crld_lst)))
		_conn_reload_data_destroy (storage_data);
}

static int
_conn_reload_data_cmp_by_priority (const CList *lst_a,
                                   const CList *lst_b,
                                   const void *user_data)
{
	const ConnReloadData *a = c_list_entry (lst_a, ConnReloadData, crld_lst);
	const ConnReloadData *b = c_list_entry (lst_b, ConnReloadData, crld_lst);

	/* we sort more important entries first. */

	/* sorting by storage-priority implies sorting by storage-type too.
	 * That is, because for different storage-types, we assign different storage-priorities
	 * and their sort order corresponds (with inverted order). Assert for that. */
	nm_assert (   a->storage_type == b->storage_type
	           || (   (a->storage_priority != b->storage_priority)
	               && (a->storage_type < b->storage_type) == (a->storage_priority > b->storage_priority)));

	/* sort by storage-priority, smaller is more important. */
	NM_CMP_FIELD_UNSAFE (a, b, storage_priority);

	/* newer files are more important. */
	NM_CMP_FIELD (b, a, stat_mtime.tv_sec);
	NM_CMP_FIELD (b, a, stat_mtime.tv_nsec);

	NM_CMP_FIELD_STR (a, b, filename);

	nm_assert_not_reached ();
	return 0;
}

/* stat(@loaded_path) and if the path is the same as eny of the ones from
 * @crld_lst_head, move the found entry to the front and return TRUE.
 * Otherwise, do nothing and return FALSE. */
static gboolean
_conn_reload_data_prioritize_loaded (CList *crld_lst_head,
                                     const char *loaded_path)
{
	ConnReloadData *storage_data;
	struct stat st_loaded;

	if (loaded_path[0] != '/')
		return FALSE;

	/* we compare the file based on the inode, not based on the path.
	 * stat() the file. */
	if (stat (loaded_path, &st_loaded) != 0)
		return FALSE;

	c_list_for_each_entry (storage_data, crld_lst_head, crld_lst) {
		if (   storage_data->stat_dev == st_loaded.st_dev
		    && storage_data->stat_ino == st_loaded.st_ino) {
			nm_c_list_move_to_front (crld_lst_head, &storage_data->crld_lst);
			return TRUE;
		}
	}

	return FALSE;
}

/*****************************************************************************/

static void
_storage_assert (gpointer plugin  /* NMSKeyfilePlugin  */,
                 gpointer storage /* NMSKeyfileStorage */,
                 gboolean tracked)
{
	nm_assert (!plugin || NMS_IS_KEYFILE_PLUGIN (plugin));
	nm_assert (NMS_IS_KEYFILE_STORAGE (storage));
	nm_assert (!plugin || plugin == nm_settings_storage_get_plugin (storage));
	nm_assert (NMS_KEYFILE_STORAGE (storage)->full_filename);
	nm_assert (NMS_KEYFILE_STORAGE (storage)->full_filename[0] == '/');
	nm_assert (NMS_KEYFILE_STORAGE (storage)->uuid);
	nm_assert (nm_utils_is_uuid (NMS_KEYFILE_STORAGE (storage)->uuid));

	nm_assert (   !tracked
	           || !plugin
	           || c_list_contains (&NMS_KEYFILE_PLUGIN_GET_PRIVATE (plugin)->storages.lst_head,
	                               &NMS_KEYFILE_STORAGE (storage)->storage_lst));

	nm_assert (   !tracked
	           || !plugin
	           || storage == g_hash_table_lookup (NMS_KEYFILE_PLUGIN_GET_PRIVATE (plugin)->storages.idx,
	                                              NMS_KEYFILE_STORAGE (storage)->uuid));
}

void
_nms_keyfile_storage_clear (NMSKeyfileStorage *storage)
{
	_storage_assert (NULL, storage, TRUE);

	c_list_unlink (&storage->storage_lst);

	_storage_reload_data_head_clear (storage);

	nm_clear_g_free (&storage->full_filename);

	g_clear_object (&storage->connection_exported);

	nm_clear_g_free (&storage->uuid);
}

static void
_storage_destroy (NMSKeyfileStorage *storage)
{
	_storage_assert (NULL, storage, TRUE);

	_nms_keyfile_storage_clear (storage);
	g_object_unref (storage);
}

static guint
_storage_idx_hash (NMSKeyfileStorage *storage)
{
	_storage_assert (NULL, storage, TRUE);

	return nm_str_hash (storage->uuid);
}

static gboolean
_storage_idx_equal (NMSKeyfileStorage *a,
                    NMSKeyfileStorage *b)
{
	_storage_assert (NULL, a, TRUE);
	_storage_assert (NULL, b, TRUE);

	return nm_streq (a->uuid, b->uuid);
}

static ConnReloadHead *
_storage_reload_data_head_ensure (NMSKeyfileStorage *storage)
{
	ConnReloadHead *hd;

	hd = storage->reload_data_head;
	if (!hd) {
		hd = g_slice_new (ConnReloadHead);
		*hd = (ConnReloadHead) {
			.crld_lst_head = C_LIST_INIT (hd->crld_lst_head),
		};
		storage->reload_data_head = hd;
	}
	return hd;
}

static void
_storage_reload_data_head_clear (NMSKeyfileStorage *storage)
{
	ConnReloadHead *hd;

	hd = g_steal_pointer (&storage->reload_data_head);
	if (!hd)
		return;

	_conn_reload_data_destroy_all (&hd->crld_lst_head);
	g_free (hd->loaded_path_run);
	g_free (hd->loaded_path_etc);
	g_slice_free (ConnReloadHead, hd);
}

static gboolean
_storage_has_equal_connection (NMSKeyfileStorage *storage,
                               NMConnection *connection)
{
	nm_assert (NM_IS_CONNECTION (connection));

	return    storage->connection_exported
	       && nm_connection_compare (connection,
	                                 storage->connection_exported,
	                                   NM_SETTING_COMPARE_FLAG_IGNORE_AGENT_OWNED_SECRETS
	                                 | NM_SETTING_COMPARE_FLAG_IGNORE_NOT_SAVED_SECRETS);
}

/*****************************************************************************/

static NMSKeyfileStorage *
_storages_get (NMSKeyfilePlugin *self,
               const char *uuid,
               gboolean create)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileStorage *storage;

	nm_assert (uuid && nm_utils_is_uuid (uuid));

	storage = g_hash_table_lookup (priv->storages.idx, &uuid);
	if (   !storage
	    && create) {
		storage = nms_keyfile_storage_new (self, uuid);
		c_list_link_tail (&priv->storages.lst_head, &storage->storage_lst);
		g_hash_table_add (priv->storages.idx, storage);
	}

	return storage;
}

static void
_storages_set_full_filename (NMSKeyfilePluginPrivate *priv,
                             NMSKeyfileStorage *storage,
                             char *full_filename_take)
{
	if (storage->full_filename) {
		nm_assert (!nm_streq0 (full_filename_take, storage->full_filename));
		g_hash_table_remove (priv->storages.filename_idx, storage->full_filename);
		g_free (storage->full_filename);
	}
	storage->full_filename = full_filename_take;
	g_hash_table_insert (priv->storages.filename_idx, full_filename_take, storage);
}

static void
_storages_remove (NMSKeyfilePluginPrivate *priv,
                  NMSKeyfileStorage *storage,
                  gboolean destroy)
{
	nm_assert (priv);
	nm_assert (storage);
	nm_assert (c_list_contains (&priv->storages.lst_head, &storage->storage_lst));
	nm_assert (g_hash_table_lookup (priv->storages.idx, storage) == storage);

	if (destroy)
		g_hash_table_remove (priv->storages.idx, storage);
	else {
		c_list_unlink (&storage->storage_lst);
		g_hash_table_steal (priv->storages.idx, storage);
	}
}

/*****************************************************************************/

static void
_load_dir (NMSKeyfilePlugin *self,
           guint storage_priority,
           NMSKeyfileStorageType storage_type,
           const char *dirname,
           const char *plugin_dir)
{
	const char *filename;
	GDir *dir;

	if (!dirname)
		return;

	dir = g_dir_open (dirname, 0, NULL);
	if (!dir)
		return;

	while ((filename = g_dir_read_name (dir))) {
		gs_unref_object NMConnection *connection = NULL;
		gs_free_error GError *error = NULL;
		NMSKeyfileStorage *storage;
		ConnReloadData *storage_data;
		gs_free char *full_filename = NULL;
		struct stat st;
		ConnReloadHead *hd;

		if (_ignore_filename (storage_type, filename)) {
			gs_free char *loaded_uuid = NULL;
			gs_free char *loaded_path = NULL;

			if (!nms_keyfile_loaded_uuid_read (dirname,
			                                   filename,
			                                   NULL,
			                                   &loaded_uuid,
			                                   &loaded_path)) {
				_LOGT ("load: \"%s/%s\": skip file due to filename pattern", dirname, filename);
				continue;
			}
			if (!NM_IN_SET (storage_type, NMS_KEYFILE_STORAGE_TYPE_RUN,
			                              NMS_KEYFILE_STORAGE_TYPE_ETC)) {
				_LOGT ("load: \"%s/%s\": skip loaded file from read-only directory", dirname, filename);
				continue;
			}
			storage = _storages_get (self, loaded_uuid, TRUE);
			hd = _storage_reload_data_head_ensure (storage);
			if (storage_type == NMS_KEYFILE_STORAGE_TYPE_RUN) {
				nm_assert (!hd->loaded_path_run);
				hd->loaded_path_run = g_steal_pointer (&loaded_path);
			} else {
				nm_assert (!hd->loaded_path_etc);
				hd->loaded_path_etc = g_steal_pointer (&loaded_path);
			}
			continue;
		}

		full_filename = g_build_filename (dirname, filename, NULL);

		connection = _read_from_file (full_filename, plugin_dir, &st, &error);
		if (!connection) {
			_LOGW ("load: \"%s\": failed to load connection: %s", full_filename, error->message);
			continue;
		}

		storage = _storages_get (self, nm_connection_get_uuid (connection), TRUE);
		hd = _storage_reload_data_head_ensure (storage);
		storage_data = _conn_reload_data_new (storage_priority,
		                                      storage_type,
		                                      g_steal_pointer (&full_filename),
		                                      g_steal_pointer (&connection),
		                                      &st);
		c_list_link_tail (&hd->crld_lst_head, &storage_data->crld_lst);
	}

	g_dir_close (dir);
}

static void
_do_reload_all (NMSKeyfilePlugin *self,
                gboolean overwrite_in_memory)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	CList lst_conn_info_deleted = C_LIST_INIT (lst_conn_info_deleted);
	const char *plugin_dir = _get_plugin_dir (priv);
	gs_unref_ptrarray GPtrArray *events_mod = NULL;
	NMSKeyfileStorage *storage_safe;
	NMSKeyfileStorage *storage;
	guint i;

	priv->initialized = TRUE;

	/* while reloading, we let the filename index degrade. */
	g_hash_table_remove_all (priv->storages.filename_idx);

	/* collect all information by loading it from the files. On repeated reloads,
	 * this merges new information with content from previous loads. */
	_load_dir (self, 1, NMS_KEYFILE_STORAGE_TYPE_RUN, priv->dirname_run, plugin_dir);
	_load_dir (self, 2, NMS_KEYFILE_STORAGE_TYPE_ETC, priv->dirname_etc, plugin_dir);
	for (i = 0; priv->dirname_libs && priv->dirname_libs[i]; i++)
		_load_dir (self, 3 + i, NMS_KEYFILE_STORAGE_TYPE_LIB, priv->dirname_libs[i], plugin_dir);

	/* sort out the loaded information. */
	c_list_for_each_entry_safe (storage, storage_safe, &priv->storages.lst_head, storage_lst) {
		ConnReloadHead *hd;
		ConnReloadData *rd, *rd_best;
		gboolean connection_modified;
		gboolean connection_renamed;
		gboolean loaded_path_masked = FALSE;
		const char *loaded_dirname = NULL;
		gs_free char *loaded_path = NULL;

		hd = storage->reload_data_head;

		nm_assert ((!storage->full_filename) == (!storage->connection_exported));
		nm_assert (!hd || (   !c_list_is_empty (&hd->crld_lst_head)
		                   || hd->loaded_path_run
		                   || hd->loaded_path_etc));
		nm_assert (hd || storage->full_filename);

		/* find and steal the loaded-path (if any) */
		if (hd) {
			if (hd->loaded_path_run) {
				if (hd->loaded_path_etc) {
					gs_free char *f1 = NULL;
					gs_free char *f2 = NULL;

					_LOGT ("load: \"%s\": shadowed by \"%s\"",
					       (f1 = nms_keyfile_loaded_uuid_filename (priv->dirname_etc, storage->uuid, FALSE)),
					       (f2 = nms_keyfile_loaded_uuid_filename (priv->dirname_run, storage->uuid, FALSE)));
					nm_clear_g_free (&hd->loaded_path_etc);
				}
				loaded_dirname = priv->dirname_run;
				loaded_path = g_steal_pointer (&hd->loaded_path_run);
			} else if (hd->loaded_path_etc) {
				loaded_dirname = priv->dirname_etc;
				loaded_path = g_steal_pointer (&hd->loaded_path_etc);
			}
		}
		nm_assert ((!loaded_path) == (!loaded_dirname));

		/* sort storage datas by priority. */
		if (hd)
			c_list_sort (&hd->crld_lst_head, _conn_reload_data_cmp_by_priority, NULL);

		if (loaded_path) {
			if (nm_streq (loaded_path, NM_KEYFILE_PATH_NMLOADED_NULL)) {
				loaded_path_masked = TRUE;
				nm_clear_g_free (&loaded_path);
			} else if (!_conn_reload_data_prioritize_loaded (&hd->crld_lst_head, loaded_path)) {
				gs_free char *f1 = NULL;

				_LOGT ("load: \"%s\": ignore invalid target \"%s\"",
				       (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, storage->uuid, FALSE)),
				       loaded_path);
				nm_clear_g_free (&loaded_path);
			}
		}

		rd_best = hd
		          ? c_list_first_entry (&hd->crld_lst_head, ConnReloadData, crld_lst)
		          : NULL;
		if (   !rd_best
		    || loaded_path_masked) {

			/* after reload, no file references this profile (or the files are masked from loading
			 * via a symlink to /dev/null). */

			if (_LOGT_ENABLED ()) {
				gs_free char *f1 = NULL;

				if (!hd) {
					_LOGT ("load: \"%s\": file no longer exists for profile with UUID \"%s\" (remove profile)",
					       storage->full_filename,
					       storage->uuid);
				} else if (rd_best) {
					c_list_for_each_entry (rd, &hd->crld_lst_head, crld_lst) {
						const char *remove_profile_msg = "";

						if (nm_streq0 (rd->full_filename, storage->full_filename))
							remove_profile_msg = " (remove profile)";
						_LOGT ("load: \"%s\": profile %s masked by \"%s\" file symlinking \"%s\"%s",
						       rd->full_filename,
						       storage->uuid,
						       f1 ?: (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, storage->uuid, FALSE)),
						       NM_KEYFILE_PATH_NMLOADED_NULL,
						       remove_profile_msg);
					}
				} else {
					_LOGT ("load: \"%s\": symlinks \"%s\" but there are no profiles with UUID \"%s\"",
					       (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, storage->uuid, FALSE)),
					       loaded_path,
					       storage->uuid);
				}
			}

			if (!storage->full_filename)
				_storages_remove (priv, storage, TRUE);
			else {
				_storages_remove (priv, storage, FALSE);
				c_list_link_tail (&lst_conn_info_deleted, &storage->storage_lst);
			}
			continue;
		}

		c_list_for_each_entry (rd, &hd->crld_lst_head, crld_lst) {
			if (rd_best != rd) {
				_LOGT ("load: \"%s\": profile %s shadowed by \"%s\" file",
				       rd->full_filename,
				       storage->uuid,
				       rd_best->full_filename);
			}
		}

		storage->storage_type_exported = rd_best->storage_type;
		connection_modified = !_storage_has_equal_connection (storage, rd_best->connection);
		connection_renamed =    !storage->full_filename
		                     || !nm_streq (storage->full_filename, rd_best->full_filename);

		{
			gs_free char *f1 = NULL;

			_LOGT ("load: \"%s\": profile %s (%s) loaded (%s)"
			       "%s%s%s"
			       "%s%s%s",
			       rd_best->full_filename,
			       storage->uuid,
			       nm_connection_get_id (rd_best->connection),
			       connection_modified ? (storage->full_filename ? "updated" : "added" ) : "unchanged",
			       NM_PRINT_FMT_QUOTED (loaded_path,
			                            " (hinted by \"",
			                            (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, storage->uuid, FALSE)),
			                            "\")",
			                            ""),
			       NM_PRINT_FMT_QUOTED (storage->full_filename && connection_renamed,
			                            " (renamed from \"",
			                            storage->full_filename,
			                            "\")",
			                            ""));
		}

		if (connection_modified)
			nm_g_object_ref_set (&storage->connection_exported, rd_best->connection);

		if (connection_renamed) {
			g_free (storage->full_filename);
			storage->full_filename = g_steal_pointer (&rd->full_filename);
		}
		if (!nm_g_hash_table_insert (priv->storages.filename_idx,
		                             storage->full_filename,
		                             storage)) {
			/* There must be no duplicates here. However, we don't assert against that,
			 * because it would mean to assert that readdir() syscall never returns duplicates.
			 * We don't want to assert against kernel IO results. */
		}

		/* we don't need the reload data anymore. Drop it. */
		_storage_reload_data_head_clear (storage);

		if (connection_modified || connection_renamed) {
			if (!events_mod)
				events_mod = g_ptr_array_new_with_free_func (g_free);
			g_ptr_array_add (events_mod, g_strdup (storage->uuid));
		}
	}

	/* raise events. */

	c_list_for_each_entry_safe (storage, storage_safe, &lst_conn_info_deleted, storage_lst) {
		_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
		                                                    storage->uuid,
		                                                    NM_SETTINGS_STORAGE (storage),
		                                                    NULL);
		_storage_destroy (storage);
	}

	if (events_mod) {
		for (i = 0; i < events_mod->len; i++) {
			const char *uuid = events_mod->pdata[i];

			storage = _storages_get (self, uuid, FALSE);
			if (   !storage
			    || !storage->connection_exported) {
				/* hm? The profile was deleted in the meantime? That is only possible
				 * if the signal handler called again into the plugin. Ignore it. */
				continue;
			}

			_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
			                                                    uuid,
			                                                    NM_SETTINGS_STORAGE (storage),
			                                                    storage->connection_exported);
		}
	}
}

static gboolean
_do_load_connection (NMSKeyfilePlugin *self,
                     const char *full_filename,
                     NMSettingsStorage **out_storage,
                     NMConnection **out_connection,
                     GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileStorageType storage_type;
	const char *dirname;
	const char *filename;
	NMSKeyfileStorage *storage;
	gs_unref_object NMConnection *connection_new = NULL;
	gboolean connection_modified;
	gboolean connection_renamed;
	GError *local = NULL;
	gboolean loaded_uuid_success;
	gs_free char *loaded_uuid_filename = NULL;

	nm_assert (!out_storage || !*out_storage);
	nm_assert (!out_connection || !*out_connection);

	if (!_path_detect_storage_type (full_filename,
	                                NM_CAST_STRV_CC (priv->dirname_libs),
	                                priv->dirname_etc,
	                                priv->dirname_run,
	                                &storage_type,
	                                &dirname,
	                                &filename,
	                                error))
		return FALSE;

	connection_new = _read_from_file (full_filename, _get_plugin_dir (priv), NULL, &local);
	if (!connection_new) {
		_LOGT ("load: \"%s\": failed to load connection: %s", full_filename, local->message);
		g_propagate_error (error, local);
		return FALSE;
	}

	storage = _storages_get (self, nm_connection_get_uuid (connection_new), TRUE);

	connection_modified = !_storage_has_equal_connection (storage, connection_new);
	connection_renamed =    !storage->full_filename
	                     || !nm_streq (storage->full_filename, full_filename);

	/* mark the profile as loaded, so that it's still used after restart.
	 *
	 * For the moment, only do this in the /run directory, meaning the
	 * information is lost after reboot.
	 *
	 * In the future, maybe we can be smarter about this and persist it to /etc,
	 * so that the preferred loaded file is still preferred after reboot. */
	loaded_uuid_success = nms_keyfile_loaded_uuid_write (priv->dirname_run,
	                                                     storage->uuid,
	                                                     full_filename,
	                                                     TRUE,
	                                                     &loaded_uuid_filename);

	_LOGT ("load: \"%s/%s\": profile %s (%s) loaded (%s)"
	       "%s%s%s"
	       " (%s%s%s)",
	       dirname,
	       filename,
	       storage->uuid,
	       nm_connection_get_id (connection_new),
	       connection_modified ? (storage->connection_exported ? "updated" : "added" ) : "unchanged",
	       NM_PRINT_FMT_QUOTED (storage->full_filename && connection_renamed,
	                            " (renamed from \"",
	                            storage->full_filename,
	                            "\")",
	                            ""),
	       NM_PRINT_FMT_QUOTED (loaded_uuid_success,
	                            "indicated by \"",
	                            loaded_uuid_filename,
	                            "\"",
	                            "failed to write loaded file"));

	storage->storage_type_exported = storage_type;

	if (connection_renamed) {
		_storages_set_full_filename (priv,
		                             storage,
		                             g_build_filename (dirname, filename, NULL));
	}

	NM_SET_OUT (out_storage, NM_SETTINGS_STORAGE (g_object_ref (storage)));

	if (connection_modified) {
		NM_SET_OUT (out_connection, g_object_ref (connection_new));
		nm_g_object_ref_set (&storage->connection_exported, connection_new);
	} else
		NM_SET_OUT (out_connection, g_object_ref (storage->connection_exported));

	if (connection_modified || connection_renamed) {
		_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
		                                                    storage->uuid,
		                                                    NM_SETTINGS_STORAGE (storage),
		                                                    storage->connection_exported);
	}

	return TRUE;
}

static gboolean
_do_commit_changes (NMSKeyfilePlugin *self,
                    NMSKeyfileStorage *storage,
                    NMConnection *connection,
                    NMSettingsStorageCommitReason commit_reason,
                    CommitChangesFlags commit_flags,
                    GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	gs_unref_object NMConnection *connection_clone = NULL;
	gs_unref_object NMConnection *reread = NULL;
	gs_free char *filename_written = NULL;
	gboolean loaded_uuid_success;
	gs_free char *loaded_uuid_filename = NULL;
	NMSKeyfileStorageType storage_type;
	NMSKeyfileStorageType storage_type_rd;
	gboolean connection_modified;
	gboolean connection_renamed;
	GError *local = NULL;
	const char *dirname;

	_storage_assert (self, storage, TRUE);
	nm_assert (NM_IS_CONNECTION (connection));
	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (!error || !*error);

	if (!nm_streq (nm_connection_get_uuid (connection), storage->uuid)) {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_INVALID_ARGUMENT, "missmatching UUID for commit");
		g_return_val_if_reached (FALSE);
	}

	if (!_path_detect_storage_type (storage->full_filename,
	                                NM_CAST_STRV_CC (priv->dirname_libs),
	                                priv->dirname_etc,
	                                priv->dirname_run,
	                                &storage_type_rd,
	                                NULL,
	                                NULL,
	                                error))
		g_return_val_if_reached (FALSE);

	storage_type = _storage_type_select_for_commit (storage_type_rd,
	                                                commit_flags,
	                                                !!priv->dirname_etc);

	if (storage_type == NMS_KEYFILE_STORAGE_TYPE_ETC)
		dirname = priv->dirname_etc;
	else {
		nm_assert (storage_type == NMS_KEYFILE_STORAGE_TYPE_RUN);
		dirname = priv->dirname_run;
	}

	if (!nms_keyfile_writer_connection (connection,
	                                    dirname,
	                                    _get_plugin_dir (priv),
	                                    storage->full_filename,
	                                    (storage_type != storage_type_rd),
	                                    NM_FLAGS_ALL (commit_reason,   NM_SETTINGS_STORAGE_COMMIT_REASON_USER_ACTION
	                                                                 | NM_SETTINGS_STORAGE_COMMIT_REASON_ID_CHANGED),
	                                    &filename_written,
	                                    &reread,
	                                    NULL,
	                                    &local)) {
		_LOGW ("commit: failure to write %s (%s) to disk: %s",
		       storage->uuid,
		       nm_connection_get_id (connection_clone),
		       local->message);
		g_propagate_error (error, local);
		return FALSE;
	}

	/* mark the profile as loaded, so that it's still used after restart.
	 *
	 * For the moment, only do this in the /run directory, meaning the
	 * information is lost after reboot.
	 *
	 * In the future, maybe we can be smarter about this and persist it to /etc,
	 * so that the preferred loaded file is still preferred after reboot. */
	loaded_uuid_success = nms_keyfile_loaded_uuid_write (priv->dirname_run,
	                                                     storage->uuid,
	                                                     filename_written,
	                                                     TRUE,
	                                                     &loaded_uuid_filename);

	connection_modified = !_storage_has_equal_connection (storage, connection);
	connection_renamed = !nm_streq (storage->full_filename, filename_written);

	_LOGT ("commit: \"%s\": profile %s (%s) written%s"
	       "%s%s%s"
	       " (%s%s%s)",
	       filename_written,
	       storage->uuid,
	       nm_connection_get_id (connection),
	       connection_modified ? " (modified)" : "",
	       NM_PRINT_FMT_QUOTED (connection_renamed,
	                            " (renamed from \"",
	                            storage->full_filename,
	                            "\")",
	                            ""),
	       NM_PRINT_FMT_QUOTED (loaded_uuid_success,
	                            "indicated by \"",
	                            loaded_uuid_filename,
	                            "\"",
	                            "failed to write loaded file"));

	if (connection_renamed) {
		_storages_set_full_filename (priv,
		                             storage,
		                             g_steal_pointer (&filename_written));
	}

	if (connection_modified)
		nm_g_object_ref_set (&storage->connection_exported, connection);

	if (connection_modified || connection_renamed) {
		_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
		                                                    storage->uuid,
		                                                    NM_SETTINGS_STORAGE (storage),
		                                                    storage->connection_exported);
	}

	return TRUE;
}

static gboolean
_do_delete (NMSKeyfilePlugin *self,
            NMSKeyfileStorage *storage,
            GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	gs_unref_object NMConnection *connection_clone = NULL;
	gs_unref_object NMConnection *reread = NULL;
	gs_free char *filename_written = NULL;
	gboolean loaded_uuid_success;
	gs_free char *loaded_uuid_filename = NULL;
	NMSKeyfileStorageType storage_type;
	NMSKeyfileStorageType storage_type_rd;
	gboolean connection_modified;
	gboolean connection_renamed;
	GError *local = NULL;
	const char *dirname;

	///XXX
	CommitChangesFlags commit_flags = 0;
	NMSettingsStorageCommitReason commit_reason = 0;
	NMConnection *connection = NULL;

	_storage_assert (self, storage, TRUE);
	nm_assert (NM_IS_CONNECTION (connection));
	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (!error || !*error);

	if (!nm_streq (nm_connection_get_uuid (connection), storage->uuid)) {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_INVALID_ARGUMENT, "missmatching UUID for commit");
		g_return_val_if_reached (FALSE);
	}

	if (!_path_detect_storage_type (storage->full_filename,
	                                NM_CAST_STRV_CC (priv->dirname_libs),
	                                priv->dirname_etc,
	                                priv->dirname_run,
	                                &storage_type_rd,
	                                NULL,
	                                NULL,
	                                error))
		g_return_val_if_reached (FALSE);

	storage_type = _storage_type_select_for_commit (storage_type_rd,
	                                                commit_flags,
	                                                !!priv->dirname_etc);

	if (storage_type == NMS_KEYFILE_STORAGE_TYPE_ETC)
		dirname = priv->dirname_etc;
	else {
		nm_assert (storage_type == NMS_KEYFILE_STORAGE_TYPE_RUN);
		dirname = priv->dirname_run;
	}

	if (!nms_keyfile_writer_connection (connection,
	                                    dirname,
	                                    _get_plugin_dir (priv),
	                                    storage->full_filename,
	                                    (storage_type != storage_type_rd),
	                                    NM_FLAGS_ALL (commit_reason,   NM_SETTINGS_STORAGE_COMMIT_REASON_USER_ACTION
	                                                                 | NM_SETTINGS_STORAGE_COMMIT_REASON_ID_CHANGED),
	                                    &filename_written,
	                                    &reread,
	                                    NULL,
	                                    &local)) {
		_LOGW ("commit: failure to write %s (%s) to disk: %s",
		       storage->uuid,
		       nm_connection_get_id (connection_clone),
		       local->message);
		g_propagate_error (error, local);
		return FALSE;
	}

	/* mark the profile as loaded, so that it's still used after restart.
	 *
	 * For the moment, only do this in the /run directory, meaning the
	 * information is lost after reboot.
	 *
	 * In the future, maybe we can be smarter about this and persist it to /etc,
	 * so that the preferred loaded file is still preferred after reboot. */
	loaded_uuid_success = nms_keyfile_loaded_uuid_write (priv->dirname_run,
	                                                     storage->uuid,
	                                                     filename_written,
	                                                     TRUE,
	                                                     &loaded_uuid_filename);

	connection_modified = !_storage_has_equal_connection (storage, connection);
	connection_renamed = !nm_streq (storage->full_filename, filename_written);

	_LOGT ("commit: \"%s\": profile %s (%s) written%s"
	       "%s%s%s"
	       " (%s%s%s)",
	       filename_written,
	       storage->uuid,
	       nm_connection_get_id (connection),
	       connection_modified ? " (modified)" : "",
	       NM_PRINT_FMT_QUOTED (connection_renamed,
	                            " (renamed from \"",
	                            storage->full_filename,
	                            "\")",
	                            ""),
	       NM_PRINT_FMT_QUOTED (loaded_uuid_success,
	                            "indicated by \"",
	                            loaded_uuid_filename,
	                            "\"",
	                            "failed to write loaded file"));

	if (connection_renamed) {
		_storages_set_full_filename (priv,
		                             storage,
		                             g_steal_pointer (&filename_written));
	}

	if (connection_modified)
		nm_g_object_ref_set (&storage->connection_exported, connection);

	if (connection_modified || connection_renamed) {
		_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
		                                                    storage->uuid,
		                                                    NM_SETTINGS_STORAGE (storage),
		                                                    storage->connection_exported);
	}

	return TRUE;

	return FALSE;
}

static gboolean
_do_add_connection (NMSKeyfilePlugin *self,
                    NMConnection *connection,
                    gboolean save_to_disk,
                    NMSettingsStorage **out_storage,
                    NMConnection **out_connection,
                    GError **error)
{
#if 0
//XXX
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (config);
	gs_free char *path = NULL;
	gs_unref_object NMConnection *reread = NULL;

	if (!nms_keyfile_writer_connection (connection,
	                                    save_to_disk,
	                                    NULL,
	                                    FALSE,
	                                    &path,
	                                    &reread,
	                                    NULL,
	                                    error))
		return NULL;

	return NM_SETTINGS_CONNECTION (update_connection (self, reread ?: connection, path, NULL, FALSE, NULL, error));
#endif
	return FALSE;
}

/*****************************************************************************/

static void
config_changed_cb (NMConfig *config,
                   NMConfigData *config_data,
                   NMConfigChangeFlags changes,
                   NMConfigData *old_data,
                   NMSKeyfilePlugin *self)
{
	gs_free char *old_value = NULL;
	gs_free char *new_value = NULL;

	old_value = nm_config_data_get_value (old_data,    NM_CONFIG_KEYFILE_GROUP_KEYFILE, NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES, NM_CONFIG_GET_VALUE_TYPE_SPEC);
	new_value = nm_config_data_get_value (config_data, NM_CONFIG_KEYFILE_GROUP_KEYFILE, NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES, NM_CONFIG_GET_VALUE_TYPE_SPEC);

	if (!nm_streq0 (old_value, new_value))
		_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NM_SETTINGS_PLUGIN (self));
}

static GSList *
get_unmanaged_specs (NMSettingsPlugin *config)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (config);
	gs_free char *value = NULL;

	value = nm_config_data_get_value (nm_config_get_data (priv->config),
	                                  NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                                  NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES,
	                                  NM_CONFIG_GET_VALUE_TYPE_SPEC);
	return nm_match_spec_split (value);
}

/*****************************************************************************/

#if 0
static void
connection_removed_cb (NMSettingsConnection *sett_conn, NMSKeyfilePlugin *self)
{
	g_hash_table_remove (NMS_KEYFILE_PLUGIN_GET_PRIVATE (self)->storages,
	                     nm_settings_connection_get_uuid (sett_conn));
}

/* Monitoring */

static void
remove_connection (NMSKeyfilePlugin *self, NMSKeyfileConnection *connection)
{
	gboolean removed;

	g_return_if_fail (connection != NULL);

	_LOGI ("removed " NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection));

	/* Removing from the hash table should drop the last reference */
	g_object_ref (connection);
	g_signal_handlers_disconnect_by_func (connection, connection_removed_cb, self);
	removed = g_hash_table_remove (NMS_KEYFILE_PLUGIN_GET_PRIVATE (self)->storages,
	                               nm_settings_connection_get_uuid (NM_SETTINGS_CONNECTION (connection)));
	nm_settings_connection_signal_remove (NM_SETTINGS_CONNECTION (connection));
	g_object_unref (connection);

	g_return_if_fail (removed);
}

static NMSKeyfileConnection *
find_by_path (NMSKeyfilePlugin *self, const char *path)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	GHashTableIter iter;
	NMSettingsConnection *candidate = NULL;

	g_return_val_if_fail (path != NULL, NULL);

	g_hash_table_iter_init (&iter, priv->storages);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &candidate)) {
		if (g_strcmp0 (path, nm_settings_connection_get_filename (candidate)) == 0)
			return NMS_KEYFILE_CONNECTION (candidate);
	}
	return NULL;
}

/* update_connection:
 * @self: the plugin instance
 * @source: if %NULL, this re-reads the connection from @full_path
 *   and updates it. When passing @source, this adds a connection from
 *   memory.
 * @full_path: the filename of the keyfile to be loaded
 * @connection: an existing connection that might be updated.
 *   If given, @connection must be an existing connection that is currently
 *   owned by the plugin.
 * @protect_existing_connection: if %TRUE, and !@connection, we don't allow updating
 *   an existing connection with the same UUID.
 *   If %TRUE and @connection, allow updating only if the reload would modify
 *   @connection (without changing its UUID) or if we would create a new connection.
 *   In other words, if this parameter is %TRUE, we only allow creating a
 *   new connection (with an unseen UUID) or updating the passed in @connection
 *   (whereas the UUID cannot change).
 *   Note, that this allows for @connection to be replaced by a new connection.
 * @protected_connections: (allow-none): if given, we only update an
 *   existing connection if it is not contained in this hash.
 * @error: error in case of failure
 *
 * Loads a connection from file @full_path. This can both be used to
 * load a connection initially or to update an existing connection.
 *
 * If you pass in an existing connection and the reloaded file happens
 * to have a different UUID, the connection is deleted.
 * Beware, that means that after the function, you have a dangling pointer
 * if the returned connection is different from @connection.
 *
 * Returns: the updated connection.
 * */
static NMSKeyfileConnection *
update_connection (NMSKeyfilePlugin *self,
                   NMConnection *source,
                   const char *full_path,
                   NMSKeyfileConnection *connection,
                   gboolean protect_existing_connection,
                   GHashTable *protected_connections,
                   GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileConnection *connection_new;
	NMSKeyfileConnection *connection_by_uuid;
	GError *local = NULL;
	const char *uuid;

	g_return_val_if_fail (!source || NM_IS_CONNECTION (source), NULL);
	g_return_val_if_fail (full_path || source, NULL);

	if (full_path)
		_LOGD ("loading from file \"%s\"...", full_path);

	if (   !nm_utils_file_is_in_path (full_path, nms_keyfile_utils_get_path ())
	    && !nm_utils_file_is_in_path (full_path, NM_KEYFILE_PATH_NAME_RUN)) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
		                     "File not in recognized system-connections directory");
		return FALSE;
	}

	connection_new = nms_keyfile_connection_new (source, full_path, nms_keyfile_utils_get_path (), &local);
	if (!connection_new) {
		/* Error; remove the connection */
		if (source)
			_LOGW ("error creating connection %s: %s", nm_connection_get_uuid (source), local->message);
		else
			_LOGW ("error loading connection from file %s: %s", full_path, local->message);
		if (   connection
		    && !protect_existing_connection
		    && (!protected_connections || !g_hash_table_contains (protected_connections, connection)))
			remove_connection (self, connection);
		g_propagate_error (error, local);
		return NULL;
	}

	uuid = nm_settings_connection_get_uuid (NM_SETTINGS_CONNECTION (connection_new));
	connection_by_uuid = g_hash_table_lookup (priv->storages, uuid);

	if (   connection
	    && connection != connection_by_uuid) {

		if (   (protect_existing_connection && connection_by_uuid != NULL)
		    || (protected_connections && g_hash_table_contains (protected_connections, connection))) {
			NMSKeyfileConnection *conflicting = (protect_existing_connection && connection_by_uuid != NULL) ? connection_by_uuid : connection;

			if (source)
				_LOGW ("cannot update protected "NMS_KEYFILE_CONNECTION_LOG_FMT" connection due to conflicting UUID %s", NMS_KEYFILE_CONNECTION_LOG_ARG (conflicting), uuid);
			else
				_LOGW ("cannot load %s due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, full_path, NMS_KEYFILE_CONNECTION_LOG_ARG (conflicting));
			g_object_unref (connection_new);
			g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
			                      "Cannot update protected connection due to conflicting UUID");
			return NULL;
		}

		/* The new connection has a different UUID then the original one.
		 * Remove @connection. */
		remove_connection (self, connection);
	}

	if (   connection_by_uuid
	    && (   (!connection && protect_existing_connection)
	        || (protected_connections && g_hash_table_contains (protected_connections, connection_by_uuid)))) {
		if (source)
			_LOGW ("cannot update connection due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_by_uuid));
		else
			_LOGW ("cannot load %s due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, full_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_by_uuid));
		g_object_unref (connection_new);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
		                      "Skip updating protected connection during reload");
		return NULL;
	}

	if (connection_by_uuid) {
		const char *old_path;

		old_path = nm_settings_connection_get_filename (NM_SETTINGS_CONNECTION (connection_by_uuid));

		if (nm_connection_compare (nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_by_uuid)),
		                           nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_new)),
		                           NM_SETTING_COMPARE_FLAG_IGNORE_AGENT_OWNED_SECRETS |
		                           NM_SETTING_COMPARE_FLAG_IGNORE_NOT_SAVED_SECRETS)) {
			/* Nothing to do... except updating the path. */
			if (old_path && g_strcmp0 (old_path, full_path) != 0)
				_LOGI ("rename \"%s\" to "NMS_KEYFILE_CONNECTION_LOG_FMT" without other changes", old_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		} else {
			/* An existing connection changed. */
			if (source)
				_LOGI ("update "NMS_KEYFILE_CONNECTION_LOG_FMT" from %s", NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new), NMS_KEYFILE_CONNECTION_LOG_PATH (old_path));
			else if (!g_strcmp0 (old_path, nm_settings_connection_get_filename (NM_SETTINGS_CONNECTION (connection_new))))
				_LOGI ("update "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
			else if (old_path)
				_LOGI ("rename \"%s\" to "NMS_KEYFILE_CONNECTION_LOG_FMT, old_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
			else
				_LOGI ("update and persist "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));

			if (!nm_settings_connection_update (NM_SETTINGS_CONNECTION (connection_by_uuid),
			                                    nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_new)),
			                                    NM_SETTINGS_CONNECTION_PERSIST_MODE_KEEP_SAVED,
			                                    NM_SETTINGS_CONNECTION_COMMIT_REASON_NONE,
			                                    "keyfile-update",
			                                    &local)) {
				/* Shouldn't ever get here as 'connection_new' was verified by the reader already
				 * and the UUID did not change. */
				g_assert_not_reached ();
			}
			g_assert_no_error (local);
		}
		nm_settings_connection_set_filename (NM_SETTINGS_CONNECTION (connection_by_uuid), full_path);
		g_object_unref (connection_new);
		return connection_by_uuid;
	} else {
		if (source)
			_LOGI ("add connection "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		else
			_LOGI ("new connection "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		g_hash_table_insert (priv->storages, g_strdup (uuid), connection_new);

		g_signal_connect (connection_new, NM_SETTINGS_CONNECTION_REMOVED,
		                  G_CALLBACK (connection_removed_cb),
		                  self);

		if (!source) {
			/* Only raise the signal if we were called without source, i.e. if we read the connection from file.
			 * Otherwise, we were called by add_connection() which does not expect the signal. */
			_nm_settings_plugin_emit_signal_connection_added (NM_SETTINGS_PLUGIN (self),
			                                                  NM_SETTINGS_CONNECTION (connection_new));
		}

		return connection_new;
	}
}
#endif

/*****************************************************************************/

static void
reload_connections (NMSettingsPlugin *plugin)
{
	_do_reload_all (NMS_KEYFILE_PLUGIN (plugin), FALSE);
}

static gboolean
load_connection (NMSettingsPlugin *plugin,
                 const char *filename,
                 NMSettingsStorage **out_storage,
                 NMConnection **out_connection,
                 GError **error)
{
	return _do_load_connection (NMS_KEYFILE_PLUGIN (plugin),
	                            filename,
	                            out_storage,
	                            out_connection,
	                            error);
}

static gboolean
add_connection (NMSettingsPlugin *config,
                NMConnection *connection,
                gboolean save_to_disk,
                NMSettingsStorage **out_storage,
                NMConnection **out_connection,
                GError **error)
{
	return _do_add_connection (NMS_KEYFILE_PLUGIN (config),
	                           connection,
	                           save_to_disk,
	                           out_storage,
	                           out_connection,
	                           error);
}

static gboolean
commit_changes (NMSettingsPlugin *plugin,
                NMSettingsStorage *storage,
                NMConnection *connection,
                NMSettingsStorageCommitReason commit_reason,
                //XXX unused
                NMConnection **out_reread_connection,
                char **out_logmsg_change,
                GError **error)
{
	return _do_commit_changes (NMS_KEYFILE_PLUGIN (plugin),
	                           NMS_KEYFILE_STORAGE (storage),
	                           connection,
	                           commit_reason,
	                             COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_ETC
	                           | COMMIT_CHANGES_FLAG_ALLOW_STORAGE_TYPE_RUN,
	                           error);
}

static gboolean
delete (NMSettingsPlugin *plugin,
        NMSettingsStorage *storage,
        GError **error)
{
	return _do_delete (NMS_KEYFILE_PLUGIN (plugin),
	                   NMS_KEYFILE_STORAGE (storage),
	                   error);
}

/*****************************************************************************/

static void
nms_keyfile_plugin_init (NMSKeyfilePlugin *plugin)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (plugin);

	priv->config = g_object_ref (nm_config_get ());

	c_list_init (&priv->storages.lst_head);
	priv->storages.filename_idx = g_hash_table_new (nm_str_hash, g_str_equal);
	priv->storages.idx = g_hash_table_new_full ((GHashFunc) _storage_idx_hash,
	                                            (GEqualFunc) _storage_idx_equal,
	                                            (GDestroyNotify) _storage_destroy,
	                                            NULL);

	/* dirname_libs are a set of read-only directories with lower priority than /etc or /run.
	 * There is nothing complicated about having multiple of such directories, so dirname_libs
	 * is a list (which currently only has at most one directory). */
	priv->dirname_libs = g_new0 (char *, 2);
	priv->dirname_libs[0] = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_LIB), FALSE);
	priv->dirname_run = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_RUN), FALSE);
	priv->dirname_etc = nm_config_data_get_value (NM_CONFIG_GET_DATA_ORIG,
	                                              NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                                              NM_CONFIG_KEYFILE_KEY_KEYFILE_PATH,
	                                              NM_CONFIG_GET_VALUE_STRIP);
	if (priv->dirname_etc && priv->dirname_etc[0] == '\0') {
		/* special case: configure an empty keyfile path so that NM has no writable keyfile
		 * directory. In this case, NM will only honor dirname_libs and dirname_run, meaning
		 * it cannot persist profile to non-volatile memory. */
		nm_clear_g_free (&priv->dirname_etc);
	} else if (!priv->dirname_etc || priv->dirname_etc[0] != '/') {
		/* either invalid path or unspecified. Use the default. */
		g_free (priv->dirname_etc);
		priv->dirname_etc = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_ETC_DEFAULT), FALSE);
	} else
		nm_sd_utils_path_simplify (priv->dirname_etc, FALSE);

	/* no duplicates */
	if (NM_IN_STRSET (priv->dirname_libs[0], priv->dirname_etc, priv->dirname_run))
		nm_clear_g_free (&priv->dirname_libs[0]);
	if (NM_IN_STRSET (priv->dirname_etc, priv->dirname_run))
		nm_clear_g_free (&priv->dirname_etc);
}

static void
constructed (GObject *object)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (object);
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);

	G_OBJECT_CLASS (nms_keyfile_plugin_parent_class)->constructed (object);

	if (nm_config_data_has_value (nm_config_get_data_orig (priv->config),
	                              NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                              NM_CONFIG_KEYFILE_KEY_KEYFILE_HOSTNAME,
	                              NM_CONFIG_GET_VALUE_RAW))
		_LOGW ("'hostname' option is deprecated and has no effect");

	if (nm_config_data_has_value (nm_config_get_data_orig (priv->config),
	                              NM_CONFIG_KEYFILE_GROUP_MAIN,
	                              NM_CONFIG_KEYFILE_KEY_MAIN_MONITOR_CONNECTION_FILES,
	                              NM_CONFIG_GET_VALUE_RAW))
		_LOGW ("'monitor-connection-files' option is deprecated and has no effect");

	g_signal_connect (G_OBJECT (priv->config),
	                  NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_CALLBACK (config_changed_cb),
	                  self);
}

NMSKeyfilePlugin *
nms_keyfile_plugin_new (void)
{
	return g_object_new (NMS_TYPE_KEYFILE_PLUGIN, NULL);
}

static void
dispose (GObject *object)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (object);
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);

	nm_clear_pointer (&priv->storages.filename_idx, g_hash_table_destroy);
	nm_clear_pointer (&priv->storages.idx, g_hash_table_destroy);

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config, config_changed_cb, object);
		g_clear_object (&priv->config);
	}

	nm_clear_pointer (&priv->dirname_libs, g_strfreev);
	nm_clear_g_free (&priv->dirname_etc);
	nm_clear_g_free (&priv->dirname_run);

	G_OBJECT_CLASS (nms_keyfile_plugin_parent_class)->dispose (object);
}

static void
nms_keyfile_plugin_class_init (NMSKeyfilePluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMSettingsPluginClass *plugin_class = NM_SETTINGS_PLUGIN_CLASS (klass);

	object_class->constructed = constructed;
	object_class->dispose     = dispose;

	plugin_class->get_unmanaged_specs = get_unmanaged_specs;
	plugin_class->reload_connections  = reload_connections;
	plugin_class->load_connection     = load_connection;
	plugin_class->add_connection      = add_connection;
	plugin_class->commit_changes      = commit_changes;
	plugin_class->delete              = delete;
}
