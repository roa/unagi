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
 *  \brief Main file
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <getopt.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xfixes.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_keysyms.h>

#include <basedir.h>
#include <basedir_fs.h>
#include <confuse.h>

#include "structs.h"
#include "display.h"
#include "event.h"
#include "atoms.h"
#include "util.h"
#include "plugin.h"
#include "key.h"

#ifdef __DEBUG__
/*
 * Basic painting performance benchmark
 */
#include <float.h>
#include <math.h>
#endif

conf_t globalconf;

#define CONFIG_FILENAME PACKAGE_NAME ".conf"

/** Parse the configuration file with confuse
 *
 * \param config_fp The configuration file stream
 * \return Return true if parsing succeeded
 */
static bool
_unagi_parse_configuration_file(FILE *config_fp)
{
  cfg_opt_t opts[] = {
    CFG_STR("rendering", "render", CFGF_NONE),
    CFG_STR_LIST("plugins", "{}", CFGF_NONE),
    CFG_END()
  };

  globalconf.cfg = cfg_init(opts, CFGF_NONE);
  if(cfg_parse_fp(globalconf.cfg, config_fp) == CFG_PARSE_ERROR)
    return false;

  return true;
}

/** Display help information */
static inline void
_unagi_display_help(void)
{
  printf("Usage: " PACKAGE_NAME "[options]\n\
  -h, --help                show help\n\
  -v, --version             show version\n\
  -c, --config FILE         configuration file path\n\
  -r, --rendering-path PATH rendering backend path\n\
  -p, --plugins-path PATH   plugins path\n");
}

/** Parse command line parameters
 *
 * \param argc The number of command line parameters
 * \param argv The strings list of arguments
 */
static void
_unagi_parse_command_line_parameters(int argc, char **argv)
{
  const struct option long_options[] = {
    { "help", 0, NULL, 'h' },
    { "version", 0, NULL, 'v' },
    { "config", 1, NULL, 'c' },
    { "rendering-path", 1, NULL, 'r' },
    { "plugins-path", 1, NULL, 'p' },
    { NULL, 0, NULL, 0 }
  };

  int opt;
  FILE *config_fp = NULL;

  while((opt = getopt_long(argc, argv, "vhc:r:p:",
			   long_options, NULL)) != -1)
    {
      switch(opt)
	{
	case 'v':
	  printf(VERSION "\n");
	  exit(EXIT_SUCCESS);
	  break;
	case 'h':
	  _unagi_display_help();
	  exit(EXIT_SUCCESS);
	  break;
	case 'c':
	  if(!strlen(optarg) || !(config_fp = fopen(optarg, "r")))
	    {
	      _unagi_display_help();
	      exit(EXIT_FAILURE);
	    }
	  break;
	case 'r':
	  if(optarg && strlen(optarg))
	    globalconf.rendering_dir = strdup(optarg);
	  else
	    fatal("-r option requires a directory");
	  break;
	case 'p':
	  if(optarg && strlen(optarg))
	    globalconf.plugins_dir = strdup(optarg);
	  else
	    fatal("-p option requires a directory");
	  break;
	}
    }

  /* Get the configuration file */
  if(!config_fp)
    {
      /* Look    for    the    configuration    file    in    Autoconf
	 $sysconfigdir/xdg, then fall back on XDG if not found */
      if((config_fp = fopen(XDG_CONFIG_DIR "/" CONFIG_FILENAME, "r")) == NULL)
	{
	  xdgHandle xdg;
	  xdgInitHandle(&xdg);
	  config_fp = xdgConfigOpen(CONFIG_FILENAME, "r", &xdg);
	  xdgWipeHandle(&xdg);
	}

      if(!config_fp)
	fatal("Can't open configuration file");
    }

  /* Parse configuration file */
  if(!_unagi_parse_configuration_file(config_fp))
    {
      fclose(config_fp);
      fatal("Can't parse configuration file");
    }

  fclose(config_fp);

  /* Get the rendering backend path if not given in the command line
     parameters */
  if(!globalconf.rendering_dir)
    globalconf.rendering_dir = strdup(RENDERING_DIR);

  /* Get  the  plugins   path  if  not  given  in   the  command  line
     parameters */
  if(!globalconf.plugins_dir)
    globalconf.plugins_dir = strdup(PLUGINS_DIR);
}

