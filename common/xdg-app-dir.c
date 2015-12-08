/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#include <gio/gio.h>
#include <libsoup/soup.h>
#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-dir.h"
#include "xdg-app-utils.h"
#include "xdg-app-run.h"

#include "errno.h"

struct XdgAppDir {
  GObject parent;

  gboolean user;
  GFile *basedir;
  OstreeRepo *repo;

  SoupSession *soup_session;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppDirClass;

struct XdgAppDeploy {
  GObject parent;

  GFile *dir;
  GKeyFile *metadata;
  XdgAppContext *system_overrides;
  XdgAppContext *user_overrides;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppDeployClass;


G_DEFINE_TYPE (XdgAppDir, xdg_app_dir, G_TYPE_OBJECT)
G_DEFINE_TYPE (XdgAppDeploy, xdg_app_deploy, G_TYPE_OBJECT)

G_DEFINE_QUARK (xdg-app-dir-error-quark, xdg_app_dir_error)

enum {
  PROP_0,

  PROP_USER,
  PROP_PATH
};

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static void
xdg_app_deploy_finalize (GObject *object)
{
  XdgAppDeploy *self = XDG_APP_DEPLOY (object);

  g_clear_object (&self->dir);
  g_clear_pointer (&self->metadata, g_key_file_unref);
  g_clear_pointer (&self->system_overrides, g_key_file_unref);
  g_clear_pointer (&self->user_overrides, g_key_file_unref);

  G_OBJECT_CLASS (xdg_app_deploy_parent_class)->finalize (object);
}

static void
xdg_app_deploy_class_init (XdgAppDeployClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdg_app_deploy_finalize;

}

static void
xdg_app_deploy_init (XdgAppDeploy *self)
{
}

GFile *
xdg_app_deploy_get_dir (XdgAppDeploy *deploy)
{
  return g_object_ref (deploy->dir);
}

GFile *
xdg_app_deploy_get_files (XdgAppDeploy *deploy)
{
  return g_file_get_child (deploy->dir, "files");
}

XdgAppContext *
xdg_app_deploy_get_overrides (XdgAppDeploy *deploy)
{
  XdgAppContext *overrides = xdg_app_context_new ();

  if (deploy->system_overrides)
    xdg_app_context_merge (overrides, deploy->system_overrides);

  if (deploy->user_overrides)
    xdg_app_context_merge (overrides, deploy->user_overrides);

  return overrides;
}

GKeyFile *
xdg_app_deploy_get_metadata (XdgAppDeploy *deploy)
{
  return g_key_file_ref (deploy->metadata);
}

static XdgAppDeploy *
xdg_app_deploy_new (GFile *dir, GKeyFile *metadata)
{
  XdgAppDeploy *deploy;

  deploy = g_object_new (XDG_APP_TYPE_DEPLOY, NULL);
  deploy->dir = g_object_ref (dir);
  deploy->metadata = g_key_file_ref (metadata);

  return deploy;
}

GFile *
xdg_app_get_system_base_dir_location (void)
{
  return g_file_new_for_path (XDG_APP_SYSTEMDIR);
}

GFile *
xdg_app_get_user_base_dir_location (void)
{
  g_autofree char *base = g_build_filename (g_get_user_data_dir (), "xdg-app", NULL);
  return g_file_new_for_path (base);
}

static void
xdg_app_dir_finalize (GObject *object)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);

  g_clear_object (&self->soup_session);

  G_OBJECT_CLASS (xdg_app_dir_parent_class)->finalize (object);
}

static void
xdg_app_dir_set_property(GObject         *object,
                         guint            prop_id,
                         const GValue    *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->basedir = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
      break;
    case PROP_USER:
      self->user = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_get_property(GObject         *object,
                         guint            prop_id,
                         GValue          *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->basedir);
      break;
    case PROP_USER:
      g_value_set_boolean (value, self->user);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_class_init (XdgAppDirClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_dir_get_property;
  object_class->set_property = xdg_app_dir_set_property;
  object_class->finalize = xdg_app_dir_finalize;

  g_object_class_install_property (object_class,
                                   PROP_USER,
                                   g_param_spec_boolean ("user",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
xdg_app_dir_init (XdgAppDir *self)
{
}

gboolean
xdg_app_dir_is_user (XdgAppDir *self)
{
  return self->user;
}

GFile *
xdg_app_dir_get_path (XdgAppDir *self)
{
  return self->basedir;
}

GKeyFile *
xdg_app_load_override_keyfile (const char *app_id, gboolean user, GError **error)
{
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) override_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;
  g_autoptr(GKeyFile) metakey = g_key_file_new ();

  if (user)
    base_dir = xdg_app_get_user_base_dir_location ();
  else
    base_dir = xdg_app_get_system_base_dir_location ();

  override_dir = g_file_get_child (base_dir, "overrides");
  file = g_file_get_child (override_dir, app_id);

  if (g_file_load_contents (file, NULL,
                            &metadata_contents, &metadata_size, NULL, NULL))
    {

      if (!g_key_file_load_from_data (metakey,
                                      metadata_contents, metadata_size,
                                      0, error))
        return NULL;
    }

  return g_steal_pointer (&metakey);
}

XdgAppContext *
xdg_app_load_override_file (const char *app_id, gboolean user, GError **error)
{
  XdgAppContext *overrides = xdg_app_context_new ();
  g_autoptr(GKeyFile) metakey = NULL;

  metakey = xdg_app_load_override_keyfile (app_id, user, error);
  if (metakey == NULL)
    return NULL;

  if (!xdg_app_context_load_metadata (overrides, metakey, error))
    return NULL;

  return g_steal_pointer (&overrides);
}

gboolean
xdg_app_save_override_keyfile (GKeyFile    *metakey,
                               const char       *app_id,
                               gboolean          user,
                               GError          **error)
{
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) override_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *parent = NULL;

  if (user)
    base_dir = xdg_app_get_user_base_dir_location ();
  else
    base_dir = xdg_app_get_system_base_dir_location ();

  override_dir = g_file_get_child (base_dir, "overrides");
  file = g_file_get_child (override_dir, app_id);

  filename = g_file_get_path (file);
  parent = g_path_get_dirname (filename);
  if (g_mkdir_with_parents  (parent, 0755))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return g_key_file_save_to_file (metakey, filename, error);
}

XdgAppDeploy *
xdg_app_dir_load_deployed (XdgAppDir    *self,
                           const char   *ref,
                           const char   *checksum,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_auto(GStrv) ref_parts = NULL;
  g_autofree char *metadata_contents = NULL;
  XdgAppDeploy *deploy;
  gsize metadata_size;

  deploy_dir = xdg_app_dir_get_if_deployed (self, ref, checksum, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, XDG_APP_DIR_ERROR, XDG_APP_DIR_ERROR_NOT_DEPLOYED, "%s not installed", ref);
      return NULL;
    }

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return NULL;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return NULL;

  deploy = xdg_app_deploy_new (deploy_dir, metakey);

  ref_parts = g_strsplit (ref, "/", -1);
  g_assert (g_strv_length (ref_parts) == 4);

  /* Only apps have overrides */
  if (strcmp (ref_parts[0], "app") == 0)
    {
      /* Only load system overrides for system installed apps */
      if (!self->user)
        {
          deploy->system_overrides = xdg_app_load_override_file (ref_parts[1], FALSE, error);
          if (deploy->system_overrides == NULL)
            return NULL;
        }

      /* Always load user overrides */
      deploy->user_overrides = xdg_app_load_override_file (ref_parts[1], TRUE, error);
      if (deploy->user_overrides == NULL)
        return NULL;
    }

  return deploy;
}

GFile *
xdg_app_dir_get_deploy_dir (XdgAppDir     *self,
                            const char    *ref)
{
  return g_file_resolve_relative_path (self->basedir, ref);
}

GFile *
xdg_app_dir_get_exports_dir (XdgAppDir     *self)
{
  return g_file_get_child (self->basedir, "exports");
}

GFile *
xdg_app_dir_get_removed_dir (XdgAppDir     *self)
{
  return g_file_get_child (self->basedir, ".removed");
}

OstreeRepo *
xdg_app_dir_get_repo (XdgAppDir *self)
{
  return self->repo;
}

char *
xdg_app_dir_get_origin (XdgAppDir      *self,
                        const char     *ref,
                        GCancellable   *cancellable,
                        GError        **error)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) origin = NULL;
  char *repository = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      xdg_app_fail (error, "%s is not installed", ref);
      return NULL;
    }

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
    return NULL;

  return repository;
}

