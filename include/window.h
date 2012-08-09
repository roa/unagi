/* -*-mode:c;coding:utf-8; c-basic-offset:2;fill-column:70;c-file-style:"gnu"-*-
 *
 * Copyright (C) 2009 Arnaud "arnau" Fontaine <arnau@mini-dweeb.org>
 *
 * This  program is  free  software: you  can  redistribute it  and/or
 * modify  it under the  terms of  the GNU  General Public  License as
 * published by the Free Software  Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of
 * MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 *
 * You should have  received a copy of the  GNU General Public License
 *  along      with      this      program.      If      not,      see
 *  <http://www.gnu.org/licenses/>.
 */

/** \file
 *  \brief Windows management
 */

#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include <xcb/xcb.h>
#include <xcb/damage.h>
#include <xcb/xfixes.h>

#include "util.h"

#define WINDOW_FULLY_DAMAGED_RATIO 0.9

typedef struct _window_t
{
  xcb_window_t id;
  xcb_get_window_attributes_reply_t *attributes;
  xcb_get_geometry_reply_t *geometry;
  xcb_xfixes_region_t region;
  xcb_xfixes_fetch_region_cookie_t shape_cookie;
  bool is_rectangular;
  xcb_damage_damage_t damage;
  bool damaged;
  float damaged_ratio;
  short damage_notify_counter;
  xcb_pixmap_t pixmap;
  void *rendering;
  struct _window_t *next;
} window_t;

void window_free_pixmap(window_t *);
void window_list_cleanup(void);

/** Get the  window object  associated with the  given Window  XID. As
 *  this is a very common operation, use a binary tree rather than the
 *  linked list. The linked list is still useful to get windows sorted
 *  by stacking order
 *
 * \param WINDOW_ID The Window XID to look for
 */
#define window_list_get(WINDOW_ID) util_itree_get(globalconf.windows_itree, \
                                                  WINDOW_ID)

void window_list_remove_window(window_t *);
void window_register_notify(const window_t *);
void window_get_root_background_pixmap(void);
xcb_pixmap_t window_get_root_background_pixmap_finalise(void);
xcb_pixmap_t window_new_root_background_pixmap(void);
xcb_pixmap_t window_get_pixmap(const window_t *);
bool window_is_rectangular(window_t *);
xcb_xfixes_region_t window_get_region(window_t *, bool, bool);
bool window_is_visible(const window_t *);
void window_get_invisible_window_pixmap(window_t *);
void window_get_invisible_window_pixmap_finalise(window_t *);
void window_manage_existing(const int nwindows, const xcb_window_t *);
window_t *window_add(const xcb_window_t, bool);
void window_restack(window_t *, xcb_window_t);
void window_paint_all(window_t *);

static inline float
window_get_damaged_ratio(window_t *window, xcb_damage_notify_event_t *event)
{
  window->damaged_ratio += (float) (event->area.width * event->area.height) /
    (float) (window->geometry->width * window->geometry->height);

  return window->damaged_ratio;
}

#define DO_GEOMETRY_WITH_BORDER(kind)					\
  static inline uint16_t						\
  window_##kind##_with_border(const xcb_get_geometry_reply_t *geometry)	\
  {									\
    return (uint16_t) (geometry->kind + (geometry->border_width * 2));	\
  }

DO_GEOMETRY_WITH_BORDER(width)
DO_GEOMETRY_WITH_BORDER(height)

#endif
