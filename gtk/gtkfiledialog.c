/*
 * GTK - The GIMP Toolkit
 * Copyright (C) 2022 Red Hat, Inc.
 * All rights reserved.
 *
 * This Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkfiledialog.h"

#include "deprecated/gtkdialog.h"
#include "gtkfilechoosernativeprivate.h"
#include "gtkdialogerror.h"
#include <glib/gi18n-lib.h>

/**
 * GtkFileDialog:
 *
 * A `GtkFileDialog` object collects the arguments that
 * are needed to present a file chooser dialog to the
 * user, such as a title for the dialog and whether it
 * should be modal.
 *
 * The dialog is shown with [method@Gtk.FileDialog.open],
 * [method@Gtk.FileDialog.save], etc. These APIs follow the
 * GIO async pattern, and the result can be obtained by calling
 * the corresponding finish function, for example
 * [method@Gtk.FileDialog.open_finish].
 *
 * Since: 4.10
 */

/* {{{ GObject implementation */

struct _GtkFileDialog
{
  GObject parent_instance;

  char *title;
  unsigned int modal : 1;

  GListModel *filters;
  GListModel *shortcut_folders;
  GtkFileFilter *current_filter;
  GFile *current_folder;
};

enum
{
  PROP_TITLE = 1,
  PROP_MODAL,
  PROP_FILTERS,
  PROP_SHORTCUT_FOLDERS,
  PROP_CURRENT_FILTER,
  PROP_CURRENT_FOLDER,

  NUM_PROPERTIES
};

static GParamSpec *properties[NUM_PROPERTIES];

G_DEFINE_TYPE (GtkFileDialog, gtk_file_dialog, G_TYPE_OBJECT)

static void
gtk_file_dialog_init (GtkFileDialog *self)
{
  self->modal = TRUE;
}

static void
gtk_file_dialog_finalize (GObject *object)
{
  GtkFileDialog *self = GTK_FILE_DIALOG (object);

  g_free (self->title);
  g_clear_object (&self->filters);
  g_clear_object (&self->shortcut_folders);
  g_clear_object (&self->current_filter);
  g_clear_object (&self->current_folder);

  G_OBJECT_CLASS (gtk_file_dialog_parent_class)->finalize (object);
}

