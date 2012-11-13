/** Copyright 2011-2012 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#include "clientlist.h"
#include "settings.h"
#include "globals.h"
#include "layout.h"
#include "stack.h"
#include "utils.h"
#include "hook.h"
#include "mouse.h"
#include "ewmh.h"
#include "rules.h"
#include "ipc-protocol.h"
// system
#include <glib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <regex.h>
#include <stdbool.h>
#include <string.h>
// gui
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

int g_monitor_float_treshold = 24;

int* g_window_border_width;
int* g_window_border_inner_width;
int* g_raise_on_focus;
int* g_snap_gap;
int* g_smart_window_surroundings;
unsigned long g_window_border_active_color;
unsigned long g_window_border_normal_color;
unsigned long g_window_border_urgent_color;
unsigned long g_window_border_inner_color;

GHashTable* g_clients; // container of all clients

// atoms from dwm.c
// default atoms
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast };
static Atom g_wmatom[WMLast];

static HSClient* create_client() {
    HSClient* hc = g_new0(HSClient, 1);
    hc->float_size.width = 100;
    hc->float_size.height = 100;
    hc->title = g_string_new("");
    hc->urgent = false;
    hc->fullscreen = false;
    hc->ewmhfullscreen = false;
    hc->pseudotile = false;
    hc->ewmhrequests = true;
    hc->ewmhnotify = true;
    return hc;
}

static void fetch_colors() {
    g_window_border_width = &(settings_find("window_border_width")->value.i);
    g_window_border_inner_width = &(settings_find("window_border_inner_width")->value.i);
    g_window_gap = &(settings_find("window_gap")->value.i);
    g_snap_gap = &(settings_find("snap_gap")->value.i);
    g_smart_window_surroundings = &(settings_find("smart_window_surroundings")->value.i);
    g_raise_on_focus = &(settings_find("raise_on_focus")->value.i);
    char* str = settings_find("window_border_normal_color")->value.s;
    g_window_border_normal_color = getcolor(str);
    str = settings_find("window_border_active_color")->value.s;
    g_window_border_active_color = getcolor(str);
    str = settings_find("window_border_urgent_color")->value.s;
    g_window_border_urgent_color = getcolor(str);
    str = settings_find("window_border_inner_color")->value.s;
    g_window_border_inner_color = getcolor(str);
}

void clientlist_init() {
    // init regex simple..
    fetch_colors();
    g_wmatom[WMProtocols] = XInternAtom(g_display, "WM_PROTOCOLS", False);
    g_wmatom[WMDelete] = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    g_wmatom[WMState] = XInternAtom(g_display, "WM_STATE", False);
    g_wmatom[WMTakeFocus] = XInternAtom(g_display, "WM_TAKE_FOCUS", False);
    // init actual client list
    g_clients = g_hash_table_new_full(g_int_hash, g_int_equal,
                                      NULL, (GDestroyNotify)client_destroy);
}

void reset_client_colors() {
    fetch_colors();
    all_monitors_apply_layout();
}

static void client_move_to_floatpos(void* key, void* client_void, void* data) {
    (void)key;
    (void)data;
    HSClient* client = client_void;
    if (client) {
        int x = client->float_size.x;
        int y = client->float_size.y;
        unsigned int w = client->float_size.width;
        unsigned int h = client->float_size.height;
        XMoveResizeWindow(g_display, client->window, x, y, w, h);
    }
}

void clientlist_destroy() {
    // move all clients to their original floating position
    g_hash_table_foreach(g_clients, client_move_to_floatpos, NULL);

    g_hash_table_destroy(g_clients);
}


void clientlist_foreach(GHFunc func, gpointer data) {
    g_hash_table_foreach(g_clients, func, data);
}

HSClient* get_client_from_window(Window window) {
    return (HSClient*) g_hash_table_lookup(g_clients, &window);
}

HSClient* manage_client(Window win) {
    if (is_herbstluft_window(g_display, win)) {
        // ignore our own window
        return NULL;
    }
    if (get_client_from_window(win)) {
        return NULL;
    }
    // init client
    HSClient* client = create_client();
    client->pid = window_pid(g_display, win);
    HSMonitor* m = get_current_monitor();
    // set to window properties
    client->window = win;
    client_update_title(client);

    unsigned int border, depth;
    Window root_win;
    int x, y;
    unsigned int w, h;
    XGetGeometry(g_display, win, &root_win, &x, &y, &w, &h, &border, &depth);
    // treat wanted coordinates as floating coords
    client->float_size.x = x;
    client->float_size.y = y;
    client->float_size.width = w;
    client->float_size.height = h;

    // apply rules
    HSClientChanges changes;
    client_changes_init(&changes, client);
    rules_apply(client, &changes);
    if (changes.tag_name) {
        client->tag = find_tag(changes.tag_name->str);
    }

    if (!changes.manage) {
        client_changes_free_members(&changes);
        client_destroy(client);
        // map it... just to be sure
        XMapWindow(g_display, win);
        return NULL;
    }

    // actually manage it
    g_hash_table_insert(g_clients, &(client->window), client);
    // insert to layout
    if (!client->tag) {
        client->tag = m->tag;
    }
    // get events from window
    XSelectInput(g_display, win, CLIENT_EVENT_MASK);
    // insert window to the stack
    client->slice = slice_create_client(client);
    stack_insert_slice(client->tag->stack, client->slice);
    // insert window to the tag
    frame_insert_window(lookup_frame(client->tag->frame, changes.tree_index->str), win);
    client_update_wm_hints(client);
    if (changes.focus) {
        // give focus to window if wanted
        // TODO: make this faster!
        // WARNING: this solution needs O(C + exp(D)) time where W is the count
        // of clients on this tag and D is the depth of the binary layout tree
        frame_focus_window(client->tag->frame, win);
    }

    ewmh_window_update_tag(client->window, client->tag);
    tag_set_flags_dirty();
    client_set_fullscreen(client, changes.fullscreen);
    ewmh_update_window_state(client);
    // add client after setting the correct tag for the new client
    // this ensures a panel can read the tag property correctly at this point
    ewmh_add_client(client->window);

    HSMonitor* monitor = find_monitor_with_tag(client->tag);
    if (monitor) {
        if (monitor != get_current_monitor()
            && changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag);
        }
        // TODO: monitor_apply_layout() maybe is called twice here if it
        // already is called by monitor_set_tag()
        monitor_apply_layout(monitor);
    } else {
        if (changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag);
        }
    }

    client_changes_free_members(&changes);
    grab_client_buttons(client, false);

    return client;
}

void unmanage_client(Window win) {
    HSClient* client = get_client_from_window(win);
    if (!client) {
        return;
    }
    // remove from tag
    frame_remove_window(client->tag->frame, win);
    // ignore events from it
    XSelectInput(g_display, win, 0);
    //XUngrabButton(g_display, AnyButton, AnyModifier, win);
    // permanently remove it
    HSTag* tag = client->tag;
    g_hash_table_remove(g_clients, &win);
    // and arrange monitor after the client has been removed from the stack
    HSMonitor* m = find_monitor_with_tag(tag);
    tag_update_focus_layer(tag);
    if (m) monitor_apply_layout(m);
    ewmh_remove_client(win);
    tag_set_flags_dirty();
}

// destroys a special client
void client_destroy(HSClient* client) {
    if (client->tag && client->slice) {
        stack_remove_slice(client->tag->stack, client->slice);
    }
    if (client->slice) {
        slice_destroy(client->slice);
    }
    if (client) {
        /* free window title */
        g_string_free(client->title, true);
    }
    g_free(client);
}

