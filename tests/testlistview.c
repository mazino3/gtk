#include <gtk/gtk.h>

#define ROWS 30

GSList *pending = NULL;
guint active = 0;

static void
got_files (GObject      *enumerate,
           GAsyncResult *res,
           gpointer      store);

static gboolean
start_enumerate (GListStore *store)
{
  GFileEnumerator *enumerate;
  GFile *file = g_object_get_data (G_OBJECT (store), "file");
  GError *error = NULL;

  enumerate = g_file_enumerate_children (file,
                                         G_FILE_ATTRIBUTE_STANDARD_TYPE
                                         "," G_FILE_ATTRIBUTE_STANDARD_ICON
                                         "," G_FILE_ATTRIBUTE_STANDARD_NAME
                                         "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                         0,
                                         NULL,
                                         &error);

  if (enumerate == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES) && active)
        {
          g_clear_error (&error);
          pending = g_slist_prepend (pending, g_object_ref (store));
          return TRUE;
        }

      g_clear_error (&error);
      g_object_unref (store);
      return FALSE;
    }

  if (active > 20)
    {
      g_object_unref (enumerate);
      pending = g_slist_prepend (pending, g_object_ref (store));
      return TRUE;
    }

  active++;
  g_file_enumerator_next_files_async (enumerate,
                                      g_file_is_native (file) ? 5000 : 100,
                                      G_PRIORITY_DEFAULT_IDLE,
                                      NULL,
                                      got_files,
                                      g_object_ref (store));

  g_object_unref (enumerate);
  return TRUE;
}

static void
got_files (GObject      *enumerate,
           GAsyncResult *res,
           gpointer      store)
{
  GList *l, *files;
  GFile *file = g_object_get_data (store, "file");
  GPtrArray *array;

  files = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (enumerate), res, NULL);
  if (files == NULL)
    {
      g_object_unref (store);
      if (pending)
        {
          GListStore *next = pending->data;
          pending = g_slist_remove (pending, next);
          start_enumerate (next);
        }
      active--;
      return;
    }

  array = g_ptr_array_new ();
  g_ptr_array_new_with_free_func (g_object_unref);
  for (l = files; l; l = l->next)
    {
      GFileInfo *info = l->data;
      GFile *child;

      child = g_file_get_child (file, g_file_info_get_name (info));
      g_object_set_data_full (G_OBJECT (info), "file", child, g_object_unref);
      g_ptr_array_add (array, info);
    }
  g_list_free (files);

  g_list_store_splice (store, g_list_model_get_n_items (store), 0, array->pdata, array->len);
  g_ptr_array_unref (array);

  g_file_enumerator_next_files_async (G_FILE_ENUMERATOR (enumerate),
                                      g_file_is_native (file) ? 5000 : 100,
                                      G_PRIORITY_DEFAULT_IDLE,
                                      NULL,
                                      got_files,
                                      store);
}

static char *
get_file_path (GFileInfo *info)
{
  GFile *file;

  file = g_object_get_data (G_OBJECT (info), "file");
  return g_file_get_path (file);
}

static GListModel *
create_list_model_for_directory (gpointer file)
{
  GtkSortListModel *sort;
  GListStore *store;
  GtkSorter *sorter;

  if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_DIRECTORY)
    return NULL;

  store = g_list_store_new (G_TYPE_FILE_INFO);
  g_object_set_data_full (G_OBJECT (store), "file", g_object_ref (file), g_object_unref);

  if (!start_enumerate (store))
    return NULL;

  sorter = gtk_string_sorter_new (gtk_cclosure_expression_new (G_TYPE_STRING, NULL, 0, NULL, (GCallback) get_file_path, NULL, NULL));
  sort = gtk_sort_list_model_new (G_LIST_MODEL (store), sorter);
  g_object_unref (sorter);

  g_object_unref (store);
  return G_LIST_MODEL (sort);
}

