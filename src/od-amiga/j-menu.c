/************************************************************************ 
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
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

#include <libraries/gadtools.h>
#include <proto/gadtools.h>

//#define JWTRACING_ENABLED 1
#include "j.h"

static ULONG  count_items(uaecptr menu);
static WORD   parse_flags(WORD aos3flags);
static STRPTR parse_label(UWORD flags, uaecptr intuitextptr);
static STRPTR parse_key(UWORD flags, uaecptr keytext);

/* gadtools uses inverted flags as intuition */
/****i* januae/swap_enabled ************************************************
*
*   NAME
*       swap_enabled() -- Invert a menu-item enabled flag.
*
*   SYNOPSIS
*       static WORD swap_enabled(WORD flags);
*
*   FUNCTION
*       Toggle ITEMENABLED while preserving the remaining flag bits.
*
*   INPUTS
*       flags - Source menu-item flags.
*
*   RESULT
*       Flags with ITEMENABLED toggled.
*
****************************************************************************
*
*/

static WORD swap_enabled(WORD flags) {

  if(flags & ITEMENABLED) {
    flags=flags - ITEMENABLED;
  }
  else {
    flags=flags | ITEMENABLED;
  }
  return flags;
}

/* clear all non gadtools flags out of intuition flags */
/****i* januae/parse_flags *************************************************
*
*   NAME
*       parse_flags() -- Convert guest menu-item flags.
*
*   SYNOPSIS
*       static WORD parse_flags(WORD aos3flags);
*
*   FUNCTION
*       Keep supported Intuition menu-item flags and invert the ITEMENABLED
*       bit for the GadTools representation.
*
*   INPUTS
*       aos3flags - Guest menu-item flags.
*
*   RESULT
*       Converted menu-item flags.
*
****************************************************************************
*
*/

static WORD parse_flags(WORD aos3flags) {
  WORD arosflags;

  JWLOG("aos3flags: %x\n",aos3flags);

  /* keep relevan flags */
  aos3flags=aos3flags & (CHECKIT | ITEMTEXT | COMMSEQ |
			 ITEMENABLED | CHECKED | HIGHNONE);

  arosflags=swap_enabled(aos3flags);

  JWLOG("arosflags: %x\n",arosflags);
  return arosflags;
}

/* return the label text pointer
 * we uses the original aos3 strings, no need to copy them
 *
 * a BIG problem here are MENUBARs. If you create them, they
 * are specified with 0xff as type. Once they are a real
 * MenuItem, ItemFill points to am image of the menubar, which we
 * can't detect (?). So we abuse HIGHNONE, as no "real" item
 * uses this anyways (hopefully)
 */
/****i* januae/parse_label *************************************************
*
*   NAME
*       parse_label() -- Resolve a guest menu-item label.
*
*   SYNOPSIS
*       static STRPTR parse_label(UWORD flags, uaecptr intuitextptr);
*
*   FUNCTION
*       Return NM_BARLABEL for entries identified as separators, or resolve
*       the guest IntuiText string address.
*
*   INPUTS
*       flags - Converted item flags. intuitextptr - Address of guest
*       IntuiText data.
*
*   RESULT
*       A GadTools separator marker or a pointer into guest text memory.
*
*   NOTES
*       The returned text is not copied. HIGHNONE is treated as a separator
*       marker.
*
****************************************************************************
*
*/

static STRPTR parse_label(UWORD flags, uaecptr intuitextptr) {

  if((flags & HIGHFLAGS)==HIGHNONE) {
    /* assume BAR */
    JWLOG("NM_BARLABEL\n");
    return NM_BARLABEL;
  }

  JWLOG("nm_Label: %s\n",get_real_address(get_long(intuitextptr)));
  return get_real_address(get_long(intuitextptr));
}

/* return command key or NULL*/
/****i* januae/parse_key ***************************************************
*
*   NAME
*       parse_key() -- Resolve a menu command key.
*
*   SYNOPSIS
*       static STRPTR parse_key(UWORD flags, uaecptr keytext);
*
*   FUNCTION
*       Expose the guest command-key address when COMMSEQ is set.
*
*   INPUTS
*       flags - Menu-item flags. keytext - Guest command-key address.
*
*   RESULT
*       A key pointer, or NULL when COMMSEQ is absent.
*
****************************************************************************
*
*/