void window_unfocus(Window window) {
    HSDebug("window_unfocus NORMAL\n");
    window_update_border(window, g_window_border_normal_color);
    HSClient* c = get_client_from_window(window);
    if (c) {
        if (c->urgent) {
            HSDebug("window_unfocus URGENT\n");
            window_update_border(window, g_window_border_urgent_color);
        }
        grab_client_buttons(c, false);
    }
}

static Window lastfocus = 0;
void window_unfocus_last() {
    if (lastfocus) {
        window_unfocus(lastfocus);
    }
    // give focus to root window
    XSetInputFocus(g_display, g_root, RevertToPointerRoot, CurrentTime);
    if (lastfocus) {
        /* only emit the hook if the focus *really* changes */
        hook_emit_list("focus_changed", "0x0", "", NULL);
        ewmh_update_active_window(None);
        tag_update_each_focus_layer();
    }
    lastfocus = 0;
}

void window_focus(Window window) {
    HSClient* client = get_client_from_window(window);
    assert(client != NULL);
    // set keyboard focus
    if (!client->neverfocus) {
        XSetInputFocus(g_display, window, RevertToPointerRoot, CurrentTime);
    }
    client_sendevent(client, g_wmatom[WMTakeFocus]);

    if (window != lastfocus) {
        /* FIXME: this is a workaround because window_focus always is called
         * twice.  see BUGS for more information
         *
         * only emit the hook if the focus *really* changes */
        // unfocus last one
        window_unfocus(lastfocus);
        ewmh_update_active_window(window);
        tag_update_each_focus_layer();
        char* title = client ? client->title->str : "?";
        char winid_str[STRING_BUF_SIZE];
        snprintf(winid_str, STRING_BUF_SIZE, "0x%x", (unsigned int)window);
        hook_emit_list("focus_changed", winid_str, title, NULL);
    }

    // change window-colors
    HSDebug("window_focus ACTIVE\n");
    window_update_border(window, g_window_border_active_color);

    lastfocus = window;
    /* do some specials for the max layout */
    bool is_max_layout = frame_focused_window(g_cur_frame) == window
                         && g_cur_frame->content.clients.layout == LAYOUT_MAX
                         && get_current_monitor()->tag->floating == false;
    if (*g_raise_on_focus || is_max_layout) {
        client_raise(client);
    }
    tag_update_focus_layer(get_current_monitor()->tag);
    grab_client_buttons(get_client_from_window(window), true);
    client_set_urgent(client, false);
}