static void
gtk_file_dialog_get_property (GObject      *object,
                              unsigned int  property_id,
                              GValue       *value,
                              GParamSpec   *pspec)
{
  GtkFileDialog *self = GTK_FILE_DIALOG (object);

  switch (property_id)
    {
    case PROP_TITLE:
      g_value_set_string (value, self->title);
      break;

    case PROP_MODAL:
      g_value_set_boolean (value, self->modal);
      break;

    case PROP_FILTERS:
      g_value_set_object (value, self->filters);
      break;

    case PROP_SHORTCUT_FOLDERS:
      g_value_set_object (value, self->shortcut_folders);
      break;

    case PROP_CURRENT_FILTER:
      g_value_set_object (value, self->current_filter);
      break;

    case PROP_CURRENT_FOLDER:
      g_value_set_object (value, self->current_folder);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gtk_file_dialog_set_property (GObject      *object,
                              unsigned int  prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  GtkFileDialog *self = GTK_FILE_DIALOG (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      gtk_file_dialog_set_title (self, g_value_get_string (value));
      break;

    case PROP_MODAL:
      gtk_file_dialog_set_modal (self, g_value_get_boolean (value));
      break;

    case PROP_FILTERS:
      gtk_file_dialog_set_filters (self, g_value_get_object (value));
      break;

    case PROP_SHORTCUT_FOLDERS:
      gtk_file_dialog_set_shortcut_folders (self, g_value_get_object (value));
      break;

    case PROP_CURRENT_FILTER:
      gtk_file_dialog_set_current_filter (self, g_value_get_object (value));
      break;

    case PROP_CURRENT_FOLDER:
      gtk_file_dialog_set_current_folder (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gtk_file_dialog_class_init (GtkFileDialogClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = gtk_file_dialog_finalize;
  object_class->get_property = gtk_file_dialog_get_property;
  object_class->set_property = gtk_file_dialog_set_property;

  /**
   * GtkFileDialog:title: (attributes org.gtk.Property.get=gtk_file_dialog_get_title org.gtk.Property.set=gtk_file_dialog_set_title)
   *
   * A title that may be shown on the file chooser dialog.
   *
   * Since: 4.10
   */
  properties[PROP_TITLE] =
      g_param_spec_string ("title", NULL, NULL,
                           NULL,
                           G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkFileDialog:modal: (attributes org.gtk.Property.get=gtk_file_dialog_get_modal org.gtk.Property.set=gtk_file_dialog_set_modal)
   *
   * Whether the file chooser dialog is modal.
   *
   * Since: 4.10
   */
  properties[PROP_MODAL] =
      g_param_spec_boolean ("modal", NULL, NULL,
                            TRUE,
                            G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkFileDialog:filters: (attributes org.gtk.Property.get=gtk_file_dialog_get_filters org.gtk.Property.set=gtk_file_dialog_set_filters)
   *
   * The list of filters.
   *
   * Since: 4.10
   */
  properties[PROP_FILTERS] =
      g_param_spec_object ("filters", NULL, NULL,
                           G_TYPE_LIST_MODEL,
                           G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkFileDialog:shortcut-folders: (attributes org.gtk.Property.get=gtk_file_dialog_get_shortcut_folders org.gtk.Property.set=gtk_file_dialog_set_shortcut_folders)
   *
   * The list of shortcut folders.
   *
   * Since: 4.10
   */
  properties[PROP_SHORTCUT_FOLDERS] =
      g_param_spec_object ("shortcut-folders", NULL, NULL,
                           G_TYPE_LIST_MODEL,
                           G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkFileDialog:current-filter: (attributes org.gtk.Property.get=gtk_file_dialog_get_current_filter org.gtk.Property.set=gtk_file_dialog_set_current_filter)
   *
   * The current filter, that is, the filter that is initially
   * active in the file chooser dialog.
   *
   * Since: 4.10
   */
  properties[PROP_CURRENT_FILTER] =
      g_param_spec_object ("current-filter", NULL, NULL,
                           GTK_TYPE_FILE_FILTER,
                           G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkFileDialog:current-folder: (attributes org.gtk.Property.get=gtk_file_dialog_get_current_folder org.gtk.Property.set=gtk_file_dialog_set_current_folder)
   *
   * The current folder, that is, the directory that is initially
   * opened in the file chooser dialog, unless overridden by parameters
   * of the async call.
   *
   * Since: 4.10
   */
  properties[PROP_CURRENT_FOLDER] =
      g_param_spec_object ("current-folder", NULL, NULL,
                           G_TYPE_FILE,
                           G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, NUM_PROPERTIES, properties);
}

/* }}} */
/* {{{ Utilities */

static void
file_chooser_set_filters (GtkFileChooser *chooser,
                          GListModel     *filters)
{
  if (!filters)
    return;

  for (unsigned int i = 0; i < g_list_model_get_n_items (filters); i++)
    {
      GtkFileFilter *filter = g_list_model_get_item (filters, i);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_file_chooser_add_filter (chooser, filter);
G_GNUC_END_IGNORE_DEPRECATIONS
      g_object_unref (filter);
    }
}

static void
file_chooser_set_shortcut_folders (GtkFileChooser *chooser,
                                   GListModel     *shortcut_folders)
{
  if (!shortcut_folders)
    return;

  for (unsigned int i = 0; i < g_list_model_get_n_items (shortcut_folders); i++)
    {
      GFile *folder = g_list_model_get_item (shortcut_folders, i);
      GError *error = NULL;

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      if (!gtk_file_chooser_add_shortcut_folder (chooser, folder, &error))
        {
          g_critical ("%s", error->message);
          g_clear_error (&error);
        }
G_GNUC_END_IGNORE_DEPRECATIONS

      g_object_unref (folder);
    }
}

/* }}} */
/* {{{ API: Constructor */

/**
 * gtk_file_dialog_new:
 *
 * Creates a new `GtkFileDialog` object.
 *
 * Returns: the new `GtkFileDialog`
 *
 * Since: 4.10
 */
GtkFileDialog *
gtk_file_dialog_new (void)
{
  return g_object_new (GTK_TYPE_FILE_DIALOG, NULL);
}

/* }}} */
/* {{{ API: Getters and setters */

/**
 * gtk_file_dialog_get_title:
 * @self: a `GtkFileDialog`
 *
 * Returns the title that will be shown on the
 * file chooser dialog.
 *
 * Returns: the title
 *
 * Since: 4.10
 */
const char *
gtk_file_dialog_get_title (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);

  return self->title;
}

/**
 * gtk_file_dialog_set_title:
 * @self: a `GtkFileDialog`
 * @title: the new title
 *
 * Sets the title that will be shown on the
 * file chooser dialog.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_title (GtkFileDialog *self,
                           const char    *title)
{
  char *new_title;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));
  g_return_if_fail (title != NULL);

  if (g_strcmp0 (self->title, title) == 0)
    return;

  new_title = g_strdup (title);
  g_free (self->title);
  self->title = new_title;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

/**
 * gtk_file_dialog_get_modal:
 * @self: a `GtkFileDialog`
 *
 * Returns whether the file chooser dialog
 * blocks interaction with the parent window
 * while it is presented.
 *
 * Returns: `TRUE` if the file chooser dialog is modal
 *
 * Since: 4.10
 */
gboolean
gtk_file_dialog_get_modal (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), TRUE);

  return self->modal;
}

/**
 * gtk_file_dialog_set_modal:
 * @self: a `GtkFileDialog`
 * @modal: the new value
 *
 * Sets whether the file chooser dialog
 * blocks interaction with the parent window
 * while it is presented.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_modal (GtkFileDialog *self,
                           gboolean       modal)
{
  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  if (self->modal == modal)
    return;

  self->modal = modal;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MODAL]);
}

/**
 * gtk_file_dialog_get_filters:
 * @self: a `GtkFileDialog`
 *
 * Gets the filters that will be offered to the user
 * in the file chooser dialog.
 *
 * Returns: (transfer none) (nullable): the filters, as
 *   a `GListModel` of `GtkFileFilters`
 *
 * Since: 4.10
 */
GListModel *
gtk_file_dialog_get_filters (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);

  return self->filters;
}

/**
 * gtk_file_dialog_set_filters:
 * @self: a `GtkFileDialog`
 * @filters: a `GListModel` of `GtkFileFilters`
 *
 * Sets the filters that will be offered to the user
 * in the file chooser dialog.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_filters (GtkFileDialog *self,
                             GListModel    *filters)
{
  g_return_if_fail (GTK_IS_FILE_DIALOG (self));
  g_return_if_fail (G_IS_LIST_MODEL (filters));

  if (!g_set_object (&self->filters, filters))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FILTERS]);
}

/**
 * gtk_file_dialog_get_shortcut_folders:
 * @self: a `GtkFileDialog`
 *
 * Gets the shortcut folders that will be available to
 * the user in the file chooser dialog.
 *
 * Returns: (nullable) (transfer none): the shortcut
 *   folders, as a `GListModel` of `GFiles`
 *
 * Since: 4.10
 */
GListModel *
gtk_file_dialog_get_shortcut_folders (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);

  return self->shortcut_folders;
}

/**
 * gtk_file_dialog_set_shortcut_folders:
 * @self: a `GtkFileDialog`
 * @shortcut_folders: a `GListModel` of `GFiles`
 *
 * Sets the shortcut folders that will be available to
 * the user in the file chooser dialog.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_shortcut_folders (GtkFileDialog *self,
                                      GListModel    *shortcut_folders)
{
  g_return_if_fail (GTK_IS_FILE_DIALOG (self));
  g_return_if_fail (G_IS_LIST_MODEL (shortcut_folders));

  if (!g_set_object (&self->shortcut_folders, shortcut_folders))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SHORTCUT_FOLDERS]);
}

/**
 * gtk_file_dialog_get_current_filter:
 * @self: a `GtkFileDialog`
 *
 * Gets the filter that will be selected by default
 * in the file chooser dialog.
 *
 * Returns: (transfer none) (nullable): the current filter
 *
 * Since: 4.10
 */
GtkFileFilter *
gtk_file_dialog_get_current_filter (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);

  return self->current_filter;
}

/**
 * gtk_file_dialog_set_current_filter:
 * @self: a `GtkFileDialog`
 * @filter: (nullable): a `GtkFileFilter`
 *
 * Sets the filters that will be selected by default
 * in the file chooser dialog.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_current_filter (GtkFileDialog *self,
                                    GtkFileFilter *filter)
{
  g_return_if_fail (GTK_IS_FILE_DIALOG (self));
  g_return_if_fail (filter == NULL || GTK_IS_FILE_FILTER (filter));

  if (!g_set_object (&self->current_filter, filter))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CURRENT_FILTER]);
}

/**
 * gtk_file_dialog_get_current_folder:
 * @self: a `GtkFileDialog`
 *
 * Gets the folder that will be set as the
 * initial folder in the file chooser dialog.
 *
 * Returns: (nullable) (transfer none): the folder
 *
 * Since: 4.10
 */
GFile *
gtk_file_dialog_get_current_folder (GtkFileDialog *self)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);

  return self->current_folder;
}

/**
 * gtk_file_dialog_set_current_folder:
 * @self: a `GtkFileDialog`
 * @folder: (nullable): a `GFile`
 *
 * Sets the folder that will be set as the
 * initial folder in the file chooser dialog,
 * unless overridden by parameters of the async
 * call.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_set_current_folder (GtkFileDialog *self,
                                    GFile         *folder)
{
  g_return_if_fail (GTK_IS_FILE_DIALOG (self));
  g_return_if_fail (folder == NULL || G_IS_FILE (folder));

  if (!g_set_object (&self->current_folder, folder))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CURRENT_FOLDER]);
}

/* }}} */
/* {{{ Async implementation */

static void response_cb (GTask *task,
                         int    response);

static void
cancelled_cb (GCancellable *cancellable,
              GTask        *task)
{
  response_cb (task, GTK_RESPONSE_CLOSE);
}

static void
response_cb (GTask *task,
             int    response)
{
  GCancellable *cancellable;

  cancellable = g_task_get_cancellable (task);

  if (cancellable)
    g_signal_handlers_disconnect_by_func (cancellable, cancelled_cb, task);

  if (response == GTK_RESPONSE_ACCEPT)
    {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      GtkFileChooser *chooser;
      GListModel *files;

      chooser = GTK_FILE_CHOOSER (g_task_get_task_data (task));
      files = gtk_file_chooser_get_files (chooser);
      g_task_return_pointer (task, files, g_object_unref);
G_GNUC_END_IGNORE_DEPRECATIONS
    }
  else if (response == GTK_RESPONSE_CLOSE)
    g_task_return_new_error (task, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED, "Aborted by application");
  else if (response == GTK_RESPONSE_CANCEL)
    g_task_return_new_error (task, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED, "Cancelled by user");
  else
    g_task_return_new_error (task, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_FAILED, "Unknown failure (%d)", response);

  g_object_unref (task);
}

static void
dialog_response (GtkDialog *dialog,
                 int        response,
                 GTask     *task)
{
  response_cb (task, response);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static GtkFileChooserNative *
create_file_chooser (GtkFileDialog        *self,
                     GtkWindow            *parent,
                     GtkFileChooserAction  action,
                     GFile                *current_file,
                     const char           *current_name,
                     gboolean              select_multiple)
{
  GtkFileChooserNative *chooser;
  const char *accept;
  const char *title;

  switch (action)
    {
    case GTK_FILE_CHOOSER_ACTION_OPEN:
      accept = _("_Open");
      title = select_multiple ? _("Pick Files") : _("Pick a File");
      break;

    case GTK_FILE_CHOOSER_ACTION_SAVE:
      accept = _("_Save");
      title = _("Save a File");
      break;

    case GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:
      accept = _("_Select");
      title = select_multiple ? _("Select Folders") : _("Select a Folder");
      break;

    default:
      g_assert_not_reached ();
    }

  chooser = gtk_file_chooser_native_new (title, parent, action, accept, _("_Cancel"));
  gtk_file_chooser_native_set_use_portal (chooser, TRUE);

  gtk_native_dialog_set_modal (GTK_NATIVE_DIALOG (chooser), self->modal);
  gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (chooser), select_multiple);

  file_chooser_set_filters (GTK_FILE_CHOOSER (chooser), self->filters);
  if (self->current_filter)
    gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (chooser), self->current_filter);
  file_chooser_set_shortcut_folders (GTK_FILE_CHOOSER (chooser), self->shortcut_folders);
  if (self->current_folder)
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser), self->current_folder, NULL);
  if (current_file)
    gtk_file_chooser_set_file (GTK_FILE_CHOOSER (chooser), current_file, NULL);
  else if (current_name)
    gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (chooser), current_name);

  return chooser;
}
G_GNUC_END_IGNORE_DEPRECATIONS