static STRPTR parse_key(UWORD flags, uaecptr keytext) {

  if(flags & COMMSEQ) {
    return get_real_address(keytext);
  }
  return NULL; /* if !NULL, AROS ignores !COMMSEQ ??*/
}

/* count required number of gadtools rows */
/****i* januae/count_items *************************************************
*
*   NAME
*       count_items() -- Count flattened menu entries.
*
*   SYNOPSIS
*       static ULONG count_items(uaecptr menu);
*
*   FUNCTION
*       Traverse guest menu, item and subitem chains and count entries
*       needed for the GadTools menu array, including its terminator.
*
*   INPUTS
*       menu - Guest Menu linked-list address.
*
*   RESULT
*       Required entry count including NM_END.
*
****************************************************************************
*
*/

static ULONG count_items(uaecptr menu) {
  uaecptr item;
  uaecptr sub;
  ULONG i=0;

  while(menu) {
    i++;
    item=get_long(menu + 18);
    while(item) {
      i++;
      sub=get_long(item+28);
      while(sub) {
        i++;
        sub=get_long(sub);
      }
      item=get_long(item);
    }
    menu=get_long(menu);
  }

  i++; /* end tag */

  JWLOG("items counted: %d\n",i);
  return i;
}

/****i* januae/get_aos3_menustrip ******************************************
*
*   NAME
*       get_aos3_menustrip() -- Read a guest window menu strip.
*
*   SYNOPSIS
*       uaecptr get_aos3_menustrip(uaecptr aos3win);
*
*   FUNCTION
*       Read the menu-strip pointer from the guest window structure.
*
*   INPUTS
*       aos3win - Guest window address.
*
*   RESULT
*       Guest menu-strip address, or zero if no menu strip exists.
*
****************************************************************************
*
*/

uaecptr get_aos3_menustrip(uaecptr aos3win) {
  uaecptr aos3menustrip;

  aos3menustrip=get_long(aos3win + 28);
  if(!aos3menustrip) {
    JWLOG("aos3 window %lx has no menustrip\n",aos3win);
    return NULL;
  }
  JWLOG("aos3menustrip: %lx\n",aos3menustrip);
  return aos3menustrip;
}


/********************************
 * clone_menu
 *
 * take the menu tree from aos3
 * and add it to aros window
 ********************************/
/****i* januae/clone_menu **************************************************
*
*   NAME
*       clone_menu() -- Install a host copy of a guest menu.
*
*   SYNOPSIS
*       void clone_menu(JanusWin *jwin);
*
*   FUNCTION
*       Translate guest menus, items and subitems into GadTools NewMenu
*       entries, create and lay out a host menu, and install it on the
*       corresponding AROS window.
*
*   INPUTS
*       jwin - Janus window containing the guest and host window state.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The translation uses guest label addresses directly and replaces the
*       host menu strip when one is already present.
*
****************************************************************************
*
*/