/** Perform cleanup on normal exit */
static void
_unagi_exit_cleanup(void)
{
  debug("Cleaning resources up");

  /* Free resources related to the plugins */
  plugin_unload_all();

  /* Destroy the  linked-list of  windows which has  to be  done after
     unloading the plugins as the  plugins may use the windows list to
     free memory */
  window_list_cleanup();

  /* Free resources related  to the rendering backend which  has to be
     done  after the  windows  list  cleanup as  the  latter free  the
     rendering information associated with each window */
  rendering_unload();

  /* Free resources related to the keymaps */
  xcb_key_symbols_free(globalconf.keysyms);

  /* Free resources related to EWMH */
  xcb_ewmh_connection_wipe(&globalconf.ewmh);

  cfg_free(globalconf.cfg);
  free(globalconf.rendering_dir);
  free(globalconf.plugins_dir);

  /* Free resources related to X connection */
  if(globalconf.connection)
    {
      /* Destroy CM window, thus giving up _NET_WM_CM_Sn ownership */
      if(globalconf.cm_window != XCB_NONE)
        xcb_destroy_window(globalconf.connection, globalconf.cm_window);

      xcb_disconnect(globalconf.connection);
    }

  ev_loop_destroy(globalconf.event_loop);
}

/** Perform  cleanup when  a  signal (SIGHUP,  SIGINT  or SIGTERM)  is
 *  received
 */
static void
_unagi_exit_on_signal(struct ev_loop *loop, ev_signal *w, int revents)
{
  ev_break(loop, EVBREAK_ALL);
}

static void
_unagi_paint_callback(EV_P_ ev_timer *w, int revents)
{
#ifdef __DEBUG__
  /* Meaningful to measure painting performances */
  static double paint_time_min = DBL_MAX;
  static double paint_time_max = 0;

  /* For online computation of standard deviation */
  static double paint_time_mean = 0;
  static double paint_time_variance_sum = 0;
#endif

  /* Now paint the windows */
  if(globalconf.damaged)
    {
#ifdef __DEBUG__
      debug("COUNT: %u: Begin re-painting", globalconf.paint_counter);
#endif
      window_t *windows = NULL;
      for(plugin_t *plugin = globalconf.plugins; plugin; plugin = plugin->next)
        if(plugin->enable && plugin->vtable->render_windows &&
           (windows = (*plugin->vtable->render_windows)()))
          break;

      if(!windows)
        windows = globalconf.windows;

#ifdef __DEBUG__
      /* Display damaged regions */
      xcb_xfixes_fetch_region_reply_t *r = \
        xcb_xfixes_fetch_region_reply(globalconf.connection,
                                      xcb_xfixes_fetch_region(globalconf.connection,
                                                              globalconf.damaged),
                                      NULL);
      if(r)
        {
          xcb_rectangle_t *rects = xcb_xfixes_fetch_region_rectangles(r);

          for(int i = 0; i < xcb_xfixes_fetch_region_rectangles_length(r);
              i++)
            debug("Damaged region #%d: %dx%d +%d+%d",
                  i, rects[i].width, rects[i].height,
                  rects[i].x, rects[i].y);

          free(r);
        }
#endif
      window_paint_all(windows);
      display_reset_damaged();

      const float paint_time = (float) (ev_time() - ev_now(globalconf.event_loop));
      globalconf.paint_time_sum += paint_time;

      const float current_average = globalconf.paint_time_sum /
        (float) ++globalconf.paint_counter;

      /* The next repaint  interval is computed from  the refresh rate
         interval and repaint global average time */
      const float current_interval = globalconf.refresh_rate_interval -
        current_average;

      /* When repainting the whole screen, the painting may have taken
         a  long time  but the  next repaint  should not  be too  soon
         neither */
      if(current_interval < MINIMUM_REPAINT_INTERVAL)
        globalconf.repaint_interval = globalconf.refresh_rate_interval;
      else
        globalconf.repaint_interval = current_interval;

      /* Rearm the paint timer watcher */
      globalconf.event_paint_timer_watcher.repeat = globalconf.repaint_interval;
      ev_timer_again(globalconf.event_loop, &globalconf.event_paint_timer_watcher);

#ifdef __DEBUG__
      if(paint_time < paint_time_min)
        paint_time_min = paint_time;
      if(paint_time > paint_time_max)
        paint_time_max = paint_time;

      /* Compute standard deviation for this iteration */
      const double delta = paint_time - paint_time_mean;
      paint_time_mean += (double) delta / globalconf.paint_counter;
      paint_time_variance_sum += delta * (paint_time - paint_time_mean);

      debug("Painting time in seconds (#%u): %.6f, min=%.6f, max=%.6f, "
            "average=%.6f (+/- %.6Lf)",
            globalconf.paint_counter, paint_time, paint_time_min,
            paint_time_max, current_average,
            sqrtl(paint_time_variance_sum / globalconf.paint_counter));
#endif /* __DEBUG__ */

      /* Some events may have been queued while calling this callback,
         so make sure by calling this watcher again */
      ev_invoke(globalconf.event_loop, &globalconf.event_io_watcher, 0);
    }
}

