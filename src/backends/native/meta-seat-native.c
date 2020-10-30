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
 */

#include "config.h"

#include "backends/native/meta-seat-native.h"

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

enum
{
  PROP_0,
  PROP_SEAT_ID,
  N_PROPS,

  /* This property is overridden */
  PROP_TOUCH_MODE,
};

static GParamSpec *props[N_PROPS] = { NULL };

G_DEFINE_TYPE (MetaSeatNative, meta_seat_native, CLUTTER_TYPE_SEAT)

static gboolean
meta_seat_native_handle_event_post (ClutterSeat        *seat,
                                    const ClutterEvent *event)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);
  ClutterInputDevice *device = clutter_event_get_source_device (event);
  ClutterEventType event_type = event->type;

  if (event_type == CLUTTER_PROXIMITY_IN)
    {
      MetaCursorRendererNative *renderer;

      if (!seat_native->tablet_cursors)
        {
          seat_native->tablet_cursors = g_hash_table_new_full (NULL, NULL, NULL,
                                                               g_object_unref);
        }

      renderer = meta_cursor_renderer_native_new (meta_get_backend (), device);
      g_hash_table_insert (seat_native->tablet_cursors, device, renderer);
      return TRUE;
    }
  else if (event_type == CLUTTER_PROXIMITY_OUT)
    {
      if (seat_native->tablet_cursors)
        g_hash_table_remove (seat_native->tablet_cursors, device);
      return TRUE;
    }
  else if (event_type == CLUTTER_DEVICE_ADDED)
    {
      if (clutter_input_device_get_device_mode (device) != CLUTTER_INPUT_MODE_LOGICAL)
        seat_native->devices = g_list_prepend (seat_native->devices, g_object_ref (device));
    }
  else if (event_type == CLUTTER_DEVICE_REMOVED)
    {
      GList *l = g_list_find (seat_native->devices, device);

      if (l)
        {
          seat_native->devices = g_list_delete_link (seat_native->devices, l);
          g_object_unref (device);
        }
    }

  return FALSE;
}

static void
proxy_kbd_a11y_flags_changed (MetaSeatImpl          *impl,
                              MetaKeyboardA11yFlags  new_flags,
                              MetaKeyboardA11yFlags  what_changed,
                              MetaSeatNative        *seat_native)
{
  g_signal_emit_by_name (seat_native,
                         "kbd-a11y-flags-changed",
                         new_flags, what_changed);
}

static void
proxy_kbd_a11y_mods_state_changed (MetaSeatImpl   *impl,
                                   xkb_mod_mask_t  new_latched_mods,
                                   xkb_mod_mask_t  new_locked_mods,
                                   MetaSeatNative *seat_native)
{
  g_signal_emit_by_name (seat_native,
                         "kbd-a11y-mods-state-changed",
                         new_latched_mods,
                         new_locked_mods);
}

static void
meta_seat_native_constructed (GObject *object)
{
  MetaSeatNative *seat = META_SEAT_NATIVE (object);

  seat->impl = meta_seat_impl_new (seat, seat->seat_id);
  g_signal_connect (seat->impl, "kbd-a11y-flags-changed",
                    G_CALLBACK (proxy_kbd_a11y_flags_changed), seat);
  g_signal_connect (seat->impl, "kbd-a11y-mods-state-changed",
                    G_CALLBACK (proxy_kbd_a11y_mods_state_changed), seat);

  seat->core_pointer = meta_seat_impl_get_pointer (seat->impl);
  seat->core_keyboard = meta_seat_impl_get_keyboard (seat->impl);
  seat->kms_cursor_renderer =
    meta_kms_cursor_renderer_new (meta_get_backend ());

  if (G_OBJECT_CLASS (meta_seat_native_parent_class)->constructed)
    G_OBJECT_CLASS (meta_seat_native_parent_class)->constructed (object);
}