void client_setup_border(HSClient* client, bool focused) {
    unsigned long colors[] = {
        g_window_border_normal_color,
        g_window_border_active_color,
    };
    if (client->urgent) {
        HSDebug("client_setup_border URGENT\n");
        window_update_border(client->window, g_window_border_urgent_color);
    } else {
        HSDebug("client_setup_border %s\n", (focused ? "ACTIVE" : "NORMAL"));
        window_update_border(client->window, colors[focused ? 1 : 0]);
    }
}

void client_resize_fullscreen(HSClient* client, HSMonitor* m) {
    if (!client || !m) {
        HSDebug("client_resize_fullscreen() got invalid parameters\n");
        return;
    }
    XSetWindowBorderWidth(g_display, client->window, 0);
    client->last_size = m->rect;
    client->last_border_width = 0;
    XMoveResizeWindow(g_display, client->window,
                      m->rect.x, m->rect.y, m->rect.width, m->rect.height);

}

void client_raise(HSClient* client) {
    assert(client);
    stack_raise_slide(client->tag->stack, client->slice);
}

void client_resize(HSClient* client, XRectangle rect, HSFrame* frame) {
    // ensure minimum size
    if (rect.width < WINDOW_MIN_WIDTH) {
        rect.width = WINDOW_MIN_WIDTH;
    }
    if (rect.height < WINDOW_MIN_HEIGHT) {
        rect.height = WINDOW_MIN_HEIGHT;
    }
    if (!client) {
        HSDebug("Warning: client_resize(NULL, ...) was called\n");
        return;
    }
    Window win = client->window;
    if (client->pseudotile) {
        XRectangle size = client->float_size;
        // floating sizes don't include window border, tiling sizes do.
        // so convert the floating size to a tiling size
        size.width  += *g_window_border_width * 2;
        size.height += *g_window_border_width * 2;
        // ensure size is not larger than rect-tile
        size.width  = MIN(size.width, rect.width);
        size.height = MIN(size.height, rect.height);

        // center it
        rect.x = rect.x + rect.width/2 - size.width/2;
        rect.y = rect.y + rect.height/2 - size.height/2;
        rect.width = size.width;
        rect.height = size.height;
    }
    int border_width = *g_window_border_width;
    if (*g_smart_window_surroundings && !client->pseudotile
        && (frame->content.clients.count == 1
            || frame->content.clients.layout == LAYOUT_MAX)) {
        border_width = 0;
    }
    if (RECTANGLE_EQUALS(client->last_size, rect)
        && client->last_border_width == border_width) {
        return;
    }
    client->last_size = rect;
    client->last_border_width = border_width;

    // apply border width
    rect.width -= border_width * 2;
    rect.height -= border_width * 2;
    if (!*g_smart_window_surroundings
        || (frame->content.clients.count != 1
            && frame->content.clients.layout != LAYOUT_MAX)) {
        // apply window gap
        rect.width -= *g_window_gap;
        rect.height -= *g_window_gap;
    }

    XSetWindowBorderWidth(g_display, win, border_width);
    XMoveResizeWindow(g_display, win, rect.x, rect.y, rect.width, rect.height);
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        unsigned long current_border_color = get_window_border_color(client);
        HSDebug("client_resize %s\n",
                current_border_color == g_window_border_active_color
                ? "ACTIVE" : "NORMAL");
        set_window_double_border(g_display, win, *g_window_border_inner_width,
                                 g_window_border_inner_color,
                                 current_border_color);
    }
    //// send new size to client
    //// WHY SHOULD I? -> faster? only one call?
    //XConfigureEvent ce;
    //ce.type = ConfigureNotify;
    //ce.display = g_display;
    //ce.event = win;
    //ce.window = win;
    //ce.x = rect.x;
    //ce.y = rect.y;
    //ce.width = rect.width;
    //ce.height = rect.height;
    //ce.border_width = 0;
    //ce.above = None;
    //ce.override_redirect = False;
    //XSendEvent(g_display, win, False, StructureNotifyMask, (XEvent *)&ce);
}