static GFile *
finish_file_op (GtkFileDialog  *self,
                GTask          *task,
                GError        **error)
{
  GListModel *files;

  files = g_task_propagate_pointer (task, error);
  if (files)
    {
      GFile *file;

      g_assert (g_list_model_get_n_items (files) == 1);

      file = g_list_model_get_item (files, 0);
      g_object_unref (files);

      return file;
    }

  return NULL;
}

static GListModel *
finish_multiple_files_op (GtkFileDialog  *self,
                          GTask          *task,
                          GError        **error)
{
  return G_LIST_MODEL (g_task_propagate_pointer (task, error));
}

/* }}} */
/* {{{ Async API */

/**
 * gtk_file_dialog_open:
 * @self: a `GtkFileDialog`
 * @parent: (nullable): the parent `GtkWindow`
 * @current_file: (nullable): the file to select initially
 * @cancellable: (nullable): a `GCancellable` to cancel the operation
 * @callback: (scope async): a callback to call when the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * This function initiates a file selection operation by
 * presenting a file chooser dialog to the user.
 *
 * If you pass @current_file, the file chooser will initially be
 * opened in the parent directory of that file, otherwise, it
 * will be in the directory [property@Gtk.FileDialog:current-folder].
 *
 * The @callback will be called when the dialog is dismissed.
 * It should call [method@Gtk.FileDialog.open_finish]
 * to obtain the result.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_open (GtkFileDialog       *self,
                      GtkWindow           *parent,
                      GFile               *current_file,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GtkFileChooserNative *chooser;
  GTask *task;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  chooser = create_file_chooser (self, parent, GTK_FILE_CHOOSER_ACTION_OPEN,
                                 current_file, NULL, FALSE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gtk_file_dialog_open);
  g_task_set_task_data (task, chooser, (GDestroyNotify) gtk_native_dialog_destroy);

  if (cancellable)
    g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), task);

  g_signal_connect (chooser, "response", G_CALLBACK (dialog_response), task);

  gtk_native_dialog_show (GTK_NATIVE_DIALOG (chooser));
}

/**
 * gtk_file_dialog_open_finish:
 * @self: a `GtkFileDialog`
 * @result: a `GAsyncResult`
 * @error: return location for a [enum@Gtk.DialogError] error
 *
 * Finishes the [method@Gtk.FileDialog.open] call and
 * returns the resulting file.
 *
 * Returns: (nullable) (transfer full): the file that was selected.
 *   Otherwise, `NULL` is returned and @error is set
 *
 * Since: 4.10
 */