static void
meta_seat_native_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (object);

  switch (prop_id)
    {
    case PROP_SEAT_ID:
      seat_native->seat_id = g_value_dup_string (value);
      break;
    case PROP_TOUCH_MODE:
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_seat_native_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (object);

  switch (prop_id)
    {
    case PROP_SEAT_ID:
      g_value_set_string (value, seat_native->seat_id);
      break;
    case PROP_TOUCH_MODE:
      g_value_set_boolean (value, seat_native->impl->touch_mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_seat_native_finalize (GObject *object)
{
  MetaSeatNative *seat = META_SEAT_NATIVE (object);
  GList *iter;

  g_clear_object (&seat->core_pointer);
  g_clear_object (&seat->core_keyboard);
  g_clear_object (&seat->impl);

  for (iter = seat->devices; iter; iter = g_list_next (iter))
    {
      ClutterInputDevice *device = iter->data;

      g_object_unref (device);
    }
  g_list_free (seat->devices);

  g_hash_table_destroy (seat->reserved_virtual_slots);

  g_clear_pointer (&seat->tablet_cursors, g_hash_table_unref);
  g_object_unref (seat->cursor_renderer);

  g_free (seat->seat_id);

  G_OBJECT_CLASS (meta_seat_native_parent_class)->finalize (object);
}

static ClutterInputDevice *
meta_seat_native_get_pointer (ClutterSeat *seat)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);

  return seat_native->core_pointer;
}

static ClutterInputDevice *
meta_seat_native_get_keyboard (ClutterSeat *seat)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);

  return seat_native->core_keyboard;
}

static const GList *
meta_seat_native_peek_devices (ClutterSeat *seat)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);

  return (const GList *) seat_native->devices;
}

static void
meta_seat_native_bell_notify (ClutterSeat *seat)
{
  MetaDisplay *display = meta_get_display ();

  meta_bell_notify (display, NULL);
}

static ClutterKeymap *
meta_seat_native_get_keymap (ClutterSeat *seat)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);

  if (!seat_native->keymap)
    seat_native->keymap = meta_seat_impl_get_keymap (seat_native->impl);

  return CLUTTER_KEYMAP (seat_native->keymap);
}

static void
meta_seat_native_copy_event_data (ClutterSeat        *seat,
                                  const ClutterEvent *src,
                                  ClutterEvent       *dest)
{
  MetaEventNative *event_evdev;

  event_evdev = _clutter_event_get_platform_data (src);
  if (event_evdev != NULL)
    _clutter_event_set_platform_data (dest, meta_event_native_copy (event_evdev));
}

static void
meta_seat_native_free_event_data (ClutterSeat  *seat,
                                  ClutterEvent *event)
{
  MetaEventNative *event_evdev;

  event_evdev = _clutter_event_get_platform_data (event);
  if (event_evdev != NULL)
    meta_event_native_free (event_evdev);
}

static guint
bump_virtual_touch_slot_base (MetaSeatNative *seat_native)
{
  while (TRUE)
    {
      if (seat_native->virtual_touch_slot_base < 0x100)
        seat_native->virtual_touch_slot_base = 0x100;

      seat_native->virtual_touch_slot_base +=
        CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS;

      if (!g_hash_table_lookup (seat_native->reserved_virtual_slots,
                                GUINT_TO_POINTER (seat_native->virtual_touch_slot_base)))
        break;
    }

  return seat_native->virtual_touch_slot_base;
}

static ClutterVirtualInputDevice *
meta_seat_native_create_virtual_device (ClutterSeat            *seat,
                                        ClutterInputDeviceType  device_type)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);
  guint slot_base;

  slot_base = bump_virtual_touch_slot_base (seat_native);
  g_hash_table_add (seat_native->reserved_virtual_slots,
                    GUINT_TO_POINTER (slot_base));

  return g_object_new (META_TYPE_VIRTUAL_INPUT_DEVICE_NATIVE,
                       "seat", seat,
                       "slot-base", slot_base,
                       "device-type", device_type,
                       NULL);
}

void
meta_seat_native_release_touch_slots (MetaSeatNative *seat,
                                      guint           base_slot)
{
  g_hash_table_remove (seat->reserved_virtual_slots,
                       GUINT_TO_POINTER (base_slot));
}

static ClutterVirtualDeviceType
meta_seat_native_get_supported_virtual_device_types (ClutterSeat *seat)
{
  return (CLUTTER_VIRTUAL_DEVICE_TYPE_KEYBOARD |
          CLUTTER_VIRTUAL_DEVICE_TYPE_POINTER |
          CLUTTER_VIRTUAL_DEVICE_TYPE_TOUCHSCREEN);
}

