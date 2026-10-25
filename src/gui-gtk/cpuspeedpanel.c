/************************************************************************ 
 *
 * cpuspeedpanel.c
 *
 * Copyright 2003-2005 Richard Drummond
 * Copyright 2011      Oliver Brunner - aros<at>oliver-brunner.de
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

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include "cpuspeedpanel.h"
#include "chooserwidget.h"
#include "util.h"

static void cpuspeedpanel_init (CpuSpeedPanel *cspanel);
static void cpuspeedpanel_class_init (CpuSpeedPanelClass *class);
static void update_state (CpuSpeedPanel *cspanel);
static void on_speed_changed    (GtkWidget *w, CpuSpeedPanel *cspanel);
static void on_adjust_changed   (GtkWidget *w, CpuSpeedPanel *cspanel);
static void on_idleenabled_toggled (GtkWidget *w, CpuSpeedPanel *cspanel);
static void on_idlerate_changed (GtkWidget *w, CpuSpeedPanel *cspanel);

/****i* januae-gtk-internal/cpuspeedpanel_get_type
    NAME
        cpuspeedpanel_get_type -- Register the CPU speed panel as a GTK frame subclass on first use.
    FUNCTION
        Register the CPU speed panel as a GTK frame subclass on first use.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
guint cpuspeedpanel_get_type () {

  static guint cpuspeedpanel_type = 0;

  if (!cpuspeedpanel_type) {
    static const GTypeInfo cpuspeedpanel_info = {
      sizeof (CpuSpeedPanelClass),
      NULL,		/* base_init */
      NULL,		/* base_finalize */
      (GClassInitFunc) cpuspeedpanel_class_init,
      NULL,		/* class_finalize */
      NULL,		/* class_data */
      sizeof (CpuSpeedPanel),
      0,		/* n_preallocs */
      (GInstanceInitFunc) cpuspeedpanel_init,
      NULL,           /* value_table */
    };
    cpuspeedpanel_type = g_type_register_static (GTK_TYPE_FRAME, "CpuSpeedPanel", &cpuspeedpanel_info, 0);
  }

  return cpuspeedpanel_type;
}

enum {
    SPEED_CHANGE_SIGNAL,
    IDLE_CHANGE_SIGNAL,
    LAST_SIGNAL
};

static guint cpuspeedpanel_signals[LAST_SIGNAL];