GFile *
gtk_file_dialog_open_finish (GtkFileDialog   *self,
                             GAsyncResult    *result,
                             GError         **error)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gtk_file_dialog_open, NULL);

  return finish_file_op (self, G_TASK (result), error);
}

/**
 * gtk_file_dialog_select_folder:
 * @self: a `GtkFileDialog`
 * @parent: (nullable): the parent `GtkWindow`
 * @current_folder: (nullable): the folder to select initially
 * @cancellable: (nullable): a `GCancellable` to cancel the operation
 * @callback: (scope async): a callback to call when the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * This function initiates a directory selection operation by
 * presenting a file chooser dialog to the user.
 *
 * If you pass @current_folder, the file chooser will initially be
 * opened in the parent directory of that folder, otherwise, it
 * will be in the directory [property@Gtk.FileDialog:current-folder].
 *
 * The @callback will be called when the dialog is dismissed.
 * It should call [method@Gtk.FileDialog.select_folder_finish]
 * to obtain the result.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_select_folder (GtkFileDialog       *self,
                               GtkWindow           *parent,
                               GFile               *current_folder,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GtkFileChooserNative *chooser;
  GTask *task;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  chooser = create_file_chooser (self, parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                 current_folder, NULL, FALSE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gtk_file_dialog_select_folder);
  g_task_set_task_data (task, chooser, (GDestroyNotify) gtk_native_dialog_destroy);

  if (cancellable)
    g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), task);

  g_signal_connect (chooser, "response", G_CALLBACK (dialog_response), task);

  gtk_native_dialog_show (GTK_NATIVE_DIALOG (chooser));
}

/**
 * gtk_file_dialog_select_folder_finish:
 * @self: a `GtkFileDialog`
 * @result: a `GAsyncResult`
 * @error: return location for a [enum@Gtk.DialogError] error
 *
 * Finishes the [method@Gtk.FileDialog.select_folder] call and
 * returns the resulting file.
 *
 * Returns: (nullable) (transfer full): the file that was selected.
 *   Otherwise, `NULL` is returned and @error is set
 *
 * Since: 4.10
 */