void clone_menu(JanusWin *jwin) {
  struct Window   *aroswin=jwin->aroswin;
  uaecptr          aos3win=(uaecptr) jwin->aos3win;
  struct NewMenu  *newmenu;
  struct NewMenu  *m;
  struct Menu     *arosmenu;
  APTR             vi;
  uaecptr          aos3menustrip;
  uaecptr          aos3item;
  uaecptr          aos3sub;
  uaecptr          aos3itemfill;
  ULONG            i;
  ULONG            nr_entries;

  JWLOG("started for jwin %lx\n", jwin);

  if(!aroswin || !aos3win) {
    JWLOG("aoswin or aos3win is NULL\n");
    return;
  }

  /* TODO: FreeVisualInfo.. why not already done ? */
  vi = GetVisualInfoA(jwin->jscreen->arosscreen, NULL);
  if(!vi) {
    JWLOG("ERROR: no vi (you have to use that editor!)\n");
    return;
  }

  /* we still have an old menu strip, 
   * we want to get rid of it (necessary?) 
   */
  if(jwin->arosmenu) {
    ClearMenuStrip(jwin->aroswin);
  }

  aos3menustrip=get_aos3_menustrip(aos3win);
  if(!aos3menustrip) {
    JWLOG("no aos3menustrip => do nothing\n");
    return;
  }

  nr_entries=count_items(aos3menustrip);

  if(nr_entries != jwin->nr_menu_items) {
    /* we need other mem */
    FreeVecPooled(jwin->mempool, jwin->newmenu_mem);
    jwin->newmenu_mem=AllocVecPooled(jwin->mempool, 
                                     nr_entries * sizeof(struct Menu));
  }
  newmenu=jwin->newmenu_mem;

  i=0;
  while(aos3menustrip) {
    /*********** Menu ************/
    newmenu[i].nm_Type=NM_TITLE;
    newmenu[i].nm_Label=get_real_address(get_long(aos3menustrip+14));
    newmenu[i].nm_CommKey=NULL;
    /* Menu has other flags than MenuItem */
    if(get_word(aos3menustrip+12) & MENUENABLED) {
      newmenu[i].nm_Flags=0;
    }
    else {
      newmenu[i].nm_Flags=MENUENABLED;
    }
    newmenu[i].nm_MutualExclude=0;
    newmenu[i].nm_UserData=NULL;
    i++;

    aos3item=get_long(aos3menustrip + 18);
    while(aos3item) {
      /*********** item ************/
      aos3itemfill=get_long(aos3item+18); /* IntuiText */
      if(aos3itemfill) { 
	newmenu[i].nm_Type =NM_ITEM;
	newmenu[i].nm_Flags=parse_flags(get_word(aos3item+12)); 

	newmenu[i].nm_Label=parse_label(newmenu[i].nm_Flags, aos3itemfill+12);

	newmenu[i].nm_CommKey=parse_key(newmenu[i].nm_Flags, aos3item+26);
	newmenu[i].nm_MutualExclude=0;
	newmenu[i].nm_UserData=NULL;
	i++;

	aos3sub=get_long(aos3item+28);
	while(aos3sub) {
	  /*********** subitem ************/
	  aos3itemfill=get_long(aos3sub+18); /* IntuiText */
	  if(aos3itemfill) {
	    newmenu[i].nm_Type =NM_SUB;
	    newmenu[i].nm_Flags=parse_flags(get_word(aos3sub+12)); 
	    newmenu[i].nm_Label=parse_label(newmenu[i].nm_Flags, aos3itemfill+12);
	    newmenu[i].nm_CommKey=parse_key(newmenu[i].nm_Flags, aos3sub+26);
	    newmenu[i].nm_MutualExclude=0;
	    newmenu[i].nm_UserData=NULL;
	    i++;
	  }
	  aos3sub=get_long(aos3sub); /* Next */
	}
      }
      else {
        /* ignore */
        JWLOG("TODO: !aos3itemfill <==================\n");
      }
      aos3item=get_long(aos3item); /* NextItem */
    }

    aos3menustrip=get_long(aos3menustrip); /* NextMenu */
  }

  newmenu[i].nm_Type=NM_END;
  newmenu[i].nm_Label=NULL;

  nr_entries=i;

  /*The first argument is a pointer to an array of NewMenu structures */
  arosmenu=CreateMenus(newmenu, NULL);
  if(!arosmenu) {
    JWLOG("failed to CreateMenus(%lx)\n",newmenu);
    return;
  }

  if(!LayoutMenus(arosmenu, vi, GTMN_NewLookMenus, TRUE, TAG_DONE)) {
    JWLOG("LayoutMenus failed!\n");
    return;
  }

  SetMenuStrip(aroswin, arosmenu);

  jwin->arosmenu=arosmenu;

  return;
}

/******************************
 * wait_menu_shown
 *
 * waits until the MIDRAWN flag
 * indicates, that aos3 
 * intuition finished showing
 * our menu.
 *
 * Timeout, if it did not open
 ******************************/
