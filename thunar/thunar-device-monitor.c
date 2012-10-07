/*-
 * Copyright (c) 2009-2010 Jannis Pohlmann <jannis@xfce.org>
 * Copyright (c) 2012      Nick Schermer <nick@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gio/gio.h>
#ifdef HAVE_GIO_UNIX
#include <gio/gunixmounts.h>
#endif

#include <thunar/thunar-device-monitor.h>
#include <thunar/thunar-private.h>



/* signal identifiers */
enum
{
  DEVICE_ADDED,
  DEVICE_REMOVED,
  DEVICE_CHANGED,
  DEVICE_PRE_UNMOUNT,
  LAST_SIGNAL
};



static void           thunar_device_monitor_finalize               (GObject                *object);
static void           thunar_device_monitor_volume_added           (GVolumeMonitor         *volume_monitor,
                                                                    GVolume                *volume,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_volume_removed         (GVolumeMonitor         *volume_monitor,
                                                                    GVolume                *volume,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_volume_changed         (GVolumeMonitor         *volume_monitor,
                                                                    GVolume                *volume,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_mount_added            (GVolumeMonitor         *volume_monitor,
                                                                    GMount                 *mount,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_mount_removed          (GVolumeMonitor         *volume_monitor,
                                                                    GMount                 *mount,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_mount_changed          (GVolumeMonitor         *volume_monitor,
                                                                    GMount                 *mount,
                                                                    ThunarDeviceMonitor    *monitor);
static void           thunar_device_monitor_mount_pre_unmount      (GVolumeMonitor         *volume_monitor,
                                                                    GMount                 *mount,
                                                                    ThunarDeviceMonitor    *monitor);



struct _ThunarDeviceMonitorClass
{
  GObjectClass __parent__;

  /* signals */
  void (*device_added)       (ThunarDeviceMonitor *monitor,
                              ThunarDevice        *device);
  void (*device_removed)     (ThunarDeviceMonitor *monitor,
                              ThunarDevice        *device);
  void (*device_changed)     (ThunarDeviceMonitor *monitor,
                              ThunarDevice        *device);
  void (*device_pre_unmount) (ThunarDeviceMonitor *monitor,
                              ThunarDevice        *device);
};

struct _ThunarDeviceMonitor
{
  GObject __parent__;

  GVolumeMonitor *volume_monitor;

  /* GVolume/GMount -> ThunarDevice */
  GHashTable     *devices;

  GList          *hidden_volumes;
};



static guint device_monitor_signals[LAST_SIGNAL];



G_DEFINE_TYPE (ThunarDeviceMonitor, thunar_device_monitor, G_TYPE_OBJECT)



static void
thunar_device_monitor_class_init (ThunarDeviceMonitorClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = thunar_device_monitor_finalize;

  device_monitor_signals[DEVICE_ADDED] =
      g_signal_new (I_("device-added"),
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (ThunarDeviceMonitorClass, device_added),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__OBJECT,
                    G_TYPE_NONE, 1, G_TYPE_OBJECT);

  device_monitor_signals[DEVICE_REMOVED] =
      g_signal_new (I_("device-removed"),
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (ThunarDeviceMonitorClass, device_removed),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__OBJECT,
                    G_TYPE_NONE, 1, G_TYPE_OBJECT);

  device_monitor_signals[DEVICE_CHANGED] =
      g_signal_new (I_("device-changed"),
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (ThunarDeviceMonitorClass, device_changed),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__OBJECT,
                    G_TYPE_NONE, 1, G_TYPE_OBJECT);

  device_monitor_signals[DEVICE_PRE_UNMOUNT] =
      g_signal_new (I_("device-pre-unmount"),
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (ThunarDeviceMonitorClass, device_pre_unmount),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__OBJECT,
                    G_TYPE_NONE, 1, G_TYPE_OBJECT);
}



static void
thunar_device_monitor_init (ThunarDeviceMonitor *monitor)
{
  GList *list;
  GList *lp;

  /* table for GVolume/GMount (key) -> ThunarDevice (value) */
  monitor->devices = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, g_object_unref);

  /* gio volume monitor */
  monitor->volume_monitor = g_volume_monitor_get ();

  /* load all volumes */
  list = g_volume_monitor_get_volumes (monitor->volume_monitor);
  for (lp = list; lp != NULL; lp = lp->next)
    {
      thunar_device_monitor_volume_added (monitor->volume_monitor, lp->data, monitor);
      g_object_unref (G_OBJECT (lp->data));
    }
  g_list_free (list);

  /* load all mount */
  list = g_volume_monitor_get_mounts (monitor->volume_monitor);
  for (lp = list; lp != NULL; lp = lp->next)
    {
      thunar_device_monitor_mount_added (monitor->volume_monitor, lp->data, monitor);
      g_object_unref (G_OBJECT (lp->data));
    }
  g_list_free (list);

  /* watch changes */
  g_signal_connect (monitor->volume_monitor, "volume-added", G_CALLBACK (thunar_device_monitor_volume_added), monitor);
  g_signal_connect (monitor->volume_monitor, "volume-removed", G_CALLBACK (thunar_device_monitor_volume_removed), monitor);
  g_signal_connect (monitor->volume_monitor, "volume-changed", G_CALLBACK (thunar_device_monitor_volume_changed), monitor);
  g_signal_connect (monitor->volume_monitor, "mount-added", G_CALLBACK (thunar_device_monitor_mount_added), monitor);
  g_signal_connect (monitor->volume_monitor, "mount-removed", G_CALLBACK (thunar_device_monitor_mount_removed), monitor);
  g_signal_connect (monitor->volume_monitor, "mount-changed", G_CALLBACK (thunar_device_monitor_mount_changed), monitor);
  g_signal_connect (monitor->volume_monitor, "mount-pre-unmount", G_CALLBACK (thunar_device_monitor_mount_pre_unmount), monitor);
}



static void
thunar_device_monitor_finalize (GObject *object)
{
  ThunarDeviceMonitor *monitor = THUNAR_DEVICE_MONITOR (object);

  /* detatch from the monitor */
  g_signal_handlers_disconnect_matched (monitor->volume_monitor, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, monitor);
  g_object_unref (monitor->volume_monitor);

  /* clear list of devices */
  g_hash_table_destroy (monitor->devices);

  /* clear list of hidden volumes */
  g_list_free_full (monitor->hidden_volumes, g_object_unref);

  (*G_OBJECT_CLASS (thunar_device_monitor_parent_class)->finalize) (object);
}



#ifdef HAVE_GIO_UNIX
static gboolean
thunar_device_monitor_mount_is_internal (GMount *mount)
{
  const gchar *point_mount_path;
  gboolean     is_internal = FALSE;
  GFile       *root;
  GList       *lp;
  GList       *mount_points;
  gchar       *mount_path;

  _thunar_return_val_if_fail (G_IS_MOUNT (mount), FALSE);

  /* determine the mount path */
  root = g_mount_get_root (mount);
  mount_path = g_file_get_path (root);
  g_object_unref (root);

  /* assume non-internal if we cannot determine the path */
  if (mount_path == NULL)
    return FALSE;

  if (g_unix_is_mount_path_system_internal (mount_path))
    {
      /* mark as internal */
      is_internal = TRUE;
    }
  else
    {
      /* get a list of all mount points */
      mount_points = g_unix_mount_points_get (NULL);

      /* search for the mount point associated with the mount entry */
      for (lp = mount_points; !is_internal && lp != NULL; lp = lp->next)
        {
          point_mount_path = g_unix_mount_point_get_mount_path (lp->data);

          /* check if this is the mount point we are looking for */
          if (g_strcmp0 (mount_path, point_mount_path) == 0)
            {
              /* mark as internal if the user cannot mount this device */
              if (!g_unix_mount_point_is_user_mountable (lp->data))
                is_internal = TRUE;
            }

          /* free the mount point, we no longer need it */
          g_unix_mount_point_free (lp->data);
        }

      /* free the mount point list */
      g_list_free (mount_points);
    }

  g_free (mount_path);

  return is_internal;
}
#endif



static gboolean
thunar_device_monitor_has_location (ThunarDeviceMonitor *monitor,
                                    GFile               *location)
{

  _thunar_return_val_if_fail (G_IS_FILE (location), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor), FALSE);

  return FALSE;
}



static gboolean
thunar_device_monitor_mount_is_hidden (GMount              *mount,
                                       ThunarDeviceMonitor *monitor)
{
  GVolume *volume;
  GFile   *location;

  _thunar_return_val_if_fail (G_IS_MOUNT (mount), TRUE);
  _thunar_return_val_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor), TRUE);

  /* never show shadowed mounts */
  if (g_mount_is_shadowed (mount))
    return TRUE;

  /* skip mounts with a volume, we prefer a volume as device */
  volume = g_mount_get_volume (mount);
  if (volume != NULL)
    {
      g_object_unref (volume);
      return TRUE;
    }

  location = g_mount_get_root (mount);

  /* skip ghoto locations, since those also have a volume
   * and igore locations already in the device list */
  if (g_file_has_uri_scheme (location, "gphoto2")
      || thunar_device_monitor_has_location (monitor, location))
    {
      g_object_unref (location);
      return TRUE;
    }

  return FALSE;
}



static gboolean
thunar_device_monitor_volume_is_visible (GVolume *volume)
{
  gboolean         can_eject = FALSE;
  gboolean         can_mount = FALSE;
  gboolean         can_unmount = FALSE;
  gboolean         is_removable = FALSE;
  gboolean         is_internal = FALSE;
  GDrive          *drive;
  GMount          *mount;

  _thunar_return_val_if_fail (G_IS_VOLUME (volume), TRUE);

  /* determine the mount for the volume (if it is mounted at all) */
  mount = g_volume_get_mount (volume);
  if (mount != NULL)
    {
#ifdef HAVE_GIO_UNIX
      is_internal = thunar_device_monitor_mount_is_internal (mount);
#endif

      /* check if the volume can be unmounted */
      can_unmount = g_mount_can_unmount (mount);

      /* release the mount */
      g_object_unref (mount);
    }

  /* don't show internal volumes */
  if (is_internal)
    return FALSE;

  /* check if the volume can be ejected */
  can_eject = g_volume_can_eject (volume);

  /* determine the drive for the volume */
  drive = g_volume_get_drive (volume);
  if (drive != NULL)
    {
      /* check if the drive media can be removed */
      is_removable = g_drive_is_media_removable (drive);

      /* release the drive */
      g_object_unref (drive);
    }

  /* determine whether the device can be mounted */
  can_mount = g_volume_can_mount (volume);

  return (can_eject || can_unmount || is_removable || can_mount);
}



static void
thunar_device_monitor_volume_added (GVolumeMonitor      *volume_monitor,
                                    GVolume             *volume,
                                    ThunarDeviceMonitor *monitor)
{
  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_VOLUME (volume));

  /* add to internal list */
  monitor->hidden_volumes = g_list_prepend (monitor->hidden_volumes, g_object_ref (volume));

  /* change visibility in changed */
  thunar_device_monitor_volume_changed (volume_monitor, volume, monitor);
}



