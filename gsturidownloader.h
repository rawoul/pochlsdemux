/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * gsturidownloader.h
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

#ifndef GSTURIDOWNLOADER_H_
# define GSTURIDOWNLOADER_H_

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstUriDownloader GstUriDownloader;
typedef struct _GstUriDownloaderClass GstUriDownloaderClass;

typedef GstFlowReturn (*GstUriDownloaderChainFunction)
  (GstBuffer * buffer, gpointer user_data);

struct _GstUriDownloader
{
  GstObject parent;

  GstElement *urisrc;
  GstBus *bus;
  GstPad *pad;
  GMutex download_lock;

  GCond cond;
  gboolean cancelled;
  gboolean eos;

  GstUriDownloaderChainFunction chain;
  gpointer priv;
};

struct _GstUriDownloaderClass
{
  GstObjectClass parent_class;
};

GType gst_uri_downloader_get_type (void);

GstUriDownloader *gst_uri_downloader_new (void);
void gst_uri_downloader_cancel (GstUriDownloader *downloader);

gboolean gst_uri_downloader_stream_uri (GstUriDownloader * downloader,
    const gchar * uri, gint64 range_start, gint64 range_end,
    GstUriDownloaderChainFunction chain_func, gpointer user_data);

GstBuffer *gst_uri_downloader_fetch_uri (GstUriDownloader * downloader,
    const gchar * uri, gint64 range_start, gint64 range_end);

#define GST_TYPE_URI_DOWNLOADER \
  (gst_uri_downloader_get_type())
#define GST_URI_DOWNLOADER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_URI_DOWNLOADER, GstUriDownloader))
#define GST_URI_DOWNLOADER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_URI_DOWNLOADER, GstUriDownloaderClass))
#define GST_IS_URI_DOWNLOADER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_URI_DOWNLOADER))
#define GST_IS_URI_DOWNLOADER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_URI_DOWNLOADER))

G_END_DECLS

#endif /* !GSTURIDOWNLOADER_H_ */