/****i* januae/wait_menu_shown *********************************************
*
*   NAME
*       wait_menu_shown() -- Wait for a guest menu to be drawn.
*
*   SYNOPSIS
*       static BOOL wait_menu_shown(uaecptr menu_flags);
*
*   FUNCTION
*       Poll MIDRAWN in guest menu flags with bounded delays.
*
*   INPUTS
*       menu_flags - Guest menu flag word address.
*
*   RESULT
*       TRUE if MIDRAWN is observed; otherwise FALSE after 20 attempts.
*
*   NOTES
*       Each unsuccessful attempt delays for five ticks.
*
****************************************************************************
*
*/

static BOOL wait_menu_shown(uaecptr menu_flags) {
  ULONG i;
  UWORD flags;

  JWLOG("wait for menu to be drawn ..\n");

  i=0;
  while(i<20) {
    flags=get_word(menu_flags);
    //JWLOG("flags: %x\n", flags);
    //JWLOG("flags 2: %x\n", flags|MIDRAWN);
    if(flags & MIDRAWN) {
      JWLOG("menu drawn (after waiting %d delays(5))\n",i);
      return TRUE;
    }
    i++;
    Delay(5);
  }
  JWLOG("ERROR: menu not drawn !!\n");

  return FALSE;
}

/* wait_menu_closed
 *
 * wait until all menus windows of this window are closed,
 * so it is safe, to sync windows again, for example
 */
/****i* januae/wait_window_menu_closed *************************************
*
*   NAME
*       wait_window_menu_closed() -- Wait for menu dismissal.
*
*   SYNOPSIS
*       void wait_window_menu_closed(JanusWin *jwin);
*
*   FUNCTION
*       Poll the guest menu strip until MIDRAWN clears, or stop after a
*       bounded number of attempts.
*
*   INPUTS
*       jwin - Janus window whose guest menu is being inspected.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The function returns immediately for a missing guest window or menu
*       strip.
*
****************************************************************************
*
*/

void wait_window_menu_closed(JanusWin *jwin) {
  uaecptr          aos3win;
  uaecptr          aos3menustrip;
  uaecptr          menuflags;
  ULONG            i;
  UWORD            flags;

  aos3win=(uaecptr) jwin->aos3win;
  if(!aos3win) {
    return;
  }

  aos3menustrip=get_long(aos3win + 28);
  if(!aos3menustrip) {
    JWLOG("aos3 window %lx has no menustrip\n",aos3win);
    return;
  }

  menuflags=aos3menustrip + 12;

  i=0;
  while(i<20) {
    flags=get_word(menuflags);
    //JWLOG("flags: %x\n", flags);
    //JWLOG("flags 2: %x\n", flags|MIDRAWN);
    if(!(flags & MIDRAWN)) {
      JWLOG("menu closed (after waiting %d delays(5))\n",i);
      return;
    }
    i++;
    Delay(5);
  }
  JWLOG("could not wait unti menu for aos3 window %lx was closed, sorry.\n",aos3win);
}

/****i* januae/wait_menuitem_shown *****************************************
*
*   NAME
*       wait_menuitem_shown() -- Wait for a guest menu item to appear.
*
*   SYNOPSIS
*       static BOOL wait_menuitem_shown(uaecptr menu_flags);
*
*   FUNCTION
*       Poll the ISDRAWN flag in guest menu-item state.
*
*   INPUTS
*       menu_flags - Guest menu-item flag word address.
*
*   RESULT
*       TRUE when the item is drawn; FALSE after 20 attempts.
*
****************************************************************************
*
*/

static BOOL wait_menuitem_shown(uaecptr menu_flags) {
  ULONG i;
  UWORD flags;

  i=0;
  while(i<20) {
    flags=get_word(menu_flags);
    if(flags & ISDRAWN) {
      JWLOG("menu item drawn (%d)\n",i);
      return TRUE;
    }
    i++;
    Delay(5);
  }
  JWLOG("menu item not drawn !!\n");
  return FALSE;
}

/*****************************************************************
 * wait_menu_mouse_pos
 *
 * as the janusd can not move the mouse at once, we have to wait
 * until the mouse is on position. Use Delay() to not busy wait.
 *****************************************************************/