static void
thunar_device_monitor_volume_removed (GVolumeMonitor      *volume_monitor,
                                      GVolume             *volume,
                                      ThunarDeviceMonitor *monitor)
{
  ThunarDevice *device;
  GList        *lp;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_VOLUME (volume));

  /* remove the volume */
  lp = g_list_find (monitor->hidden_volumes, volume);
  if (lp != NULL)
    {
      /* silently drop it */
      monitor->hidden_volumes = g_list_delete_link (monitor->hidden_volumes, lp);

      /* release ref from hidden list */
      g_object_unref (G_OBJECT (volume));
    }
  else
    {
      /* find device */
      device = g_hash_table_lookup (monitor->devices, volume);

      /* meh */
      _thunar_return_if_fail (THUNAR_IS_DEVICE (device));
      if (G_UNLIKELY (device == NULL))
        return;

      /* the device is not visble for the user */
      g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_REMOVED], 0, device);

      /* drop it */
      g_hash_table_remove (monitor->devices, volume);
    }
}



static void
thunar_device_monitor_volume_changed (GVolumeMonitor      *volume_monitor,
                                      GVolume             *volume,
                                      ThunarDeviceMonitor *monitor)
{
  GList        *lp;
  ThunarDevice *device;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_VOLUME (volume));

  lp = g_list_find (monitor->hidden_volumes, volume);
  if (lp != NULL)
    {
      /* check if the volume should be visible again */
      if (thunar_device_monitor_volume_is_visible (volume))
        {
          /* remove from the hidden list */
          monitor->hidden_volumes = g_list_delete_link (monitor->hidden_volumes, lp);

          /* create a new device for this volume */
          device = g_object_new (THUNAR_TYPE_DEVICE,
                                 "device", volume,
                                 "kind", THUNAR_DEVICE_KIND_VOLUME,
                                 NULL);

          /* insert to list (takes ref from hidden list) */
          g_hash_table_insert (monitor->devices, volume, device);

          /* notify */
          g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_ADDED], 0, device);
        }
    }
  else
    {
      /* find device */
      device = g_hash_table_lookup (monitor->devices, volume);

      /* meh */
      _thunar_return_if_fail (THUNAR_IS_DEVICE (device));
      if (G_UNLIKELY (device == NULL))
        return;

      if (!thunar_device_monitor_volume_is_visible (volume))
        {
          /* remove from table */
          g_hash_table_steal (monitor->devices, volume);

          /* insert volume in hidden table, take ref from table */
          monitor->hidden_volumes = g_list_prepend (monitor->hidden_volumes, volume);

          /* the device is not visble for the user */
          g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_REMOVED], 0, device);

          /* destroy device */
          g_object_unref (G_OBJECT (device));
        }
      else
        {
          /* the device changed */
          g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_CHANGED], 0, device);
        }
    }
}