gboolean
xdg_app_dir_ensure_path (XdgAppDir     *self,
                         GCancellable  *cancellable,
                         GError       **error)
{
  return gs_file_ensure_directory (self->basedir, TRUE, cancellable, error);
}

gboolean
xdg_app_dir_ensure_repo (XdgAppDir *self,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) repodir = NULL;
  g_autoptr(OstreeRepo) repo = NULL;

  if (self->repo == NULL)
    {
      if (!xdg_app_dir_ensure_path (self, cancellable, error))
        goto out;

      repodir = g_file_get_child (self->basedir, "repo");
      repo = ostree_repo_new (repodir);

      if (!g_file_query_exists (repodir, cancellable))
        {
          if (!ostree_repo_create (repo,
                                   self->user ? OSTREE_REPO_MODE_BARE_USER : OSTREE_REPO_MODE_BARE,
                                   cancellable, error))
            {
              gs_shutil_rm_rf (repodir, cancellable, NULL);
              goto out;
            }
        }
      else
        {
          if (!ostree_repo_open (repo, cancellable, error))
            {
              g_autofree char *repopath = NULL;

              repopath = g_file_get_path (repodir);
              g_prefix_error (error, "While opening repository %s: ", repopath);
              goto out;
            }
        }

      self->repo = g_object_ref (repo);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_pull (XdgAppDir *self,
                  const char *repository,
                  const char *ref,
                  GCancellable *cancellable,
                  GError **error)
{
  gboolean ret = FALSE;
  GSConsole *console = NULL;
  g_autoptr(OstreeAsyncProgress) progress = NULL;
  const char *refs[2];

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
    }

  refs[0] = ref;
  refs[1] = NULL;
  if (!ostree_repo_pull (self->repo, repository,
                         (char **)refs, OSTREE_REPO_PULL_FLAGS_NONE,
                         progress,
                         cancellable, error))
    {
      g_prefix_error (error, "While pulling %s from remote %s: ", ref, repository);
      goto out;
    }

  if (console)
    gs_console_end_status_line (console, NULL, NULL);

  ret = TRUE;
 out:
  return ret;
}

char *
xdg_app_dir_current_ref (XdgAppDir *self,
                         const char *name,
                         GCancellable *cancellable)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  base = g_file_get_child (xdg_app_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

  current_link = g_file_get_child (dir, "current");

  file_info = g_file_query_info (current_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strconcat ("app/", name, "/", g_file_info_get_symlink_target (file_info), NULL);
}

gboolean
xdg_app_dir_drop_current_ref (XdgAppDir *self,
                              const char *name,
                              GCancellable *cancellable,
                              GError **error)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;

  base = g_file_get_child (xdg_app_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

  current_link = g_file_get_child (dir, "current");

  return g_file_delete (current_link, cancellable, error);
}

gboolean
xdg_app_dir_make_current_ref (XdgAppDir *self,
                              const char *ref,
                              GCancellable *cancellable,
                              GError **error)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_auto(GStrv) ref_parts = NULL;
  g_autofree char *rest = NULL;
  gboolean ret = FALSE;

  ref_parts = g_strsplit (ref, "/", -1);

  g_assert (g_strv_length (ref_parts) == 4);
  g_assert (strcmp (ref_parts[0], "app") == 0);

  base = g_file_get_child (xdg_app_dir_get_path (self), ref_parts[0]);
  dir = g_file_get_child (base, ref_parts[1]);

  current_link = g_file_get_child (dir, "current");

  g_file_delete (current_link, cancellable, NULL);

  if (*ref_parts[3] != 0)
    {
      rest = g_strdup_printf ("%s/%s", ref_parts[2], ref_parts[3]);
      if (!g_file_make_symbolic_link (current_link, rest, cancellable, error))
        goto out;
    }

  ret = TRUE;

 out:
  return ret;
}