GFile *
gtk_file_dialog_select_folder_finish (GtkFileDialog  *self,
                                      GAsyncResult   *result,
                                      GError        **error)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gtk_file_dialog_select_folder, NULL);

  return finish_file_op (self, G_TASK (result), error);
}

/**
 * gtk_file_dialog_save:
 * @self: a `GtkFileDialog`
 * @parent: (nullable): the parent `GtkWindow`
 * @current_file: (nullable): the initial file
 * @current_name: (nullable): the initial filename to offer
 * @cancellable: (nullable): a `GCancellable` to cancel the operation
 * @callback: (scope async): a callback to call when the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * This function initiates a file save operation by
 * presenting a file chooser dialog to the user.
 *
 * You should pass either @current_file if you have a file to
 * save to, or @current_name, if you are creating a new file.
 *
 * If you pass @current_file, the file chooser will initially be
 * opened in the parent directory of that file, otherwise, it
 * will be in the directory [property@Gtk.FileDialog:current-folder].
 *
 * The @callback will be called when the dialog is dismissed.
 * It should call [method@Gtk.FileDialog.save_finish]
 * to obtain the result.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_save (GtkFileDialog       *self,
                      GtkWindow           *parent,
                      GFile               *current_file,
                      const char          *current_name,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GtkFileChooserNative *chooser;
  GTask *task;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  chooser = create_file_chooser (self, parent, GTK_FILE_CHOOSER_ACTION_SAVE,
                                 current_file, current_name, FALSE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gtk_file_dialog_save);
  g_task_set_task_data (task, chooser, (GDestroyNotify) gtk_native_dialog_destroy);

  if (cancellable)
    g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), task);

  g_signal_connect (chooser, "response", G_CALLBACK (dialog_response), task);

  gtk_native_dialog_show (GTK_NATIVE_DIALOG (chooser));
}

/**
 * gtk_file_dialog_save_finish:
 * @self: a `GtkFileDialog`
 * @result: a `GAsyncResult`
 * @error: return location for a [enum@Gtk.DialogError] error
 *
 * Finishes the [method@Gtk.FileDialog.save] call and
 * returns the resulting file.
 *
 * Returns: (nullable) (transfer full): the file that was selected.
 *   Otherwise, `NULL` is returned and @error is set
 *
 * Since: 4.10
 */