static void
thunar_device_monitor_mount_added (GVolumeMonitor      *volume_monitor,
                                   GMount              *mount,
                                   ThunarDeviceMonitor *monitor)
{
  ThunarDevice     *device;
  GFile            *location;
  ThunarDeviceKind  kind = THUNAR_DEVICE_KIND_MOUNT_LOCAL;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_MOUNT (mount));

  if (!thunar_device_monitor_mount_is_hidden (mount, monitor))
    {
      location = g_mount_get_root (mount);
      if (G_LIKELY (location != NULL))
        {
          if (g_file_has_uri_scheme (location, "file")
              || g_file_has_uri_scheme (location, "archive"))
            kind = THUNAR_DEVICE_KIND_MOUNT_LOCAL;
          else
            kind = THUNAR_DEVICE_KIND_MOUNT_REMOTE;

          g_object_unref (location);
        }

      /* create a new device for this mount */
      device = g_object_new (THUNAR_TYPE_DEVICE,
                             "device", mount,
                             "kind", kind,
                             NULL);

      /* insert to list */
      g_hash_table_insert (monitor->devices, g_object_ref (mount), device);

      /* notify */
      g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_ADDED], 0, device);
    }
}



static void
thunar_device_monitor_mount_removed (GVolumeMonitor      *volume_monitor,
                                     GMount              *mount,
                                     ThunarDeviceMonitor *monitor)
{
  ThunarDevice *device;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_MOUNT (mount));

  /* check if we have a device for this mount */
  device = g_hash_table_lookup (monitor->devices, mount);
  if (device != NULL)
    {
      /* notify */
      g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_REMOVED], 0, device);

      /* drop it */
      g_hash_table_remove (monitor->devices, mount);
    }
}



