/* $Id$ */
/*-
 * Copyright (c) 2005 Benedikt Meurer <benny@xfce.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_FAM_H
#include <fam.h>
#endif
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <gdk/gdk.h>

#include <thunar-vfs/thunar-vfs-monitor.h>
#include <thunar-vfs/thunar-vfs-alias.h>



/* minimum timer interval (in ms) */
#define THUNAR_VFS_MONITOR_TIMER_INTERVAL (250)

/* tagging for notifications, so we can make sure that
 * (slow) FAM events don't override properly feeded events.
 */
#define THUNAR_VFS_MONITOR_TAG_FAM  (0)
#define THUNAR_VFS_MONITOR_TAG_FEED (1)



typedef struct _ThunarVfsMonitorNotification ThunarVfsMonitorNotification;



static void     thunar_vfs_monitor_class_init           (ThunarVfsMonitorClass *klass);
static void     thunar_vfs_monitor_init                 (ThunarVfsMonitor      *monitor);
static void     thunar_vfs_monitor_finalize             (GObject               *object);
static void     thunar_vfs_monitor_queue_notification   (ThunarVfsMonitor      *monitor,
                                                         gint                   id,
                                                         gint                   tag,
                                                         ThunarVfsMonitorEvent  event,
                                                         const gchar           *filename);
static gboolean thunar_vfs_monitor_notifications_timer  (gpointer               user_data);
#ifdef HAVE_LIBFAM
static void     thunar_vfs_monitor_fam_cancel           (ThunarVfsMonitor      *monitor);
static gboolean thunar_vfs_monitor_fam_watch            (GIOChannel            *channel,
                                                         GIOCondition           condition,
                                                         gpointer               user_data);
#endif



struct _ThunarVfsMonitorClass
{
  GObjectClass __parent__;
};

struct _ThunarVfsMonitor
{
  GObject __parent__;

  GMemChunk                    *handle_chunk;
  GList                        *handles;

  gint                          notifications_timer_id;
  ThunarVfsMonitorNotification *notifications;

  /* the monitor lock and cond */
  GCond                        *cond;
  GMutex                       *lock;

  /* the current handle id */
  gint                          current_id;

#ifdef HAVE_LIBFAM
  /* FAM/Gamin support */
  FAMConnection                 fc;
  gint                          fc_watch_id;
#endif
};

struct _ThunarVfsMonitorHandle
{
  ThunarVfsMonitorCallback callback;
  gpointer                 user_data;
  ThunarVfsPath           *path;
  gboolean                 directory;

  union
  {
#ifdef HAVE_LIBFAM
    FAMRequest             fr;
#endif
    gint                   id;
  };
};

struct _ThunarVfsMonitorNotification
{
  gint                          id;       /* the unique id of the handle */
  gint                          tag;      /* the notification source tag */
  gchar                        *filename; /* the name/path of the file that changed or NULL if the handle path should be used */
  ThunarVfsMonitorEvent         event;    /* the type of the event */
  ThunarVfsMonitorNotification *next;     /* the pointer to the next notification in the queue */
};



static GObjectClass *thunar_vfs_monitor_parent_class;



GType
thunar_vfs_monitor_get_type (void)
{
  static GType type = G_TYPE_INVALID;

  if (G_UNLIKELY (type == G_TYPE_INVALID))
    {
      static const GTypeInfo info =
      {
        sizeof (ThunarVfsMonitorClass),
        NULL,
        NULL,
        (GClassInitFunc) thunar_vfs_monitor_class_init,
        NULL,
        NULL,
        sizeof (ThunarVfsMonitor),
        0,
        (GInstanceInitFunc) thunar_vfs_monitor_init,
        NULL,
      };

      type = g_type_register_static (G_TYPE_OBJECT, I_("ThunarVfsMonitor"), &info, 0);
    }

  return type;
}



static void
thunar_vfs_monitor_class_init (ThunarVfsMonitorClass *klass)
{
  GObjectClass *gobject_class;

  /* determine the parent type class */
  thunar_vfs_monitor_parent_class = g_type_class_peek_parent (klass);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = thunar_vfs_monitor_finalize;
}