void client_resize_tiling(HSClient* client, XRectangle rect, HSFrame* frame) {
    HSMonitor* m;
    if (client->fullscreen && (m = find_monitor_with_tag(client->tag))) {
        client_resize_fullscreen(client, m);
    } else {
        client_resize(client, rect, frame);
    }
}

void client_resize_floating(HSClient* client, HSMonitor* m) {
    if (!client || !m) return;
    if (client->fullscreen) {
        client_resize_fullscreen(client, m);
        return;
    }

    // ensure minimal size
    if (client->float_size.width < WINDOW_MIN_WIDTH)
        client->float_size.width = WINDOW_MIN_WIDTH;
    if (client->float_size.height < WINDOW_MIN_HEIGHT)
        client->float_size.height = WINDOW_MIN_HEIGHT;

    client->last_border_width = *g_window_border_width;
    client->last_size = client->float_size;
    client->last_size.x += m->rect.x + m->pad_left;
    client->last_size.y += m->rect.y + m->pad_up;

    // ensure position is on monitor
    int space = g_monitor_float_treshold;
    client->last_size.x =
        CLAMP(client->last_size.x,
              m->rect.x + m->pad_left - client->last_size.width + space,
              m->rect.x + m->rect.width - m->pad_left - m->pad_right - space);
    client->last_size.y =
        CLAMP(client->last_size.y,
              m->rect.y + m->pad_up - client->last_size.height + space,
              m->rect.y + m->rect.height - m->pad_up - m->pad_down - space);
    XRectangle rect = client->last_size;
    XSetWindowBorderWidth(g_display, client->window, *g_window_border_width);
    XMoveResizeWindow(g_display, client->window,
        rect.x, rect.y, rect.width, rect.height);
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        unsigned long current_border_color = get_window_border_color(client);
        HSDebug("client_resize %s\n",
                current_border_color == g_window_border_active_color
                ? "ACTIVE" : "NORMAL");
        set_window_double_border(g_display, client->window,
                                 *g_window_border_inner_width,
                                 g_window_border_inner_color,
                                 current_border_color);
    }
}

XRectangle client_outer_floating_rect(HSClient* client) {
    XRectangle rect = client->float_size;
    rect.width  += *g_window_border_width * 2 + *g_snap_gap;
    rect.height += *g_window_border_width * 2 + *g_snap_gap;
    return rect;
}

