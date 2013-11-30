/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * gsturidownloader.c:
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "gsturidownloader.h"

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT uridownloader_debug
GST_DEBUG_CATEGORY (uridownloader_debug);

static void gst_uri_downloader_finalize (GObject * object);
static void gst_uri_downloader_dispose (GObject * object);

static GstFlowReturn gst_uri_downloader_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean gst_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

#define gst_uri_downloader_parent_class parent_class
G_DEFINE_TYPE (GstUriDownloader, gst_uri_downloader, GST_TYPE_OBJECT);

static void
gst_uri_downloader_class_init (GstUriDownloaderClass * klass)
{
  GObjectClass *gobject_class;

  GST_DEBUG_CATEGORY_INIT (uridownloader_debug, "uridownloader",
      0, "URI downloader");

  gobject_class = (GObjectClass *) klass;

  gobject_class->dispose = gst_uri_downloader_dispose;
  gobject_class->finalize = gst_uri_downloader_finalize;
}

static void
gst_uri_downloader_init (GstUriDownloader * downloader)
{
  downloader->pad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (downloader->pad,
      GST_DEBUG_FUNCPTR (gst_uri_downloader_chain));
  gst_pad_set_event_function (downloader->pad,
      GST_DEBUG_FUNCPTR (gst_uri_downloader_sink_event));
  gst_pad_set_element_private (downloader->pad, downloader);

  downloader->bus = gst_bus_new ();

  g_mutex_init (&downloader->download_lock);
  g_cond_init (&downloader->cond);
}

static void
gst_uri_downloader_dispose (GObject * object)
{
  GstUriDownloader *downloader = GST_URI_DOWNLOADER (object);

  gst_uri_downloader_cancel (downloader);

  g_mutex_lock (&downloader->download_lock);
  if (downloader->urisrc != NULL) {
    gst_element_set_state (downloader->urisrc, GST_STATE_NULL);
    gst_object_unref (downloader->urisrc);
    downloader->urisrc = NULL;
  }
  g_mutex_unlock (&downloader->download_lock);

  if (downloader->bus != NULL) {
    gst_object_unref (downloader->bus);
    downloader->bus = NULL;
  }

  if (downloader->pad) {
    gst_object_unref (downloader->pad);
    downloader->pad = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_uri_downloader_finalize (GObject * object)
{
  GstUriDownloader *downloader = GST_URI_DOWNLOADER (object);

  g_mutex_clear (&downloader->download_lock);
  g_cond_clear (&downloader->cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

GstUriDownloader *
gst_uri_downloader_new (void)
{
  return g_object_new (GST_TYPE_URI_DOWNLOADER, NULL);
}

static gboolean
gst_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstUriDownloader *downloader;

  downloader = GST_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_DEBUG_OBJECT (downloader, "Got EOS on the fetcher pad");
    GST_OBJECT_LOCK (downloader);
    downloader->eos = TRUE;
    g_cond_signal (&downloader->cond);
    GST_OBJECT_UNLOCK (downloader);
  }

  gst_event_unref (event);

  return TRUE;
}

static GstBusSyncReply
gst_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data)
{
  GstUriDownloader *downloader = (GstUriDownloader *) (data);

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR ||
      GST_MESSAGE_TYPE (message) == GST_MESSAGE_WARNING) {
    GError *err = NULL;
    gchar *dbg_info = NULL;

    gst_message_parse_error (message, &err, &dbg_info);
    GST_WARNING_OBJECT (downloader,
        "Received error: %s from %s, the download will be cancelled",
        GST_OBJECT_NAME (message->src), err->message);
    GST_DEBUG ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
    g_error_free (err);
    g_free (dbg_info);

    /* remove the sync handler to avoid duplicated messages */
    gst_bus_set_sync_handler (downloader->bus, NULL, NULL, NULL);

    /* stop the download */
    g_cond_signal (&downloader->cond);
  }

  gst_message_unref (message);

  return GST_BUS_DROP;
}

static GstFlowReturn
gst_uri_downloader_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstUriDownloader *downloader;
  GstFlowReturn ret;

  downloader = GST_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  GST_LOG_OBJECT (downloader, "got %" G_GSIZE_FORMAT " bytes buffer",
      gst_buffer_get_size (buf));

  if (downloader->chain)
    ret = downloader->chain (buf, downloader->priv);
  else
    ret = GST_FLOW_OK;

  return ret;
}

static void
gst_uri_downloader_stop (GstUriDownloader * downloader)
{
  if (!downloader->urisrc)
    return;

  GST_DEBUG_OBJECT (downloader, "stopping source element %s",
      GST_ELEMENT_NAME (downloader->urisrc));

  gst_bus_set_sync_handler (downloader->bus, NULL, NULL, NULL);
  gst_pad_set_active (downloader->pad, FALSE);
  gst_bus_set_flushing (downloader->bus, TRUE);
  gst_element_set_state (downloader->urisrc, GST_STATE_READY);
}

void
gst_uri_downloader_cancel (GstUriDownloader * downloader)
{
  GST_DEBUG_OBJECT (downloader, "cancel download");
  GST_OBJECT_LOCK (downloader);
  downloader->cancelled = TRUE;
  g_cond_signal (&downloader->cond);
  GST_OBJECT_UNLOCK (downloader);
}

