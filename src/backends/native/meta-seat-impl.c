/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2010  Intel Corp.
 * Copyright (C) 2014  Jonas Ådahl
 * Copyright (C) 2016  Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Damien Lespiau <damien.lespiau@intel.com>
 * Author: Jonas Ådahl <jadahl@gmail.com>
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config.h"

#include "backends/native/meta-seat-impl.h"

#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input.h>
#include <math.h>

#include "backends/meta-cursor-tracker-private.h"
#include "backends/native/meta-barrier-native.h"
#include "backends/native/meta-event-native.h"
#include "backends/native/meta-input-device-native.h"
#include "backends/native/meta-input-device-tool-native.h"
#include "backends/native/meta-keymap-native.h"
#include "backends/native/meta-virtual-input-device-native.h"
#include "clutter/clutter-mutter.h"
#include "core/bell.h"

/*
 * Clutter makes the assumption that two core devices have ID's 2 and 3 (core
 * pointer and core keyboard).
 *
 * Since the two first devices that will ever be created will be the virtual
 * pointer and virtual keyboard of the first seat, we fulfill the made
 * assumptions by having the first device having ID 2 and following 3.
 */
#define INITIAL_DEVICE_ID 2

/* Try to keep the pointer inside the stage. Hopefully no one is using
 * this backend with stages smaller than this. */
#define INITIAL_POINTER_X 16
#define INITIAL_POINTER_Y 16

#define AUTOREPEAT_VALUE 2

#define DISCRETE_SCROLL_STEP 10.0

#ifndef BTN_STYLUS3
#define BTN_STYLUS3 0x149 /* Linux 4.15 */
#endif

struct _MetaEventSource
{
  GSource source;

  MetaSeatImpl *seat_impl;
  GPollFD event_poll_fd;
};

static MetaOpenDeviceCallback  device_open_callback;
static MetaCloseDeviceCallback device_close_callback;
static gpointer                device_callback_data;

#ifdef CLUTTER_ENABLE_DEBUG
static const char *device_type_str[] = {
  "pointer",            /* CLUTTER_POINTER_DEVICE */
  "keyboard",           /* CLUTTER_KEYBOARD_DEVICE */
  "extension",          /* CLUTTER_EXTENSION_DEVICE */
  "joystick",           /* CLUTTER_JOYSTICK_DEVICE */
  "tablet",             /* CLUTTER_TABLET_DEVICE */
  "touchpad",           /* CLUTTER_TOUCHPAD_DEVICE */
  "touchscreen",        /* CLUTTER_TOUCHSCREEN_DEVICE */
  "pen",                /* CLUTTER_PEN_DEVICE */
  "eraser",             /* CLUTTER_ERASER_DEVICE */
  "cursor",             /* CLUTTER_CURSOR_DEVICE */
  "pad",                /* CLUTTER_PAD_DEVICE */
};
#endif /* CLUTTER_ENABLE_DEBUG */

enum
{
  PROP_0,
  PROP_SEAT,
  PROP_SEAT_ID,
  N_PROPS,
};

static GParamSpec *props[N_PROPS] = { NULL };

enum
{
  KBD_A11Y_FLAGS_CHANGED,
  KBD_A11Y_MODS_STATE_CHANGED,
  TOUCH_MODE,
  BELL,
  MODS_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

G_DEFINE_TYPE (MetaSeatImpl, meta_seat_impl, G_TYPE_OBJECT)

static void process_events (MetaSeatImpl *seat);
void meta_seat_impl_constrain_pointer (MetaSeatImpl       *seat,
                                       ClutterInputDevice *core_pointer,
                                       uint64_t            time_us,
                                       float               x,
                                       float               y,
                                       float              *new_x,
                                       float              *new_y);
void meta_seat_impl_filter_relative_motion (MetaSeatImpl       *seat,
                                            ClutterInputDevice *device,
                                            float               x,
                                            float               y,
                                            float              *dx,
                                            float              *dy);
void meta_seat_impl_clear_repeat_timer (MetaSeatImpl *seat);

void
meta_seat_impl_set_libinput_seat (MetaSeatImpl         *seat,
                                  struct libinput_seat *libinput_seat)
{
  g_assert (seat->libinput_seat == NULL);

  libinput_seat_ref (libinput_seat);
  libinput_seat_set_user_data (libinput_seat, seat);
  seat->libinput_seat = libinput_seat;
}

void
meta_seat_impl_sync_leds (MetaSeatImpl *seat)
{
  GSList *iter;
  MetaInputDeviceNative *device_evdev;
  int caps_lock, num_lock, scroll_lock;
  enum libinput_led leds = 0;

  caps_lock = xkb_state_led_index_is_active (seat->xkb, seat->caps_lock_led);
  num_lock = xkb_state_led_index_is_active (seat->xkb, seat->num_lock_led);
  scroll_lock = xkb_state_led_index_is_active (seat->xkb, seat->scroll_lock_led);

  if (caps_lock)
    leds |= LIBINPUT_LED_CAPS_LOCK;
  if (num_lock)
    leds |= LIBINPUT_LED_NUM_LOCK;
  if (scroll_lock)
    leds |= LIBINPUT_LED_SCROLL_LOCK;

  for (iter = seat->devices; iter; iter = iter->next)
    {
      device_evdev = iter->data;
      meta_input_device_native_update_leds (device_evdev, leds);
    }
}

MetaTouchState *
meta_seat_impl_lookup_touch_state (MetaSeatImpl *seat,
                                   int           seat_slot)
{
  if (!seat->touch_states)
    return NULL;
  return g_hash_table_lookup (seat->touch_states, GINT_TO_POINTER (seat_slot));
}

static void
meta_touch_state_free (MetaTouchState *state)
{
  g_slice_free (MetaTouchState, state);
}

MetaTouchState *
meta_seat_impl_acquire_touch_state (MetaSeatImpl *seat,
                                    int           seat_slot)
{
  MetaTouchState *touch_state;

  if (!seat->touch_states)
    {
      seat->touch_states =
        g_hash_table_new_full (NULL, NULL, NULL,
                               (GDestroyNotify) meta_touch_state_free);
    }

  g_assert (!g_hash_table_contains (seat->touch_states,
                                    GINT_TO_POINTER (seat_slot)));

  touch_state = g_slice_new0 (MetaTouchState);
  *touch_state = (MetaTouchState) {
    .seat = seat,
    .seat_slot = seat_slot,
  };

  g_hash_table_insert (seat->touch_states, GINT_TO_POINTER (seat_slot),
                       touch_state);

  return touch_state;
}

void
meta_seat_impl_release_touch_state (MetaSeatImpl *seat,
                                    int           seat_slot)
{
  if (!seat->touch_states)
    return;
  g_hash_table_remove (seat->touch_states, GINT_TO_POINTER (seat_slot));
}

void
meta_seat_impl_clear_repeat_timer (MetaSeatImpl *seat)
{
  if (seat->repeat_timer)
    {
      g_clear_handle_id (&seat->repeat_timer, g_source_remove);
      g_clear_object (&seat->repeat_device);
    }
}

static void
dispatch_libinput (MetaSeatImpl *seat)
{
  libinput_dispatch (seat->libinput);
  process_events (seat);
}

static gboolean
keyboard_repeat (gpointer data)
{
  MetaSeatImpl *seat = data;
  GSource *source;

  /* There might be events queued in libinput that could cancel the
     repeat timer. */
  dispatch_libinput (seat);
  if (!seat->repeat_timer)
    return G_SOURCE_REMOVE;

  g_return_val_if_fail (seat->repeat_device != NULL, G_SOURCE_REMOVE);
  source = g_main_context_find_source_by_id (NULL, seat->repeat_timer);

  meta_seat_impl_notify_key (seat,
                             seat->repeat_device,
                             g_source_get_time (source),
                             seat->repeat_key,
                             AUTOREPEAT_VALUE,
                             FALSE);

  return G_SOURCE_CONTINUE;
}

static void
queue_event (MetaSeatImpl *seat,
             ClutterEvent *event)
{
  _clutter_event_push (event, FALSE);
}

static int
update_button_count (MetaSeatImpl *seat,
                     uint32_t      button,
                     uint32_t      state)
{
  if (state)
    {
      return ++seat->button_count[button];
    }
  else
    {
      /* Handle cases where we newer saw the initial pressed event. */
      if (seat->button_count[button] == 0)
        {
          meta_topic (META_DEBUG_INPUT,
                      "Counting release of key 0x%x and count is already 0",
                      button);
          return 0;
        }

      return --seat->button_count[button];
    }
}

void
meta_seat_impl_notify_key (MetaSeatImpl       *seat,
                           ClutterInputDevice *device,
                           uint64_t            time_us,
                           uint32_t            key,
                           uint32_t            state,
                           gboolean            update_keys)
{
  ClutterEvent *event = NULL;
  enum xkb_state_component changed_state;

  if (state != AUTOREPEAT_VALUE)
    {
      /* Drop any repeated button press (for example from virtual devices. */
      int count = update_button_count (seat, key, state);
      if ((state && count > 1) ||
          (!state && count != 0))
        {
          meta_topic (META_DEBUG_INPUT,
                      "Dropping repeated %s of key 0x%x, count %d, state %d",
                      state ? "press" : "release", key, count, state);
          return;
        }
    }

  event = meta_key_event_new_from_evdev (device,
                                         seat->core_keyboard,
                                         seat->xkb,
                                         seat->button_state,
                                         us2ms (time_us), key, state);
  meta_event_native_set_event_code (event, key);

  /* We must be careful and not pass multiple releases to xkb, otherwise it gets
     confused and locks the modifiers */
  if (state != AUTOREPEAT_VALUE)
    {
      changed_state = xkb_state_update_key (seat->xkb,
                                            event->key.hardware_keycode,
                                            state ? XKB_KEY_DOWN : XKB_KEY_UP);
    }
  else
    {
      changed_state = 0;
      clutter_event_set_flags (event, CLUTTER_EVENT_FLAG_REPEATED);
    }

  queue_event (seat, event);

  if (update_keys && (changed_state & XKB_STATE_LEDS))
    {
      g_signal_emit (seat, signals[MODS_STATE_CHANGED], 0);
      meta_seat_impl_sync_leds (seat);
      meta_input_device_native_a11y_maybe_notify_toggle_keys (META_INPUT_DEVICE_NATIVE (seat->core_keyboard));
    }

  if (state == 0 ||             /* key release */
      !seat->repeat ||
      !xkb_keymap_key_repeats (xkb_state_get_keymap (seat->xkb),
                               event->key.hardware_keycode))
    {
      meta_seat_impl_clear_repeat_timer (seat);
      return;
    }

  if (state == 1)               /* key press */
    seat->repeat_count = 0;

  seat->repeat_count += 1;
  seat->repeat_key = key;

  switch (seat->repeat_count)
    {
    case 1:
    case 2:
      {
        uint32_t interval;

        meta_seat_impl_clear_repeat_timer (seat);
        seat->repeat_device = g_object_ref (device);

        if (seat->repeat_count == 1)
          interval = seat->repeat_delay;
        else
          interval = seat->repeat_interval;

        seat->repeat_timer =
          clutter_threads_add_timeout_full (CLUTTER_PRIORITY_EVENTS,
                                            interval,
                                            keyboard_repeat,
                                            seat,
                                            NULL);
        return;
      }
    default:
      return;
    }
}

static ClutterEvent *
new_absolute_motion_event (MetaSeatImpl       *seat,
                           ClutterInputDevice *input_device,
                           uint64_t            time_us,
                           float               x,
                           float               y,
                           double             *axes)
{
  ClutterEvent *event;

  event = clutter_event_new (CLUTTER_MOTION);

  if (clutter_input_device_get_device_type (input_device) != CLUTTER_TABLET_DEVICE)
    {
      meta_seat_impl_constrain_pointer (seat,
                                        seat->core_pointer,
                                        time_us,
                                        seat->pointer_x,
                                        seat->pointer_y,
                                        &x, &y);
    }

  meta_event_native_set_time_usec (event, time_us);
  event->motion.time = us2ms (time_us);
  meta_xkb_translate_state (event, seat->xkb, seat->button_state);
  event->motion.x = x;
  event->motion.y = y;

  /* This may happen early at startup */
  if (seat->viewports)
    {
      meta_input_device_native_translate_coordinates (input_device,
                                                      seat->viewports,
                                                      &event->motion.x,
                                                      &event->motion.y);
    }

  event->motion.axes = axes;
  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  if (clutter_input_device_get_device_type (input_device) == CLUTTER_TABLET_DEVICE)
    {
      MetaInputDeviceNative *device_evdev =
        META_INPUT_DEVICE_NATIVE (input_device);

      clutter_event_set_device_tool (event, device_evdev->last_tool);
      clutter_event_set_device (event, input_device);
      meta_input_device_native_update_coords (META_INPUT_DEVICE_NATIVE (input_device),
                                              x, y);
    }
  else
    {
      clutter_event_set_device (event, seat->core_pointer);
      meta_input_device_native_update_coords (META_INPUT_DEVICE_NATIVE (seat->core_pointer),
                                              x, y);
    }

  if (clutter_input_device_get_device_type (input_device) != CLUTTER_TABLET_DEVICE)
    {
      seat->pointer_x = x;
      seat->pointer_y = y;
    }

  return event;
}

void
meta_seat_impl_notify_relative_motion (MetaSeatImpl       *seat,
                                       ClutterInputDevice *input_device,
                                       uint64_t            time_us,
                                       float               dx,
                                       float               dy,
                                       float               dx_unaccel,
                                       float               dy_unaccel)
{
  float new_x, new_y;
  ClutterEvent *event;

  meta_seat_impl_filter_relative_motion (seat,
                                         input_device,
                                         seat->pointer_x,
                                         seat->pointer_y,
                                         &dx,
                                         &dy);

  new_x = seat->pointer_x + dx;
  new_y = seat->pointer_y + dy;
  event = new_absolute_motion_event (seat, input_device,
                                     time_us, new_x, new_y, NULL);

  meta_event_native_set_relative_motion (event,
                                         dx, dy,
                                         dx_unaccel, dy_unaccel);

  queue_event (seat, event);
}

void
meta_seat_impl_notify_absolute_motion (MetaSeatImpl       *seat,
                                       ClutterInputDevice *input_device,
                                       uint64_t            time_us,
                                       float               x,
                                       float               y,
                                       double             *axes)
{
  ClutterEvent *event;

  event = new_absolute_motion_event (seat, input_device, time_us, x, y, axes);

  queue_event (seat, event);
}

void
meta_seat_impl_notify_button (MetaSeatImpl       *seat,
                              ClutterInputDevice *input_device,
                              uint64_t            time_us,
                              uint32_t            button,
                              uint32_t            state)
{
  MetaInputDeviceNative *device_evdev = (MetaInputDeviceNative *) input_device;
  ClutterEvent *event = NULL;
  int button_nr;
  static int maskmap[8] =
    {
      CLUTTER_BUTTON1_MASK, CLUTTER_BUTTON3_MASK, CLUTTER_BUTTON2_MASK,
      CLUTTER_BUTTON4_MASK, CLUTTER_BUTTON5_MASK, 0, 0, 0
    };
  int button_count;

  /* Drop any repeated button press (for example from virtual devices. */
  button_count = update_button_count (seat, button, state);
  if ((state && button_count > 1) ||
      (!state && button_count != 0))
    {
      meta_topic (META_DEBUG_INPUT,
                  "Dropping repeated %s of button 0x%x, count %d",
                  state ? "press" : "release", button, button_count);
      return;
    }

  /* The evdev button numbers don't map sequentially to clutter button
   * numbers (the right and middle mouse buttons are in the opposite
   * order) so we'll map them directly with a switch statement */
  switch (button)
    {
    case BTN_LEFT:
    case BTN_TOUCH:
      button_nr = CLUTTER_BUTTON_PRIMARY;
      break;

    case BTN_RIGHT:
    case BTN_STYLUS:
      button_nr = CLUTTER_BUTTON_SECONDARY;
      break;

    case BTN_MIDDLE:
    case BTN_STYLUS2:
      button_nr = CLUTTER_BUTTON_MIDDLE;
      break;

    case 0x149: /* BTN_STYLUS3 */
      button_nr = 8;
      break;

    default:
      /* For compatibility reasons, all additional buttons go after the old 4-7 scroll ones */
      if (clutter_input_device_get_device_type (input_device) == CLUTTER_TABLET_DEVICE)
        button_nr = button - BTN_TOOL_PEN + 4;
      else
        button_nr = button - (BTN_LEFT - 1) + 4;
      break;
    }

  if (button_nr < 1 || button_nr > 12)
    {
      g_warning ("Unhandled button event 0x%x", button);
      return;
    }

  if (state)
    event = clutter_event_new (CLUTTER_BUTTON_PRESS);
  else
    event = clutter_event_new (CLUTTER_BUTTON_RELEASE);

  if (button_nr < G_N_ELEMENTS (maskmap))
    {
      /* Update the modifiers */
      if (state)
        seat->button_state |= maskmap[button_nr - 1];
      else
        seat->button_state &= ~maskmap[button_nr - 1];
    }

  meta_event_native_set_time_usec (event, time_us);
  event->button.time = us2ms (time_us);
  meta_xkb_translate_state (event, seat->xkb, seat->button_state);
  event->button.button = button_nr;

  if (clutter_input_device_get_device_type (input_device) == CLUTTER_TABLET_DEVICE)
    {
      graphene_point_t point;

      clutter_input_device_get_coords (input_device, NULL, &point);
      event->button.x = point.x;
      event->button.y = point.y;
    }
  else
    {
      event->button.x = seat->pointer_x;
      event->button.y = seat->pointer_y;
    }

  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  if (device_evdev->last_tool)
    {
      /* Apply the button event code as per the tool mapping */
      uint32_t mapped_button;

      mapped_button = meta_input_device_tool_native_get_button_code (device_evdev->last_tool,
                                                                     button_nr);
      if (mapped_button != 0)
        button = mapped_button;
    }

  meta_event_native_set_event_code (event, button);

  if (clutter_input_device_get_device_type (input_device) == CLUTTER_TABLET_DEVICE)
    {
      clutter_event_set_device_tool (event, device_evdev->last_tool);
      clutter_event_set_device (event, input_device);
    }
  else
    {
      clutter_event_set_device (event, seat->core_pointer);
    }

  queue_event (seat, event);
}

static MetaSeatImpl *
seat_from_device (ClutterInputDevice *device)
{
  MetaInputDeviceNative *device_evdev = META_INPUT_DEVICE_NATIVE (device);

  return meta_input_device_native_get_seat_impl (device_evdev);
}

static void
notify_scroll (ClutterInputDevice       *input_device,
               uint64_t                  time_us,
               double                    dx,
               double                    dy,
               ClutterScrollSource       scroll_source,
               ClutterScrollFinishFlags  flags,
               gboolean                  emulated)
{
  MetaSeatImpl *seat;
  ClutterEvent *event = NULL;
  double scroll_factor;

  seat = seat_from_device (input_device);

  event = clutter_event_new (CLUTTER_SCROLL);

  meta_event_native_set_time_usec (event, time_us);
  event->scroll.time = us2ms (time_us);
  meta_xkb_translate_state (event, seat->xkb, seat->button_state);

  /* libinput pointer axis events are in pointer motion coordinate space.
   * To convert to Xi2 discrete step coordinate space, multiply the factor
   * 1/10. */
  event->scroll.direction = CLUTTER_SCROLL_SMOOTH;
  scroll_factor = 1.0 / DISCRETE_SCROLL_STEP;
  clutter_event_set_scroll_delta (event,
                                  scroll_factor * dx,
                                  scroll_factor * dy);

  event->scroll.x = seat->pointer_x;
  event->scroll.y = seat->pointer_y;
  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);
  event->scroll.scroll_source = scroll_source;
  event->scroll.finish_flags = flags;

