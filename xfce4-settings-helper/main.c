/* $Id$ */
/*
 *  Copyright (c) 2008 Stephan Arts <stephan@xfce.org>
 *  Copyright (c) 2008 Nick Schermer <nick@xfce.org>
 *  Copyright (c) 2008 Jannis Pohlmann <jannis@xfce.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <glib.h>
#include <gtk/gtk.h>
#include <dbus/dbus.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#endif

#include <xfconf/xfconf.h>
#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>

#include "accessibility.h"
#include "pointers.h"
#include "keyboards.h"
#include "keyboard-layout.h"
#include "keyboard-shortcuts.h"
#include "workspaces.h"
#include "clipboard-manager.h"

#ifdef HAVE_XRANDR
#include "displays.h"
#endif

#define HELPER_DBUS_NAME "org.xfce.SettingsHelper"


static XfceSMClient *sm_client = NULL;

static gboolean opt_version = FALSE;
static gboolean opt_debug = FALSE;
static GOptionEntry option_entries[] =
{
    { "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version, N_("Version information"), NULL },
    { "debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, N_("Start in debug mode (don't fork to the background)"), NULL },
    { NULL }
};



static void
signal_handler (gint signum,
                gpointer user_data)
{
    /* quit the main loop */
    gtk_main_quit ();
}



static gboolean
xfce_settings_helper_set_autostart_enabled (gboolean enabled)
{
    gboolean ret = TRUE;
    XfceRc *rcfile = xfce_rc_config_open (XFCE_RESOURCE_CONFIG,
                                          "autostart/" AUTOSTART_FILENAME,
                                          FALSE);

    if (G_UNLIKELY (rcfile == NULL))
    {
        g_warning ("Failed to create per-user autostart directory");
        return FALSE;
    }

    xfce_rc_set_group (rcfile, "Desktop Entry");
    if (xfce_rc_read_bool_entry (rcfile, "Hidden", enabled) == enabled)
    {
        xfce_rc_write_bool_entry (rcfile, "Hidden", !enabled);
        xfce_rc_flush (rcfile);
    }

    if (xfce_rc_is_dirty (rcfile))
    {
        g_warning ("Failed to write autostart file");
        ret = FALSE;
    }

    xfce_rc_close (rcfile);

    return ret;
}