static gboolean
gst_uri_downloader_set_range (GstUriDownloader * downloader,
    gint64 range_start, gint64 range_end)
{
  gboolean ret = TRUE;

  g_return_val_if_fail (range_start >= 0, FALSE);
  g_return_val_if_fail (range_end >= -1, FALSE);

  if (range_start || (range_end >= 0)) {
    GstEvent *seek;

    seek = gst_event_new_seek (1.0, GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, range_start, GST_SEEK_TYPE_SET, range_end);

    ret = gst_element_send_event (downloader->urisrc, seek);
  }

  return ret;
}

static gboolean
gst_uri_downloader_set_uri (GstUriDownloader * downloader, const gchar * uri)
{
  GstPad *pad;

  if (!gst_uri_is_valid (uri))
    return FALSE;

  if (downloader->urisrc) {
    GstURIHandler *uri_handler = GST_URI_HANDLER (downloader->urisrc);

    if (gst_uri_handler_set_uri (uri_handler, uri, NULL)) {
      GST_DEBUG_OBJECT (downloader, "reusing element %s to download URI %s",
          GST_ELEMENT_NAME (downloader->urisrc), uri);
      return TRUE;
    }

    gst_element_set_state (downloader->urisrc, GST_STATE_NULL);
    gst_object_unref (downloader->urisrc);
  }

  GST_DEBUG_OBJECT (downloader, "creating source element for URI %s", uri);
  downloader->urisrc =
      gst_element_make_from_uri (GST_URI_SRC, uri, NULL, NULL);
  if (!downloader->urisrc) {
    GST_ERROR_OBJECT (downloader, "no element can handle URI %s", uri);
    return FALSE;
  }

  /* add a sync handler for the bus messages to detect errors */
  gst_element_set_bus (downloader->urisrc, downloader->bus);
  gst_bus_set_sync_handler (downloader->bus,
      gst_uri_downloader_bus_handler, downloader, NULL);

  pad = gst_element_get_static_pad (downloader->urisrc, "src");
  gst_pad_link_full (pad, downloader->pad, GST_PAD_LINK_CHECK_NOTHING);
  gst_object_unref (pad);

  return TRUE;
}

gboolean
gst_uri_downloader_stream_uri (GstUriDownloader * downloader,
    const gchar * uri, gint64 range_start, gint64 range_end,
    GstUriDownloaderChainFunction chain_func, gpointer user_data)
{
  GstStateChangeReturn ret;

  GST_INFO_OBJECT (downloader, "fetching URI %s", uri);

  g_mutex_lock (&downloader->download_lock);
  downloader->chain = chain_func;
  downloader->priv = user_data;
  downloader->eos = FALSE;

  gst_pad_set_active (downloader->pad, TRUE);

  GST_OBJECT_LOCK (downloader);
  if (downloader->cancelled)
    goto quit;

  if (!gst_uri_downloader_set_uri (downloader, uri)) {
    GST_ERROR_OBJECT (downloader, "failed to set URI");
    goto quit;
  }

  gst_bus_set_flushing (downloader->bus, FALSE);
  GST_OBJECT_UNLOCK (downloader);

  /* set to ready state first to allow setting range */
  ret = gst_element_set_state (downloader->urisrc, GST_STATE_READY);

  GST_OBJECT_LOCK (downloader);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR_OBJECT (downloader, "failed to set src to READY");
    goto quit;
  }

  if (downloader->cancelled)
    goto quit;

  if (!gst_uri_downloader_set_range (downloader, range_start, range_end)) {
    GST_ERROR_OBJECT (downloader, "failed to set range");
    goto quit;
  }

  GST_OBJECT_UNLOCK (downloader);

  ret = gst_element_set_state (downloader->urisrc, GST_STATE_PLAYING);

  GST_OBJECT_LOCK (downloader);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR_OBJECT (downloader, "failed to set src to PLAYING");
    goto quit;
  }

  if (downloader->cancelled)
    goto quit;

  if (!downloader->eos) {
    /* wait until:
     *   - the download succeed (EOS in the src pad)
     *   - the download failed (Error message on the fetcher bus)
     *   - the download was canceled
     */
    GST_DEBUG_OBJECT (downloader, "waiting to fetch the URI %s", uri);
    g_cond_wait (&downloader->cond, GST_OBJECT_GET_LOCK (downloader));
  }

quit:
  {
    gboolean ret = FALSE;

    if (downloader->cancelled) {
      GST_DEBUG_OBJECT (downloader, "download interrupted");
    } else if (downloader->eos) {
      GST_DEBUG_OBJECT (downloader, "URI fetched successfully");
      ret = TRUE;
    }

    downloader->cancelled = FALSE;
    gst_uri_downloader_stop (downloader);
    GST_OBJECT_UNLOCK (downloader);
    g_mutex_unlock (&downloader->download_lock);

    return ret;
  }
}

typedef struct {
  GstBuffer *buffer;
} dl_context;

static GstFlowReturn
_download_chain (GstBuffer * buffer, gpointer user_data)
{
  dl_context *ctx = user_data;

  if (ctx->buffer)
    gst_buffer_append (ctx->buffer, buffer);
  else
    ctx->buffer = buffer;

  return GST_FLOW_OK;
}

GstBuffer *
gst_uri_downloader_fetch_uri (GstUriDownloader * downloader,
    const gchar * uri, gint64 range_start, gint64 range_end)
{
  dl_context ctx = { NULL };

  if (!gst_uri_downloader_stream_uri (downloader, uri,
        range_start, range_end, _download_chain, &ctx))
    gst_buffer_replace (&ctx.buffer, NULL);

  return ctx.buffer;
}