typedef struct _RowData RowData;
struct _RowData
{
  GtkWidget *depth_box;
  GtkWidget *expander;
  GtkWidget *icon;
  GtkWidget *name;

  GtkTreeListRow *current_item;
  GBinding *expander_binding;
};

static void row_data_notify_item (GtkListItem *item,
                                  GParamSpec  *pspec,
                                  RowData     *data);
static void
row_data_unbind (RowData *data)
{
  if (data->current_item == NULL)
    return;

  g_binding_unbind (data->expander_binding);

  g_clear_object (&data->current_item);
}

static void
row_data_bind (RowData        *data,
               GtkTreeListRow *item)
{
  GFileInfo *info;
  GIcon *icon;
  guint depth;

  row_data_unbind (data);

  if (item == NULL)
    return;

  data->current_item = g_object_ref (item);

  depth = gtk_tree_list_row_get_depth (item);
  gtk_widget_set_size_request (data->depth_box, 16 * depth, 0);

  gtk_widget_set_sensitive (data->expander, gtk_tree_list_row_is_expandable (item));
  data->expander_binding = g_object_bind_property (item, "expanded", data->expander, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  info = gtk_tree_list_row_get_item (item);

  icon = g_file_info_get_icon (info);
  gtk_widget_set_visible (data->icon, icon != NULL);
  if (icon)
    gtk_image_set_from_gicon (GTK_IMAGE (data->icon), icon);

  gtk_label_set_label (GTK_LABEL (data->name), g_file_info_get_display_name (info));

  g_object_unref (info);
}

static void
row_data_notify_item (GtkListItem *item,
                      GParamSpec  *pspec,
                      RowData     *data)
{
  row_data_bind (data, gtk_list_item_get_item (item));
}

static void
row_data_free (gpointer _data)
{
  RowData *data = _data;

  row_data_unbind (data);

  g_slice_free (RowData, data);
}

static void
setup_widget (GtkListItem *list_item,
              gpointer     unused)
{
  GtkWidget *box, *child;
  RowData *data;

  data = g_slice_new0 (RowData);
  g_signal_connect (list_item, "notify::item", G_CALLBACK (row_data_notify_item), data);
  g_object_set_data_full (G_OBJECT (list_item), "row-data", data, row_data_free);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_list_item_set_child (list_item, box);

  child = gtk_label_new (NULL);
  gtk_label_set_width_chars (GTK_LABEL (child), 5);
  gtk_box_append (GTK_BOX (box), child);

  data->depth_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (box), data->depth_box);
  
  data->expander = g_object_new (GTK_TYPE_TOGGLE_BUTTON, "css-name", "expander-widget", NULL);
  gtk_button_set_has_frame (GTK_BUTTON (data->expander), FALSE);
  gtk_box_append (GTK_BOX (box), data->expander);
  child = g_object_new (GTK_TYPE_SPINNER, "css-name", "expander", NULL);
  g_object_bind_property (data->expander, "active", child, "spinning", G_BINDING_SYNC_CREATE);
  gtk_button_set_child (GTK_BUTTON (data->expander), child);

  data->icon = gtk_image_new ();
  gtk_box_append (GTK_BOX (box), data->icon);

  data->name = gtk_label_new (NULL);
  gtk_box_append (GTK_BOX (box), data->name);
}

static GListModel *
create_list_model_for_file_info (gpointer file_info,
                                 gpointer unused)
{
  GFile *file = g_object_get_data (file_info, "file");

  if (file == NULL)
    return NULL;

  return create_list_model_for_directory (file);
}

