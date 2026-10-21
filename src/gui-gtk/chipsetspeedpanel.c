/*
 * chipsetspeedpanel.c
 *
 * Part of the Gtk+ UI for E-UAE
 * Copyright 2003-2004 Richard Drummond
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include "chipsetspeedpanel.h"
#include "chooserwidget.h"
#include "util.h"

#include "sysconfig.h"
#include "sysdeps.h"

#include "custom.h"

static void chipsetspeedpanel_init (ChipsetSpeedPanel *pathent);
static void chipsetspeedpanel_class_init (ChipsetSpeedPanelClass *class);
static void update_state (ChipsetSpeedPanel *panel);

static void on_framerate_changed (GtkWidget *w, ChipsetSpeedPanel *panel);
static void on_sprite_collisions_changed (GtkWidget *w, ChipsetSpeedPanel *panel);
static void on_immediate_blits_changed (GtkWidget *w, ChipsetSpeedPanel *panel);

/****i* januae-gui-gtk/chipsetspeedpanel_get_type **************************
*
*   NAME
*       chipsetspeedpanel_get_type()
*
*   SYNOPSIS
*       guint chipsetspeedpanel_get_type (void);
*
*   FUNCTION
*       Register the chipset speed frame widget on first use and return its
*       GTK type.
*
*   RESULT
*       The registered chipset speed panel type.
*
****************************************************************************
*
*/
guint chipsetspeedpanel_get_type (void)
{
    static guint chipsetspeedpanel_type = 0;

    if (!chipsetspeedpanel_type) {
	static const GtkTypeInfo chipsetspeedpanel_info = {
	    (char *) "ChipsetSpeedPanel",
	    sizeof (ChipsetSpeedPanel),
	    sizeof (ChipsetSpeedPanelClass),
	    (GtkClassInitFunc) chipsetspeedpanel_class_init,
	    (GtkObjectInitFunc) chipsetspeedpanel_init,
	    NULL,
	    NULL,
	    (GtkClassInitFunc) NULL
	};
	chipsetspeedpanel_type = gtk_type_unique (gtk_frame_get_type (), &chipsetspeedpanel_info);
    }
    return chipsetspeedpanel_type;
}

enum {
    FRAMERATE_CHANGE_SIGNAL,
    SPRITE_COLLISION_CHANGE_SIGNAL,
    IMMEDIATE_BLITS_CHANGE_SIGNAL,
    LAST_SIGNAL
};

static guint chipsetspeedpanel_signals[LAST_SIGNAL];

/****i* januae-gui-gtk/chipsetspeedpanel_class_init ************************
*
*   NAME
*       chipsetspeedpanel_class_init()
*
*   SYNOPSIS
*       static void chipsetspeedpanel_class_init (ChipsetSpeedPanelClass
*       *class);
*
*   FUNCTION
*       Register the frame-rate, sprite-collision and immediate-blit change
*       signals on the panel class.
*
*   INPUTS
*       class - Widget class being initialized.
*
****************************************************************************
*
*/
static void chipsetspeedpanel_class_init (ChipsetSpeedPanelClass *class)
{
    gtkutil_add_signals_to_class ((GtkObjectClass *)class,
				   GTK_STRUCT_OFFSET (ChipsetSpeedPanelClass, chipsetspeedpanel),
				   chipsetspeedpanel_signals,
				   "framerate-changed",
				   "sprite-collisions-changed",
				   "immediate-blits-changed",
				   (void*)0);
    class->chipsetspeedpanel = NULL;
}