/****i* januae-gtk-internal/cpuspeedpanel_class_init
    NAME
        cpuspeedpanel_class_init -- Register the CPU speed and idle change signals on the panel class.
    FUNCTION
        Register the CPU speed and idle change signals on the panel class.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void cpuspeedpanel_class_init (CpuSpeedPanelClass *class)
{
    gtkutil_add_signals_to_class ((GtkObjectClass *)class,
				   GTK_STRUCT_OFFSET (CpuSpeedPanelClass, cpuspeedpanel),
				   cpuspeedpanel_signals,
				   "cpuspeed-changed",
				   "cpuidle-changed",
				   (void *)0);
    class->cpuspeedpanel = NULL;
}

/****i* januae-gtk-internal/cpuspeedpanel_init
    NAME
        cpuspeedpanel_init -- Create the speed selector, cycle adjustment and STOP-idling controls and connect their callbacks.
    FUNCTION
        Create the speed selector, cycle adjustment and STOP-idling controls and connect their callbacks.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void cpuspeedpanel_init (CpuSpeedPanel *cspanel)
{
    GtkWidget *table;
/*    GtkWidget *hbuttonbox, *button1, *button2; */

    gtk_frame_set_label (GTK_FRAME(cspanel), "Emulation speed");
    gtk_container_set_border_width (GTK_CONTAINER (cspanel), PANEL_BORDER_WIDTH);
    gtk_frame_set_label_align (GTK_FRAME(cspanel), 0.01, 0.5);

    gtkutil_add_table (GTK_WIDGET (cspanel),
	make_label ("Speed"), 1, 1, GTK_FILL,
	cspanel->speed_widget = make_chooser (3, "Maximum", "Approximate 7MHz 68000", "Adjustable"), 2, 1, GTK_EXPAND | GTK_FILL,
	GTKUTIL_ROW_END,
	make_label("Cycles per\ninstruction"), 1, 1, GTK_FILL,
        cspanel->adjust_widget = gtk_hscale_new (GTK_ADJUSTMENT (gtk_adjustment_new (2048, 1, 5121, 1, 512, 1))), 2, 1, GTK_FILL,
	GTKUTIL_ROW_END,

        cspanel->idleenabled_widget = gtk_check_button_new_with_label ("Idle on STOP instruction"), 1, 2, GTK_EXPAND,
	GTKUTIL_ROW_END,
	make_label("Idle rate"), 1, 1, GTK_FILL,
	cspanel->idlerate_widget = gtk_hscale_new (GTK_ADJUSTMENT (gtk_adjustment_new (1, 1, 21, 1, 1, 1))), 2, 1, GTK_FILL,
	GTKUTIL_ROW_END,

	GTKUTIL_TABLE_END
    );

    gtk_scale_set_digits (GTK_SCALE (cspanel->adjust_widget), 0);
    gtk_range_set_update_policy (GTK_RANGE (cspanel->adjust_widget), GTK_UPDATE_DISCONTINUOUS);

    gtk_scale_set_digits (GTK_SCALE (cspanel->idlerate_widget), 0);
    gtk_range_set_update_policy (GTK_RANGE (cspanel->idlerate_widget), GTK_UPDATE_DISCONTINUOUS);



    gtk_signal_connect (GTK_OBJECT (GTK_COMBO(cspanel->speed_widget)->popwin), "hide",
												GTK_SIGNAL_FUNC (on_speed_changed),
												cspanel);

    gtk_signal_connect (GTK_OBJECT ( GTK_RANGE(cspanel->adjust_widget)->adjustment), "value-changed",
												GTK_SIGNAL_FUNC (on_adjust_changed),
												cspanel);
    gtk_signal_connect (GTK_OBJECT (cspanel->idleenabled_widget), "toggled",
												GTK_SIGNAL_FUNC (on_idleenabled_toggled),
												cspanel);
    gtk_signal_connect (GTK_OBJECT (GTK_RANGE (cspanel->idlerate_widget)->adjustment), "value-changed",
												GTK_SIGNAL_FUNC (on_idlerate_changed),
												cspanel);

    update_state (cspanel);
}