static void
meta_seat_native_compress_motion (ClutterSeat        *seat,
                                  ClutterEvent       *event,
                                  const ClutterEvent *to_discard)
{
  double dx, dy;
  double dx_unaccel, dy_unaccel;
  double dst_dx = 0.0, dst_dy = 0.0;
  double dst_dx_unaccel = 0.0, dst_dy_unaccel = 0.0;

  if (!meta_event_native_get_relative_motion (to_discard,
                                              &dx, &dy,
                                              &dx_unaccel, &dy_unaccel))
    return;

  meta_event_native_get_relative_motion (event,
                                         &dst_dx, &dst_dy,
                                         &dst_dx_unaccel, &dst_dy_unaccel);
  meta_event_native_set_relative_motion (event,
                                         dx + dst_dx,
                                         dy + dst_dy,
                                         dx_unaccel + dst_dx_unaccel,
                                         dy_unaccel + dst_dy_unaccel);
}

static void
meta_seat_native_warp_pointer (ClutterSeat *seat,
                               int          x,
                               int          y)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);
  MetaBackend *backend = meta_get_backend ();
  MetaCursorRenderer *cursor_renderer =
    meta_backend_get_cursor_renderer (backend);
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);

  meta_seat_impl_warp_pointer (seat_native->impl, x, y);
  meta_cursor_renderer_update_position (cursor_renderer);
  meta_cursor_tracker_update_position (cursor_tracker);
}

static gboolean
meta_seat_native_query_state (ClutterSeat          *seat,
                              ClutterInputDevice   *device,
                              ClutterEventSequence *sequence,
                              graphene_point_t     *coords,
                              ClutterModifierType  *modifiers)
{
  MetaSeatNative *seat_native = META_SEAT_NATIVE (seat);

  return meta_seat_impl_query_state (seat_native->impl, device, sequence,
                                     coords, modifiers);
}

static void
meta_seat_native_class_init (MetaSeatNativeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterSeatClass *seat_class = CLUTTER_SEAT_CLASS (klass);

  object_class->constructed = meta_seat_native_constructed;
  object_class->set_property = meta_seat_native_set_property;
  object_class->get_property = meta_seat_native_get_property;
  object_class->finalize = meta_seat_native_finalize;

  seat_class->get_pointer = meta_seat_native_get_pointer;
  seat_class->get_keyboard = meta_seat_native_get_keyboard;
  seat_class->peek_devices = meta_seat_native_peek_devices;
  seat_class->bell_notify = meta_seat_native_bell_notify;
  seat_class->get_keymap = meta_seat_native_get_keymap;
  seat_class->copy_event_data = meta_seat_native_copy_event_data;
  seat_class->free_event_data = meta_seat_native_free_event_data;
  seat_class->create_virtual_device = meta_seat_native_create_virtual_device;
  seat_class->get_supported_virtual_device_types = meta_seat_native_get_supported_virtual_device_types;
  seat_class->compress_motion = meta_seat_native_compress_motion;
  seat_class->warp_pointer = meta_seat_native_warp_pointer;
  seat_class->handle_event_post = meta_seat_native_handle_event_post;
  seat_class->query_state = meta_seat_native_query_state;

  props[PROP_SEAT_ID] =
    g_param_spec_string ("seat-id",
                         "Seat ID",
                         "Seat ID",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, N_PROPS, props);

  g_object_class_override_property (object_class, PROP_TOUCH_MODE,
                                    "touch-mode");
}

static void
meta_seat_native_init (MetaSeatNative *seat)
{
  seat->reserved_virtual_slots = g_hash_table_new (NULL, NULL);
}

/**
 * meta_seat_native_release_devices:
 *
 * Releases all the evdev devices that Clutter is currently managing. This api
 * is typically used when switching away from the Clutter application when
 * switching tty. The devices can be reclaimed later with a call to
 * meta_seat_native_reclaim_devices().
 *
 * This function should only be called after clutter has been initialized.
 */
void
meta_seat_native_release_devices (MetaSeatNative *seat)
{
  g_return_if_fail (META_IS_SEAT_NATIVE (seat));

  if (seat->released)
    {
      g_warning ("meta_seat_native_release_devices() shouldn't be called "
                 "multiple times without a corresponding call to "
                 "meta_seat_native_reclaim_devices() first");
      return;
    }

  meta_seat_impl_release_devices (seat->impl);
  seat->released = TRUE;
}

/**
 * meta_seat_native_reclaim_devices:
 *
 * This causes Clutter to re-probe for evdev devices. This is must only be
 * called after a corresponding call to meta_seat_native_release_devices()
 * was previously used to release all evdev devices. This API is typically
 * used when a clutter application using evdev has regained focus due to
 * switching ttys.
 *
 * This function should only be called after clutter has been initialized.
 */