GFile *
gtk_file_dialog_save_finish (GtkFileDialog   *self,
                             GAsyncResult    *result,
                             GError         **error)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gtk_file_dialog_save, NULL);

  return finish_file_op (self, G_TASK (result), error);
}

/**
 * gtk_file_dialog_open_multiple:
 * @self: a `GtkFileDialog`
 * @parent: (nullable): the parent `GtkWindow`
 * @cancellable: (nullable): a `GCancellable` to cancel the operation
 * @callback: (scope async): a callback to call when the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * This function initiates a multi-file selection operation by
 * presenting a file chooser dialog to the user.
 *
 * The file chooser will initially be opened in the directory
 * [property@Gtk.FileDialog:current-folder].
 *
 * The @callback will be called when the dialog is dismissed.
 * It should call [method@Gtk.FileDialog.open_multiple_finish]
 * to obtain the result.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_open_multiple (GtkFileDialog       *self,
                               GtkWindow           *parent,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GtkFileChooserNative *chooser;
  GTask *task;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  chooser = create_file_chooser (self, parent, GTK_FILE_CHOOSER_ACTION_OPEN,
                                 NULL, NULL, TRUE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gtk_file_dialog_open_multiple);
  g_task_set_task_data (task, chooser, (GDestroyNotify) gtk_native_dialog_destroy);

  if (cancellable)
    g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), task);

  g_signal_connect (chooser, "response", G_CALLBACK (dialog_response), task);

  gtk_native_dialog_show (GTK_NATIVE_DIALOG (chooser));
}

/**
 * gtk_file_dialog_open_multiple_finish:
 * @self: a `GtkFileDialog`
 * @result: a `GAsyncResult`
 * @error: return location for a [enum@Gtk.DialogError] error
 *
 * Finishes the [method@Gtk.FileDialog.open] call and
 * returns the resulting files in a `GListModel`.
 *
 * Returns: (nullable) (transfer full): the file that was selected,
 *   as a `GListModel` of `GFiles`. Otherwise, `NULL` is returned
 *   and @error is set
 *
 * Since: 4.10
 */