static void
thunar_device_monitor_mount_changed (GVolumeMonitor      *volume_monitor,
                                     GMount              *mount,
                                     ThunarDeviceMonitor *monitor)
{
  ThunarDevice *device;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_MOUNT (mount));

  /* check if we have a device for this mount */
  device = g_hash_table_lookup (monitor->devices, mount);
  if (device != NULL)
    {
      /* notify */
      g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_CHANGED], 0, device);
    }
}



static void
thunar_device_monitor_mount_pre_unmount (GVolumeMonitor      *volume_monitor,
                                         GMount              *mount,
                                         ThunarDeviceMonitor *monitor)
{
  ThunarDevice *device;
  GVolume      *volume;

  _thunar_return_if_fail (G_IS_VOLUME_MONITOR (volume_monitor));
  _thunar_return_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor));
  _thunar_return_if_fail (monitor->volume_monitor == volume_monitor);
  _thunar_return_if_fail (G_IS_MOUNT (mount));

  /* check if we have a device for this mount */
  device = g_hash_table_lookup (monitor->devices, mount);
  if (device == NULL)
    {
      /* maybe a volume device? */
      volume = g_mount_get_volume (mount);
      if (volume != NULL)
        {
          device = g_hash_table_lookup (monitor->devices, volume);
          g_object_unref (volume);
        }
    }

  if (device != NULL)
    {
      /* notify */
      g_signal_emit (G_OBJECT (monitor), device_monitor_signals[DEVICE_PRE_UNMOUNT], 0, device);
    }
}



static void
thunar_device_monitor_list_prepend (gpointer key,
                                    gpointer value,
                                    gpointer user_data)
{
  GList **list = user_data;

  _thunar_return_if_fail (THUNAR_IS_DEVICE (value));
  *list = g_list_prepend (*list, g_object_ref (value));
}



ThunarDeviceMonitor *
thunar_device_monitor_get (void)
{
static ThunarDeviceMonitor *monitor = NULL;

  if (G_UNLIKELY (monitor == NULL))
    {
      monitor = g_object_new (THUNAR_TYPE_DEVICE_MONITOR, NULL);
      g_object_add_weak_pointer (G_OBJECT (monitor), (gpointer) &monitor);
    }
  else
    {
      g_object_ref (G_OBJECT (monitor));
    }

  return monitor;
}



GList *
thunar_device_monitor_get_devices (ThunarDeviceMonitor *monitor)
{
  GList *list = NULL;

  _thunar_return_val_if_fail (THUNAR_IS_DEVICE_MONITOR (monitor), NULL);

  g_hash_table_foreach (monitor->devices, thunar_device_monitor_list_prepend, &list);

  return list;
}