static void
thunar_vfs_monitor_init (ThunarVfsMonitor *monitor)
{
  /* initialize the monitor */
  monitor->notifications_timer_id = -1;
  monitor->cond = g_cond_new ();
  monitor->lock = g_mutex_new ();

#ifdef HAVE_LIBFAM
  if (FAMOpen2 (&monitor->fc, PACKAGE_NAME) == 0)
    {
      GIOChannel *channel;

      channel = g_io_channel_unix_new (FAMCONNECTION_GETFD (&monitor->fc));
      monitor->fc_watch_id = g_io_add_watch (channel, G_IO_ERR | G_IO_HUP | G_IO_IN,
                                             thunar_vfs_monitor_fam_watch, monitor);
      g_io_channel_unref (channel);

#ifdef HAVE_FAMNOEXISTS
      /* luckily gamin offers a way to avoid the FAMExists events */
      FAMNoExists (&monitor->fc);
#endif
    }
  else
    {
      monitor->fc_watch_id = -1;
    }
#endif

  /* allocate the memory chunk for the handles */
  monitor->handle_chunk = g_mem_chunk_create (ThunarVfsMonitorHandle, 64, G_ALLOC_AND_FREE);
}



static void
thunar_vfs_monitor_finalize (GObject *object)
{
  ThunarVfsMonitorNotification *notification;
  ThunarVfsMonitor             *monitor = THUNAR_VFS_MONITOR (object);
  GList                        *lp;

#ifdef HAVE_LIBFAM
  if (monitor->fc_watch_id >= 0)
    thunar_vfs_monitor_fam_cancel (monitor);
#endif

  /* drop the notifications timer source */
  if (G_UNLIKELY (monitor->notifications_timer_id >= 0))
    g_source_remove (monitor->notifications_timer_id);

  /* drop all pending notifications */
  while (monitor->notifications != NULL)
    {
      notification = monitor->notifications;
      monitor->notifications = notification->next;
      g_free (notification);
    }

  /* drop all handles */
  for (lp = monitor->handles; lp != NULL; lp = lp->next)
    thunar_vfs_path_unref (((ThunarVfsMonitorHandle *) lp->data)->path);
  g_list_free (monitor->handles);

  /* release the memory chunk */
  g_mem_chunk_destroy (monitor->handle_chunk);

  /* release the monitor lock */
  g_mutex_free (monitor->lock);
  g_cond_free (monitor->cond);

  (*G_OBJECT_CLASS (thunar_vfs_monitor_parent_class)->finalize) (object);
}



static void
thunar_vfs_monitor_queue_notification (ThunarVfsMonitor     *monitor,
                                       gint                  id,
                                       gint                  tag,
                                       ThunarVfsMonitorEvent event,
                                       const gchar          *filename)
{
  ThunarVfsMonitorNotification *notification;
  gint                          length;

  g_return_if_fail (THUNAR_VFS_IS_MONITOR (monitor));
  g_return_if_fail (id > 0 && id <= monitor->current_id);

  /* check if we already have a matching notification */
  for (notification = monitor->notifications; notification != NULL; notification = notification->next)
    if (notification->id == id && strcmp (filename, notification->filename) == 0)
      {
        if (tag >= notification->tag)
          {
            notification->tag = tag;
            notification->event = event;
          }
        return;
      }

  /* allocate a new notification */
  if (G_LIKELY (filename != NULL))
    {
      length = strlen (filename);
      notification = g_malloc (sizeof (ThunarVfsMonitorNotification) + length + 1);
      notification->filename = ((gchar *) notification) + sizeof (ThunarVfsMonitorNotification);
      memcpy (notification->filename, filename, length + 1);
    }
  else
    {
      notification = g_new (ThunarVfsMonitorNotification, 1);
      notification->filename = NULL;
    }

  /* prepend the notification to the queue */
  notification->id = id;
  notification->tag = tag;
  notification->next = monitor->notifications;
  notification->event = event;
  monitor->notifications = notification;

  /* schedule the notification timer if not already active */
  if (G_UNLIKELY (monitor->notifications_timer_id < 0))
    monitor->notifications_timer_id = g_timeout_add (THUNAR_VFS_MONITOR_TIMER_INTERVAL, thunar_vfs_monitor_notifications_timer, monitor);
}