  _clutter_event_set_pointer_emulated (event, emulated);

  queue_event (seat, event);
}

static void
notify_discrete_scroll (ClutterInputDevice     *input_device,
                        uint64_t                time_us,
                        ClutterScrollDirection  direction,
                        ClutterScrollSource     scroll_source,
                        gboolean                emulated)
{
  MetaSeatImpl *seat;
  ClutterEvent *event = NULL;

  if (direction == CLUTTER_SCROLL_SMOOTH)
    return;

  seat = seat_from_device (input_device);

  event = clutter_event_new (CLUTTER_SCROLL);

  meta_event_native_set_time_usec (event, time_us);
  event->scroll.time = us2ms (time_us);
  meta_xkb_translate_state (event, seat->xkb, seat->button_state);

  event->scroll.direction = direction;

  event->scroll.x = seat->pointer_x;
  event->scroll.y = seat->pointer_y;
  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);
  event->scroll.scroll_source = scroll_source;

  _clutter_event_set_pointer_emulated (event, emulated);

  queue_event (seat, event);
}

static void
check_notify_discrete_scroll (MetaSeatImpl       *seat,
                              ClutterInputDevice *device,
                              uint64_t            time_us,
                              ClutterScrollSource scroll_source)
{
  int i, n_xscrolls, n_yscrolls;

  n_xscrolls = floor (fabs (seat->accum_scroll_dx) / DISCRETE_SCROLL_STEP);
  n_yscrolls = floor (fabs (seat->accum_scroll_dy) / DISCRETE_SCROLL_STEP);

  for (i = 0; i < n_xscrolls; i++)
    {
      notify_discrete_scroll (device, time_us,
                              seat->accum_scroll_dx > 0 ?
                              CLUTTER_SCROLL_RIGHT : CLUTTER_SCROLL_LEFT,
                              scroll_source, TRUE);
    }

  for (i = 0; i < n_yscrolls; i++)
    {
      notify_discrete_scroll (device, time_us,
                              seat->accum_scroll_dy > 0 ?
                              CLUTTER_SCROLL_DOWN : CLUTTER_SCROLL_UP,
                              scroll_source, TRUE);
    }

  seat->accum_scroll_dx = fmodf (seat->accum_scroll_dx, DISCRETE_SCROLL_STEP);
  seat->accum_scroll_dy = fmodf (seat->accum_scroll_dy, DISCRETE_SCROLL_STEP);
}

void
meta_seat_impl_notify_scroll_continuous (MetaSeatImpl             *seat,
                                         ClutterInputDevice       *input_device,
                                         uint64_t                  time_us,
                                         double                    dx,
                                         double                    dy,
                                         ClutterScrollSource       scroll_source,
                                         ClutterScrollFinishFlags  finish_flags)
{
  if (finish_flags & CLUTTER_SCROLL_FINISHED_HORIZONTAL)
    seat->accum_scroll_dx = 0;
  else
    seat->accum_scroll_dx += dx;

  if (finish_flags & CLUTTER_SCROLL_FINISHED_VERTICAL)
    seat->accum_scroll_dy = 0;
  else
    seat->accum_scroll_dy += dy;

  notify_scroll (input_device, time_us, dx, dy, scroll_source,
                 finish_flags, FALSE);
  check_notify_discrete_scroll (seat, input_device, time_us, scroll_source);
}

static ClutterScrollDirection
discrete_to_direction (double discrete_dx,
                       double discrete_dy)
{
  if (discrete_dx > 0)
    return CLUTTER_SCROLL_RIGHT;
  else if (discrete_dx < 0)
    return CLUTTER_SCROLL_LEFT;
  else if (discrete_dy > 0)
    return CLUTTER_SCROLL_DOWN;
  else if (discrete_dy < 0)
    return CLUTTER_SCROLL_UP;
  else
    g_assert_not_reached ();
  return 0;
}