static int
strvcmp(char **a, char **b)
{
  return strcmp (*a, *b);
}

gboolean
xdg_app_dir_list_refs_for_name (XdgAppDir      *self,
                                const char     *kind,
                                const char     *name,
                                char         ***refs_out,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  base = g_file_get_child (xdg_app_dir_get_path (self), kind);
  dir = g_file_get_child (base, name);

  refs = g_ptr_array_new ();

  if (!g_file_query_exists (dir, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
    {
      g_autoptr(GFile) child = NULL;
      g_autoptr(GFileEnumerator) dir_enum2 = NULL;
      g_autoptr(GFileInfo) child_info2 = NULL;
      const char *arch;

      arch = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY ||
          strcmp (arch, "data") == 0 /* There used to be a data dir here, lets ignore it */)
        {
          g_clear_object (&child_info);
          continue;
        }

      child = g_file_get_child (dir, arch);
      g_clear_object (&dir_enum2);
      dir_enum2 = g_file_enumerate_children (child, OSTREE_GIO_FAST_QUERYINFO,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             cancellable, error);
      if (!dir_enum2)
        goto out;

      while ((child_info2 = g_file_enumerator_next_file (dir_enum2, cancellable, &temp_error)))
        {
          const char *branch;

          if (g_file_info_get_file_type (child_info2) == G_FILE_TYPE_DIRECTORY)
            {
              branch = g_file_info_get_name (child_info2);
              g_ptr_array_add (refs,
                               g_strdup_printf ("%s/%s/%s/%s", kind, name, arch, branch));
            }

          g_clear_object (&child_info2);
        }


      if (temp_error != NULL)
        goto out;

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    goto out;

  g_ptr_array_sort (refs, (GCompareFunc)strvcmp);

  ret = TRUE;

out:
  if (ret)
    {
      g_ptr_array_add (refs, NULL);
      *refs_out = (char **)g_ptr_array_free (refs, FALSE);
      refs = NULL;
    }

  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

gboolean
xdg_app_dir_list_refs (XdgAppDir      *self,
                       const char     *kind,
                       char         ***refs_out,
                       GCancellable   *cancellable,
                       GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) base;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  refs = g_ptr_array_new ();

  base = g_file_get_child (xdg_app_dir_get_path (self), kind);

  if (!g_file_query_exists (base, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dir_enum = g_file_enumerate_children (base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
    {
      gchar **sub_refs = NULL;
      const char *name;
      int i;

      if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&child_info);
          continue;
        }

      name = g_file_info_get_name (child_info);

      if (!xdg_app_dir_list_refs_for_name (self, kind, name, &sub_refs, cancellable, error))
        goto out;

      for (i = 0; sub_refs[i] != NULL; i++)
        g_ptr_array_add (refs, sub_refs[i]);
      g_free (sub_refs);

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    goto out;

  ret = TRUE;

  g_ptr_array_sort (refs, (GCompareFunc)strvcmp);

out:
  if (ret)
    {
      g_ptr_array_add (refs, NULL);
      *refs_out = (char **)g_ptr_array_free (refs, FALSE);
      refs = NULL;
    }

  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

char *
xdg_app_dir_read_active (XdgAppDir *self,
                         const char *ref,
                         GCancellable *cancellable)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strdup (g_file_info_get_symlink_target (file_info));
}

gboolean
xdg_app_dir_set_active (XdgAppDir *self,
                        const char *ref,
                        const char *checksum,
                        GCancellable *cancellable,
                        GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autofree char *tmpname = NULL;
  g_autoptr(GFile) active_tmp_link = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autoptr (GError) my_error = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  if (checksum != NULL)
    {
      tmpname = gs_fileutil_gen_tmp_name (".active-", NULL);
      active_tmp_link = g_file_get_child (deploy_base, tmpname);
      if (!g_file_make_symbolic_link (active_tmp_link, checksum, cancellable, error))
        goto out;

      if (!gs_file_rename (active_tmp_link,
                           active_link,
                           cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_delete (active_link, cancellable, &my_error) &&
          !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, my_error);
          my_error = NULL;
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}


gboolean
xdg_app_dir_run_triggers (XdgAppDir *self,
			  GCancellable *cancellable,
			  GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GFile) triggersdir = NULL;
  GError *temp_error = NULL;

  g_debug ("running triggers");

  triggersdir = g_file_new_for_path (XDG_APP_TRIGGERDIR);

  dir_enum = g_file_enumerate_children (triggersdir, "standard::type,standard::name",
                                        0, cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      g_autoptr(GFile) child = NULL;
      const char *name;
      GError *trigger_error = NULL;

      name = g_file_info_get_name (child_info);

      child = g_file_get_child (triggersdir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_REGULAR &&
	  g_str_has_suffix (name, ".trigger"))
	{
	  g_autoptr(GPtrArray) argv_array = NULL;

	  g_debug ("running trigger %s", name);

	  argv_array = g_ptr_array_new_with_free_func (g_free);
	  g_ptr_array_add (argv_array, g_strdup (HELPER));
	  g_ptr_array_add (argv_array, g_strdup ("-a"));
	  g_ptr_array_add (argv_array, g_file_get_path (self->basedir));
	  g_ptr_array_add (argv_array, g_strdup ("-e"));
	  g_ptr_array_add (argv_array, g_strdup ("-F"));
	  g_ptr_array_add (argv_array, g_strdup ("/usr"));
	  g_ptr_array_add (argv_array, g_file_get_path (child));
	  g_ptr_array_add (argv_array, NULL);

	  if (!g_spawn_sync ("/",
			     (char **)argv_array->pdata,
			     NULL,
			     G_SPAWN_DEFAULT,
			     NULL, NULL,
			     NULL, NULL,
			     NULL, &trigger_error))
	    {
	      g_warning ("Error running trigger %s: %s", name, trigger_error->message);
	      g_clear_error (&trigger_error);
	    }
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
read_fd (int           fd,
         struct stat  *stat_buf,
         gchar       **contents,
         gsize        *length,
         GError      **error)
{
  gchar *buf;
  gsize bytes_read;
  gsize size;
  gsize alloc_size;

  size = stat_buf->st_size;

  alloc_size = size + 1;
  buf = g_try_malloc (alloc_size);

  if (buf == NULL)
    {
      g_set_error (error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOMEM,
                   "not enough memory");
      return FALSE;
    }

  bytes_read = 0;
  while (bytes_read < size)
    {
      gssize rc;

      rc = read (fd, buf + bytes_read, size - bytes_read);

      if (rc < 0)
        {
          if (errno != EINTR)
            {
              int save_errno = errno;

              g_free (buf);
              g_set_error (error,
                           G_FILE_ERROR,
                           g_file_error_from_errno (save_errno),
                           "Failed to read from exported file");
              return FALSE;
            }
        }
      else if (rc == 0)
        break;
      else
        bytes_read += rc;
    }

  buf[bytes_read] = '\0';

  if (length)
    *length = bytes_read;

  *contents = buf;

  return TRUE;
}

/* This is conservative, but lets us avoid escaping most
   regular Exec= lines, which is nice as that can sometimes
   cause problems for apps launching desktop files. */
static gboolean
need_quotes (const char *str)
{
  const char *p;

  for (p = str; *p; p++)
    {
      if (!g_ascii_isalnum (*p) &&
          strchr ("-_%.=:/@", *p) == NULL)
        return TRUE;
    }

  return FALSE;
}

static char *
maybe_quote (const char *str)
{
  if (need_quotes (str))
    return g_shell_quote (str);
  return g_strdup (str);
}

static gboolean
export_desktop_file (const char    *app,
                     const char    *branch,
                     const char    *arch,
                     int            parent_fd,
                     const char    *name,
                     struct stat   *stat_buf,
                     char         **target,
                     GCancellable  *cancellable,
                     GError       **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int desktop_fd = -1;
  g_autofree char *tmpfile_name = NULL;
  g_autoptr(GOutputStream) out_stream = NULL;
  g_autofree gchar *data = NULL;
  gsize data_len;
  g_autofree gchar *new_data = NULL;
  gsize new_data_len;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree gchar *old_exec = NULL;
  gint old_argc;
  g_auto(GStrv) old_argv = NULL;
  g_auto(GStrv) groups = NULL;
  GString *new_exec = NULL;
  g_autofree char *escaped_app = maybe_quote (app);
  g_autofree char *escaped_branch = maybe_quote (branch);
  g_autofree char *escaped_arch = maybe_quote (arch);
  int i;

  if (!gs_file_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error))
    goto out;

  if (!read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    goto out;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, data, data_len, G_KEY_FILE_KEEP_TRANSLATIONS, error))
    goto out;

  if (g_str_has_suffix (name, ".service"))
    {
      g_autofree gchar *dbus_name = NULL;
      g_autofree gchar *expected_dbus_name = g_strndup (name, strlen (name) - strlen (".service"));

      dbus_name = g_key_file_get_string (keyfile, "D-BUS Service", "Name", NULL);

      if (dbus_name == NULL || strcmp (dbus_name, expected_dbus_name) != 0)
        {
          xdg_app_fail (error, "dbus service file %s has wrong name", name);
          return FALSE;
        }
    }

  groups = g_key_file_get_groups (keyfile, NULL);

  for (i = 0; groups[i] != NULL; i++)
    {
      g_key_file_remove_key (keyfile, groups[i], "TryExec", NULL);

      /* Remove this to make sure nothing tries to execute it outside the sandbox*/
      g_key_file_remove_key (keyfile, groups[i], "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

      new_exec = g_string_new ("");
      g_string_append_printf (new_exec, XDG_APP_BINDIR"/xdg-app run --branch=%s --arch=%s", escaped_branch, escaped_arch);

      old_exec = g_key_file_get_string (keyfile, groups[i], "Exec", NULL);
      if (old_exec && g_shell_parse_argv (old_exec, &old_argc, &old_argv, NULL) && old_argc >= 1)
        {
          int i;
          g_autofree char *command = maybe_quote (old_argv[0]);

          g_string_append_printf (new_exec, " --command=%s", command);

          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);

          for (i = 1; i < old_argc; i++)
            {
              g_autofree char *arg = maybe_quote (old_argv[i]);
              g_string_append (new_exec, " ");
              g_string_append (new_exec, arg);
            }
        }
      else
        {
          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);
        }

      g_key_file_set_string (keyfile, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, new_exec->str);
    }

  new_data = g_key_file_to_data (keyfile, &new_data_len, error);
  if (new_data == NULL)
    goto out;

  if (!gs_file_open_in_tmpdir_at (parent_fd, 0755, &tmpfile_name, &out_stream, cancellable, error))
    goto out;

  if (!g_output_stream_write_all (out_stream, new_data, new_data_len, NULL, cancellable, error))
    goto out;

  if (!g_output_stream_close (out_stream, cancellable, error))
    goto out;

  if (target)
    *target = g_steal_pointer (&tmpfile_name);

  ret = TRUE;
 out:

  if (new_exec != NULL)
    g_string_free (new_exec, TRUE);

  return ret;
}

static gboolean
rewrite_export_dir (const char    *app,
                    const char    *branch,
                    const char    *arch,
                    int            source_parent_fd,
                    const char    *source_name,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  g_auto(GLnxDirFdIterator) source_iter = {0};
  g_autoptr(GHashTable) visited_children = NULL;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  visited_children = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (g_hash_table_contains (visited_children, dent->d_name))
          continue;

      /* Avoid processing the same file again if it was re-created during an export */
      g_hash_table_insert (visited_children, g_strdup (dent->d_name), GINT_TO_POINTER(1));

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          if (!rewrite_export_dir (app, branch, arch,
                                   source_iter.fd, dent->d_name,
                                   cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          if (!xdg_app_has_name_prefix (dent->d_name, app))
            {
              g_warning ("Non-prefixed filename %s in app %s, removing.\n", dent->d_name, app);
              if (unlinkat (source_iter.fd, dent->d_name, 0) != 0 && errno != ENOENT)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }

          if (g_str_has_suffix (dent->d_name, ".desktop") || g_str_has_suffix (dent->d_name, ".service"))
            {
              g_autofree gchar *new_name = NULL;

              if (!export_desktop_file (app, branch, arch, source_iter.fd, dent->d_name, &stbuf, &new_name, cancellable, error))
                goto out;

              g_hash_table_insert (visited_children, g_strdup (new_name), GINT_TO_POINTER(1));

              if (renameat (source_iter.fd, new_name, source_iter.fd, dent->d_name) != 0)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
        }
      else
        {
          g_warning ("Not exporting file %s of unsupported type\n", dent->d_name);
          if (unlinkat (source_iter.fd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
 out:

  return ret;
}

gboolean
xdg_app_rewrite_export_dir (const char *app,
                            const char *branch,
                            const char *arch,
                            GFile    *source,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;

  /* The fds are closed by this call */
  if (!rewrite_export_dir (app, branch, arch,
                           AT_FDCWD, gs_file_get_path_cached (source),
                           cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}


static gboolean
export_dir (int            source_parent_fd,
            const char    *source_name,
            const char    *source_symlink_prefix,
            const char    *source_relpath,
            int            destination_parent_fd,
            const char    *destination_name,
            GCancellable  *cancellable,
            GError       **error)
{
  gboolean ret = FALSE;
  int res;
  g_auto(GLnxDirFdIterator) source_iter = {0};
  glnx_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0755);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree gchar *child_symlink_prefix = g_build_filename ("..", source_symlink_prefix, dent->d_name, NULL);
          g_autofree gchar *child_relpath = g_strconcat (source_relpath, dent->d_name, "/", NULL);

          if (!export_dir (source_iter.fd, dent->d_name, child_symlink_prefix, child_relpath, destination_dfd, dent->d_name,
                           cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          g_autofree gchar *target = NULL;

          target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          if (unlinkat (destination_dfd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          if (symlinkat (target, destination_dfd, dent->d_name) != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
 out:

  return ret;
}

gboolean
xdg_app_export_dir (GFile    *source,
                    GFile    *destination,
                    const char *symlink_prefix,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!export_dir (AT_FDCWD, gs_file_get_path_cached (source), symlink_prefix, "",
                   AT_FDCWD, gs_file_get_path_cached (destination),
                   cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

gboolean
xdg_app_dir_update_exports (XdgAppDir *self,
                            const char *changed_app,
                            GCancellable *cancellable,
                            GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) exports = NULL;
  g_autofree char *current_ref = NULL;
  g_autofree char *active_id = NULL;
  g_autofree char *symlink_prefix = NULL;

  exports = xdg_app_dir_get_exports_dir (self);

  if (!gs_file_ensure_directory (exports, TRUE, cancellable, error))
    goto out;

  if (changed_app &&
      (current_ref = xdg_app_dir_current_ref (self, changed_app, cancellable)) &&
      (active_id = xdg_app_dir_read_active (self, current_ref, cancellable)))
    {
      g_autoptr(GFile) deploy_base = NULL;
      g_autoptr(GFile) active = NULL;
      g_autoptr(GFile) export = NULL;

      deploy_base = xdg_app_dir_get_deploy_dir (self, current_ref);
      active = g_file_get_child (deploy_base, active_id);
      export = g_file_get_child (active, "export");

      if (g_file_query_exists (export, cancellable))
        {
          symlink_prefix = g_build_filename ("..", "app", changed_app, "current", "active", "export", NULL);
          if (!xdg_app_export_dir (export, exports,
                                   symlink_prefix,
                                   cancellable,
                                   error))
            goto out;
        }
    }

  if (!xdg_app_remove_dangling_symlinks (exports, cancellable, error))
    goto out;

  if (!xdg_app_dir_run_triggers (self, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

gboolean
xdg_app_dir_deploy (XdgAppDir *self,
                    const char *ref,
                    const char *checksum,
                    GCancellable *cancellable,
                    GError **error)
{
  gboolean ret = FALSE;
  g_autofree char *resolved_ref = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) dotref = NULL;
  g_autoptr(GFile) export = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  if (checksum == NULL)
    {
      g_autofree char *origin = xdg_app_dir_get_origin (self, ref, NULL, NULL);
      g_autofree char *origin_and_ref = NULL;

      /* There may be several remotes with the same branch (if we for
       * instance changed the origin, so prepend the current origin to
       * make sure we get the right one */

      if (origin)
        origin_and_ref = g_strdup_printf ("%s:%s", origin, ref);
      else
        origin_and_ref = g_strdup (ref);

      g_debug ("No checksum specified, getting tip of %s", origin_and_ref);
      if (!ostree_repo_resolve_rev (self->repo, origin_and_ref, FALSE, &resolved_ref, error))
        {
          g_prefix_error (error, "While trying to resolve ref %s: ", ref);
          goto out;
        }

      checksum = resolved_ref;
      g_debug ("tip resolved to: %s", checksum);
    }
  else
    {
      g_autoptr(GFile) root = NULL;
      g_autofree char *commit = NULL;

      g_debug ("Looking for checksum %s in local repo", checksum);
      if (!ostree_repo_read_commit (self->repo, checksum, &root, &commit, cancellable, NULL))
        {
           GSConsole *console = NULL;
           g_autoptr(OstreeAsyncProgress) progress = NULL;
           const char *refs[2];
           g_autoptr(GFile) origin = NULL;
           g_autofree char *repository = NULL;

           refs[0] = checksum;
           refs[1] = NULL;

           origin = g_file_get_child (deploy_base, "origin");
           if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
             goto out;

           g_debug ("Pulling checksum %s from remote %s", checksum, repository);

           console = gs_console_get ();
           if (console)
             {
               gs_console_begin_status_line (console, "", NULL, NULL);
               progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
             }

           if (!ostree_repo_pull (self->repo, repository,
                                  (char **)refs, OSTREE_REPO_PULL_FLAGS_NONE,
                                  progress,
                                  cancellable, error))
             {
               g_prefix_error (error, "Failed to pull %s from remote %s: ", checksum, repository);
               goto out;
             }
        }
    }

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
                   "%s version %s already deployed", ref, checksum);
      goto out;
    }

  if (!ostree_repo_read_commit (self->repo, checksum, &root, NULL, cancellable, error))
    {
      g_prefix_error (error, "Failed to read commit %s: ", checksum);
      goto out;
    }

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (file_info == NULL)
    goto out;

  if (!ostree_repo_checkout_tree (self->repo,
                                  self->user ? OSTREE_REPO_CHECKOUT_MODE_USER : OSTREE_REPO_CHECKOUT_MODE_NONE,
                                  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                  checkoutdir,
                                  OSTREE_REPO_FILE (root), file_info,
                                  cancellable, error))
    {
      g_autofree char *rootpath = NULL;
      g_autofree char *checkoutpath = NULL;

      rootpath = g_file_get_path (root);
      checkoutpath = g_file_get_path (checkoutdir);
      g_prefix_error (error, "While trying to checkout %s into %s: ", rootpath, checkoutpath);
      goto out;
    }

  dotref = g_file_resolve_relative_path (checkoutdir, "files/.ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, cancellable, error))
    goto out;

  export = g_file_get_child (checkoutdir, "export");
  if (g_file_query_exists (export, cancellable))
    {
      g_auto(GStrv) ref_parts = NULL;

      ref_parts = g_strsplit (ref, "/", -1);

      if (!xdg_app_rewrite_export_dir (ref_parts[1], ref_parts[3], ref_parts[2], export,
                                       cancellable,
                                       error))
        goto out;
    }

  if (!xdg_app_dir_set_active (self, ref, checksum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_collect_deployed_refs (XdgAppDir *self,
				   const char *type,
				   const char *name_prefix,
				   const char *branch,
				   const char *arch,
				   GHashTable *hash,
				   GCancellable *cancellable,
				   GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  dir = g_file_get_child (self->basedir, type);
  if (!g_file_query_exists (dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' && (name_prefix == NULL || g_str_has_prefix (name, name_prefix)))
	{
	  g_autoptr(GFile) child1 = g_file_get_child (dir, name);
	  g_autoptr(GFile) child2 = g_file_get_child (child1, branch);
	  g_autoptr(GFile) child3 = g_file_get_child (child2, arch);
	  g_autoptr(GFile) active = g_file_get_child (child3, "active");

	  if (g_file_query_exists (active, cancellable))
	    g_hash_table_add (hash, g_strdup (name));
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_list_deployed (XdgAppDir *self,
                           const char *ref,
                           char ***deployed_checksums,
                           GCancellable *cancellable,
                           GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GPtrArray) checksums = NULL;
  GError *temp_error = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GError) my_error = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checksums = g_ptr_array_new_with_free_func (g_free);

  dir_enum = g_file_enumerate_children (deploy_base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &my_error);
  if (!dir_enum)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        ret = TRUE; /* Success, but empty */
      else
        g_propagate_error (error, g_steal_pointer (&my_error));
      goto out;
    }

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (deploy_base, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' &&
          strlen (name) == 64)
        g_ptr_array_add (checksums, g_strdup (name));

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;

 out:
  if (ret)
    {
      g_ptr_array_add (checksums, NULL);
      *deployed_checksums = (char **)g_ptr_array_free (g_steal_pointer (&checksums), FALSE);
    }

  return ret;

}

static gboolean
dir_is_locked (GFile *dir)
{
  glnx_fd_close int ref_fd = -1;
  struct flock lock = {0};
  g_autoptr(GFile) reffile = NULL;

  reffile = g_file_resolve_relative_path (dir, "files/.ref");

  ref_fd = open (gs_file_get_path_cached (reffile), O_RDWR | O_CLOEXEC);
  if (ref_fd != -1)
    {
      lock.l_type = F_WRLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = 0;
      lock.l_len = 0;

      if (fcntl (ref_fd, F_GETLK, &lock) == 0)
	return lock.l_type != F_UNLCK;
    }

  return FALSE;
}

gboolean
xdg_app_dir_undeploy (XdgAppDir *self,
                      const char *ref,
                      const char *checksum,
		      gboolean force_remove,
                      GCancellable *cancellable,
                      GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) removed_subdir = NULL;
  g_autoptr(GFile) removed_dir = NULL;
  g_autofree char *tmpname = NULL;
  g_autofree char *active = NULL;
  int i;

  g_assert (ref != NULL);
  g_assert (checksum != NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_UNDEPLOYED,
                   "%s version %s already undeployed", ref, checksum);
      goto out;
    }

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  active = xdg_app_dir_read_active (self, ref, cancellable);
  if (active != NULL && strcmp (active, checksum) == 0)
    {
      g_auto(GStrv) deployed_checksums = NULL;
      const char *some_deployment;

      /* We're removing the active deployment, start by repointing that
         to another deployment if one exists */

      if (!xdg_app_dir_list_deployed (self, ref,
                                      &deployed_checksums,
                                      cancellable, error))
        goto out;

      some_deployment = NULL;
      for (i = 0; deployed_checksums[i] != NULL; i++)
        {
          if (strcmp (deployed_checksums[i], checksum) == 0)
            continue;

          some_deployment = deployed_checksums[i];
          break;
        }

      if (!xdg_app_dir_set_active (self, ref, some_deployment, cancellable, error))
        goto out;
    }

  removed_dir = xdg_app_dir_get_removed_dir (self);
  if (!gs_file_ensure_directory (removed_dir, TRUE, cancellable, error))
    goto out;

  tmpname = gs_fileutil_gen_tmp_name ("", checksum);
  removed_subdir = g_file_get_child (removed_dir, tmpname);

  if (!gs_file_rename (checkoutdir,
                       removed_subdir,
                       cancellable, error))
    goto out;

  if (force_remove || !dir_is_locked (removed_subdir))
    {
      GError *tmp_error = NULL;

      if (!gs_shutil_rm_rf (removed_subdir, cancellable, &tmp_error))
        {
          g_warning ("Unable to remove old checkout: %s\n", tmp_error->message);
          g_error_free (tmp_error);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_cleanup_removed (XdgAppDir      *self,
			     GCancellable   *cancellable,
			     GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) removed_dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  removed_dir = xdg_app_dir_get_removed_dir (self);
  if (!g_file_query_exists (removed_dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (removed_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);
      g_autoptr(GFile) child = g_file_get_child (removed_dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
	  !dir_is_locked (child))
	{
          GError *tmp_error = NULL;
          if (!gs_shutil_rm_rf (child, cancellable, &tmp_error))
            {
              g_warning ("Unable to remove old checkout: %s\n", tmp_error->message);
              g_error_free (tmp_error);
            }
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}


gboolean
xdg_app_dir_prune (XdgAppDir      *self,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  gint objects_total, objects_pruned;
  guint64 pruned_object_size_total;
  g_autofree char *formatted_freed_size = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!ostree_repo_prune (self->repo,
                          OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY,
                          0,
                          &objects_total,
                          &objects_pruned,
                          &pruned_object_size_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (pruned_object_size_total, 0);
  g_debug ("Pruned %d/%d objects, size %s", objects_total, objects_pruned, formatted_freed_size);

  ret = TRUE;
 out:
  return ret;

}

GFile *
xdg_app_dir_get_if_deployed (XdgAppDir     *self,
                             const char    *ref,
                             const char    *checksum,
                             GCancellable  *cancellable)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  deploy_dir = g_file_get_child (deploy_base, checksum ? checksum : "active");

  if (g_file_query_file_type (deploy_dir, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY)
    return g_object_ref (deploy_dir);
  return NULL;
}

XdgAppDir*
xdg_app_dir_new (GFile *path, gboolean user)
{
  return g_object_new (XDG_APP_TYPE_DIR, "path", path, "user", user, NULL);
}

XdgAppDir *
xdg_app_dir_get_system (void)
{
  static XdgAppDir *system = NULL;

  if (system == NULL)
    {
      g_autoptr(GFile) path = xdg_app_get_system_base_dir_location ();
      system = xdg_app_dir_new (path, FALSE);
    }

  return g_object_ref (system);
}

XdgAppDir *
xdg_app_dir_get_user  (void)
{
  static XdgAppDir *user = NULL;

  if (user == NULL)
    {
      g_autoptr(GFile) path = xdg_app_get_user_base_dir_location ();
      user = xdg_app_dir_new (path, TRUE);
    }

  return g_object_ref (user);
}

XdgAppDir *
xdg_app_dir_get (gboolean user)
{
  if (user)
    return xdg_app_dir_get_user ();
  else
    return xdg_app_dir_get_system ();
}

static char *
get_group (const char *remote_name)
{
  return g_strdup_printf ("remote \"%s\"", remote_name);
}

char *
xdg_app_dir_get_remote_title (XdgAppDir *self,
                              const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.title", NULL);

  return NULL;
}

gboolean
xdg_app_dir_get_remote_noenumerate (XdgAppDir *self,
                                    const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_boolean (config, group, "xa.noenumerate", NULL);

  return TRUE;
}

char **
xdg_app_dir_list_remotes (XdgAppDir *self,
                          GCancellable *cancellable,
                          GError **error)
{
  char **res;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    return NULL;

  res = ostree_repo_remote_list (self->repo, NULL);
  if (res == NULL)
    res = g_new0 (char *, 1); /* Return empty array, not error */

  return res;
}

gboolean
xdg_app_dir_list_remote_refs (XdgAppDir *self,
                              const char *remote,
                              GHashTable **refs,
                              GCancellable *cancellable,
                              GError **error)
{
  g_autoptr(GError) my_error = NULL;
  if (error == NULL)
    error = &my_error;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!ostree_repo_remote_list_refs (self->repo, remote,
                                     refs, cancellable, error))
    return FALSE;

  return TRUE;
}

char *
xdg_app_dir_fetch_remote_title (XdgAppDir *self,
                                const char *remote,
                                GCancellable *cancellable,
                                GError **error)
{
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) extensions = NULL;
  GVariantDict dict;
  g_autofree char *title = NULL;

  if (error == NULL)
    error = &my_error;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!ostree_repo_remote_fetch_summary (self->repo, remote,
                                         &summary_bytes, NULL,
                                         cancellable, error))
    return FALSE;

  if (summary_bytes == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Remote title not available; server has no summary file");
      return FALSE;
    }

  summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                      summary_bytes, FALSE);
  extensions = g_variant_get_child_value (summary, 1);

  g_variant_dict_init (&dict, extensions);
  g_variant_dict_lookup (&dict, "xa.title", "s", &title);
  g_variant_dict_end (&dict);

  if (title == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Remote title not set");
      return FALSE;
    }

  return g_steal_pointer (&title);
}

static void
ensure_soup_session (XdgAppDir *self)
{
  const char *http_proxy;

  if (self->soup_session != NULL)
    return;

  self->soup_session =
    soup_session_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                   SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                   SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                   SOUP_SESSION_TIMEOUT, 60,
                                   SOUP_SESSION_IDLE_TIMEOUT, 60,
                                   NULL);
  http_proxy = g_getenv ("http_proxy");
  if (http_proxy)
    {
      g_autoptr(SoupURI) proxy_uri = soup_uri_new (http_proxy);

      if (!proxy_uri)
        g_warning ("Invalid proxy URI '%s'", http_proxy);
      else
        g_object_set (self->soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
    }

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    soup_session_add_feature (self->soup_session, (SoupSessionFeature*)soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));
}

static GBytes *
xdg_app_dir_load_uri (XdgAppDir *self,
                      const char *uri,
                      GCancellable *cancellable,
                      GError **error)
{
  g_autofree char *scheme = NULL;
  g_autoptr(GBytes) bytes = NULL;

  scheme = g_uri_parse_scheme (uri);
  if (strcmp (scheme, "file") == 0)
    {
      char *buffer;
      gsize length;
      g_autoptr(GFile) file = NULL;

      g_debug ("Loading %s using GIO", uri);

      file = g_file_new_for_uri (uri);
      if (!g_file_load_contents (file, cancellable, &buffer, &length, NULL, NULL))
        return NULL;

      bytes = g_bytes_new_take (buffer, length);
    }
  else if (strcmp (scheme, "http") == 0 ||
           strcmp (scheme, "https") == 0)
    {
      g_autoptr(SoupMessage) msg = NULL;

      ensure_soup_session (self);

      g_debug ("Loading %s using libsoup", uri);
      msg = soup_message_new ("GET", uri);
      soup_session_send_message (self->soup_session, msg);

      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        {
          GIOErrorEnum code;

          switch (msg->status_code)
            {
            case 404:
            case 410:
              code = G_IO_ERROR_NOT_FOUND;
              break;
            default:
              code = G_IO_ERROR_FAILED;
            }

          g_set_error (error, G_IO_ERROR, code,
                       "Server returned status %u: %s",
                       msg->status_code,
                       soup_status_get_phrase (msg->status_code));
          return NULL;
        }

      bytes = g_bytes_new (msg->response_body->data, msg->response_body->length);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported uri scheme %s", scheme);
      return FALSE;
    }

  g_debug ("Received %" G_GSIZE_FORMAT " bytes", g_bytes_get_size (bytes));

  return g_steal_pointer (&bytes);
}

GBytes *
xdg_app_dir_fetch_remote_object (XdgAppDir *self,
                                 const char *remote_name,
                                 const char *checksum,
                                 const char *type,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autofree char *base_url = NULL;
  g_autofree char *object_url = NULL;
  g_autofree char *part1 = NULL;
  g_autofree char *part2 = NULL;
  g_autoptr(GBytes) bytes = NULL;

  if (!ostree_repo_remote_get_url (self->repo, remote_name, &base_url, error))
    return NULL;

  part1 = g_strndup (checksum, 2);
  part2 = g_strdup_printf ("%s.%s", checksum + 2, type);

  object_url = g_build_filename (base_url, "objects", part1, part2, NULL);

  bytes = xdg_app_dir_load_uri (self, object_url, cancellable, error);
  if (bytes == NULL)
    return NULL;

  return g_steal_pointer (&bytes);
}

GBytes *
xdg_app_dir_fetch_metadata (XdgAppDir *self,
                            const char *remote_name,
                            const char *commit,
                            GCancellable *cancellable,
                            GError **error)
{
  g_autoptr(GBytes) commit_bytes = NULL;
  g_autoptr(GBytes) root_bytes = NULL;
  g_autoptr(GBytes) filez_bytes = NULL;
  g_autoptr(GVariant) commit_variant = NULL;
  g_autoptr(GVariant) root_variant = NULL;
  g_autoptr(GVariant) root_csum = NULL;
  g_autoptr(GVariant) files_variant = NULL;
  g_autofree char *file_checksum = NULL;
  g_autofree char *root_checksum = NULL;
  g_autoptr(GConverter) zlib_decomp = NULL;
  g_autoptr(GInputStream) zlib_input = NULL;
  g_autoptr(GMemoryOutputStream) data_stream = NULL;
  g_autoptr(GMemoryInputStream) dataz_stream = NULL;
  gsize filez_size;
  const guchar *filez_data;
  guint32 archive_header_size;
  int i, n;

  commit_bytes = xdg_app_dir_fetch_remote_object (self, remote_name,
                                                  commit, "commit",
                                                  cancellable, error);
  if (commit_bytes == NULL)
    return NULL;

  commit_variant = g_variant_new_from_bytes (OSTREE_COMMIT_GVARIANT_FORMAT,
                                             commit_bytes, FALSE);

  if (!ostree_validate_structureof_commit (commit_variant, error))
    return NULL;

  g_variant_get_child (commit_variant, 6, "@ay", &root_csum);
  root_checksum = ostree_checksum_from_bytes_v (root_csum);

  root_bytes = xdg_app_dir_fetch_remote_object (self, remote_name,
                                                root_checksum, "dirtree",
                                                cancellable, error);
  if (root_bytes == NULL)
    return NULL;

  root_variant = g_variant_new_from_bytes (OSTREE_TREE_GVARIANT_FORMAT,
                                           root_bytes, FALSE);

  if (!ostree_validate_structureof_dirtree (root_variant, error))
    return NULL;

  files_variant = g_variant_get_child_value (root_variant, 0);

  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      g_autoptr(GVariant) csum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (strcmp (filename, "metadata") != 0)
        continue;

      file_checksum = ostree_checksum_from_bytes_v (csum);
      break;
    }

  if (file_checksum == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Can't find metadata file");
      return NULL;
    }

  filez_bytes = xdg_app_dir_fetch_remote_object (self, remote_name,
                                                file_checksum, "filez",
                                                cancellable, error);
  if (filez_bytes == NULL)
    return NULL;

  filez_data = g_bytes_get_data (filez_bytes, &filez_size);

  if (filez_size < 8)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid header");
      return NULL;
    }

  archive_header_size = GUINT32_FROM_BE (*(guint32 *)filez_data);

  archive_header_size += 4 + 4; /* Include header-size and padding */

  if (archive_header_size > filez_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "File header size %u exceeds file size",
                   (guint)archive_header_size);
      return NULL;
    }

  dataz_stream = (GMemoryInputStream*)g_memory_input_stream_new_from_data (filez_data + archive_header_size,
                                                                           g_bytes_get_size (filez_bytes) - archive_header_size,
                                                                           NULL);

  zlib_decomp = (GConverter*)g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);
  zlib_input = g_converter_input_stream_new (G_INPUT_STREAM (dataz_stream), zlib_decomp);

  data_stream = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);

  if (g_output_stream_splice (G_OUTPUT_STREAM (data_stream), zlib_input,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    return NULL;

  return g_memory_output_stream_steal_as_bytes (data_stream);
}