static gboolean
thunar_vfs_monitor_notifications_timer (gpointer user_data)
{
  ThunarVfsMonitorNotification *notification;
  ThunarVfsMonitorHandle       *handle;
  ThunarVfsMonitor             *monitor = THUNAR_VFS_MONITOR (user_data);
  ThunarVfsPath                *path;
  GList                        *lp;

  /* take an additional reference on the monitor, * so we don't accidently
   * release the monitor while processing the notifications.
   */
  g_object_ref (G_OBJECT (monitor));

  /* aquire the lock on the monitor */
  g_mutex_lock (monitor->lock);

  /* reset the timer id */
  monitor->notifications_timer_id = -1;

  /* process all pending notifications */
  while (monitor->notifications != NULL)
    {
      /* grab the first notification from the queue */
      notification = monitor->notifications;
      monitor->notifications = notification->next;

      /* lookup the handle for the current notification */
      for (lp = monitor->handles; lp != NULL; lp = lp->next)
        if (((ThunarVfsMonitorHandle *) lp->data)->id == notification->id)
          break;

      /* check if there's a valid handle */
      if (G_LIKELY (lp != NULL))
        {
          /* grab the handle pointer */
          handle = lp->data;

          /* determine the event path for the notification */
          if (G_UNLIKELY (notification->filename == NULL))
            path = thunar_vfs_path_ref (handle->path);
          else if (G_UNLIKELY (*notification->filename != '/'))
            path = thunar_vfs_path_relative (handle->path, notification->filename);
          else
            path = thunar_vfs_path_new (notification->filename, NULL);

          /* invoke the callback (w/o the monitor lock) */
          GDK_THREADS_ENTER ();
          g_mutex_unlock (monitor->lock);
          (*handle->callback) (monitor, handle, notification->event, handle->path, path, handle->user_data);
          g_mutex_lock (monitor->lock);
          GDK_THREADS_LEAVE ();

          /* cleanup */
          thunar_vfs_path_unref (path);
        }

      /* release the current notification */
      g_free (notification);
    }

  /* notify all waiting parties */
  g_cond_broadcast (monitor->cond);

  /* release the lock on the monitor */
  g_mutex_unlock (monitor->lock);

  /* drop the additional reference on the monitor */
  g_object_unref (G_OBJECT (monitor));

  /* drop the timer source */
  return FALSE;
}



#ifdef HAVE_LIBFAM
static void
thunar_vfs_monitor_fam_cancel (ThunarVfsMonitor *monitor)
{
  g_return_if_fail (THUNAR_VFS_IS_MONITOR (monitor));
  g_return_if_fail (monitor->fc_watch_id >= 0);

  /* close the FAM connection */
  FAMClose (&monitor->fc);

  /* remove the I/O watch */
  g_source_remove (monitor->fc_watch_id);
  monitor->fc_watch_id = -1;
}



static gboolean
thunar_vfs_monitor_fam_watch (GIOChannel  *channel,
                              GIOCondition condition,
                              gpointer     user_data)
{
  ThunarVfsMonitorEvent event;
  ThunarVfsMonitor     *monitor = THUNAR_VFS_MONITOR (user_data);
  FAMEvent              fe;

  /* check for an error on the FAM connection */
  if (G_UNLIKELY ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0))
    {
error:
      /* terminate the FAM connection */
      thunar_vfs_monitor_fam_cancel (monitor);

      /* thats it, no more FAM */
      return FALSE;
    }

  /* process all pending FAM events */
  while (FAMPending (&monitor->fc))
    {
      /* query the next pending event */
      if (G_UNLIKELY (FAMNextEvent (&monitor->fc, &fe) < 0))
        goto error;

      /* translate the event code */
      switch (fe.code)
        {
        case FAMChanged:
          event = THUNAR_VFS_MONITOR_EVENT_CHANGED;
          break;

        case FAMCreated:
          event = THUNAR_VFS_MONITOR_EVENT_CREATED;
          break;

        case FAMDeleted:
          event = THUNAR_VFS_MONITOR_EVENT_DELETED;
          break;

        default:
          /* ignore all other events */
          continue;
        }

      /* schedule a notification for the monitor */
      g_mutex_lock (monitor->lock);
      thunar_vfs_monitor_queue_notification (monitor, fe.fr.reqnum, THUNAR_VFS_MONITOR_TAG_FAM, event, fe.filename);
      g_mutex_unlock (monitor->lock);
    }

  return TRUE;
}
#endif



/**
 * thunar_vfs_monitor_get_default:
 *
 * Returns the shared #ThunarVfsMonitor instance. The caller
 * is responsible to call g_object_unref() on the returned
 * object when no longer needed.
 *
 * Return value: a reference to the shared #ThunarVfsMonitor
 *               instance.
 **/