void
meta_seat_impl_notify_discrete_scroll (MetaSeatImpl        *seat,
                                       ClutterInputDevice  *input_device,
                                       uint64_t             time_us,
                                       double               discrete_dx,
                                       double               discrete_dy,
                                       ClutterScrollSource  scroll_source)
{
  notify_scroll (input_device, time_us,
                 discrete_dx * DISCRETE_SCROLL_STEP,
                 discrete_dy * DISCRETE_SCROLL_STEP,
                 scroll_source, CLUTTER_SCROLL_FINISHED_NONE,
                 TRUE);
  notify_discrete_scroll (input_device, time_us,
                          discrete_to_direction (discrete_dx, discrete_dy),
                          scroll_source, FALSE);

}

void
meta_seat_impl_notify_touch_event (MetaSeatImpl       *seat,
                                   ClutterInputDevice *input_device,
                                   ClutterEventType    evtype,
                                   uint64_t            time_us,
                                   int                 slot,
                                   double              x,
                                   double              y)
{
  ClutterEvent *event = NULL;

  event = clutter_event_new (evtype);

  meta_event_native_set_time_usec (event, time_us);
  event->touch.time = us2ms (time_us);
  event->touch.x = x;
  event->touch.y = y;
  meta_input_device_native_translate_coordinates (input_device,
                                                  seat->viewports,
                                                  &event->touch.x,
                                                  &event->touch.y);

  /* "NULL" sequences are special cased in clutter */
  event->touch.sequence = GINT_TO_POINTER (MAX (1, slot + 1));
  meta_xkb_translate_state (event, seat->xkb, seat->button_state);

  if (evtype == CLUTTER_TOUCH_BEGIN ||
      evtype == CLUTTER_TOUCH_UPDATE)
    event->touch.modifier_state |= CLUTTER_BUTTON1_MASK;

  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  queue_event (seat, event);
}


/*
 * MetaEventSource for reading input devices
 */
static gboolean
meta_event_prepare (GSource *source,
                    int     *timeout)
{
  gboolean retval;

  *timeout = -1;
  retval = clutter_events_pending ();

  return retval;
}

static gboolean
meta_event_check (GSource *source)
{
  MetaEventSource *event_source = (MetaEventSource *) source;
  gboolean retval;

  retval = ((event_source->event_poll_fd.revents & G_IO_IN) ||
            clutter_events_pending ());

  return retval;
}

static void
constrain_to_barriers (MetaSeatImpl       *seat,
                       ClutterInputDevice *device,
                       uint32_t            time,
                       float              *new_x,
                       float              *new_y)
{
  meta_barrier_manager_native_process (seat->barrier_manager,
                                       device,
                                       time,
                                       new_x, new_y);
}

/*
 * The pointer constrain code is mostly a rip-off of the XRandR code from Xorg.
 * (from xserver/randr/rrcrtc.c, RRConstrainCursorHarder)
 *
 * Copyright © 2006 Keith Packard
 * Copyright 2010 Red Hat, Inc
 *
 */

static void
constrain_all_screen_monitors (ClutterInputDevice *device,
                               MetaViewportInfo   *viewports,
                               float              *x,
                               float              *y)
{
  graphene_point_t current;
  float cx, cy;
  int i, n_views;

  clutter_input_device_get_coords (device, NULL, &current);

  cx = current.x;
  cy = current.y;

  /* if we're trying to escape, clamp to the CRTC we're coming from */

  n_views = meta_viewport_info_get_num_views (viewports);

  for (i = 0; i < n_views; i++)
    {
      int left, right, top, bottom;
      cairo_rectangle_int_t rect;

      meta_viewport_info_get_view (viewports, i, &rect, NULL);

      left = rect.x;
      right = left + rect.width;
      top = rect.y;
      bottom = top + rect.height;

      if ((cx >= left) && (cx < right) && (cy >= top) && (cy < bottom))
        {
          if (*x < left)
            *x = left;
          if (*x >= right)
            *x = right - 1;
          if (*y < top)
            *y = top;
          if (*y >= bottom)
            *y = bottom - 1;

          return;
        }
    }
}

void
meta_seat_impl_constrain_pointer (MetaSeatImpl       *seat,
                                  ClutterInputDevice *core_pointer,
                                  uint64_t            time_us,
                                  float               x,
                                  float               y,
                                  float              *new_x,
                                  float              *new_y)
{
  /* Constrain to barriers */
  constrain_to_barriers (seat, core_pointer,
                         us2ms (time_us),
                         new_x, new_y);

  /* Bar to constraints */
  if (seat->pointer_constraint)
    {
      meta_pointer_constraint_impl_constrain (seat->pointer_constraint,
                                              core_pointer,
                                              us2ms (time_us),
                                              x, y,
                                              new_x, new_y);
    }

  if (seat->viewports)
    {
      /* if we're moving inside a monitor, we're fine */
      if (meta_viewport_info_get_view_at (seat->viewports, *new_x, *new_y) >= 0)
        return;

      /* if we're trying to escape, clamp to the CRTC we're coming from */
      constrain_all_screen_monitors (core_pointer, seat->viewports, new_x, new_y);
    }
}

static void
relative_motion_across_outputs (MetaViewportInfo   *viewports,
                                int                 view,
                                float               cur_x,
                                float               cur_y,
                                float              *dx_inout,
                                float              *dy_inout)
{
  int cur_view = view;
  float x = cur_x, y = cur_y;
  float target_x = cur_x, target_y = cur_y;
  float dx = *dx_inout, dy = *dy_inout;
  MetaDisplayDirection direction = -1;

  while (cur_view >= 0)
    {
      MetaLine2 left, right, top, bottom, motion;
      MetaVector2 intersection;
      cairo_rectangle_int_t rect;
      float scale;

      meta_viewport_info_get_view (viewports, cur_view, &rect, &scale);

      motion = (MetaLine2) {
        .a = { x, y },
        .b = { x + (dx * scale), y + (dy * scale) }
      };
      left = (MetaLine2) {
        { rect.x, rect.y },
        { rect.x, rect.y + rect.height }
      };
      right = (MetaLine2) {
        { rect.x + rect.width, rect.y },
        { rect.x + rect.width, rect.y + rect.height }
      };
      top = (MetaLine2) {
        { rect.x, rect.y },
        { rect.x + rect.width, rect.y }
      };
      bottom = (MetaLine2) {
        { rect.x, rect.y + rect.height },
        { rect.x + rect.width, rect.y + rect.height }
      };

      target_x = motion.b.x;
      target_y = motion.b.y;

      if (direction != META_DISPLAY_RIGHT &&
          meta_line2_intersects_with (&motion, &left, &intersection))
        direction = META_DISPLAY_LEFT;
      else if (direction != META_DISPLAY_LEFT &&
               meta_line2_intersects_with (&motion, &right, &intersection))
        direction = META_DISPLAY_RIGHT;
      else if (direction != META_DISPLAY_DOWN &&
               meta_line2_intersects_with (&motion, &top, &intersection))
        direction = META_DISPLAY_UP;
      else if (direction != META_DISPLAY_UP &&
               meta_line2_intersects_with (&motion, &bottom, &intersection))
        direction = META_DISPLAY_DOWN;
      else
        /* We reached the dest logical monitor */
        break;

      x = intersection.x;
      y = intersection.y;
      dx -= intersection.x - motion.a.x;
      dy -= intersection.y - motion.a.y;

      cur_view = meta_viewport_info_get_neighbor (viewports, cur_view,
                                                  direction);
    }

  *dx_inout = target_x - cur_x;
  *dy_inout = target_y - cur_y;
}

void
meta_seat_impl_filter_relative_motion (MetaSeatImpl       *seat,
                                       ClutterInputDevice *device,
                                       float               x,
                                       float               y,
                                       float              *dx,
                                       float              *dy)
{
  int view = -1, dest_view;
  float new_dx, new_dy, scale;

  if (meta_is_stage_views_scaled ())
    return;

  if (seat->viewports)
    view = meta_viewport_info_get_view_at (seat->viewports, x, y);
  if (view < 0)
    return;

  meta_viewport_info_get_view (seat->viewports, view, NULL, &scale);
  new_dx = (*dx) * scale;
  new_dy = (*dy) * scale;

  dest_view = meta_viewport_info_get_view_at (seat->viewports,
                                              x + new_dx,
                                              y + new_dy);
  if (dest_view >= 0 && dest_view != view)
    {
      /* If we are crossing monitors, attempt to bisect the distance on each
       * axis and apply the relative scale for each of them.
       */
      new_dx = *dx;
      new_dy = *dy;
      relative_motion_across_outputs (seat->viewports, view,
                                      x, y, &new_dx, &new_dy);
    }

  *dx = new_dx;
  *dy = new_dy;
}

static void
notify_absolute_motion (ClutterInputDevice *input_device,
                        uint64_t            time_us,
                        float               x,
                        float               y,
                        double             *axes)
{
  MetaSeatImpl *seat;
  ClutterEvent *event;

  seat = seat_from_device (input_device);
  event = new_absolute_motion_event (seat, input_device, time_us, x, y, axes);

  queue_event (seat, event);
}

static void
notify_relative_tool_motion (ClutterInputDevice *input_device,
                             uint64_t            time_us,
                             float               dx,
                             float               dy,
                             double             *axes)
{
  MetaInputDeviceNative *device_evdev;
  ClutterEvent *event;
  MetaSeatImpl *seat;
  float x, y;

  device_evdev = META_INPUT_DEVICE_NATIVE (input_device);
  seat = seat_from_device (input_device);
  x = device_evdev->pointer_x + dx;
  y = device_evdev->pointer_y + dy;

  meta_seat_impl_filter_relative_motion (seat,
                                         input_device,
                                         seat->pointer_x,
                                         seat->pointer_y,
                                         &dx,
                                         &dy);

  event = new_absolute_motion_event (seat, input_device, time_us, x, y, axes);
  meta_event_native_set_relative_motion (event, dx, dy, 0, 0);

  queue_event (seat, event);
}

static void
notify_pinch_gesture_event (ClutterInputDevice          *input_device,
                            ClutterTouchpadGesturePhase  phase,
                            uint64_t                     time_us,
                            double                       dx,
                            double                       dy,
                            double                       angle_delta,
                            double                       scale,
                            uint32_t                     n_fingers)
{
  MetaSeatImpl *seat;
  ClutterEvent *event = NULL;
  graphene_point_t pos;

  seat = seat_from_device (input_device);

  event = clutter_event_new (CLUTTER_TOUCHPAD_PINCH);

  clutter_input_device_get_coords (seat->core_pointer, NULL, &pos);

  meta_event_native_set_time_usec (event, time_us);
  event->touchpad_pinch.phase = phase;
  event->touchpad_pinch.time = us2ms (time_us);
  event->touchpad_pinch.x = pos.x;
  event->touchpad_pinch.y = pos.y;
  event->touchpad_pinch.dx = dx;
  event->touchpad_pinch.dy = dy;
  event->touchpad_pinch.angle_delta = angle_delta;
  event->touchpad_pinch.scale = scale;
  event->touchpad_pinch.n_fingers = n_fingers;

  meta_xkb_translate_state (event, seat->xkb, seat->button_state);

  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  queue_event (seat, event);
}

static void
notify_swipe_gesture_event (ClutterInputDevice          *input_device,
                            ClutterTouchpadGesturePhase  phase,
                            uint64_t                     time_us,
                            uint32_t                     n_fingers,
                            double                       dx,
                            double                       dy)
{
  MetaSeatImpl *seat;
  ClutterEvent *event = NULL;
  graphene_point_t pos;

  seat = seat_from_device (input_device);

  event = clutter_event_new (CLUTTER_TOUCHPAD_SWIPE);

  meta_event_native_set_time_usec (event, time_us);
  event->touchpad_swipe.phase = phase;
  event->touchpad_swipe.time = us2ms (time_us);

  clutter_input_device_get_coords (seat->core_pointer, NULL, &pos);
  event->touchpad_swipe.x = pos.x;
  event->touchpad_swipe.y = pos.y;
  event->touchpad_swipe.dx = dx;
  event->touchpad_swipe.dy = dy;
  event->touchpad_swipe.n_fingers = n_fingers;

  meta_xkb_translate_state (event, seat->xkb, seat->button_state);

  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  queue_event (seat, event);
}