/****i* januae/wait_menu_mouse_pos *****************************************
*
*   NAME
*       wait_menu_mouse_pos() -- Wait for a guest pointer position.
*
*   SYNOPSIS
*       static BOOL wait_menu_mouse_pos(JanusWin *jwin, WORD MouseX, WORD
*       MouseY);
*
*   FUNCTION
*       Poll guest screen pointer coordinates until they equal the requested
*       location.
*
*   INPUTS
*       jwin - Janus window owning the guest screen. MouseX, MouseY - Target
*       guest pointer coordinates.
*
*   RESULT
*       TRUE when both coordinates match; FALSE after 20 attempts.
*
****************************************************************************
*
*/

static BOOL wait_menu_mouse_pos(JanusWin *jwin, WORD MouseX, WORD MouseY) {
  ULONG i;
  uaecptr scr;

  scr=jwin->jscreen->aos3screen;
  i=0;
  while(i<20) {
    if((get_word(scr+16) == MouseY) &&
       (get_word(scr+18) == MouseX)) {
      JWLOG("mouse on position (%d)\n",i);
      return TRUE;
    }
    i++;
    Delay(5);
  }

  JWLOG("ERROR: mouse not moved to %d, %d!!\n", MouseX, MouseY);
  return FALSE;
}

/* click_menu
 *
 * click on menu, item, subitem
 *
 * -1: don't use this level
 *
 * This is not what you would expect here. It is more Copperfield
 * Magic than amigaOS programming. We disabled all gfx updates
 * and locked out all mouse events from AROS. So now it is time
 * for the illusion. As nobody can see us, we move the amigaOS
 * mousepointer first to the menu, then to the item and if
 * necessary to the subitem. And then, we simply release the
 * right mouse button of the virtual uae mouse. voila ;).
 *
 * After that, we switch on mouse/updates again and nobody
 * has noticed it (hopefully ;)). Perfect illusion, or David?
 */
/****i* januae/click_menu **************************************************
*
*   NAME
*       click_menu() -- Drive a guest menu selection.
*
*   SYNOPSIS
*       void click_menu(JanusWin *jwin, WORD menu, WORD item, WORD sub);
*
*   FUNCTION
*       Locate the requested guest menu and item, submit intermediate
*       pointer coordinates through menux and menuy, and wait for menu
*       drawing and pointer movement.
*
*   INPUTS
*       jwin - Janus window. menu, item, sub - Zero-based menu indices; -1
*       skips the item or subitem level.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Pointer movement is performed by the guest mouse synchronization
*       path. Selection is abandoned if a required wait fails.
*
****************************************************************************
*
*/