ThunarVfsMonitor*
thunar_vfs_monitor_get_default (void)
{
  static ThunarVfsMonitor *monitor = NULL;

  if (G_UNLIKELY (monitor == NULL))
    {
      monitor = g_object_new (THUNAR_VFS_TYPE_MONITOR, NULL);
      g_object_add_weak_pointer (G_OBJECT (monitor), (gpointer) &monitor);
    }
  else
    {
      g_object_ref (G_OBJECT (monitor));
    }

  return monitor;
}



/**
 * thunar_vfs_monitor_add_directory:
 * @monitor   : a #ThunarVfsMonitor.
 * @path      : the #ThunarVfsPath of the directory that should be watched.
 * @callback  : the callback function to invoke.
 * @user_data : additional data to pass to @callback.
 *
 * Return value: the #ThunarVfsMonitorHandle for the new watch.
 **/
ThunarVfsMonitorHandle*
thunar_vfs_monitor_add_directory (ThunarVfsMonitor        *monitor,
                                  ThunarVfsPath           *path,
                                  ThunarVfsMonitorCallback callback,
                                  gpointer                 user_data)
{
  ThunarVfsMonitorHandle *handle;
#ifdef HAVE_LIBFAM
  gchar                  *absolute_path;
#endif

  g_return_val_if_fail (THUNAR_VFS_IS_MONITOR (monitor), NULL);
  g_return_val_if_fail (callback != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  /* acquire the monitor lock */
  g_mutex_lock (monitor->lock);

  /* allocate a new handle */
  handle = g_chunk_new (ThunarVfsMonitorHandle, monitor->handle_chunk);
  handle->path = thunar_vfs_path_ref (path);
  handle->callback = callback;
  handle->user_data = user_data;
  handle->directory = TRUE;
  handle->id = ++monitor->current_id;

#ifdef HAVE_LIBFAM
  if (G_LIKELY (monitor->fc_watch_id >= 0))
    {
      /* schedule the watch on the FAM daemon */
      absolute_path = thunar_vfs_path_dup_string (path);
      if (FAMMonitorDirectory2 (&monitor->fc, absolute_path, &handle->fr) < 0)
        thunar_vfs_monitor_fam_cancel (monitor);
      g_free (absolute_path);
    }
#endif

  /* add the handle to the monitor */
  monitor->handles = g_list_prepend (monitor->handles, handle);

  /* release the monitor lock */
  g_mutex_unlock (monitor->lock);

  return handle;
}



/**
 * thunar_vfs_monitor_add_file:
 * @monitor   : a #ThunarVfsMonitor.
 * @path      : the #ThunarVfsPath of the file that should be watched.
 * @callback  : the callback function to invoke.
 * @user_data : additional data to pass to @callback.
 *
 * Return value: the #ThunarVfsMonitorHandle for the new watch.
 **/
ThunarVfsMonitorHandle*
thunar_vfs_monitor_add_file (ThunarVfsMonitor        *monitor,
                             ThunarVfsPath           *path,
                             ThunarVfsMonitorCallback callback,
                             gpointer                 user_data)
{
  ThunarVfsMonitorHandle *handle;
#ifdef HAVE_LIBFAM
  gchar                  *absolute_path;
#endif

  g_return_val_if_fail (THUNAR_VFS_IS_MONITOR (monitor), NULL);
  g_return_val_if_fail (callback != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  /* acquire the monitor lock */
  g_mutex_lock (monitor->lock);

  /* allocate a new handle */
  handle = g_chunk_new (ThunarVfsMonitorHandle, monitor->handle_chunk);
  handle->path = thunar_vfs_path_ref (path);
  handle->callback = callback;
  handle->user_data = user_data;
  handle->directory = FALSE;
  handle->id = ++monitor->current_id;

#ifdef HAVE_LIBFAM
  if (G_LIKELY (monitor->fc_watch_id >= 0))
    {
      /* schedule the watch on the FAM daemon */
      absolute_path = thunar_vfs_path_dup_string (path);
      if (FAMMonitorFile2 (&monitor->fc, absolute_path, &handle->fr) < 0)
        thunar_vfs_monitor_fam_cancel (monitor);
      g_free (absolute_path);
    }
#endif

  /* add the handle to the monitor */
  monitor->handles = g_list_prepend (monitor->handles, handle);

  /* release the monitor lock */
  g_mutex_unlock (monitor->lock);

  return handle;
}



/**
 * thunar_vfs_monitor_remove:
 * @monitor : a #ThunarVfsMonitor.
 * @handle  : a valid #ThunarVfsMonitorHandle for @monitor.
 *
 * Removes @handle from @monitor.
 **/
void
thunar_vfs_monitor_remove (ThunarVfsMonitor       *monitor,
                           ThunarVfsMonitorHandle *handle)
{
  g_return_if_fail (THUNAR_VFS_IS_MONITOR (monitor));
  g_return_if_fail (g_list_find (monitor->handles, handle) != NULL);

  /* acquire the monitor lock */
  g_mutex_lock (monitor->lock);

#ifdef HAVE_LIBFAM
  /* drop the FAM request from the daemon */
  if (G_LIKELY (monitor->fc_watch_id >= 0))
    {
      if (FAMCancelMonitor (&monitor->fc, &handle->fr) < 0)
        thunar_vfs_monitor_fam_cancel (monitor);
    }
#endif

  /* unlink the handle */
  monitor->handles = g_list_remove (monitor->handles, handle);

  /* free the handle */
  thunar_vfs_path_unref (handle->path);
  g_mem_chunk_free (monitor->handle_chunk, handle);

  /* release the monitor lock */
  g_mutex_unlock (monitor->lock);
}



/**
 * thunar_vfs_monitor_feed:
 * @monitor : a #ThunarVfsMonitor.
 * @event   : the #ThunarVfsMonitorEvent that should be emulated.
 * @path    : the #ThunarVfsPath on which @event took place.
 *
 * Explicitly injects the given @event into @monitor<!---->s event
 * processing logic.
 **/
void
thunar_vfs_monitor_feed (ThunarVfsMonitor     *monitor,
                         ThunarVfsMonitorEvent event,
                         ThunarVfsPath        *path)
{
  ThunarVfsMonitorHandle *handle;
  ThunarVfsPath          *parent;
  GList                  *lp;

  g_return_if_fail (THUNAR_VFS_IS_MONITOR (monitor));
  g_return_if_fail (event == THUNAR_VFS_MONITOR_EVENT_CHANGED
                 || event == THUNAR_VFS_MONITOR_EVENT_CREATED
                 || event == THUNAR_VFS_MONITOR_EVENT_DELETED);

  /* acquire the lock on the monitor */
  g_mutex_lock (monitor->lock);

  /* schedule notifications for all handles affected directly by this event */
  for (lp = monitor->handles; lp != NULL; lp = lp->next)
    {
      handle = (ThunarVfsMonitorHandle *) lp->data;
      if (thunar_vfs_path_equal (handle->path, path))
        thunar_vfs_monitor_queue_notification (monitor, handle->id, THUNAR_VFS_MONITOR_TAG_FEED, event, NULL);
    }

  /* schedule notifications for all directory handles affected indirectly */
  if (G_LIKELY (!thunar_vfs_path_is_root (path)))
    {
      parent = thunar_vfs_path_get_parent (path);
      for (lp = monitor->handles; lp != NULL; lp = lp->next)
        {
          handle = (ThunarVfsMonitorHandle *) lp->data;
          if (handle->directory && thunar_vfs_path_equal (handle->path, parent))
            thunar_vfs_monitor_queue_notification (monitor, handle->id, THUNAR_VFS_MONITOR_TAG_FEED, event, thunar_vfs_path_get_name (path));
        }
    }

  /* release the lock on the monitor */
  g_mutex_unlock (monitor->lock);
}



/**
 * thunar_vfs_monitor_wait:
 * @monitor : a #ThunarVfsMonitor.
 *
 * Suspends the execution of the current thread until the
 * @monitor has processed all currently pending events. The
 * calling thread must own a reference on the @monitor!
 *
 * This method should never be called from the main thread
 * or you'll lock up your application!!
 **/
void
thunar_vfs_monitor_wait (ThunarVfsMonitor *monitor)
{
  static const GTimeVal tv = { 2, 0 };

  g_return_if_fail (THUNAR_VFS_IS_MONITOR (monitor));

  g_mutex_lock (monitor->lock);
  while (g_atomic_int_get (&monitor->notifications_timer_id) >= 0)
    g_cond_timed_wait (monitor->cond, monitor->lock, (GTimeVal *) &tv);
  g_mutex_unlock (monitor->lock);
}



#define __THUNAR_VFS_MONITOR_C__
#include <thunar-vfs/thunar-vfs-aliasdef.c>