static void
notify_proximity (ClutterInputDevice *input_device,
                  uint64_t            time_us,
                  gboolean            in)
{
  MetaInputDeviceNative *device_evdev;
  MetaSeatImpl *seat;
  ClutterEvent *event = NULL;

  device_evdev = META_INPUT_DEVICE_NATIVE (input_device);
  seat = seat_from_device (input_device);

  if (in)
    event = clutter_event_new (CLUTTER_PROXIMITY_IN);
  else
    event = clutter_event_new (CLUTTER_PROXIMITY_OUT);

  meta_event_native_set_time_usec (event, time_us);

  event->proximity.time = us2ms (time_us);
  clutter_event_set_device_tool (event, device_evdev->last_tool);
  clutter_event_set_device (event, seat->core_pointer);
  clutter_event_set_source_device (event, input_device);

  queue_event (seat, event);
}

static void
notify_pad_button (ClutterInputDevice *input_device,
                   uint64_t            time_us,
                   uint32_t            button,
                   uint32_t            mode_group,
                   uint32_t            mode,
                   uint32_t            pressed)
{
  MetaSeatImpl *seat;
  ClutterEvent *event;

  seat = seat_from_device (input_device);

  if (pressed)
    event = clutter_event_new (CLUTTER_PAD_BUTTON_PRESS);
  else
    event = clutter_event_new (CLUTTER_PAD_BUTTON_RELEASE);

  meta_event_native_set_time_usec (event, time_us);
  event->pad_button.button = button;
  event->pad_button.group = mode_group;
  event->pad_button.mode = mode;
  clutter_event_set_device (event, input_device);
  clutter_event_set_source_device (event, input_device);
  clutter_event_set_time (event, us2ms (time_us));

  queue_event (seat, event);
}

static void
notify_pad_strip (ClutterInputDevice *input_device,
                  uint64_t            time_us,
                  uint32_t            strip_number,
                  uint32_t            strip_source,
                  uint32_t            mode_group,
                  uint32_t            mode,
                  double              value)
{
  ClutterInputDevicePadSource source;
  MetaSeatImpl *seat;
  ClutterEvent *event;

  seat = seat_from_device (input_device);

  if (strip_source == LIBINPUT_TABLET_PAD_STRIP_SOURCE_FINGER)
    source = CLUTTER_INPUT_DEVICE_PAD_SOURCE_FINGER;
  else
    source = CLUTTER_INPUT_DEVICE_PAD_SOURCE_UNKNOWN;

  event = clutter_event_new (CLUTTER_PAD_STRIP);
  meta_event_native_set_time_usec (event, time_us);
  event->pad_strip.strip_source = source;
  event->pad_strip.strip_number = strip_number;
  event->pad_strip.value = value;
  event->pad_strip.group = mode_group;
  event->pad_strip.mode = mode;
  clutter_event_set_device (event, input_device);
  clutter_event_set_source_device (event, input_device);
  clutter_event_set_time (event, us2ms (time_us));

  queue_event (seat, event);
}

static void
notify_pad_ring (ClutterInputDevice *input_device,
                 uint64_t            time_us,
                 uint32_t            ring_number,
                 uint32_t            ring_source,
                 uint32_t            mode_group,
                 uint32_t            mode,
                 double              angle)
{
  ClutterInputDevicePadSource source;
  MetaSeatImpl *seat;
  ClutterEvent *event;

  seat = seat_from_device (input_device);

  if (ring_source == LIBINPUT_TABLET_PAD_RING_SOURCE_FINGER)
    source = CLUTTER_INPUT_DEVICE_PAD_SOURCE_FINGER;
  else
    source = CLUTTER_INPUT_DEVICE_PAD_SOURCE_UNKNOWN;

  event = clutter_event_new (CLUTTER_PAD_RING);
  meta_event_native_set_time_usec (event, time_us);
  event->pad_ring.ring_source = source;
  event->pad_ring.ring_number = ring_number;
  event->pad_ring.angle = angle;
  event->pad_ring.group = mode_group;
  event->pad_ring.mode = mode;
  clutter_event_set_device (event, input_device);
  clutter_event_set_source_device (event, input_device);
  clutter_event_set_time (event, us2ms (time_us));

  queue_event (seat, event);
}

static gboolean
meta_event_dispatch (GSource     *g_source,
                     GSourceFunc  callback,
                     gpointer     user_data)
{
  MetaEventSource *source = (MetaEventSource *) g_source;
  MetaSeatImpl *seat;

  seat = source->seat_impl;

  /* Don't queue more events if we haven't finished handling the previous batch
   */
  if (clutter_events_pending ())
    goto queue_event;

  dispatch_libinput (seat);

 queue_event:

  return TRUE;
}
static GSourceFuncs event_funcs = {
  meta_event_prepare,
  meta_event_check,
  meta_event_dispatch,
  NULL
};

static MetaEventSource *
meta_event_source_new (MetaSeatImpl *seat)
{
  GSource *source;
  MetaEventSource *event_source;
  int fd;

  source = g_source_new (&event_funcs, sizeof (MetaEventSource));
  event_source = (MetaEventSource *) source;

  /* setup the source */
  event_source->seat_impl = seat;

  fd = libinput_get_fd (seat->libinput);
  event_source->event_poll_fd.fd = fd;
  event_source->event_poll_fd.events = G_IO_IN;

  /* and finally configure and attach the GSource */
  g_source_set_priority (source, CLUTTER_PRIORITY_EVENTS);
  g_source_add_poll (source, &event_source->event_poll_fd);
  g_source_set_can_recurse (source, TRUE);
  g_source_attach (source, NULL);

  return event_source;
}

static void
meta_event_source_free (MetaEventSource *source)
{
  GSource *g_source = (GSource *) source;

  /* ignore the return value of close, it's not like we can do something
   * about it */
  close (source->event_poll_fd.fd);

  g_source_destroy (g_source);
  g_source_unref (g_source);
}

static gboolean
has_touchscreen (MetaSeatImpl *seat)
{
  GSList *l;

  for (l = seat->devices; l; l = l->next)
    {
      ClutterInputDeviceType device_type;

      device_type = clutter_input_device_get_device_type (l->data);

      if (device_type == CLUTTER_TOUCHSCREEN_DEVICE)
        return TRUE;
    }

  return FALSE;
}

static gboolean
device_is_tablet_switch (MetaInputDeviceNative *device_native)
{
  if (libinput_device_has_capability (device_native->libinput_device,
                                      LIBINPUT_DEVICE_CAP_SWITCH) &&
      libinput_device_switch_has_switch (device_native->libinput_device,
                                         LIBINPUT_SWITCH_TABLET_MODE))
    return TRUE;

  return FALSE;
}

static gboolean
has_tablet_switch (MetaSeatImpl *seat)
{
  GSList *l;

  for (l = seat->devices; l; l = l->next)
    {
      MetaInputDeviceNative *device_native = META_INPUT_DEVICE_NATIVE (l->data);

      if (device_is_tablet_switch (device_native))
        return TRUE;
    }

  return FALSE;
}

static void
update_touch_mode (MetaSeatImpl *seat)
{
  gboolean touch_mode;

  /* No touch mode if we don't have a touchscreen, easy */
  if (!seat->has_touchscreen)
    touch_mode = FALSE;
  /* If we have a tablet mode switch, honor it being unset */
  else if (seat->has_tablet_switch && !seat->tablet_mode_switch_state)
    touch_mode = FALSE;
  /* If tablet mode is enabled, or if there is no tablet mode switch
   * (eg. kiosk machines), assume touch-mode.
   */
  else
    touch_mode = TRUE;

  if (seat->touch_mode != touch_mode)
    {
      seat->touch_mode = touch_mode;
      g_signal_emit (seat, signals[TOUCH_MODE], 0, touch_mode);
    }
}

static ClutterInputDevice *
evdev_add_device (MetaSeatImpl           *seat,
                  struct libinput_device *libinput_device)
{
  ClutterInputDeviceType type;
  ClutterInputDevice *device, *master = NULL;
  gboolean is_touchscreen, is_tablet_switch;

  device = meta_input_device_native_new (seat, libinput_device);

  seat->devices = g_slist_prepend (seat->devices, device);

  /* Clutter assumes that device types are exclusive in the
   * ClutterInputDevice API */
  type = meta_input_device_native_determine_type (libinput_device);

  if (type == CLUTTER_KEYBOARD_DEVICE)
    master = seat->core_keyboard;
  else if (type == CLUTTER_POINTER_DEVICE)
    master = seat->core_pointer;

  if (master)
    {
      _clutter_input_device_set_associated_device (device, master);
      _clutter_input_device_add_physical_device (master, device);
    }

  is_touchscreen = type == CLUTTER_TOUCHSCREEN_DEVICE;
  is_tablet_switch =
    device_is_tablet_switch (META_INPUT_DEVICE_NATIVE (device));

  seat->has_touchscreen |= is_touchscreen;
  seat->has_tablet_switch |= is_tablet_switch;

  if (is_touchscreen || is_tablet_switch)
    update_touch_mode (seat);

  return device;
}

static void
evdev_remove_device (MetaSeatImpl          *seat,
                     MetaInputDeviceNative *device_native)
{
  ClutterInputDevice *device;
  ClutterInputDeviceType device_type;
  gboolean is_touchscreen, is_tablet_switch;

  device = CLUTTER_INPUT_DEVICE (device_native);
  seat->devices = g_slist_remove (seat->devices, device);

  device_type = clutter_input_device_get_device_type (device);

  is_touchscreen = device_type == CLUTTER_TOUCHSCREEN_DEVICE;
  is_tablet_switch = device_is_tablet_switch (device_native);

  if (is_touchscreen)
    seat->has_touchscreen = has_touchscreen (seat);
  if (is_tablet_switch)
    seat->has_tablet_switch = has_tablet_switch (seat);

  if (is_touchscreen || is_tablet_switch)
    update_touch_mode (seat);

  if (seat->repeat_timer && seat->repeat_device == device)
    meta_seat_impl_clear_repeat_timer (seat);

  g_object_run_dispose (G_OBJECT (device));
  g_object_unref (device);
}

static gboolean
process_base_event (MetaSeatImpl          *seat,
                    struct libinput_event *event)
{
  ClutterInputDevice *device;
  ClutterEvent *device_event = NULL;
  struct libinput_device *libinput_device;

  switch (libinput_event_get_type (event))
    {
    case LIBINPUT_EVENT_DEVICE_ADDED:
      libinput_device = libinput_event_get_device (event);

      device = evdev_add_device (seat, libinput_device);
      device_event = clutter_event_new (CLUTTER_DEVICE_ADDED);
      clutter_event_set_device (device_event, device);
      break;

    case LIBINPUT_EVENT_DEVICE_REMOVED:
      libinput_device = libinput_event_get_device (event);

      device = libinput_device_get_user_data (libinput_device);
      device_event = clutter_event_new (CLUTTER_DEVICE_REMOVED);
      clutter_event_set_device (device_event, device);
      evdev_remove_device (seat,
                           META_INPUT_DEVICE_NATIVE (device));
      break;

    default:
      break;
    }

  if (device_event)
    {
      queue_event (seat, device_event);
      return TRUE;
    }

  return FALSE;
}

static ClutterScrollSource
translate_scroll_source (enum libinput_pointer_axis_source source)
{
  switch (source)
    {
    case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
      return CLUTTER_SCROLL_SOURCE_WHEEL;
    case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
      return CLUTTER_SCROLL_SOURCE_FINGER;
    case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
      return CLUTTER_SCROLL_SOURCE_CONTINUOUS;
    default:
      return CLUTTER_SCROLL_SOURCE_UNKNOWN;
    }
}