GListModel *
gtk_file_dialog_open_multiple_finish (GtkFileDialog   *self,
                                      GAsyncResult    *result,
                                      GError         **error)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gtk_file_dialog_open_multiple, NULL);

  return finish_multiple_files_op (self, G_TASK (result), error);
}

/**
 * gtk_file_dialog_select_multiple_folders:
 * @self: a `GtkFileDialog`
 * @parent: (nullable): the parent `GtkWindow`
 * @cancellable: (nullable): a `GCancellable` to cancel the operation
 * @callback: (scope async): a callback to call when the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * This function initiates a multi-directory selection operation by
 * presenting a file chooser dialog to the user.
 *
 * The file chooser will initially be opened in the directory
 * [property@Gtk.FileDialog:current-folder].
 *
 * The @callback will be called when the dialog is dismissed.
 * It should call [method@Gtk.FileDialog.select_multiple_folders_finish]
 * to obtain the result.
 *
 * Since: 4.10
 */
void
gtk_file_dialog_select_multiple_folders (GtkFileDialog       *self,
                                         GtkWindow           *parent,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GtkFileChooserNative *chooser;
  GTask *task;

  g_return_if_fail (GTK_IS_FILE_DIALOG (self));

  chooser = create_file_chooser (self, parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                 NULL, NULL, TRUE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gtk_file_dialog_select_multiple_folders);
  g_task_set_task_data (task, chooser, (GDestroyNotify) gtk_native_dialog_destroy);

  if (cancellable)
    g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), task);

  g_signal_connect (chooser, "response", G_CALLBACK (dialog_response), task);

  gtk_native_dialog_show (GTK_NATIVE_DIALOG (chooser));
}

/**
 * gtk_file_dialog_select_multiple_folders_finish:
 * @self: a `GtkFileDialog`
 * @result: a `GAsyncResult`
 * @error: return location for a [enum@Gtk.DialogError] error
 *
 * Finishes the [method@Gtk.FileDialog.select_multiple_folders]
 * call and returns the resulting files in a `GListModel`.
 *
 * Returns: (nullable) (transfer full): the file that was selected,
 *   as a `GListModel` of `GFiles`. Otherwise, `NULL` is returned
 *   and @error is set
 *
 * Since: 4.10
 */
GListModel *
gtk_file_dialog_select_multiple_folders_finish (GtkFileDialog   *self,
                                                GAsyncResult    *result,
                                                GError         **error)
{
  g_return_val_if_fail (GTK_IS_FILE_DIALOG (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gtk_file_dialog_select_multiple_folders, NULL);

  return finish_multiple_files_op (self, G_TASK (result), error);
}

/* }}} */

/* vim:set foldmethod=marker expandtab: */
