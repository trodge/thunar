/* $Id$ */
/*-
 * Copyright (c) 2005 Benedikt Meurer <benny@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <thunar/thunar-details-view.h>
#include <thunar/thunar-details-view-icon-renderer.h>



static void       thunar_details_view_class_init          (ThunarDetailsViewClass *klass);
static void       thunar_details_view_init                (ThunarDetailsView      *details_view);
static AtkObject *thunar_details_view_get_accessible      (GtkWidget              *widget);
static GList     *thunar_details_view_get_selected_items  (ThunarStandardView     *standard_view);
static void       thunar_details_view_row_activated       (GtkTreeView            *tree_view,
                                                           GtkTreePath            *path,
                                                           GtkTreeViewColumn      *column,
                                                           ThunarDetailsView      *details_view);



struct _ThunarDetailsViewClass
{
  ThunarStandardViewClass __parent__;
};

struct _ThunarDetailsView
{
  ThunarStandardView __parent__;
};



G_DEFINE_TYPE (ThunarDetailsView, thunar_details_view, THUNAR_TYPE_STANDARD_VIEW);



static void
thunar_details_view_class_init (ThunarDetailsViewClass *klass)
{
  ThunarStandardViewClass *thunarstandard_view_class;
  GtkWidgetClass          *gtkwidget_class;

  gtkwidget_class = GTK_WIDGET_CLASS (klass);
  gtkwidget_class->get_accessible = thunar_details_view_get_accessible;

  thunarstandard_view_class = THUNAR_STANDARD_VIEW_CLASS (klass);
  thunarstandard_view_class->get_selected_items = thunar_details_view_get_selected_items;
}



static void
thunar_details_view_init (ThunarDetailsView *details_view)
{
  GtkTreeViewColumn *column;
  GtkTreeSelection  *selection;
  GtkCellRenderer   *renderer;
  GtkWidget         *tree_view;

  /* create the tree view to embed */
  tree_view = gtk_tree_view_new ();
  g_signal_connect (G_OBJECT (tree_view), "row-activated",
                    G_CALLBACK (thunar_details_view_row_activated), details_view);
  gtk_container_add (GTK_CONTAINER (details_view), tree_view);
  gtk_widget_show (tree_view);

  /* configure general aspects of the details view */
  gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (tree_view), TRUE);
  gtk_tree_view_set_enable_search (GTK_TREE_VIEW (tree_view), TRUE);
  gtk_tree_view_set_search_column (GTK_TREE_VIEW (tree_view), THUNAR_LIST_MODEL_COLUMN_NAME);

  /* first column (icon, name) */
  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "expand", TRUE,
                         "reorderable", TRUE,
                         "resizable", TRUE,
                         "title", _("Name"),
                         NULL);
  renderer = thunar_details_view_icon_renderer_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "file", THUNAR_LIST_MODEL_COLUMN_FILE,
                                       NULL);
  renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                           "xalign", 0.0,
                           NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "text", THUNAR_LIST_MODEL_COLUMN_NAME,
                                       NULL);
  gtk_tree_view_column_set_sort_column_id (column, THUNAR_LIST_MODEL_COLUMN_NAME);
  gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

  /* second column (size) */
  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "reorderable", TRUE,
                         "resizable", TRUE,
                         "title", _("Size"),
                         NULL);
  renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                           "xalign", 1.0,
                           NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "text", THUNAR_LIST_MODEL_COLUMN_SIZE,
                                       NULL);
  gtk_tree_view_column_set_sort_column_id (column, THUNAR_LIST_MODEL_COLUMN_SIZE);
  gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

  /* third column (permissions) */
  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "reorderable", TRUE,
                         "resizable", TRUE,
                         "title", _("Permissions"),
                         NULL);
  renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                           "xalign", 0.0,
                           NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "text", THUNAR_LIST_MODEL_COLUMN_PERMISSIONS,
                                       NULL);
  gtk_tree_view_column_set_sort_column_id (column, THUNAR_LIST_MODEL_COLUMN_PERMISSIONS);
  gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

  /* fourth column (type) */
  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "reorderable", TRUE,
                         "resizable", TRUE,
                         "title", _("Type"),
                         NULL);
  renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                           "xalign", 0.0,
                           NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "text", THUNAR_LIST_MODEL_COLUMN_TYPE,
                                       NULL);
  gtk_tree_view_column_set_sort_column_id (column, THUNAR_LIST_MODEL_COLUMN_TYPE);
  gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

  /* fifth column (modification date) */
  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "reorderable", TRUE,
                         "resizable", TRUE,
                         "title", _("Date modified"),
                         NULL);
  renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                           "xalign", 0.0,
                           NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "text", THUNAR_LIST_MODEL_COLUMN_DATE_MODIFIED,
                                       NULL);
  gtk_tree_view_column_set_sort_column_id (column, THUNAR_LIST_MODEL_COLUMN_DATE_MODIFIED);
  gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

  /* configure the tree selection */
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);
  g_signal_connect_swapped (G_OBJECT (selection), "changed",
                            G_CALLBACK (thunar_standard_view_selection_changed), details_view);
}



static AtkObject*
thunar_details_view_get_accessible (GtkWidget *widget)
{
  AtkObject *object;

  /* query the atk object for the tree view class */
  object = GTK_WIDGET_CLASS (thunar_details_view_parent_class)->get_accessible (widget);

  /* set custom Atk properties for the details view */
  if (G_LIKELY (object != NULL))
    {
      atk_object_set_name (object, _("Details view"));
      atk_object_set_description (object, _("Detailed directory listing"));
    }

  return object;
}



static GList*
thunar_details_view_get_selected_items (ThunarStandardView *standard_view)
{
  GtkTreeSelection *selection;

  g_return_val_if_fail (THUNAR_IS_DETAILS_VIEW (standard_view), NULL);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (GTK_BIN (standard_view)->child));
  return gtk_tree_selection_get_selected_rows (selection, NULL);
}



static void
thunar_details_view_row_activated (GtkTreeView       *tree_view,
                                   GtkTreePath       *path,
                                   GtkTreeViewColumn *column,
                                   ThunarDetailsView *details_view)
{
  GtkTreeModel *model;
  GtkTreeIter   iter;
  ThunarFile   *file;

  g_return_if_fail (THUNAR_IS_DETAILS_VIEW (details_view));

  /* tell the controlling component that the user activated a file */
  model = gtk_tree_view_get_model (tree_view);
  gtk_tree_model_get_iter (model, &iter, path);
  file = thunar_list_model_get_file (THUNAR_LIST_MODEL (model), &iter);
  if (thunar_file_is_directory (file))
    thunar_navigator_change_directory (THUNAR_NAVIGATOR (details_view), file);
  g_object_unref (G_OBJECT (file));
}



/**
 * thunar_details_view_new:
 *
 * Allocates a new #ThunarDetailsView instance, which is not
 * associated with any #ThunarListModel yet.
 *
 * Return value: the newly allocated #ThunarDetailsView instance.
 **/
GtkWidget*
thunar_details_view_new (void)
{
  return g_object_new (THUNAR_TYPE_DETAILS_VIEW, NULL);
}