/****i* januae-gui-gtk/chipsetspeedpanel_init ******************************
*
*   NAME
*       chipsetspeedpanel_init()
*
*   SYNOPSIS
*       static void chipsetspeedpanel_init (ChipsetSpeedPanel *panel);
*
*   FUNCTION
*       Build the frame-rate scale, sprite-collision combo and
*       immediate-blit checkbox and connect their change callbacks.
*
*   INPUTS
*       panel - New panel instance.
*
*   NOTES
*       The scale represents one through twenty-one frames per displayed
*       frame. Sprite-collision changes are detected when the combo popup
*       hides.
*
****************************************************************************
*
*/
static void chipsetspeedpanel_init (ChipsetSpeedPanel *panel)
{
    GtkWidget *table;
    GSList *group = NULL;

    gtk_frame_set_label (GTK_FRAME (panel), "Emulation speed");
    gtk_container_set_border_width (GTK_CONTAINER (panel), PANEL_BORDER_WIDTH);
    gtk_frame_set_label_align (GTK_FRAME (panel), 0.01, 0.5);

    gtkutil_add_table (GTK_WIDGET (panel),
        make_label("Draw one\nframe in"), 1, 1, GTK_FILL,
        panel->framerate_widget = gtk_hscale_new (GTK_ADJUSTMENT (gtk_adjustment_new (1, 1, 21, 1, 1, 1))), 2, 1, GTK_FILL,
        GTKUTIL_ROW_END,
	make_label ("Sprite collisions"), 1, 1, GTK_FILL,
	panel->collisions_widget = make_chooser (4, "None", "Sprites only", "Sprites & playfields", "Full"), 2, 1, GTK_EXPAND | GTK_FILL,
	GTKUTIL_ROW_END,
	panel->immediate_blits_widget = gtk_check_button_new_with_label ("Immediate blits"), 1, 2, GTK_EXPAND,
	GTKUTIL_ROW_END,
	GTKUTIL_TABLE_END
    );
    gtk_scale_set_value_pos (GTK_SCALE (panel->framerate_widget), GTK_POS_TOP);
    gtk_scale_set_digits (GTK_SCALE (panel->framerate_widget), 0);

    gtk_signal_connect (GTK_OBJECT ( GTK_RANGE(panel->framerate_widget)->adjustment), "value-changed",
			                  GTK_SIGNAL_FUNC (on_framerate_changed),
			                  panel);
		gtk_signal_connect (GTK_OBJECT (GTK_COMBO(panel->collisions_widget)->popwin), "hide",
			                  GTK_SIGNAL_FUNC (on_sprite_collisions_changed),
			                  panel);
		                   
    gtk_signal_connect (GTK_OBJECT (panel->immediate_blits_widget), "toggled",
			                  GTK_SIGNAL_FUNC (on_immediate_blits_changed),
			                  panel);
}

/****i* januae-gui-gtk/on_framerate_changed ********************************
*
*   NAME
*       on_framerate_changed()
*
*   SYNOPSIS
*       static void on_framerate_changed (GtkWidget *w, ChipsetSpeedPanel
*       *panel);
*
*   FUNCTION
*       Copy the scale value into the panel frame-rate state and emit the
*       framerate-changed signal.
*
*   INPUTS
*       w - Unused signal source.
*       panel - Panel whose frame-rate scale changed.
*
****************************************************************************
*
*/
static void on_framerate_changed (GtkWidget *w, ChipsetSpeedPanel *panel)
{
    panel->framerate = (guint) GTK_ADJUSTMENT (GTK_RANGE (panel->framerate_widget)->adjustment)->value;
    gtk_signal_emit_by_name (GTK_OBJECT (panel), "framerate-changed");
}

/****i* januae-gui-gtk/on_sprite_collisions_changed ************************
*
*   NAME
*       on_sprite_collisions_changed()
*
*   SYNOPSIS
*       static void on_sprite_collisions_changed (GtkWidget *w,
*       ChipsetSpeedPanel *panel);
*
*   FUNCTION
*       Read the selected collision mode from the GTK combo and emit
*       sprite-collisions-changed.
*
*   INPUTS
*       w - Unused signal source.
*       panel - Panel with the collision selector.
*
*   NOTES
*       An absent selection is returned by combo_get_choice_num() as the
*       unsigned representation of -1.
*
****************************************************************************
*
*/
static void on_sprite_collisions_changed (GtkWidget *w, ChipsetSpeedPanel *panel)
{
    //panel->collision_level = GTK_COMBO (w)->choice;
    panel->collision_level = combo_get_choice_num(panel->collisions_widget);
    gtk_signal_emit_by_name (GTK_OBJECT (panel), "sprite-collisions-changed");
}