static ClutterInputDeviceToolType
translate_tool_type (struct libinput_tablet_tool *libinput_tool)
{
  enum libinput_tablet_tool_type tool;

  tool = libinput_tablet_tool_get_type (libinput_tool);

  switch (tool)
    {
    case LIBINPUT_TABLET_TOOL_TYPE_PEN:
      return CLUTTER_INPUT_DEVICE_TOOL_PEN;
    case LIBINPUT_TABLET_TOOL_TYPE_ERASER:
      return CLUTTER_INPUT_DEVICE_TOOL_ERASER;
    case LIBINPUT_TABLET_TOOL_TYPE_BRUSH:
      return CLUTTER_INPUT_DEVICE_TOOL_BRUSH;
    case LIBINPUT_TABLET_TOOL_TYPE_PENCIL:
      return CLUTTER_INPUT_DEVICE_TOOL_PENCIL;
    case LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH:
      return CLUTTER_INPUT_DEVICE_TOOL_AIRBRUSH;
    case LIBINPUT_TABLET_TOOL_TYPE_MOUSE:
      return CLUTTER_INPUT_DEVICE_TOOL_MOUSE;
    case LIBINPUT_TABLET_TOOL_TYPE_LENS:
      return CLUTTER_INPUT_DEVICE_TOOL_LENS;
    default:
      return CLUTTER_INPUT_DEVICE_TOOL_NONE;
    }
}

static void
input_device_update_tool (ClutterInputDevice          *input_device,
                          struct libinput_tablet_tool *libinput_tool)
{
  MetaInputDeviceNative *evdev_device = META_INPUT_DEVICE_NATIVE (input_device);
  MetaSeatImpl *seat = seat_from_device (input_device);
  ClutterInputDeviceTool *tool = NULL;
  ClutterInputDeviceToolType tool_type;
  uint64_t tool_serial;

  if (libinput_tool)
    {
      tool_serial = libinput_tablet_tool_get_serial (libinput_tool);
      tool_type = translate_tool_type (libinput_tool);
      tool = clutter_input_device_lookup_tool (input_device,
                                               tool_serial, tool_type);

      if (!tool)
        {
          tool = meta_input_device_tool_native_new (libinput_tool,
                                                    tool_serial, tool_type);
          clutter_input_device_add_tool (input_device, tool);
        }
    }

  if (evdev_device->last_tool != tool)
    {
      if (tool)
        clutter_input_device_update_from_tool (input_device, tool);

      evdev_device->last_tool = tool;
      g_signal_emit_by_name (seat->seat, "tool-changed", input_device, tool);
    }
}