// from dwm.c
int window_close_current() {
    XEvent ev;
    // if there is no focus, then there is nothing to do
    if (!g_cur_frame) return 0;
    Window win = frame_focused_window(g_cur_frame);
    if (!win) return 0;
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = g_wmatom[WMProtocols];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = g_wmatom[WMDelete];
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(g_display, win, False, NoEventMask, &ev);
    return 0;
}

void window_set_visible(Window win, bool visible) {
    static int (*action[])(Display*,Window) = {
        XUnmapWindow,
        XMapWindow,
    };
    unsigned long event_mask = CLIENT_EVENT_MASK;
    XGrabServer(g_display);
    XSelectInput(g_display, win, event_mask & ~StructureNotifyMask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK & ~SubstructureNotifyMask);
    action[visible](g_display, win);
    XSelectInput(g_display, win, event_mask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK);
    XUngrabServer(g_display);
}

void window_show(Window win) {
    window_set_visible(win, true);
}

void window_hide(Window win) {
    window_set_visible(win, false);
}

// heavily inspired by dwm.c
void client_set_urgent(HSClient* client, bool state) {
    if (client->urgent == state) {
        // nothing to do
        return;
    }

    char winid_str[STRING_BUF_SIZE];
    snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", client->window);
    hook_emit_list("urgent", state ? "on" : "off", winid_str, NULL);

    client->urgent = state;

    client_setup_border(client, client->window == frame_focused_window(g_cur_frame));

    XWMHints *wmh;
    if(!(wmh = XGetWMHints(g_display, client->window)))
        return;

    if (state) {
        wmh->flags |= XUrgencyHint;
    } else {
        wmh->flags &= ~XUrgencyHint;
    }

    XSetWMHints(g_display, client->window, wmh);
    XFree(wmh);
    // report changes to tags
    tag_set_flags_dirty();
}

// heavily inspired by dwm.c
void client_update_wm_hints(HSClient* client) {
    XWMHints* wmh = XGetWMHints(g_display, client->window);
    if (!wmh) {
        return;
    }

    Window focused_window = frame_focused_window(g_cur_frame);
    if ((focused_window == client->window)
        && wmh->flags & XUrgencyHint) {
        // remove urgency hint if window is focused
        wmh->flags &= ~XUrgencyHint;
        XSetWMHints(g_display, client->window, wmh);
    } else {
        bool newval = (wmh->flags & XUrgencyHint) ? true : false;
        if (newval != client->urgent) {
            client->urgent = newval;
            char winid_str[STRING_BUF_SIZE];
            snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", client->window);
            client_setup_border(client, focused_window == client->window);
            hook_emit_list("urgent", client->urgent ? "on":"off", winid_str, NULL);
            tag_set_flags_dirty();
        }
    }
    if (wmh->flags & InputHint) {
        client->neverfocus = !wmh->input;
    } else {
        client->neverfocus = false;
    }
    XFree(wmh);
}

void client_update_title(HSClient* client) {
    GString* new_name = window_property_to_g_string(g_display,
        client->window, g_netatom[NetWmName]);
    if (!new_name) {
        char* ch_new_name = NULL;
        /* if ewmh name isn't set, then fall back to WM_NAME */
        if (0 != XFetchName(g_display, client->window, &ch_new_name)) {
            new_name = g_string_new(ch_new_name);
            XFree(ch_new_name);
        } else {
            new_name = g_string_new("");
            HSDebug("no title for window %lx found, using \"\"\n",
                    client->window);
        }
    }
    bool changed = (0 != strcmp(client->title->str, new_name->str));
    g_string_free(client->title, true);
    client->title = new_name;
    if (changed && get_current_client() == client) {
        char buf[STRING_BUF_SIZE];
        snprintf(buf, STRING_BUF_SIZE, "0x%lx", client->window);
        hook_emit_list("window_title_changed", buf, client->title->str, NULL);
    }
}

HSClient* get_current_client() {
    Window win = frame_focused_window(g_cur_frame);
    if (!win) return NULL;
    return get_client_from_window(win);
}