gint
main (gint argc, gchar **argv)
{
    XfceClipboardManager *clipboard_daemon;
    GError               *error = NULL;
    GOptionContext       *context;
    gboolean              in_session;
    GObject              *pointer_helper;
    GObject              *keyboards_helper;
    GObject              *accessibility_helper;
    GObject              *shortcuts_helper;
    GObject              *keyboard_layout_helper;
#ifdef HAVE_XRANDR
    GObject              *displays_helper;
#endif
    GObject              *workspaces_helper;
    pid_t                 pid;
    guint                 i;
    const gint            signums[] = { SIGQUIT, SIGTERM };
    DBusConnection       *dbus_connection;
    gint                  result;

    xfce_textdomain (GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");

    context = g_option_context_new (NULL);
    g_option_context_add_main_entries (context, option_entries, GETTEXT_PACKAGE);
    g_option_context_add_group (context, gtk_get_option_group (FALSE));
    g_option_context_add_group (context, xfce_sm_client_get_option_group (argc, argv));

    gtk_init (&argc, &argv);

    /* parse options */
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_print ("%s: %s.\n", G_LOG_DOMAIN, error->message);
        g_print (_("Type '%s --help' for usage."), G_LOG_DOMAIN);
        g_print ("\n");

        g_error_free (error);
        g_option_context_free (context);

        return EXIT_FAILURE;
    }

    g_option_context_free (context);

    /* check if we should print version information */
    if (G_UNLIKELY (opt_version))
    {
        g_print ("%s %s (Xfce %s)\n\n", G_LOG_DOMAIN, PACKAGE_VERSION, xfce_version_string ());
        g_print ("%s\n", "Copyright (c) 2008-2012");
        g_print ("\t%s\n\n", _("The Xfce development team. All rights reserved."));
        g_print (_("Please report bugs to <%s>."), PACKAGE_BUGREPORT);
        g_print ("\n");

        return EXIT_SUCCESS;
    }

    dbus_connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);
    if (G_LIKELY (dbus_connection != NULL))
    {
        result = dbus_bus_request_name (dbus_connection, HELPER_DBUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
        if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
          {
              g_printerr (G_LOG_DOMAIN ": %s\n", "Another instance is already running. Leaving...");
              return EXIT_SUCCESS;
          }
    }
    else
    {
        g_error ("Failed to connect to the dbus session bus.");
        return EXIT_FAILURE;
    }

    if (!xfconf_init (&error))
    {
        g_error ("Failed to connect to xfconf daemon: %s.", error->message);
        g_error_free (error);

        return EXIT_FAILURE;
    }

    /* connect to session always, even if we quit below.  this way the
     * session manager won't wait for us to time out. */
    sm_client = xfce_sm_client_get ();
    xfce_sm_client_set_restart_style (sm_client, XFCE_SM_CLIENT_RESTART_IMMEDIATELY);
    g_signal_connect (G_OBJECT (sm_client), "quit", G_CALLBACK (gtk_main_quit), NULL);
    if (!xfce_sm_client_connect (sm_client, &error) && error)
    {
        g_printerr ("Failed to connect to session manager: %s\n", error->message);
        g_clear_error (&error);
    }

    /* if this instance is started from a saved session, disable autostart so
     * the helper is not spawned twice */
    in_session = xfce_sm_client_is_resumed (sm_client);
    xfce_settings_helper_set_autostart_enabled (in_session);

    /* daemonize the process when not running in debug mode */
    if (!opt_debug)
    {
        /* try to fork the process */
        pid = fork ();

        if (G_UNLIKELY (pid == -1))
        {
            /* show message and continue in normal mode */
            g_warning ("Failed to fork the process: %s. Continuing in non-daemon mode.", g_strerror (errno));
        }
        else if (pid > 0)
        {
            /* succesfully created a fork */
            _exit (EXIT_SUCCESS);
        }
    }

    /* create the sub daemons */
#ifdef HAVE_XRANDR
    displays_helper = g_object_new (XFCE_TYPE_DISPLAYS_HELPER, NULL);
#endif
    pointer_helper = g_object_new (XFCE_TYPE_POINTERS_HELPER, NULL);
    keyboards_helper = g_object_new (XFCE_TYPE_KEYBOARDS_HELPER, NULL);
    accessibility_helper = g_object_new (XFCE_TYPE_ACCESSIBILITY_HELPER, NULL);
    shortcuts_helper = g_object_new (XFCE_TYPE_KEYBOARD_SHORTCUTS_HELPER, NULL);
    keyboard_layout_helper = g_object_new (XFCE_TYPE_KEYBOARD_LAYOUT_HELPER, NULL);
    workspaces_helper = g_object_new (XFCE_TYPE_WORKSPACES_HELPER, NULL);

    clipboard_daemon = xfce_clipboard_manager_new ();
    if (!xfce_clipboard_manager_start (clipboard_daemon))
    {
        g_object_unref (G_OBJECT (clipboard_daemon));
        clipboard_daemon = NULL;
    }

    /* setup signal handlers to properly quit the main loop */
    if (xfce_posix_signal_handler_init (NULL))
    {
        for (i = 0; i < G_N_ELEMENTS (signums); i++)
            xfce_posix_signal_handler_set_handler (signums[i], signal_handler, NULL, NULL);
    }

    gtk_main();

    /* release the dbus name */
    if (dbus_connection != NULL)
        dbus_bus_release_name (dbus_connection, HELPER_DBUS_NAME, NULL);

    /* release the sub daemons */
#ifdef HAVE_XRANDR
    g_object_unref (G_OBJECT (displays_helper));
#endif
    g_object_unref (G_OBJECT (pointer_helper));
    g_object_unref (G_OBJECT (keyboards_helper));
    g_object_unref (G_OBJECT (accessibility_helper));
    g_object_unref (G_OBJECT (shortcuts_helper));
    g_object_unref (G_OBJECT (keyboard_layout_helper));
    g_object_unref (G_OBJECT (workspaces_helper));

    if (G_LIKELY (clipboard_daemon != NULL))
    {
        xfce_clipboard_manager_stop (clipboard_daemon);
        g_object_unref (G_OBJECT (clipboard_daemon));
    }

    xfconf_shutdown ();
    g_object_unref (G_OBJECT (sm_client));

    return EXIT_SUCCESS;
}