static double *
translate_tablet_axes (struct libinput_event_tablet_tool *tablet_event,
                       ClutterInputDeviceTool            *tool)
{
  GArray *axes = g_array_new (FALSE, FALSE, sizeof (double));
  struct libinput_tablet_tool *libinput_tool;
  double value;

  libinput_tool = libinput_event_tablet_tool_get_tool (tablet_event);

  value = libinput_event_tablet_tool_get_x (tablet_event);
  g_array_append_val (axes, value);
  value = libinput_event_tablet_tool_get_y (tablet_event);
  g_array_append_val (axes, value);

  if (libinput_tablet_tool_has_distance (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_distance (tablet_event);
      g_array_append_val (axes, value);
    }

  if (libinput_tablet_tool_has_pressure (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_pressure (tablet_event);
      value = meta_input_device_tool_native_translate_pressure (tool, value);
      g_array_append_val (axes, value);
    }

  if (libinput_tablet_tool_has_tilt (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_tilt_x (tablet_event);
      g_array_append_val (axes, value);
      value = libinput_event_tablet_tool_get_tilt_y (tablet_event);
      g_array_append_val (axes, value);
    }

  if (libinput_tablet_tool_has_rotation (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_rotation (tablet_event);
      g_array_append_val (axes, value);
    }

  if (libinput_tablet_tool_has_slider (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_slider_position (tablet_event);
      g_array_append_val (axes, value);
    }

  if (libinput_tablet_tool_has_wheel (libinput_tool))
    {
      value = libinput_event_tablet_tool_get_wheel_delta (tablet_event);
      g_array_append_val (axes, value);
    }

  if (axes->len == 0)
    {
      g_array_free (axes, TRUE);
      return NULL;
    }
  else
    return (double *) g_array_free (axes, FALSE);
}

static void
notify_continuous_axis (MetaSeatImpl                  *seat,
                        ClutterInputDevice            *device,
                        uint64_t                       time_us,
                        ClutterScrollSource            scroll_source,
                        struct libinput_event_pointer *axis_event)
{
  double dx = 0.0, dy = 0.0;
  ClutterScrollFinishFlags finish_flags = CLUTTER_SCROLL_FINISHED_NONE;

  if (libinput_event_pointer_has_axis (axis_event,
                                       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
    {
      dx = libinput_event_pointer_get_axis_value (
          axis_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

      if (fabs (dx) < DBL_EPSILON)
        finish_flags |= CLUTTER_SCROLL_FINISHED_HORIZONTAL;
    }
  if (libinput_event_pointer_has_axis (axis_event,
                                       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
    {
      dy = libinput_event_pointer_get_axis_value (
          axis_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);

      if (fabs (dy) < DBL_EPSILON)
        finish_flags |= CLUTTER_SCROLL_FINISHED_VERTICAL;
    }

  meta_seat_impl_notify_scroll_continuous (seat, device, time_us,
                                           dx, dy,
                                           scroll_source, finish_flags);
}

static void
notify_discrete_axis (MetaSeatImpl                  *seat,
                      ClutterInputDevice            *device,
                      uint64_t                       time_us,
                      ClutterScrollSource            scroll_source,
                      struct libinput_event_pointer *axis_event)
{
  double discrete_dx = 0.0, discrete_dy = 0.0;

  if (libinput_event_pointer_has_axis (axis_event,
                                       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
    {
      discrete_dx = libinput_event_pointer_get_axis_value_discrete (
          axis_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    }
  if (libinput_event_pointer_has_axis (axis_event,
                                       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
    {
      discrete_dy = libinput_event_pointer_get_axis_value_discrete (
          axis_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    }

  meta_seat_impl_notify_discrete_scroll (seat, device,
                                         time_us,
                                         discrete_dx, discrete_dy,
                                         scroll_source);
}

static void
process_tablet_axis (MetaSeatImpl          *seat,
                     struct libinput_event *event)
{
  struct libinput_device *libinput_device = libinput_event_get_device (event);
  uint64_t time;
  double x, y, dx, dy, *axes;
  float stage_width, stage_height;
  ClutterInputDevice *device;
  struct libinput_event_tablet_tool *tablet_event =
    libinput_event_get_tablet_tool_event (event);
  MetaInputDeviceNative *evdev_device;

  device = libinput_device_get_user_data (libinput_device);
  evdev_device = META_INPUT_DEVICE_NATIVE (device);

  axes = translate_tablet_axes (tablet_event,
                                evdev_device->last_tool);
  if (!axes)
    return;

  meta_viewport_info_get_extents (seat->viewports, &stage_width, &stage_height);

  time = libinput_event_tablet_tool_get_time_usec (tablet_event);

  if (meta_input_device_native_get_mapping_mode (device) == META_INPUT_DEVICE_MAPPING_RELATIVE ||
      clutter_input_device_tool_get_tool_type (evdev_device->last_tool) == CLUTTER_INPUT_DEVICE_TOOL_MOUSE ||
      clutter_input_device_tool_get_tool_type (evdev_device->last_tool) == CLUTTER_INPUT_DEVICE_TOOL_LENS)
    {
      dx = libinput_event_tablet_tool_get_dx (tablet_event);
      dy = libinput_event_tablet_tool_get_dy (tablet_event);
      notify_relative_tool_motion (device, time, dx, dy, axes);
    }
  else
    {
      x = libinput_event_tablet_tool_get_x_transformed (tablet_event, stage_width);
      y = libinput_event_tablet_tool_get_y_transformed (tablet_event, stage_height);
      notify_absolute_motion (device, time, x, y, axes);
    }
}

static gboolean
process_device_event (MetaSeatImpl          *seat,
                      struct libinput_event *event)
{
  gboolean handled = TRUE;
  struct libinput_device *libinput_device = libinput_event_get_device(event);
  ClutterInputDevice *device;
  MetaInputDeviceNative *device_evdev;

  switch (libinput_event_get_type (event))
    {
    case LIBINPUT_EVENT_KEYBOARD_KEY:
      {
        uint32_t key, key_state, seat_key_count;
        uint64_t time_us;
        struct libinput_event_keyboard *key_event =
          libinput_event_get_keyboard_event (event);

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_keyboard_get_time_usec (key_event);
        key = libinput_event_keyboard_get_key (key_event);
        key_state = libinput_event_keyboard_get_key_state (key_event) ==
                    LIBINPUT_KEY_STATE_PRESSED;
        seat_key_count =
          libinput_event_keyboard_get_seat_key_count (key_event);

        /* Ignore key events that are not seat wide state changes. */
        if ((key_state == LIBINPUT_KEY_STATE_PRESSED &&
             seat_key_count != 1) ||
            (key_state == LIBINPUT_KEY_STATE_RELEASED &&
             seat_key_count != 0))
          {
            meta_topic (META_DEBUG_INPUT,
                        "Dropping key-%s of key 0x%x because seat-wide "
                        "key count is %d",
                        key_state == LIBINPUT_KEY_STATE_PRESSED ? "press" : "release",
                        key, seat_key_count);
            break;
          }

        meta_seat_impl_notify_key (seat_from_device (device),
                                   device,
                                   time_us, key, key_state, TRUE);

        break;
      }

    case LIBINPUT_EVENT_POINTER_MOTION:
      {
        struct libinput_event_pointer *pointer_event =
          libinput_event_get_pointer_event (event);
        uint64_t time_us;
        double dx;
        double dy;
        double dx_unaccel;
        double dy_unaccel;

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_pointer_get_time_usec (pointer_event);
        dx = libinput_event_pointer_get_dx (pointer_event);
        dy = libinput_event_pointer_get_dy (pointer_event);
        dx_unaccel = libinput_event_pointer_get_dx_unaccelerated (pointer_event);
        dy_unaccel = libinput_event_pointer_get_dy_unaccelerated (pointer_event);

        meta_seat_impl_notify_relative_motion (seat_from_device (device),
                                               device,
                                               time_us,
                                               dx, dy,
                                               dx_unaccel, dy_unaccel);

        break;
      }

    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
      {
        uint64_t time_us;
        double x, y;
        float stage_width, stage_height;
        struct libinput_event_pointer *motion_event =
          libinput_event_get_pointer_event (event);
        device = libinput_device_get_user_data (libinput_device);

        meta_viewport_info_get_extents (seat->viewports,
                                        &stage_width, &stage_height);

        time_us = libinput_event_pointer_get_time_usec (motion_event);
        x = libinput_event_pointer_get_absolute_x_transformed (motion_event,
                                                               stage_width);
        y = libinput_event_pointer_get_absolute_y_transformed (motion_event,
                                                               stage_height);

        meta_seat_impl_notify_absolute_motion (seat_from_device (device),
                                               device,
                                               time_us,
                                               x, y,
                                               NULL);

        break;
      }

    case LIBINPUT_EVENT_POINTER_BUTTON:
      {
        uint32_t button, button_state, seat_button_count;
        uint64_t time_us;
        struct libinput_event_pointer *button_event =
          libinput_event_get_pointer_event (event);
        device = libinput_device_get_user_data (libinput_device);

        time_us = libinput_event_pointer_get_time_usec (button_event);
        button = libinput_event_pointer_get_button (button_event);
        button_state = libinput_event_pointer_get_button_state (button_event) ==
                       LIBINPUT_BUTTON_STATE_PRESSED;
        seat_button_count =
          libinput_event_pointer_get_seat_button_count (button_event);

        /* Ignore button events that are not seat wide state changes. */
        if ((button_state == LIBINPUT_BUTTON_STATE_PRESSED &&
             seat_button_count != 1) ||
            (button_state == LIBINPUT_BUTTON_STATE_RELEASED &&
             seat_button_count != 0))
          {
            meta_topic (META_DEBUG_INPUT,
                        "Dropping button-%s of button 0x%x because seat-wide "
                        "button count is %d",
                        button_state == LIBINPUT_BUTTON_STATE_PRESSED ? "press" : "release",
                        button, seat_button_count);
            break;
          }

        meta_seat_impl_notify_button (seat_from_device (device), device,
                                      time_us, button, button_state);
        break;
      }

    case LIBINPUT_EVENT_POINTER_AXIS:
      {
        uint64_t time_us;
        enum libinput_pointer_axis_source source;
        struct libinput_event_pointer *axis_event =
          libinput_event_get_pointer_event (event);
        MetaSeatImpl *seat;
        ClutterScrollSource scroll_source;

        device = libinput_device_get_user_data (libinput_device);
        seat = seat_from_device (device);

        time_us = libinput_event_pointer_get_time_usec (axis_event);
        source = libinput_event_pointer_get_axis_source (axis_event);
        scroll_source = translate_scroll_source (source);

        /* libinput < 0.8 sent wheel click events with value 10. Since 0.8
           the value is the angle of the click in degrees. To keep
           backwards-compat with existing clients, we just send multiples of
           the click count. */

        switch (scroll_source)
          {
          case CLUTTER_SCROLL_SOURCE_WHEEL:
            notify_discrete_axis (seat, device, time_us, scroll_source,
                                  axis_event);
            break;
          case CLUTTER_SCROLL_SOURCE_FINGER:
          case CLUTTER_SCROLL_SOURCE_CONTINUOUS:
          case CLUTTER_SCROLL_SOURCE_UNKNOWN:
            notify_continuous_axis (seat, device, time_us, scroll_source,
                                    axis_event);
            break;
          }
        break;
      }

    case LIBINPUT_EVENT_TOUCH_DOWN:
      {
        int seat_slot;
        uint64_t time_us;
        double x, y;
        float stage_width, stage_height;
        MetaSeatImpl *seat;
        MetaTouchState *touch_state;
        struct libinput_event_touch *touch_event =
          libinput_event_get_touch_event (event);

        device = libinput_device_get_user_data (libinput_device);
        device_evdev = META_INPUT_DEVICE_NATIVE (device);
        seat = seat_from_device (device);

        meta_viewport_info_get_extents (seat->viewports,
                                        &stage_width, &stage_height);

        seat_slot = libinput_event_touch_get_seat_slot (touch_event);
        time_us = libinput_event_touch_get_time_usec (touch_event);
        x = libinput_event_touch_get_x_transformed (touch_event,
                                                    stage_width);
        y = libinput_event_touch_get_y_transformed (touch_event,
                                                    stage_height);

        touch_state = meta_seat_impl_acquire_touch_state (seat, seat_slot);
        touch_state->coords.x = x;
        touch_state->coords.y = y;

        meta_seat_impl_notify_touch_event (seat, device,
                                           CLUTTER_TOUCH_BEGIN,
                                           time_us,
                                           touch_state->seat_slot,
                                           touch_state->coords.x,
                                           touch_state->coords.y);
        break;
      }

    case LIBINPUT_EVENT_TOUCH_UP:
      {
        int seat_slot;
        uint64_t time_us;
        MetaSeatImpl *seat;
        MetaTouchState *touch_state;
        struct libinput_event_touch *touch_event =
          libinput_event_get_touch_event (event);

        device = libinput_device_get_user_data (libinput_device);
        device_evdev = META_INPUT_DEVICE_NATIVE (device);
        seat = seat_from_device (device);

        seat_slot = libinput_event_touch_get_seat_slot (touch_event);
        time_us = libinput_event_touch_get_time_usec (touch_event);
        touch_state = meta_seat_impl_lookup_touch_state (seat, seat_slot);
        if (!touch_state)
          break;

        meta_seat_impl_notify_touch_event (seat, device,
                                           CLUTTER_TOUCH_END, time_us,
                                           touch_state->seat_slot,
                                           touch_state->coords.x,
                                           touch_state->coords.y);
        meta_seat_impl_release_touch_state (seat, seat_slot);
        break;
      }

    case LIBINPUT_EVENT_TOUCH_MOTION:
      {
        int seat_slot;
        uint64_t time_us;
        double x, y;
        float stage_width, stage_height;
        MetaSeatImpl *seat;
        MetaTouchState *touch_state;
        struct libinput_event_touch *touch_event =
          libinput_event_get_touch_event (event);

        device = libinput_device_get_user_data (libinput_device);
        device_evdev = META_INPUT_DEVICE_NATIVE (device);
        seat = seat_from_device (device);

        meta_viewport_info_get_extents (seat->viewports,
                                        &stage_width, &stage_height);

        seat_slot = libinput_event_touch_get_seat_slot (touch_event);
        time_us = libinput_event_touch_get_time_usec (touch_event);
        x = libinput_event_touch_get_x_transformed (touch_event,
                                                    stage_width);
        y = libinput_event_touch_get_y_transformed (touch_event,
                                                    stage_height);

        touch_state = meta_seat_impl_lookup_touch_state (seat, seat_slot);
        if (!touch_state)
          break;

        touch_state->coords.x = x;
        touch_state->coords.y = y;

        meta_seat_impl_notify_touch_event (seat, device,
                                           CLUTTER_TOUCH_UPDATE,
                                           time_us,
                                           touch_state->seat_slot,
                                           touch_state->coords.x,
                                           touch_state->coords.y);
        break;
      }
    case LIBINPUT_EVENT_TOUCH_CANCEL:
      {
        int seat_slot;
        MetaTouchState *touch_state;
        uint64_t time_us;
        MetaSeatImpl *seat;
        struct libinput_event_touch *touch_event =
          libinput_event_get_touch_event (event);

        device = libinput_device_get_user_data (libinput_device);
        device_evdev = META_INPUT_DEVICE_NATIVE (device);
        seat = seat_from_device (device);
        time_us = libinput_event_touch_get_time_usec (touch_event);

        seat_slot = libinput_event_touch_get_seat_slot (touch_event);
        touch_state = meta_seat_impl_lookup_touch_state (seat, seat_slot);
        if (!touch_state)
          break;

        meta_seat_impl_notify_touch_event (touch_state->seat,
                                           CLUTTER_INPUT_DEVICE (device_evdev),
                                           CLUTTER_TOUCH_CANCEL,
                                           time_us,
                                           touch_state->seat_slot,
                                           touch_state->coords.x,
                                           touch_state->coords.y);

        meta_seat_impl_release_touch_state (seat, seat_slot);
        break;
      }
    case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
    case LIBINPUT_EVENT_GESTURE_PINCH_END:
      {
        struct libinput_event_gesture *gesture_event =
          libinput_event_get_gesture_event (event);
        ClutterTouchpadGesturePhase phase;
        uint32_t n_fingers;
        uint64_t time_us;

        if (libinput_event_get_type (event) == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN)
          phase = CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN;
        else
          phase = libinput_event_gesture_get_cancelled (gesture_event) ?
            CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL : CLUTTER_TOUCHPAD_GESTURE_PHASE_END;

        n_fingers = libinput_event_gesture_get_finger_count (gesture_event);
        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_gesture_get_time_usec (gesture_event);
        notify_pinch_gesture_event (device, phase, time_us, 0, 0, 0, 0, n_fingers);
        break;
      }
    case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
      {
        struct libinput_event_gesture *gesture_event =
          libinput_event_get_gesture_event (event);
        double angle_delta, scale, dx, dy;
        uint32_t n_fingers;
        uint64_t time_us;

        n_fingers = libinput_event_gesture_get_finger_count (gesture_event);
        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_gesture_get_time_usec (gesture_event);
        angle_delta = libinput_event_gesture_get_angle_delta (gesture_event);
        scale = libinput_event_gesture_get_scale (gesture_event);
        dx = libinput_event_gesture_get_dx (gesture_event);
        dy = libinput_event_gesture_get_dy (gesture_event);

        notify_pinch_gesture_event (device,
                                    CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE,
                                    time_us, dx, dy, angle_delta, scale, n_fingers);
        break;
      }
    case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
    case LIBINPUT_EVENT_GESTURE_SWIPE_END:
      {
        struct libinput_event_gesture *gesture_event =
          libinput_event_get_gesture_event (event);
        ClutterTouchpadGesturePhase phase;
        uint32_t n_fingers;
        uint64_t time_us;

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_gesture_get_time_usec (gesture_event);
        n_fingers = libinput_event_gesture_get_finger_count (gesture_event);

        if (libinput_event_get_type (event) == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN)
          phase = CLUTTER_TOUCHPAD_GESTURE_PHASE_BEGIN;
        else
          phase = libinput_event_gesture_get_cancelled (gesture_event) ?
            CLUTTER_TOUCHPAD_GESTURE_PHASE_CANCEL : CLUTTER_TOUCHPAD_GESTURE_PHASE_END;

        notify_swipe_gesture_event (device, phase, time_us, n_fingers, 0, 0);
        break;
      }
    case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
      {
        struct libinput_event_gesture *gesture_event =
          libinput_event_get_gesture_event (event);
        uint32_t n_fingers;
        uint64_t time_us;
        double dx, dy;

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_gesture_get_time_usec (gesture_event);
        n_fingers = libinput_event_gesture_get_finger_count (gesture_event);
        dx = libinput_event_gesture_get_dx (gesture_event);
        dy = libinput_event_gesture_get_dy (gesture_event);

        notify_swipe_gesture_event (device,
                                    CLUTTER_TOUCHPAD_GESTURE_PHASE_UPDATE,
                                    time_us, n_fingers, dx, dy);
        break;
      }
    case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
      {
        process_tablet_axis (seat, event);
        break;
      }
    case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
      {
        uint64_t time;
        struct libinput_event_tablet_tool *tablet_event =
          libinput_event_get_tablet_tool_event (event);
        struct libinput_tablet_tool *libinput_tool = NULL;
        enum libinput_tablet_tool_proximity_state state;
        gboolean in;

        state = libinput_event_tablet_tool_get_proximity_state (tablet_event);
        time = libinput_event_tablet_tool_get_time_usec (tablet_event);
        device = libinput_device_get_user_data (libinput_device);
        in = state == LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN;

        libinput_tool = libinput_event_tablet_tool_get_tool (tablet_event);

        if (in)
          input_device_update_tool (device, libinput_tool);
        notify_proximity (device, time, in);
        if (!in)
          input_device_update_tool (device, NULL);

        break;
      }
    case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
      {
        uint64_t time_us;
        uint32_t button_state;
        struct libinput_event_tablet_tool *tablet_event =
          libinput_event_get_tablet_tool_event (event);
        uint32_t tablet_button;

        process_tablet_axis (seat, event);

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_tablet_tool_get_time_usec (tablet_event);
        tablet_button = libinput_event_tablet_tool_get_button (tablet_event);

        button_state = libinput_event_tablet_tool_get_button_state (tablet_event) ==
                       LIBINPUT_BUTTON_STATE_PRESSED;

        meta_seat_impl_notify_button (seat_from_device (device), device,
                                      time_us, tablet_button, button_state);
        break;
      }
    case LIBINPUT_EVENT_TABLET_TOOL_TIP:
      {
        uint64_t time_us;
        uint32_t button_state;
        struct libinput_event_tablet_tool *tablet_event =
          libinput_event_get_tablet_tool_event (event);

        device = libinput_device_get_user_data (libinput_device);
        time_us = libinput_event_tablet_tool_get_time_usec (tablet_event);

        button_state = libinput_event_tablet_tool_get_tip_state (tablet_event) ==
                       LIBINPUT_TABLET_TOOL_TIP_DOWN;

        /* To avoid jumps on tip, notify axes before the tip down event
           but after the tip up event */
        if (button_state)
          process_tablet_axis (seat, event);

        meta_seat_impl_notify_button (seat_from_device (device), device,
                                      time_us, BTN_TOUCH, button_state);
        if (!button_state)
          process_tablet_axis (seat, event);
        break;
      }
    case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
      {
        uint64_t time;
        uint32_t button_state, button, group, mode;
        struct libinput_tablet_pad_mode_group *mode_group;
        struct libinput_event_tablet_pad *pad_event =
          libinput_event_get_tablet_pad_event (event);

        device = libinput_device_get_user_data (libinput_device);
        time = libinput_event_tablet_pad_get_time_usec (pad_event);

        mode_group = libinput_event_tablet_pad_get_mode_group (pad_event);
        group = libinput_tablet_pad_mode_group_get_index (mode_group);
        mode = libinput_event_tablet_pad_get_mode (pad_event);

        button = libinput_event_tablet_pad_get_button_number (pad_event);
        button_state = libinput_event_tablet_pad_get_button_state (pad_event) ==
                       LIBINPUT_BUTTON_STATE_PRESSED;
        notify_pad_button (device, time, button, group, mode, button_state);
        break;
      }
    case LIBINPUT_EVENT_TABLET_PAD_STRIP:
      {
        uint64_t time;
        uint32_t number, source, group, mode;
        struct libinput_tablet_pad_mode_group *mode_group;
        struct libinput_event_tablet_pad *pad_event =
          libinput_event_get_tablet_pad_event (event);
        double value;

        device = libinput_device_get_user_data (libinput_device);
        time = libinput_event_tablet_pad_get_time_usec (pad_event);
        number = libinput_event_tablet_pad_get_strip_number (pad_event);
        value = libinput_event_tablet_pad_get_strip_position (pad_event);
        source = libinput_event_tablet_pad_get_strip_source (pad_event);

        mode_group = libinput_event_tablet_pad_get_mode_group (pad_event);
        group = libinput_tablet_pad_mode_group_get_index (mode_group);
        mode = libinput_event_tablet_pad_get_mode (pad_event);

        notify_pad_strip (device, time, number, source, group, mode, value);
        break;
      }
    case LIBINPUT_EVENT_TABLET_PAD_RING:
      {
        uint64_t time;
        uint32_t number, source, group, mode;
        struct libinput_tablet_pad_mode_group *mode_group;
        struct libinput_event_tablet_pad *pad_event =
          libinput_event_get_tablet_pad_event (event);
        double angle;

        device = libinput_device_get_user_data (libinput_device);
        time = libinput_event_tablet_pad_get_time_usec (pad_event);
        number = libinput_event_tablet_pad_get_ring_number (pad_event);
        angle = libinput_event_tablet_pad_get_ring_position (pad_event);
        source = libinput_event_tablet_pad_get_ring_source (pad_event);

        mode_group = libinput_event_tablet_pad_get_mode_group (pad_event);
        group = libinput_tablet_pad_mode_group_get_index (mode_group);
        mode = libinput_event_tablet_pad_get_mode (pad_event);

        notify_pad_ring (device, time, number, source, group, mode, angle);
        break;
      }
    case LIBINPUT_EVENT_SWITCH_TOGGLE:
      {
        struct libinput_event_switch *switch_event =
          libinput_event_get_switch_event (event);
        enum libinput_switch sw =
          libinput_event_switch_get_switch (switch_event);
        enum libinput_switch_state state =
          libinput_event_switch_get_switch_state (switch_event);

        if (sw == LIBINPUT_SWITCH_TABLET_MODE)
          {
            seat->tablet_mode_switch_state = (state == LIBINPUT_SWITCH_STATE_ON);
            update_touch_mode (seat);
          }
        break;
      }
    default:
      handled = FALSE;
    }

  return handled;
}

static void
process_event (MetaSeatImpl          *seat,
               struct libinput_event *event)
{
  if (process_base_event (seat, event))
    return;
  if (process_device_event (seat, event))
    return;
}

static void
process_events (MetaSeatImpl *seat)
{
  struct libinput_event *event;

  while ((event = libinput_get_event (seat->libinput)))
    {
      process_event(seat, event);
      libinput_event_destroy(event);
    }
}

static int
open_restricted (const char *path,
                 int         flags,
                 void       *user_data)
{
  int fd;

  if (device_open_callback)
    {
      GError *error = NULL;

      fd = device_open_callback (path, flags, device_callback_data, &error);

      if (fd < 0)
        {
          g_warning ("Could not open device %s: %s", path, error->message);
          g_error_free (error);
        }
    }
  else
    {
      fd = open (path, O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          g_warning ("Could not open device %s: %s", path, strerror (errno));
        }
    }

  return fd;
}

static void
close_restricted (int   fd,
                  void *user_data)
{
  if (device_close_callback)
    device_close_callback (fd, device_callback_data);
  else
    close (fd);
}

static const struct libinput_interface libinput_interface = {
  open_restricted,
  close_restricted
};

static void
meta_seat_impl_constructed (GObject *object)
{
  MetaSeatImpl *seat = META_SEAT_IMPL (object);
  ClutterInputDevice *device;
  MetaEventSource *source;
  struct udev *udev;
  struct xkb_keymap *xkb_keymap;

  device = meta_input_device_native_new_virtual (
      seat, CLUTTER_POINTER_DEVICE,
      CLUTTER_INPUT_MODE_LOGICAL);
  seat->pointer_x = INITIAL_POINTER_X;
  seat->pointer_y = INITIAL_POINTER_Y;
  meta_input_device_native_update_coords (META_INPUT_DEVICE_NATIVE (device),
                                          seat->pointer_x, seat->pointer_y);
  seat->core_pointer = device;

  device = meta_input_device_native_new_virtual (
      seat, CLUTTER_KEYBOARD_DEVICE,
      CLUTTER_INPUT_MODE_LOGICAL);
  seat->core_keyboard = device;

  udev = udev_new ();
  if (G_UNLIKELY (udev == NULL))
    {
      g_warning ("Failed to create udev object");
      return;
    }

  seat->libinput = libinput_udev_create_context (&libinput_interface,
                                                 seat, udev);
  if (seat->libinput == NULL)
    {
      g_critical ("Failed to create the libinput object.");
      return;
    }

  if (libinput_udev_assign_seat (seat->libinput, seat->seat_id) == -1)
    {
      g_critical ("Failed to assign a seat to the libinput object.");
      libinput_unref (seat->libinput);
      seat->libinput = NULL;
      return;
    }

  udev_unref (udev);

  seat->udev_client = g_udev_client_new ((const char *[]) { "input", NULL });

  source = meta_event_source_new (seat);
  seat->event_source = source;

  seat->keymap = g_object_new (META_TYPE_KEYMAP_NATIVE, NULL);
  xkb_keymap = meta_keymap_native_get_keyboard_map (seat->keymap);

  if (xkb_keymap)
    {
      seat->xkb = xkb_state_new (xkb_keymap);

      seat->caps_lock_led =
        xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_CAPS);
      seat->num_lock_led =
        xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_NUM);
      seat->scroll_lock_led =
        xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_SCROLL);
    }

  seat->has_touchscreen = has_touchscreen (seat);
  seat->has_tablet_switch = has_tablet_switch (seat);
  update_touch_mode (seat);

  if (G_OBJECT_CLASS (meta_seat_impl_parent_class)->constructed)
    G_OBJECT_CLASS (meta_seat_impl_parent_class)->constructed (object);
}

static void
meta_seat_impl_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  MetaSeatImpl *seat_impl = META_SEAT_IMPL (object);

  switch (prop_id)
    {
    case PROP_SEAT:
      seat_impl->seat = g_value_get_object (value);
      break;
    case PROP_SEAT_ID:
      seat_impl->seat_id = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_seat_impl_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  MetaSeatImpl *seat_impl = META_SEAT_IMPL (object);

  switch (prop_id)
    {
    case PROP_SEAT:
      g_value_set_object (value, seat_impl->seat);
      break;
    case PROP_SEAT_ID:
      g_value_set_string (value, seat_impl->seat_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_seat_impl_dispose (GObject *object)
{
  MetaSeatImpl *seat = META_SEAT_IMPL (object);

  if (seat->libinput)
    {
      libinput_unref (seat->libinput);
      seat->libinput = NULL;
    }

  G_OBJECT_CLASS (meta_seat_impl_parent_class)->dispose (object);
}

static void
meta_seat_impl_finalize (GObject *object)
{
  MetaSeatImpl *seat = META_SEAT_IMPL (object);
  GSList *iter;

  for (iter = seat->devices; iter; iter = g_slist_next (iter))
    {
      ClutterInputDevice *device = iter->data;

      g_object_unref (device);
    }
  g_slist_free (seat->devices);

  if (seat->touch_states)
    g_hash_table_destroy (seat->touch_states);

  g_object_unref (seat->udev_client);

  meta_event_source_free (seat->event_source);

  xkb_state_unref (seat->xkb);

  meta_seat_impl_clear_repeat_timer (seat);

  if (seat->libinput_seat)
    libinput_seat_unref (seat->libinput_seat);

  g_list_free (seat->free_device_ids);

  g_free (seat->seat_id);

  G_OBJECT_CLASS (meta_seat_impl_parent_class)->finalize (object);
}

ClutterInputDevice *
meta_seat_impl_get_pointer (MetaSeatImpl *seat)
{
  MetaSeatImpl *seat_impl = META_SEAT_IMPL (seat);

  return seat_impl->core_pointer;
}

ClutterInputDevice *
meta_seat_impl_get_keyboard (MetaSeatImpl *seat)
{
  MetaSeatImpl *seat_impl = META_SEAT_IMPL (seat);

  return seat_impl->core_keyboard;
}

GSList *
meta_seat_impl_get_devices (MetaSeatImpl *seat)
{
  MetaSeatImpl *seat_impl = META_SEAT_IMPL (seat);

  return g_slist_copy_deep (seat_impl->devices,
                            (GCopyFunc) g_object_ref,
                            NULL);
}

MetaKeymapNative *
meta_seat_impl_get_keymap (MetaSeatImpl *seat)
{
  return g_object_ref (seat->keymap);
}

ClutterVirtualInputDevice *
meta_seat_impl_create_virtual_device (MetaSeatImpl           *seat,
                                      ClutterInputDeviceType  device_type)
{
  return g_object_new (META_TYPE_VIRTUAL_INPUT_DEVICE_NATIVE,
                       "seat", seat->seat,
                       "device-type", device_type,
                       NULL);
}

void
meta_seat_impl_warp_pointer (MetaSeatImpl *seat,
                             int           x,
                             int           y)
{
  notify_absolute_motion (seat->core_pointer, 0, x, y, NULL);
}

gboolean
meta_seat_impl_query_state (MetaSeatImpl         *seat,
                            ClutterInputDevice   *device,
                            ClutterEventSequence *sequence,
                            graphene_point_t     *coords,
                            ClutterModifierType  *modifiers)
{
  MetaInputDeviceNative *device_native = META_INPUT_DEVICE_NATIVE (device);
  MetaSeatImpl *seat_native = META_SEAT_IMPL (seat);

  if (sequence)
    {
      MetaTouchState *touch_state;
      int slot;

      slot = meta_event_native_sequence_get_slot (sequence);
      touch_state = meta_seat_impl_lookup_touch_state (seat_native, slot);
      if (!touch_state)
        return FALSE;

      if (coords)
        {
          coords->x = touch_state->coords.x;
          coords->y = touch_state->coords.y;
        }

      if (modifiers)
        *modifiers = meta_xkb_translate_modifiers (seat_native->xkb, 0);

      return TRUE;
    }
  else
    {
      if (coords)
        {
          coords->x = device_native->pointer_x;
          coords->y = device_native->pointer_y;
        }

      if (modifiers)
        {
          *modifiers = meta_xkb_translate_modifiers (seat_native->xkb,
                                                     seat_native->button_state);
        }

      return TRUE;
    }
}

static void
meta_seat_impl_class_init (MetaSeatImplClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = meta_seat_impl_constructed;
  object_class->set_property = meta_seat_impl_set_property;
  object_class->get_property = meta_seat_impl_get_property;
  object_class->dispose = meta_seat_impl_dispose;
  object_class->finalize = meta_seat_impl_finalize;

  props[PROP_SEAT] =
    g_param_spec_object ("seat",
                         "Seat",
                         "Seat",
                         META_TYPE_SEAT_NATIVE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  props[PROP_SEAT_ID] =
    g_param_spec_string ("seat-id",
                         "Seat ID",
                         "Seat ID",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY);

  signals[KBD_A11Y_FLAGS_CHANGED] =
    g_signal_new ("kbd-a11y-flags-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT, G_TYPE_UINT);
  signals[KBD_A11Y_MODS_STATE_CHANGED] =
    g_signal_new ("kbd-a11y-mods-state-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT, G_TYPE_UINT);
  signals[TOUCH_MODE] =
    g_signal_new ("touch-mode",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[BELL] =
    g_signal_new ("bell",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  signals[MODS_STATE_CHANGED] =
    g_signal_new ("mods-state-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
meta_seat_impl_init (MetaSeatImpl *seat)
{
  seat->device_id_next = INITIAL_DEVICE_ID;

  seat->repeat = TRUE;
  seat->repeat_delay = 250;     /* ms */
  seat->repeat_interval = 33;   /* ms */

  seat->barrier_manager = meta_barrier_manager_native_new ();
}

/**
 * meta_seat_impl_set_device_callbacks: (skip)
 * @open_callback: the user replacement for open()
 * @close_callback: the user replacement for close()
 * @user_data: user data for @callback
 *
 * Through this function, the application can set a custom callback
 * to be invoked when Clutter is about to open an evdev device. It can do
 * so if special handling is needed, for example to circumvent permission
 * problems.
 *
 * Setting @callback to %NULL will reset the default behavior.
 *
 * For reliable effects, this function must be called before clutter_init().
 */
void
meta_seat_impl_set_device_callbacks (MetaOpenDeviceCallback  open_callback,
                                     MetaCloseDeviceCallback close_callback,
                                     gpointer                user_data)
{
  device_open_callback = open_callback;
  device_close_callback = close_callback;
  device_callback_data = user_data;
}

void
meta_seat_impl_update_xkb_state (MetaSeatImpl *seat)
{
  xkb_mod_mask_t latched_mods;
  xkb_mod_mask_t locked_mods;
  struct xkb_keymap *xkb_keymap;

  xkb_keymap = meta_keymap_native_get_keyboard_map (seat->keymap);

  latched_mods = xkb_state_serialize_mods (seat->xkb,
                                           XKB_STATE_MODS_LATCHED);
  locked_mods = xkb_state_serialize_mods (seat->xkb,
                                          XKB_STATE_MODS_LOCKED);
  xkb_state_unref (seat->xkb);
  seat->xkb = xkb_state_new (xkb_keymap);

  xkb_state_update_mask (seat->xkb,
                         0, /* depressed */
                         latched_mods,
                         locked_mods,
                         0, 0, seat->layout_idx);

  seat->caps_lock_led = xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_CAPS);
  seat->num_lock_led = xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_NUM);
  seat->scroll_lock_led = xkb_keymap_led_get_index (xkb_keymap, XKB_LED_NAME_SCROLL);

  meta_seat_impl_sync_leds (seat);
}

int
meta_seat_impl_acquire_device_id (MetaSeatImpl *seat)
{
  GList *first;
  int next_id;

  if (seat->free_device_ids == NULL)
    {
      int i;

      /* We ran out of free ID's, so append 10 new ones. */
      for (i = 0; i < 10; i++)
        seat->free_device_ids =
          g_list_append (seat->free_device_ids,
                         GINT_TO_POINTER (seat->device_id_next++));
    }

  first = g_list_first (seat->free_device_ids);
  next_id = GPOINTER_TO_INT (first->data);
  seat->free_device_ids = g_list_delete_link (seat->free_device_ids, first);

  return next_id;
}

static int
compare_ids (gconstpointer a,
             gconstpointer b)
{
  return GPOINTER_TO_INT (a) - GPOINTER_TO_INT (b);
}

void
meta_seat_impl_release_device_id (MetaSeatImpl       *seat,
                                  ClutterInputDevice *device)
{
  int device_id;

  device_id = clutter_input_device_get_device_id (device);
  seat->free_device_ids = g_list_insert_sorted (seat->free_device_ids,
                                                GINT_TO_POINTER (device_id),
                                                compare_ids);
}

/**
 * meta_seat_impl_release_devices:
 *
 * Releases all the evdev devices that Clutter is currently managing. This api
 * is typically used when switching away from the Clutter application when
 * switching tty. The devices can be reclaimed later with a call to
 * meta_seat_impl_reclaim_devices().
 *
 * This function should only be called after clutter has been initialized.
 */
void
meta_seat_impl_release_devices (MetaSeatImpl *seat)
{
  g_return_if_fail (META_IS_SEAT_IMPL (seat));

  if (seat->released)
    {
      g_warning ("meta_seat_impl_release_devices() shouldn't be called "
                 "multiple times without a corresponding call to "
                 "meta_seat_impl_reclaim_devices() first");
      return;
    }

  libinput_suspend (seat->libinput);
  process_events (seat);

  seat->released = TRUE;
}

/**
 * meta_seat_impl_reclaim_devices:
 *
 * This causes Clutter to re-probe for evdev devices. This is must only be
 * called after a corresponding call to meta_seat_impl_release_devices()
 * was previously used to release all evdev devices. This API is typically
 * used when a clutter application using evdev has regained focus due to
 * switching ttys.
 *
 * This function should only be called after clutter has been initialized.
 */
void
meta_seat_impl_reclaim_devices (MetaSeatImpl *seat)
{
  if (!seat->released)
    {
      g_warning ("Spurious call to meta_seat_impl_reclaim_devices() without "
                 "previous call to meta_seat_impl_release_devices");
      return;
    }

  libinput_resume (seat->libinput);
  meta_seat_impl_update_xkb_state (seat);
  process_events (seat);

  seat->released = FALSE;
}

/**
 * meta_seat_impl_set_keyboard_map: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @keymap: the new keymap
 *
 * Instructs @evdev to use the speficied keyboard map. This will cause
 * the backend to drop the state and create a new one with the new
 * map. To avoid state being lost, callers should ensure that no key
 * is pressed when calling this function.
 */
void
meta_seat_impl_set_keyboard_map (MetaSeatImpl      *seat,
                                 struct xkb_keymap *xkb_keymap)
{
  MetaKeymapNative *keymap;

  g_return_if_fail (META_IS_SEAT_IMPL (seat));

  keymap = seat->keymap;
  meta_keymap_native_set_keyboard_map (keymap, xkb_keymap);

  meta_seat_impl_update_xkb_state (seat);
}

/**
 * meta_seat_impl_get_keyboard_map: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 *
 * Retrieves the #xkb_keymap in use by the evdev backend.
 *
 * Return value: the #xkb_keymap.
 */
struct xkb_keymap *
meta_seat_impl_get_keyboard_map (MetaSeatImpl *seat)
{
  g_return_val_if_fail (META_IS_SEAT_IMPL (seat), NULL);

  return xkb_state_get_keymap (seat->xkb);
}

/**
 * meta_seat_impl_set_keyboard_layout_index: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @idx: the xkb layout index to set
 *
 * Sets the xkb layout index on the backend's #xkb_state .
 */
void
meta_seat_impl_set_keyboard_layout_index (MetaSeatImpl       *seat,
                                          xkb_layout_index_t  idx)
{
  xkb_mod_mask_t depressed_mods;
  xkb_mod_mask_t latched_mods;
  xkb_mod_mask_t locked_mods;
  struct xkb_state *state;

  g_return_if_fail (META_IS_SEAT_IMPL (seat));

  state = seat->xkb;

  depressed_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_DEPRESSED);
  latched_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_LATCHED);
  locked_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_LOCKED);

  xkb_state_update_mask (state, depressed_mods, latched_mods, locked_mods, 0, 0, idx);

  seat->layout_idx = idx;
}

/**
 * meta_seat_impl_get_keyboard_layout_index: (skip)
 */
xkb_layout_index_t
meta_seat_impl_get_keyboard_layout_index (MetaSeatImpl *seat)
{
  return seat->layout_idx;
}

/**
 * meta_seat_impl_set_keyboard_numlock: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @numlock_set: TRUE to set NumLock ON, FALSE otherwise.
 *
 * Sets the NumLock state on the backend's #xkb_state .
 */
void
meta_seat_impl_set_keyboard_numlock (MetaSeatImpl *seat,
                                     gboolean      numlock_state)
{
  xkb_mod_mask_t depressed_mods;
  xkb_mod_mask_t latched_mods;
  xkb_mod_mask_t locked_mods;
  xkb_mod_mask_t group_mods;
  xkb_mod_mask_t numlock;
  struct xkb_keymap *xkb_keymap;
  MetaKeymapNative *keymap;

  g_return_if_fail (META_IS_SEAT_IMPL (seat));

  keymap = seat->keymap;
  xkb_keymap = meta_keymap_native_get_keyboard_map (keymap);

  numlock = (1 << xkb_keymap_mod_get_index (xkb_keymap, "Mod2"));

  depressed_mods = xkb_state_serialize_mods (seat->xkb, XKB_STATE_MODS_DEPRESSED);
  latched_mods = xkb_state_serialize_mods (seat->xkb, XKB_STATE_MODS_LATCHED);
  locked_mods = xkb_state_serialize_mods (seat->xkb, XKB_STATE_MODS_LOCKED);
  group_mods = xkb_state_serialize_layout (seat->xkb, XKB_STATE_LAYOUT_EFFECTIVE);

  if (numlock_state)
    locked_mods |= numlock;
  else
    locked_mods &= ~numlock;

  xkb_state_update_mask (seat->xkb,
                         depressed_mods,
                         latched_mods,
                         locked_mods,
                         0, 0,
                         group_mods);

  meta_seat_impl_sync_leds (seat);
}

/**
 * meta_seat_impl_set_keyboard_repeat:
 * @seat: the #ClutterSeat created by the evdev backend
 * @repeat: whether to enable or disable keyboard repeat events
 * @delay: the delay in ms between the hardware key press event and
 * the first synthetic event
 * @interval: the period in ms between consecutive synthetic key
 * press events
 *
 * Enables or disables sythetic key press events, allowing for initial
 * delay and interval period to be specified.
 */
void
meta_seat_impl_set_keyboard_repeat (MetaSeatImpl *seat,
                                    gboolean      repeat,
                                    uint32_t      delay,
                                    uint32_t      interval)
{
  g_return_if_fail (META_IS_SEAT_IMPL (seat));

  seat->repeat = repeat;
  seat->repeat_delay = delay;
  seat->repeat_interval = interval;
}

struct xkb_state *
meta_seat_impl_get_xkb_state (MetaSeatImpl *seat)
{
  return seat->xkb;
}

MetaBarrierManagerNative *
meta_seat_impl_get_barrier_manager (MetaSeatImpl *seat)
{
  return seat->barrier_manager;
}

void
meta_seat_impl_set_pointer_constraint (MetaSeatImpl              *seat,
                                       MetaPointerConstraintImpl *impl)
{
  if (g_set_object (&seat->pointer_constraint, impl))
    {
      if (impl)
        meta_pointer_constraint_impl_ensure_constrained (impl, seat->core_pointer);
    }
}

void
meta_seat_impl_set_viewports (MetaSeatImpl     *seat,
                              MetaViewportInfo *viewports)
{
  g_set_object (&seat->viewports, viewports);
}

MetaSeatImpl *
meta_seat_impl_new (MetaSeatNative *seat,
                    const char     *seat_id)
{
  return g_object_new (META_TYPE_SEAT_IMPL,
                       "seat", seat,
                       "seat-id", seat_id,
                       NULL);
}

void
meta_seat_impl_notify_kbd_a11y_flags_changed (MetaSeatImpl          *impl,
                                              MetaKeyboardA11yFlags  new_flags,
                                              MetaKeyboardA11yFlags  what_changed)
{
  g_signal_emit (impl, signals[KBD_A11Y_FLAGS_CHANGED], 0,
                 new_flags, what_changed);
}

void
meta_seat_impl_notify_kbd_a11y_mods_state_changed (MetaSeatImpl   *impl,
                                                   xkb_mod_mask_t  new_latched_mods,
                                                   xkb_mod_mask_t  new_locked_mods)
{
  g_signal_emit (impl, signals[KBD_A11Y_MODS_STATE_CHANGED], 0,
                 new_latched_mods, new_locked_mods);
}

void
meta_seat_impl_notify_bell (MetaSeatImpl *impl)
{
  g_signal_emit (impl, signals[BELL], 0);
}