void client_set_fullscreen(HSClient* client, bool state) {
    if (client->fullscreen == state) {
        // nothing to do
        return;
    }

    client->fullscreen = state;
    if (client->ewmhnotify) {
        client->ewmhfullscreen = state;
    }
    HSStack* stack = client->tag->stack;
    if (state) {
        stack_slice_add_layer(stack, client->slice, LAYER_FULLSCREEN);
    } else {
        stack_slice_remove_layer(stack, client->slice, LAYER_FULLSCREEN);
    }
    tag_update_focus_layer(client->tag);
    monitor_apply_layout(find_monitor_with_tag(client->tag));

    char buf[STRING_BUF_SIZE];
    snprintf(buf, STRING_BUF_SIZE, "0x%lx", client->window);
    ewmh_update_window_state(client);
    hook_emit_list("fullscreen", state ? "on" : "off", buf, NULL);
}

void client_set_pseudotile(HSClient* client, bool state) {
    client->pseudotile = state;
    monitor_apply_layout(find_monitor_with_tag(client->tag));
}

int client_set_property_command(int argc, char** argv) {
    if (argc < 2) {
        return HERBST_INVALID_ARGUMENT;
    }

    HSClient* client = get_current_client();
    if (!client) {
        // nothing to do
        return 0;
    }

    struct {
        char* name;
        void (*func)(HSClient*, bool);
        bool* value;
    } properties[] = {
        { "fullscreen",   client_set_fullscreen, &client->fullscreen    },
        { "pseudotile",   client_set_pseudotile, &client->pseudotile    },
    };

    // find the property
    int i;
    for  (i = 0; i < LENGTH(properties); i++) {
        if (!strcmp(properties[i].name, argv[0])) {
            break;
        }
    }
    if (i >= LENGTH(properties)) {
        return HERBST_INVALID_ARGUMENT;
    }

    // if found, then change it
    bool state = string_to_bool(argv[1], *(properties[i].value));
    properties[i].func(client, state);
    return 0;
}

void window_update_border(Window window, unsigned long color) {
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        set_window_double_border(g_display, window,
                                 *g_window_border_inner_width,
                                 g_window_border_inner_color, color);
    } else {
        XSetWindowBorder(g_display, window, color);
    }
}

unsigned long get_window_border_color(HSClient* client) {
    Window win = client->window;
    unsigned long current_border_color = (win == frame_focused_window(g_cur_frame)
                                          ? g_window_border_active_color
                                          : g_window_border_normal_color);
    if (client->urgent)
        current_border_color = g_window_border_urgent_color;
    return current_border_color;
}

static bool is_client_urgent(void* key, HSClient* client, void* data) {
    (void) key;
    (void) data;
    return client->urgent;
}

HSClient* get_urgent_client() {
    return g_hash_table_find(g_clients, (GHRFunc)is_client_urgent, NULL);
}

/**
 * \brief   Resolve a window description to a client or a window id
 *
 * \param   str     Describes the window: "" means the focused one, "urgent"
 *                  resolves to a arbitrary urgent window, "0x..." just
 *                  resolves to the given window.
 * \param   ret_client  The client pointer is stored there if ret_client is
 *                      given and the specified window is managed.
 * \return          The resolved window id is stored there if the according
 *                  window has been found
 */
Window string_to_client(char* str, HSClient** ret_client) {
    Window win = 0;
    if (!strcmp(str, "")) {
        win = frame_focused_window(g_cur_frame);
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    } else if (!strcmp(str, "urgent")) {
        HSClient* client = get_urgent_client();
        if (client) {
            win = client->window;
        }
        if (ret_client) {
            *ret_client = client;
        }
    } else if (1 == sscanf(str, "0x%lx", (long unsigned int*)&win)) {
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    }
    return win;
}

// mainly from dwm.c
bool client_sendevent(HSClient *client, Atom proto) {
    int n;
    Atom *protocols;
    bool exists = false;
    XEvent ev;

    if (XGetWMProtocols(g_display, client->window, &protocols, &n)) {
        while (!exists && n--)
            exists = protocols[n] == proto;
        XFree(protocols);
    }
    if (exists) {
        ev.type = ClientMessage;
        ev.xclient.window = client->window;
        ev.xclient.message_type = g_wmatom[WMProtocols];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(g_display, client->window, False, NoEventMask, &ev);
    }
    return exists;
}