void
meta_seat_native_reclaim_devices (MetaSeatNative *seat)
{
  if (!seat->released)
    {
      g_warning ("Spurious call to meta_seat_native_reclaim_devices() without "
                 "previous call to meta_seat_native_release_devices");
      return;
    }

  meta_seat_impl_reclaim_devices (seat->impl);
  seat->released = FALSE;
}

/**
 * meta_seat_native_set_keyboard_map: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @keymap: the new keymap
 *
 * Instructs @evdev to use the specified keyboard map. This will cause
 * the backend to drop the state and create a new one with the new
 * map. To avoid state being lost, callers should ensure that no key
 * is pressed when calling this function.
 */
void
meta_seat_native_set_keyboard_map (MetaSeatNative    *seat,
                                   struct xkb_keymap *xkb_keymap)
{
  g_return_if_fail (META_IS_SEAT_NATIVE (seat));

  meta_seat_impl_set_keyboard_map (seat->impl, xkb_keymap);
}

/**
 * meta_seat_native_get_keyboard_map: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 *
 * Retrieves the #xkb_keymap in use by the evdev backend.
 *
 * Return value: the #xkb_keymap.
 */
struct xkb_keymap *
meta_seat_native_get_keyboard_map (MetaSeatNative *seat)
{
  g_return_val_if_fail (META_IS_SEAT_NATIVE (seat), NULL);

  return meta_seat_impl_get_keyboard_map (seat->impl);
}

/**
 * meta_seat_native_set_keyboard_layout_index: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @idx: the xkb layout index to set
 *
 * Sets the xkb layout index on the backend's #xkb_state .
 */
void
meta_seat_native_set_keyboard_layout_index (MetaSeatNative     *seat,
                                            xkb_layout_index_t  idx)
{
  g_return_if_fail (META_IS_SEAT_NATIVE (seat));

  meta_seat_impl_set_keyboard_layout_index (seat->impl, idx);
}

/**
 * meta_seat_native_get_keyboard_layout_index: (skip)
 */
xkb_layout_index_t
meta_seat_native_get_keyboard_layout_index (MetaSeatNative *seat)
{
  return meta_seat_impl_get_keyboard_layout_index (seat->impl);
}

/**
 * meta_seat_native_set_keyboard_numlock: (skip)
 * @seat: the #ClutterSeat created by the evdev backend
 * @numlock_set: TRUE to set NumLock ON, FALSE otherwise.
 *
 * Sets the NumLock state on the backend's #xkb_state .
 */
void
meta_seat_native_set_keyboard_numlock (MetaSeatNative *seat,
                                       gboolean        numlock_state)
{
  meta_seat_impl_set_keyboard_numlock (seat->impl, numlock_state);
}

MetaBarrierManagerNative *
meta_seat_native_get_barrier_manager (MetaSeatNative *seat)
{
  return meta_seat_impl_get_barrier_manager (seat->impl);
}

void
meta_seat_native_set_pointer_constraint (MetaSeatNative            *seat,
                                         MetaPointerConstraintImpl *impl)
{
  meta_seat_impl_set_pointer_constraint (seat->impl, impl);
}

MetaCursorRenderer *
meta_seat_native_get_cursor_renderer (MetaSeatNative     *seat,
                                      ClutterInputDevice *device)
{
  if (device == seat->core_pointer)
    {
      if (!seat->cursor_renderer)
        {
          MetaCursorRendererNative *renderer_native;

          renderer_native =
            meta_cursor_renderer_native_new (meta_get_backend (),
                                             seat->core_pointer);
          seat->cursor_renderer = META_CURSOR_RENDERER (renderer_native);
          meta_cursor_renderer_native_set_kms_cursor_renderer (renderer_native,
                                                               seat->kms_cursor_renderer);
        }

      return seat->cursor_renderer;
    }

  if (seat->tablet_cursors &&
      clutter_input_device_get_device_type (device) == CLUTTER_TABLET_DEVICE)
    return g_hash_table_lookup (seat->tablet_cursors, device);

  return NULL;
}

void
meta_seat_native_set_viewports (MetaSeatNative   *seat,
                                MetaViewportInfo *viewports)
{
  meta_seat_impl_set_viewports (seat->impl, viewports);
}