static gboolean
update_statusbar (GtkStatusbar *statusbar)
{
  GListModel *model = g_object_get_data (G_OBJECT (statusbar), "model");
  GString *string = g_string_new (NULL);
  guint n;
  gboolean result = G_SOURCE_REMOVE;

  gtk_statusbar_remove_all (statusbar, 0);

  n = g_list_model_get_n_items (model);
  g_string_append_printf (string, "%u", n);
  if (GTK_IS_FILTER_LIST_MODEL (model))
    {
      guint n_unfiltered = g_list_model_get_n_items (gtk_filter_list_model_get_model (GTK_FILTER_LIST_MODEL (model)));
      if (n != n_unfiltered)
        g_string_append_printf (string, "/%u", n_unfiltered);
    }
  g_string_append (string, " items");

  if (pending || active)
    {
      g_string_append_printf (string, " (%u directories remaining)", active + g_slist_length (pending));
      result = G_SOURCE_CONTINUE;
    }

  gtk_statusbar_push (statusbar, 0, string->str);
  g_free (string->str);

  return result;
}

static gboolean
match_file (gpointer item, gpointer data)
{
  GtkWidget *search_entry = data;
  GFileInfo *info = gtk_tree_list_row_get_item (item);
  GFile *file = g_object_get_data (G_OBJECT (info), "file");
  char *path;
  gboolean result;
  
  path = g_file_get_path (file);

  result = strstr (path, gtk_editable_get_text (GTK_EDITABLE (search_entry))) != NULL;

  g_object_unref (info);
  g_free (path);

  return result;
}

static void
search_changed_cb (GtkSearchEntry *entry,
                   GtkFilter      *custom_filter)
{
  gtk_filter_changed (custom_filter, GTK_FILTER_CHANGE_DIFFERENT);
}

int
main (int argc, char *argv[])
{
  GtkWidget *win, *vbox, *sw, *listview, *search_entry, *statusbar;
  GListModel *dirmodel;
  GtkTreeListModel *tree;
  GtkFilterListModel *filter;
  GtkFilter *custom_filter;
  GFile *root;
  GListModel *toplevels;

  gtk_init ();

  win = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (win), 400, 600);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (win), vbox);

  search_entry = gtk_search_entry_new ();
  gtk_box_append (GTK_BOX (vbox), search_entry);

  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_set_vexpand (sw, TRUE);
  gtk_search_entry_set_key_capture_widget (GTK_SEARCH_ENTRY (search_entry), sw);
  gtk_box_append (GTK_BOX (vbox), sw);

  listview = gtk_list_view_new ();
  gtk_list_view_set_functions (GTK_LIST_VIEW (listview),
                               setup_widget,
                               NULL,
                               NULL, NULL);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), listview);

  if (argc > 1)
    root = g_file_new_for_commandline_arg (argv[1]);
  else
    root = g_file_new_for_path (g_get_current_dir ());
  dirmodel = create_list_model_for_directory (root);
  tree = gtk_tree_list_model_new (FALSE,
                                  dirmodel,
                                  TRUE,
                                  create_list_model_for_file_info,
                                  NULL, NULL);
  g_object_unref (dirmodel);
  g_object_unref (root);

  custom_filter = gtk_custom_filter_new (match_file, search_entry, NULL);
  filter = gtk_filter_list_model_new (G_LIST_MODEL (tree), custom_filter);
  g_signal_connect (search_entry, "search-changed", G_CALLBACK (search_changed_cb), custom_filter);
  g_object_unref (custom_filter);

  gtk_list_view_set_model (GTK_LIST_VIEW (listview), G_LIST_MODEL (filter));

  statusbar = gtk_statusbar_new ();
  gtk_widget_add_tick_callback (statusbar, (GtkTickCallback) update_statusbar, NULL, NULL);
  g_object_set_data (G_OBJECT (statusbar), "model", filter);
  g_signal_connect_swapped (filter, "items-changed", G_CALLBACK (update_statusbar), statusbar);
  update_statusbar (GTK_STATUSBAR (statusbar));
  gtk_box_append (GTK_BOX (vbox), statusbar);

  g_object_unref (tree);
  g_object_unref (filter);

  gtk_widget_show (win);

  toplevels = gtk_window_get_toplevels ();
  while (g_list_model_get_n_items (toplevels))
    g_main_context_iteration (NULL, TRUE);

  return 0;
}