/****i* januae-gtk-internal/update_state
    NAME
        update_state -- Enable or disable speed and idle controls according to the selected CPU speed mode.
    FUNCTION
        Enable or disable speed and idle controls according to the selected CPU speed mode.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void update_state (CpuSpeedPanel *cspanel)
{
    //guint speed = GTK_COMBO (cspanel->speed_widget)->choice;
    guint speed = combo_get_choice_num(cspanel->speed_widget);
    guint idleenabled = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cspanel->idleenabled_widget));

    gtk_widget_set_sensitive (cspanel->idleenabled_widget, speed != 1);
    gtk_widget_set_sensitive (cspanel->idlerate_widget, speed != 1 && idleenabled);
    gtk_widget_set_sensitive (cspanel->adjust_widget, speed == 2);

}

/****i* januae-gtk-internal/on_speed_changed
    NAME
        on_speed_changed -- Translate the selected speed mode into the stored CPU speed and emit cpuspeed-changed.
    FUNCTION
        Translate the selected speed mode into the stored CPU speed and emit cpuspeed-changed.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void on_speed_changed (GtkWidget *w, CpuSpeedPanel *cspanel)
{
    //int speed_choice = GTK_COMBO(cspanel->speed_widget)->choice;
    int speed_choice = combo_get_choice_num(cspanel->speed_widget);

    if (speed_choice == 0) {
			cspanel->cpuspeed = -1;
		}
    else if (speed_choice == 1) {
			cspanel->cpuspeed = 0;
		}
    else {
			cspanel->cpuspeed = (guint)GTK_ADJUSTMENT (GTK_RANGE (cspanel->adjust_widget)->adjustment)->value;
		}

    gtk_signal_emit_by_name (GTK_OBJECT(cspanel), "cpuspeed-changed");

    update_state (cspanel);
}

/****i* januae-gtk-internal/on_adjust_changed
    NAME
        on_adjust_changed -- Store the cycles-per-instruction slider value and emit cpuspeed-changed.
    FUNCTION
        Store the cycles-per-instruction slider value and emit cpuspeed-changed.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void on_adjust_changed (GtkWidget *w, CpuSpeedPanel *cspanel)
{
    cspanel->cpuspeed = (guint)GTK_ADJUSTMENT (GTK_RANGE (cspanel->adjust_widget)->adjustment)->value;
    gtk_signal_emit_by_name (GTK_OBJECT(cspanel), "cpuspeed-changed");
}

/****i* januae-gtk-internal/on_idleenabled_toggled
    NAME
        on_idleenabled_toggled -- Toggle STOP-instruction idling, update the idle rate, and emit cpuidle-changed.
    FUNCTION
        Toggle STOP-instruction idling, update the idle rate, and emit cpuidle-changed.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void on_idleenabled_toggled (GtkWidget *w, CpuSpeedPanel *cspanel)
{
    cspanel->idleenabled = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cspanel->idleenabled_widget));
    if (!cspanel->idleenabled)
	cspanel->cpuidle = 0;
    else
        cspanel->cpuidle = (21 - GTK_ADJUSTMENT (GTK_RANGE (cspanel->idlerate_widget)->adjustment)->value)*15;
    update_state (cspanel);
    gtk_signal_emit_by_name (GTK_OBJECT(cspanel), "cpuidle-changed");
}

/****i* januae-gtk-internal/on_idlerate_changed
    NAME
        on_idlerate_changed -- Convert the slider value into the stored idle rate and emit cpuidle-changed.
    FUNCTION
        Convert the slider value into the stored idle rate and emit cpuidle-changed.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
static void on_idlerate_changed (GtkWidget *w, CpuSpeedPanel *cspanel)
{
    cspanel->cpuidle = (21 - GTK_ADJUSTMENT (GTK_RANGE (cspanel->idlerate_widget)->adjustment)->value)*15;
    gtk_signal_emit_by_name (GTK_OBJECT(cspanel), "cpuidle-changed");
}

/****i* januae-gtk-internal/cpuspeedpanel_new
    NAME
        cpuspeedpanel_new -- Allocate and return a new CPU speed panel widget.
    FUNCTION
        Allocate and return a new CPU speed panel widget.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
GtkWidget *cpuspeedpanel_new (void)
{
    CpuSpeedPanel *w = CPUSPEEDPANEL (gtk_type_new (cpuspeedpanel_get_type ()));

    return GTK_WIDGET (w);
}

/****i* januae-gtk-internal/cpuspeedpanel_set_cpuspeed
    NAME
        cpuspeedpanel_set_cpuspeed -- Select the speed mode for the supplied CPU speed and update the adjustable slider when applicable.
    FUNCTION
        Select the speed mode for the supplied CPU speed and update the adjustable slider when applicable.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
void cpuspeedpanel_set_cpuspeed (CpuSpeedPanel *cspanel, gint cpuspeed) {

	int choice = 0;

	if (cpuspeed == -1)
		choice = 0;
	else if (cpuspeed == 0)
		choice = 1;
	else
		choice = 2;

	gtk_list_select_item (GTK_LIST (GTK_COMBO (cspanel->speed_widget)->list), choice);

	/* manuall call it here, as gtk-mui seems to have problems detecting this change.
	 * gtk-mui bug is in gtk_list.c:gtk_real_list_select_child() ..
	 */
	gtk_signal_emit_by_name (GTK_COMBO(cspanel->speed_widget)->list, "selection-changed");

	if (choice == 2) {
		/* update adjustable speed */
		gtk_adjustment_set_value (GTK_ADJUSTMENT (GTK_RANGE (cspanel->adjust_widget)->adjustment), cpuspeed);
	}

	update_state (cspanel);
}

/****i* januae-gtk-internal/cpuspeedpanel_set_cpuidle
    NAME
        cpuspeedpanel_set_cpuidle -- Set the STOP-idling checkbox and idle slider from a requested idle rate.
    FUNCTION
        Set the STOP-idling checkbox and idle slider from a requested idle rate.
    INPUTS
        See the C function signature; callback arguments and panel pointers belong to the GTK owner.
    RESULT
        See the C return type; void functions do not return a value.
****/
void cpuspeedpanel_set_cpuidle (CpuSpeedPanel *cspanel, guint cpuidle)
{
    int enable_idle = cpuidle > 0;

    if (enable_idle) {
	cspanel->cpuidle = 21 - (cpuidle / 15);
	gtk_adjustment_set_value (GTK_ADJUSTMENT (GTK_RANGE (cspanel->idlerate_widget)->adjustment), cspanel->cpuidle);
    }
    cspanel->idleenabled = enable_idle;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cspanel->idleenabled_widget), enable_idle);
}
