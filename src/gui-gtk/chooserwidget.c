/*
 * chooserwidget.c
 *
 * Copyright 2003-2004 Richard Drummond
 * Copyright 2011      Oliver Brunner (removed)
 *
 * $Id$
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include "chooserwidget.h"
#include "util.h"

#define DebOut(...) do { kprintf("%s:%d %s(): ",__FILE__,__LINE__,__func__);kprintf(__VA_ARGS__); } while(0)

/****i* januae-gui-gtk/combo_get_choice_num ********************************
*
*   NAME
*       combo_get_choice_num()
*
*   SYNOPSIS
*       guint combo_get_choice_num (GtkWidget *combo);
*
*   FUNCTION
*       Return the zero-based position of the currently selected item in a
*       GTK combo list.
*
*   INPUTS
*       combo - GTK combo widget containing the list.
*
*   RESULT
*       Selected item index, or the unsigned representation of -1 when the
*       list or selection is absent.
*
*
****************************************************************************
*
*/
guint combo_get_choice_num (GtkWidget *combo) {

  GtkList *list;
  GList   *choice;

  g_return_if_fail (GTK_IS_COMBO (combo));

  list=GTK_LIST (GTK_COMBO (combo)->list);

  if(!list) {
    DebOut("WARNING: GTK_COMBO(%lx)->list is NULL!\n", combo);
    return -1;
  }

  choice=list->selection;
    //printf("choice=%d\n",choice);
    //D(bug("chooser_get_choice_num: choice: %lx\n",choice));

  if (choice) {
    return gtk_list_child_position (list, choice->data);
  }
  else {
    return -1;
  }
}