void click_menu(JanusWin *jwin, WORD menu, WORD item, WORD sub) {
  uaecptr          aos3win;
  uaecptr          aos3screen;
  uaecptr          aos3menustrip;
  uaecptr          aos3item;
  uaecptr          aos3sub;
  WORD             x, y;
  WORD             rx, ry;
  int              i,j,l;

  JWLOG("click menu %d, item %d, submenu %d\n",menu, item, sub);

  aos3win=(uaecptr) jwin->aos3win;
  if(!aos3win) {
    return;
  }

  aos3menustrip=get_long(aos3win + 28);
  if(!aos3menustrip) {
    JWLOG("aos3 window %lx has no menustrip\n",aos3win);
    return;
  }

  aos3screen=(uaecptr) jwin->jscreen->aos3screen;

  i=0;
  rx=0;
  ry=0;
  while(aos3menustrip) {
    /*********** Menu ************/
    if(i==menu) {
  //    rx=get_word(aos3menustrip+4);
      JWLOG("menu left edge: %d\n",get_word(aos3menustrip+4));
      JWLOG("menu width:     %d\n",get_word(aos3menustrip+8));
      JWLOG("screen barh:    %d\n",get_byte(aos3screen + 30));

      x=get_word(aos3menustrip+4) +    /* LeftEdge */
        (get_word(aos3menustrip+8) /2);/* Width / 2 */
      /* "Currently", any values supplied for TopEdge and 
       * Height are ignored by Intuition, which uses instead 
       * the top of the screen for the TopEdge and the
       * height of the screen's title bar for the Height.
       * (Libraries Manual)
       */
      y=get_byte(aos3screen + 30) / 2; /* BarHeight */
      /* move to this menu */
      JWLOG("menu xy: %d %d\n\n",x,y); /* ok */
      menux=x;
      menuy=y;
      ry=get_byte(aos3screen + 30);
      ry=ry+1;
      rx=get_word(aos3menustrip+4);

      if(!wait_menu_mouse_pos(jwin, menux, menuy)) {
        JWLOG("ERROR: mouse not moved, do nothing\n");
        return;
      };

      if(!wait_menu_shown(aos3menustrip + 12)) {
        JWLOG("ERROR: menu not shown, do nothing\n");
        return;
      } 

      /* menu drawn */

      aos3item=get_long(aos3menustrip + 18);
      j=0;
      while((item!=-1) && aos3item) {
        if(j==item) {
          JWLOG("menu item left edge: %d\n",get_word(aos3item+4));
          JWLOG("menu item width:     %d\n",get_word(aos3item+8));
          JWLOG("menu item top edge:  %d\n",get_word(aos3item+6));
          JWLOG("menu item height:    %d\n",get_word(aos3item+10));

          x=get_word(aos3item+4) +    /* LeftEdge */
            get_word(aos3item+8) /2 + /* Width / 2 */
            rx;                       /* The item LeftEdge is relative to the 
          * LeftEdge of the Menu*/
            y=get_word(aos3item+6) +    /* TopEdge */
            get_word(aos3item+10) /2 +/* Height / 2 */
            ry;
          rx=rx+get_word(aos3item+4);
          ry=ry+get_word(aos3item+6);
          /* move to this item */
          JWLOG("menu item xy: %d %d\n\n",x,y);  /* ok */
          menux=x;
          menuy=y;
          if(!wait_menu_mouse_pos(jwin, menux, menuy)) {
            JWLOG("ERROR: mouse not moved!\n");
            return;
          };

          /* if we have a subitem, we need to wait until it opens */
          if(sub!=-1) {
            JWLOG("care for subitem\n");
            if(!wait_menuitem_shown(aos3item + 12)) {
              JWLOG("ERROR: subitem not shown, ABORTED\n");
              return;
            };

            aos3sub=get_long(aos3item+28);
            l=0;
            while(aos3sub) {
              if(l==sub) {
                x=get_word(aos3sub+4) +    /* LeftEdge */
                  get_word(aos3sub+8) /2 + /* Width / 2 */
                  rx;
                y=get_word(aos3sub+6) +    /* TopEdge */
                  get_word(aos3sub+10) /2 +/* Height / 2 */
                  ry;
                /* move to this item */
                menux=x;
                menuy=y;
                JWLOG("menu subitem xy: %d %d\n",x,y);
                wait_menu_mouse_pos(jwin, menux, menuy);
              }

              l++;
              aos3sub=get_long(aos3sub); /* Next */
            }
          }
        }
        j++;
        aos3item=get_long(aos3item); /* NextItem */
      }
    }
    i++;
    aos3menustrip=get_long(aos3menustrip); /* NextMenu */
  }
}

/****i* januae/process_menu ************************************************
*
*   NAME
*       process_menu() -- Decode an AROS menu choice.
*
*   SYNOPSIS
*       void process_menu(JanusWin *jwin, UWORD selection);
*
*   FUNCTION
*       Decode menu, item and subitem indices from an Intuition selection
*       value and forward valid selections to click_menu.
*
*   INPUTS
*       jwin - Janus window. selection - Encoded menu selection.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       NOMENU and NOITEM selections are ignored; NOSUB is mapped to -1.
*
****************************************************************************
*
*/

void process_menu(JanusWin *jwin, UWORD selection) {
  WORD menu, item, sub;

  menu=MENUNUM(selection);
  item=ITEMNUM(selection);
  sub =SUBNUM (selection);

  JWLOG("menu %d, item %d, sub %d\n", menu, item , sub);

  if(menu == NOMENU || item == NOITEM) {
    /* nothing to do here */
    return;
  }

  if(sub == NOSUB) {
    sub=-1;
  }

  click_menu(jwin, menu, item, sub);
}