static void
_unagi_io_callback(EV_P_ ev_io *w, int revents)
{
  /* ev_now_update()  is   only  called  through  ev_run()   but  with
     ev_invoke  (as used  on startup  and to  process events  received
     during painting function call */
  if(revents <= 0)
    ev_now_update(globalconf.event_loop);

  ev_tstamp now = ev_now(globalconf.event_loop);

  /* Check X connection to avoid SIGSEGV */
  if(xcb_connection_has_error(globalconf.connection))
    fatal("X connection invalid");

  /* Process all events in the queue because before painting, all the
     DamageNotify have to be received */
  xcb_generic_event_t *event;
  while((event = xcb_poll_for_event(globalconf.connection)) != NULL)
    {
      event_handle(event);
      free(event);

      /* Stop processing events (but not  on startup as all the events
         must be processed) if the  repaint interval has been reached,
         otherwise DamageNotify  keep being processed forever  if many
         are received */
      if(revents != -1 && (ev_time() - now + 0.001) > globalconf.repaint_interval)
        {
          /* Process events remaining in the queue without polling the
             X connection */
          while((event = xcb_poll_for_queued_event(globalconf.connection)))
            {
              event_handle(event);
              free(event);
            }
          break;
        }
    }
}

int
main(int argc, char **argv)
{
  memset(&globalconf, 0, sizeof(globalconf));

  _unagi_parse_command_line_parameters(argc, argv);

  /* libev event loop */
  globalconf.event_loop = ev_default_loop(EVFLAG_NOINOTIFY | EVFLAG_NOSIGMASK);

  /* Set up signal handlers */
  ev_signal sighup;
  ev_signal_init(&sighup, _unagi_exit_on_signal, SIGHUP);
  ev_signal_start(globalconf.event_loop, &sighup);
  ev_unref(globalconf.event_loop);

  ev_signal sigint;
  ev_signal_init(&sigint, _unagi_exit_on_signal, SIGINT);
  ev_signal_start(globalconf.event_loop, &sigint);
  ev_unref(globalconf.event_loop);

  ev_signal sigterm;
  ev_signal_init(&sigterm, _unagi_exit_on_signal, SIGTERM);
  ev_signal_start(globalconf.event_loop, &sigterm);
  ev_unref(globalconf.event_loop);

  /* Cleanup resources upon normal exit */
  atexit(_unagi_exit_cleanup);

  globalconf.connection = xcb_connect(NULL, &globalconf.screen_nbr);
  if(xcb_connection_has_error(globalconf.connection))
    fatal("Cannot open display");

  /* Get the root window */
  globalconf.screen = xcb_aux_get_screen(globalconf.connection,
					 globalconf.screen_nbr);

  /**
   * First round-trip
   */

  /* Send requests for EWMH atoms initialisation */
  xcb_intern_atom_cookie_t *ewmh_cookies = atoms_init();

  /* Prefetch the extensions data */
  xcb_prefetch_extension_data(globalconf.connection, &xcb_composite_id);
  xcb_prefetch_extension_data(globalconf.connection, &xcb_damage_id);
  xcb_prefetch_extension_data(globalconf.connection, &xcb_xfixes_id);
  xcb_prefetch_extension_data(globalconf.connection, &xcb_randr_id);

  /* Pre-initialisation of the rendering backend */
  if(!rendering_load())
    {
      free(ewmh_cookies);
      fatal("Can't initialise rendering backend");
    }

  /* Get replies for EWMH atoms initialisation */
  if(!atoms_init_finalise(ewmh_cookies))
    /* No need to  free ewmh_cookies in case of  error as it's already
       handles by xcb-ewmh when getting the replies */
    fatal("Cannot initialise atoms");

  /* First check whether there is already a Compositing Manager (ICCCM) */
  xcb_get_selection_owner_cookie_t wm_cm_owner_cookie =
    xcb_ewmh_get_wm_cm_owner(&globalconf.ewmh, globalconf.screen_nbr);

  /* Initialiase libev event watcher on XCB connection */
  ev_io_init(&globalconf.event_io_watcher, _unagi_io_callback,
             xcb_get_file_descriptor(globalconf.connection), EV_READ);

  ev_io_start(globalconf.event_loop, &globalconf.event_io_watcher);

  /* Flush the X events queue before blocking */
  xcb_flush(globalconf.connection);

  /**
   * Second round-trip
   */

  /* All the plugins given in the configuration file

     TODO: Only there because render_init() needs to be able to look
     for opacity plugin */
  plugin_load_all();

  /* Initialise   extensions   based   on   the  cache   and   perform
     initialisation of the rendering backend */
  display_init_extensions();
  if(!(*globalconf.rendering->init)())
    return EXIT_FAILURE;

  /* Check ownership for WM_CM_Sn before actually claiming it (ICCCM) */
  xcb_window_t wm_cm_owner_win;
  if(xcb_ewmh_get_wm_cm_owner_reply(&globalconf.ewmh, wm_cm_owner_cookie,
				    &wm_cm_owner_win, NULL) &&
     wm_cm_owner_win != XCB_NONE)
    fatal("A compositing manager is already active (window=%jx)",
	  (uintmax_t) wm_cm_owner_win);

  /* Now send requests to register the CM */
  display_register_cm();
  
  /**
   * Third round-trip
   */

  /* Check  extensions  version   and  finish  initialisation  of  the
     rendering backend */
  display_init_extensions_finalise();
  if(!(*globalconf.rendering->init_finalise)())
    return EXIT_FAILURE;

  xcb_randr_get_screen_info_cookie_t randr_screen_cookie = { .sequence = 0 };
  if(globalconf.extensions.randr)
    {
      /* Get the screen refresh rate to calculate the interval between
         painting */
      randr_screen_cookie = xcb_randr_get_screen_info(globalconf.connection,
                                                      globalconf.screen->root);

      xcb_randr_select_input(globalconf.connection,
                             globalconf.screen->root,
                             XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }

  /* Validate  errors   and  get  PropertyNotify   needed  to  acquire
     _NET_WM_CM_Sn ownership */
  xcb_aux_sync(globalconf.connection);
  event_handle_poll_loop(event_handle_startup);

  globalconf.keysyms = xcb_key_symbols_alloc(globalconf.connection);
  xcb_get_modifier_mapping_cookie_t key_mapping_cookie =
    xcb_get_modifier_mapping_unchecked(globalconf.connection);

  /* Finish CM X registration */
  if(!display_register_cm_finalise())
    fatal("Could not acquire _NET_WM_CM_Sn ownership");

  /**
   * Last initialisation round-trip
   */

  /* Grab the server before performing redirection and get the tree of
     windows  to ensure  there  won't  be anything  else  at the  same
     time */
  xcb_grab_server(globalconf.connection);

  /* Now redirect windows and add existing windows */
  display_init_redirect();

  /* Validate errors handlers during redirect */
  xcb_aux_sync(globalconf.connection);
  event_handle_poll_loop(event_handle_startup);

  /* Manage existing windows */
  display_init_redirect_finalise();

  xcb_ungrab_server(globalconf.connection);

  /* Check the  plugin requirements  which will disable  plugins which
     don't meet the requirements */
  plugin_check_requirements();

  /* Set the refresh rate, necessary to define painting intervals */
  if(!randr_screen_cookie.sequence)
    {
      warn("RandR not available, falling back on 50Hz");
      globalconf.refresh_rate_interval = (float) DEFAULT_REPAINT_INTERVAL;
    }
  else
    display_set_screen_refresh_rate(randr_screen_cookie);

  globalconf.repaint_interval = globalconf.refresh_rate_interval;

  /* Initialise painting timer depending on the screen refresh rate */
  ev_init(&globalconf.event_paint_timer_watcher,
          _unagi_paint_callback);

  /* Painting must have precedence over events processing */
  ev_set_priority(&globalconf.event_paint_timer_watcher, EV_MAXPRI);

  /* Set the initial repaint interval to the screen refresh rate, it
     will be adjust later on according to the repaint times */
  globalconf.event_paint_timer_watcher.repeat = globalconf.repaint_interval;
  ev_timer_again(globalconf.event_loop, &globalconf.event_paint_timer_watcher);
 
  /* Get the lock masks reply of the request previously sent */ 
  key_lock_mask_get_reply(key_mapping_cookie);

  /* Flush existing  requests before  the loop as  DamageNotify events
     may have been received in the meantime */
  xcb_flush(globalconf.connection);

  window_paint_all(globalconf.windows);
  ev_invoke(globalconf.event_loop, &globalconf.event_io_watcher, -1);

  /* Main event and error loop */
  ev_run(globalconf.event_loop, 0);

  ev_io_stop(globalconf.event_loop, &globalconf.event_io_watcher);
  ev_timer_stop(globalconf.event_loop, &globalconf.event_paint_timer_watcher);

  return EXIT_SUCCESS;
}
