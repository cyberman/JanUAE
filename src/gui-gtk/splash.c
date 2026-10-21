/************************************************************************ 
 *
 * splash.c
 *
 * GTK splash screen
 *
 * Copyright 2012 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-UAE.
 *
 * Janus-UAE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Janus-UAE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Janus-UAE. If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 *
 ************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gtk/gtkwindow.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "threaddep/thread.h"
#include "debug.h"

#include "splash.h"

#define GUI_DEBUG 1
#ifdef  GUI_DEBUG
#define DEBUG_LOG(...) do { kprintf("%s:%d %s(): ",__FILE__,__LINE__,__func__);kprintf(__VA_ARGS__); } while(0)
#else
#define DEBUG_LOG(...) do ; while(0)
#endif

static jSplashClass *parent_class= NULL;

enum {
  TEXT_CHANGE_SIGNAL,
  TIME_CHANGE_SIGNAL,
  LAST_SIGNAL
};

static guint jsplash_signals[LAST_SIGNAL];

/***********************************************
 * jsplash_class_init
 ***********************************************/
static void jsplash_class_init (jSplashClass *class) {
  GtkWidgetClass *widget_class;
  GtkObjectClass *object_class;

  object_class = (GtkObjectClass*) class;
  widget_class = (GtkWidgetClass*) class;
  parent_class = g_type_class_peek_parent (class);

  gtkutil_add_signals_to_class (object_class,
				0,
				jsplash_signals,
				"text-changed",
				"time-changed",
				(void*)0);
}

/***********************************************
 * jsplash_init
 *
 * empty, as we do it in jsplash_new.
 ***********************************************/
static void jsplash_init (jSplash *splash) {

  DEBUG_LOG("jsplash_init\n");

}

/***********************************************
 * jsplash_get_type
 *
 * return type, if none exists.
 * create new type otherwise.
 ***********************************************/
GType jsplash_get_type (void) {

  static GType jsplash_type = 0;

  if (!jsplash_type) {
      static const GTypeInfo jsplash_info = {
        sizeof (jSplashClass),
        NULL,		/* base_init */
        NULL,		/* base_finalize */
        (GClassInitFunc) jsplash_class_init,
        NULL,		/* class_finalize */
        NULL,		/* class_data */
        sizeof (jSplash),
        0,		/* n_preallocs */
        (GInstanceInitFunc) jsplash_init,
	NULL,           /* value_table */
      };
      jsplash_type = g_type_register_static (GTK_TYPE_WINDOW, "jSplash", &jsplash_info, 0);
      parent_class = g_type_class_ref (GTK_TYPE_WINDOW);
    }

  return jsplash_type;
}

/***********************************************
 * jsplash_new
 ***********************************************/
GtkWidget* jsplash_new (void) {
  GtkWidget *jsplash;
  GtkWidget *button;

  DEBUG_LOG("entered\n");

  jsplash=g_object_new (TYPE_JSPLASH, NULL);

  button = gtk_button_new_with_label ("Hello World");

  gtk_container_add (GTK_CONTAINER (jsplash), button);
  gtk_widget_show (button);


  return jsplash;
}

/* is not called, even if enabled ? */
/*
static void jsplash_destroy(GtkObject *object) {

  g_return_if_fail (object != NULL);
  g_return_if_fail (IS_JINTEGRATION (object));



  if (GTK_OBJECT_CLASS (parent_class)->destroy)
    (* GTK_OBJECT_CLASS (parent_class)->destroy) (object);
}
*/


/***********************************************
 * change_lock (jSplash *j, gboolean status)
 *
 * updates the labels to reflect the current
 * states of the daemons.
 *
 * status is not used any more. 
 ***********************************************/

/************************** callbacks *************************/

static void text_changed(GtkWidget *me, jSplash *j) {

  DEBUG_LOG("TODO!\n");

}

static void time_changed(GtkWidget *win, jSplash *j) {

  DEBUG_LOG("TODO!\n");
}

static void hide(GtkWidget *win, jSplash *j) {
}

static void show(GtkWidget *me, jSplash *j) {


}

