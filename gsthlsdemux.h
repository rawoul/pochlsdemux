/* GStreamer
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * gsthlsdemux.h:
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

#ifndef GST_HLS_DEMUX_H_
# define GST_HLS_DEMUX_H_

#include "m3u8.h"

G_BEGIN_DECLS

typedef struct _GstHlsDemux GstHlsDemux;
typedef struct _GstHlsDemuxClass GstHlsDemuxClass;

struct _GstHlsDemux
{
  GstBin parent;

  GstPad *sinkpad;
  GstM3U8Client *client;
  GstBuffer *playlist;
  gint64 start_time;

  guint num_audio_tracks;
  guint num_video_tracks;
  guint num_subtitle_tracks;
  guint last_stream_id;

  gboolean have_group_id;
  guint group_id;

  GPtrArray *tracks;
};

struct _GstHlsDemuxClass
{
  GstBinClass parent_class;
};

GType gst_hls_demux_get_type (void);

#define GST_TYPE_HLS_DEMUX \
  (gst_hls_demux_get_type())
#define GST_HLS_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_HLS_DEMUX, GstHlsDemux))
#define GST_HLS_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_HLS_DEMUX, GstHlsDemuxClass))
#define GST_IS_HLS_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_HLS_DEMUX))
#define GST_IS_HLS_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_HLS_DEMUX))
#define GST_HLS_DEMUX_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_HLS_DEMUX, GstHlsDemuxClass))

G_END_DECLS

#endif /* GST_HLS_DEMUX_H_ */