/****i* januae-gui-gtk/on_immediate_blits_changed **************************
*
*   NAME
*       on_immediate_blits_changed()
*
*   SYNOPSIS
*       static void on_immediate_blits_changed (GtkWidget *w,
*       ChipsetSpeedPanel *panel);
*
*   FUNCTION
*       Read the immediate-blits checkbox and emit immediate-blits-changed.
*
*   INPUTS
*       w - Unused signal source.
*       panel - Panel whose checkbox changed.
*
****************************************************************************
*
*/
static void on_immediate_blits_changed (GtkWidget *w, ChipsetSpeedPanel *panel)
{
     panel->immediate_blits = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (panel->immediate_blits_widget));
     gtk_signal_emit_by_name (GTK_OBJECT(panel), "immediate-blits-changed");
}


/****i* januae-gui-gtk/chipsetspeedpanel_new *******************************
*
*   NAME
*       chipsetspeedpanel_new()
*
*   SYNOPSIS
*       GtkWidget *chipsetspeedpanel_new (void);
*
*   FUNCTION
*       Allocate a chipset speed panel widget through its registered GTK
*       type.
*
*   RESULT
*       The widget returned by GTK allocation.
*
****************************************************************************
*
*/
GtkWidget *chipsetspeedpanel_new (void)
{
    ChipsetSpeedPanel *w = CHIPSETSPEEDPANEL (gtk_type_new (chipsetspeedpanel_get_type ()));

    return GTK_WIDGET (w);
}

/****i* januae-gui-gtk/chipsetspeedpanel_set_framerate *********************
*
*   NAME
*       chipsetspeedpanel_set_framerate()
*
*   SYNOPSIS
*       void chipsetspeedpanel_set_framerate (ChipsetSpeedPanel *panel,
*       guint framerate);
*
*   FUNCTION
*       Set the frame-rate adjustment to the requested value.
*
*   INPUTS
*       panel - Panel to update.
*       framerate - Requested frame-rate scale value.
*
*   NOTES
*       GTK adjustment handling determines any clamping and signal emission.
*
****************************************************************************
*
*/
void chipsetspeedpanel_set_framerate (ChipsetSpeedPanel *panel, guint framerate)
{
    gtk_adjustment_set_value (GTK_ADJUSTMENT (GTK_RANGE (panel->framerate_widget)->adjustment), framerate);
}

/****i* januae-gui-gtk/chipsetspeedpanel_set_collision_level ***************
*
*   NAME
*       chipsetspeedpanel_set_collision_level()
*
*   SYNOPSIS
*       void chipsetspeedpanel_set_collision_level (ChipsetSpeedPanel
*       *panel, guint collision_level);
*
*   FUNCTION
*       Select the zero-based collision-mode entry in the combo list.
*
*   INPUTS
*       panel - Panel to update.
*       collision_level - Zero-based combo-list index.
*
*   NOTES
*       The function does not validate the index before calling
*       gtk_list_select_item().
*
****************************************************************************
*
*/
void chipsetspeedpanel_set_collision_level (ChipsetSpeedPanel *panel, guint collision_level)
{
    //chooserwidget_set_choice (GTK_COMBO (panel->collisions_widget), collision_level);
		gtk_list_select_item (GTK_LIST (GTK_COMBO (panel->collisions_widget)->list), collision_level);
}

/****i* januae-gui-gtk/chipsetspeedpanel_set_immediate_blits ***************
*
*   NAME
*       chipsetspeedpanel_set_immediate_blits()
*
*   SYNOPSIS
*       void chipsetspeedpanel_set_immediate_blits (ChipsetSpeedPanel
*       *panel, guint immediate_blits);
*
*   FUNCTION
*       Set the immediate-blits checkbox to the truth value of the input.
*
*   INPUTS
*       panel - Panel to update.
*       immediate_blits - Zero to clear the checkbox, nonzero to select it.
*
****************************************************************************
*
*/
void chipsetspeedpanel_set_immediate_blits (ChipsetSpeedPanel *panel, guint immediate_blits)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (panel->immediate_blits_widget), immediate_blits != 0);
}
