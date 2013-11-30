/* GStreamer
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * gsthlsdemux.c:
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

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/base/gstdataqueue.h>
#include <gst/base/gsttypefindhelper.h>

#include <openssl/aes.h>
#include <openssl/evp.h>

#include "gsturidownloader.h"
#include "gsthlsdemux.h"

static GstStaticPadTemplate video_template =
GST_STATIC_PAD_TEMPLATE ("video_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("video/mpegts, systemstream = (boolean) true"));

static GstStaticPadTemplate audio_template =
GST_STATIC_PAD_TEMPLATE ("audio_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("video/mpegts, systemstream = (boolean) true; application/x-id3"));

static GstStaticPadTemplate subtitle_template =
GST_STATIC_PAD_TEMPLATE ("subtitle_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("text/vtt"));

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-hls"));

enum
{
  PROP_0,
  PROP_LAST
};

GST_DEBUG_CATEGORY_STATIC (gst_hls_demux_debug);
#define GST_CAT_DEFAULT gst_hls_demux_debug

typedef struct _GstHlsTrack GstHlsTrack;

struct _GstHlsTrack {
  GstHlsDemux *demux;
  GstTask *task;
  GstPad *pad;
  GstM3U8Stream *stream;
  GstM3U8Media *media;
  gint64 download_time;
  guint last_seek_seqnum;
  GstDataQueue *queue;
  gboolean exposed;

  /* segment downloader */
  GstUriDownloader *downloader;
  GRecMutex download_lock;

  /* downloader context, set before fetching a segment */
  gint sequence;
  guint64 length;
  gboolean discont;
  GstClockTime next_pts;
  GstM3U8Key *key;

  guint8 aes_128_data[16];
  guint aes_128_data_size;
  EVP_CIPHER_CTX aes_ctx;
};

static void gst_hls_track_free (GstHlsTrack * track);

/* GObject */
static void gst_hls_demux_finalize (GObject * object);
static void gst_hls_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_hls_demux_get_property (GObject * object, guint prop_id,
  GValue * value, GParamSpec * pspec);

/* GstElement */
static GstStateChangeReturn gst_hls_demux_change_state (GstElement * element,
    GstStateChange transition);

/* sinkpad */
static GstFlowReturn gst_hls_demux_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);
static gboolean gst_hls_demux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

#define gst_hls_demux_parent_class parent_class
G_DEFINE_TYPE (GstHlsDemux, gst_hls_demux, GST_TYPE_BIN);

static void
gst_hls_demux_class_init (GstHlsDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  GST_DEBUG_CATEGORY_INIT (gst_hls_demux_debug, "hlsdemux",
      (GST_DEBUG_BOLD | GST_DEBUG_FG_GREEN), "HLS demuxer");

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_hls_demux_finalize;
  gobject_class->set_property = gst_hls_demux_set_property;
  gobject_class->get_property = gst_hls_demux_get_property;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_hls_demux_change_state);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&video_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&audio_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&subtitle_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_static_metadata (element_class,
      "Freebox HLS demuxer", "Demuxer",
      "HTTP Live Streaming demuxer",
      "Arnaud Vrac <avrac@freebox.fr>");
}

static void
gst_hls_demux_init (GstHlsDemux * demux)
{
  demux->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_hls_demux_chain));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_hls_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  demux->client = NULL;
  demux->playlist = NULL;
  demux->tracks = g_ptr_array_new_full (3, (GDestroyNotify) gst_hls_track_free);

  demux->num_audio_tracks = 0;
  demux->num_video_tracks = 0;
  demux->num_subtitle_tracks = 0;
  demux->last_stream_id = 0;
  demux->group_id = 0;
  demux->have_group_id = FALSE;
}

static void
gst_hls_demux_finalize (GObject * object)
{
  GstHlsDemux *demux = GST_HLS_DEMUX (object);

  if (demux->tracks)
    g_ptr_array_free (demux->tracks, TRUE);

  if (demux->playlist)
    gst_buffer_unref (demux->playlist);

  if (demux->client)
    gst_m3u8_client_free (demux->client);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_hls_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_hls_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_hls_track_free (GstHlsTrack * track)
{
  if (track->downloader)
    gst_object_unref (track->downloader);

  if (track->task)
    gst_object_unref (track->task);

  if (track->pad) {
    gst_element_remove_pad (GST_ELEMENT_CAST (track->demux), track->pad);
    gst_object_unref (track->pad);
  }

  if (track->queue)
    g_object_unref (track->queue);

  EVP_CIPHER_CTX_cleanup (&track->aes_ctx);

  g_free (track);
}

static GstM3U8Playlist *
gst_hls_track_get_playlist (GstHlsTrack * track)
{
  return track->media && track->media->playlist ?
      track->media->playlist : track->stream->playlist;
}

static gchar *
_buffer_to_utf8 (GstBuffer * buffer)
{
  gchar *data;
  gsize size;

  size = gst_buffer_get_size (buffer);

  data = g_malloc (size + 1);
  if (data) {
    size = gst_buffer_extract (buffer, 0, data, size);
    data[size] = '\0';
  }

  return data;
}

static gboolean
gst_hls_track_update_playlist (GstHlsTrack * track, gboolean * updated)
{
  GstM3U8Playlist *playlist;
  GstBuffer *buffer;
  gchar *data;

  playlist = gst_hls_track_get_playlist (track);

  GST_DEBUG_OBJECT (track->pad, "update playlist with uri %s", playlist->uri);

  track->download_time = g_get_monotonic_time ();
  buffer = gst_uri_downloader_fetch_uri (track->downloader,
      playlist->uri, 0, -1);

  if (!buffer) {
    GST_ELEMENT_ERROR (track->demux, STREAM, DECODE,
        ("Failed to download playlist"), (NULL));
    return FALSE;
  }

  data = _buffer_to_utf8 (buffer);
  gst_buffer_unref (buffer);

  if (!data || !gst_m3u8_playlist_update (playlist, data, updated)) {
    GST_ELEMENT_ERROR (track->demux, STREAM, DECODE,
        ("Invalid playlist"), (NULL));
    return FALSE;
  }

  return TRUE;
}

static gboolean
_data_queue_check_full (GstDataQueue * queue, guint visible,
    guint bytes, guint64 time, GstHlsTrack * track)
{
  // FIXME: find smarter limit based on download rate and stream bandwidth
  return bytes > 256 * 1024;
}

static void
_data_queue_item_destroy (GstDataQueueItem * item)
{
  gst_mini_object_unref (item->object);
  g_free (item);
}

static GstFlowReturn
gst_hls_track_push_event (GstHlsTrack * track, GstEvent * event)
{
  GstDataQueueItem *item = g_new (GstDataQueueItem, 1);

  GST_LOG_OBJECT (track->pad, "queue %" GST_PTR_FORMAT, event);

  item->object = GST_MINI_OBJECT_CAST (event);
  item->duration = GST_CLOCK_TIME_NONE;
  item->size = 0;
  item->visible = FALSE;
  item->destroy = (GDestroyNotify) _data_queue_item_destroy;

  if (!gst_data_queue_push_force (track->queue, item)) {
    _data_queue_item_destroy (item);
    return GST_FLOW_FLUSHING;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_hls_track_push_buffer (GstHlsTrack * track, GstBuffer * buffer)
{
  GstDataQueueItem *item = g_new (GstDataQueueItem, 1);
  gsize size;

  size = gst_buffer_get_size (buffer);

  GST_BUFFER_OFFSET (buffer) = track->length;
  GST_BUFFER_OFFSET_END (buffer) = track->length + size;

  GST_LOG_OBJECT (track->pad, "queue %" GST_PTR_FORMAT, buffer);

  item->object = GST_MINI_OBJECT_CAST (buffer);
  item->duration = GST_BUFFER_DURATION (buffer);
  item->size = size;
  item->visible = TRUE;
  item->destroy = (GDestroyNotify) _data_queue_item_destroy;

  if (!gst_data_queue_push (track->queue, item)) {
    _data_queue_item_destroy (item);
    return GST_FLOW_FLUSHING;
  }

  track->length += size;

  return GST_FLOW_OK;
}

static void
gst_hls_track_dequeue (GstHlsTrack * track)
{
  GstDataQueueItem *item;
  GstFlowReturn ret;

  if (!gst_data_queue_pop (track->queue, &item)) {
    ret = GST_FLOW_FLUSHING;
    goto pause;
  }

  if (G_LIKELY (GST_IS_BUFFER (item->object))) {
    GstBuffer *buffer = GST_BUFFER_CAST (item->object);
    ret = gst_pad_push (track->pad, buffer);

  } else if (GST_IS_EVENT (item->object)) {
    GstEvent *event = GST_EVENT_CAST (item->object);
    gst_pad_push_event (track->pad, event);
    ret = GST_FLOW_OK;

  } else {
    ret = GST_FLOW_ERROR;
  }

  g_free (item);

  if (ret != GST_FLOW_OK && ret != GST_FLOW_NOT_LINKED)
    goto pause;

  return;

pause:
  {
    const gchar *reason = gst_flow_get_name (ret);

    GST_LOG_OBJECT (track->pad, "pausing task, reason %s", reason);
    gst_pad_pause_task (track->pad);

    if (ret != GST_FLOW_OK &&
        ret != GST_FLOW_FLUSHING &&
        ret != GST_FLOW_NOT_LINKED)
      GST_ELEMENT_ERROR (track->demux, STREAM, FAILED, (NULL),
          ("stream stopped, reason %s", reason));
  }
}

static gboolean
gst_hls_track_activate (GstHlsTrack * track)
{
  GstM3U8Playlist *playlist;

  playlist = gst_hls_track_get_playlist (track);

  if (playlist->digest) {
    /* playlist was already downloaded upstream */
    track->download_time = track->demux->start_time;
  } else {
    if (!gst_hls_track_update_playlist (track, NULL))
      return FALSE;
  }

  track->sequence = playlist->media_sequence;
  track->discont = TRUE;

  gst_task_start (track->task);
  gst_pad_start_task (track->pad, (GstTaskFunction) gst_hls_track_dequeue,
      track, NULL);

  return TRUE;
}

static gboolean
gst_hls_track_expose (GstHlsTrack * track, GstCaps * caps)
{
  GstHlsDemux *demux;
  gchar *stream_id;
  GstEvent *stream_start;
  GstStreamFlags stream_flags;
  GstSegment segment;
  GstM3U8Playlist *playlist;

  demux = track->demux;

  GST_INFO_OBJECT (track->pad, "set caps %" GST_PTR_FORMAT, caps);

  /* send stream-start event */
  stream_start = gst_pad_get_sticky_event (demux->sinkpad,
      GST_EVENT_STREAM_START, 0);
  if (stream_start) {
    if (gst_event_parse_group_id (stream_start, &demux->group_id))
      demux->have_group_id = TRUE;
    else
      demux->have_group_id = FALSE;
    gst_event_unref (stream_start);
  } else if (!demux->have_group_id) {
    demux->have_group_id = TRUE;
    demux->group_id = gst_util_group_id_next ();
  }

  stream_id = gst_pad_create_stream_id_printf (track->pad,
      GST_ELEMENT_CAST (demux), "%03u", demux->last_stream_id++);
  stream_start = gst_event_new_stream_start (stream_id);
  g_free (stream_id);

  if (demux->have_group_id)
    gst_event_set_group_id (stream_start, demux->group_id);

  if (track->media) {
    stream_flags = GST_STREAM_FLAG_NONE;
    if (track->media->type == GST_M3U8_MEDIA_TYPE_SUBTITLES)
      stream_flags |= GST_STREAM_FLAG_SPARSE;
    if (track->media->is_default)
      stream_flags |= GST_STREAM_FLAG_SELECT;
    gst_event_set_stream_flags (stream_start, stream_flags);
  }

  gst_hls_track_push_event (track, stream_start);

  /* send caps */
  gst_hls_track_push_event (track, gst_event_new_caps (caps));

  /* send segment */
  gst_segment_init (&segment, GST_FORMAT_TIME);

  playlist = gst_hls_track_get_playlist (track);
  if (playlist->endlist)
    segment.duration = playlist->duration;

  gst_hls_track_push_event (track, gst_event_new_segment (&segment));

  track->next_pts = 0;

  return TRUE;
}

static gboolean
_detect_id3 (const guint8 * data, gsize size)
{
  return size >= 10 &&
      !memcmp ("ID3", data, 3) &&
      data[3] != 0xff &&
      data[4] != 0xff &&
      (data[6] & 0x80) == 0 &&
      (data[7] & 0x80) == 0 &&
      (data[8] & 0x80) == 0 &&
      (data[9] & 0x80) == 0;
}

static gboolean
_detect_webvtt (const guint8 * data, gsize size)
{
  /* skip UTF-8 BOM */
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    data += 3;
    size -= 3;
  }

  return size >= 6 && !memcmp ("WEBVTT", data, 6) && (size == 6 ||
      data[6] == '\n' || data[6] == '\r' ||
      data[6] == '\t' || data[6] == ' ');
}

static GstCaps *
_find_caps (GstBuffer * buffer)
{
  GstCaps *caps;
  GstMapInfo map;

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    return NULL;

  if (_detect_id3 (map.data, map.size)) {
    caps = gst_caps_new_empty_simple ("application/x-id3");

    /* FIXME: use timestamp in ID3 PRIV section */
    /* FIXME: extract ID3 tag for each segment */

  } else if (_detect_webvtt (map.data, map.size)) {
    caps = gst_caps_new_empty_simple ("text/vtt");

  } else {
    caps = gst_caps_new_simple ("video/mpegts",
        "systemstream", G_TYPE_BOOLEAN, TRUE, NULL);
  }

  gst_buffer_unmap (buffer, &map);

  return caps;
}

static gboolean
gst_hls_track_decrypt_aes128_init (GstHlsTrack * track, GstM3U8Key * params)
{
  GstBuffer *key_buffer;
  guint key_size;
  guint8 key[16];
  guint8 iv[16];

  GST_INFO_OBJECT (track->pad, "download AES-128 key from %s", params->uri);

  key_buffer = gst_uri_downloader_fetch_uri (track->downloader,
      params->uri, 0, -1);

  if (!key_buffer) {
    GST_ERROR_OBJECT (track->pad, "failed to download key");
    return FALSE;
  }

  key_size = gst_buffer_extract (key_buffer, 0, key, 16);
  gst_buffer_unref (key_buffer);

  if (key_size != 16) {
    GST_ERROR_OBJECT (track->pad, "AES-128 key is too small");
    return FALSE;
  }

  if (params->iv) {
    if (!gst_m3u8_hex_to_bin (params->iv, iv, 16)) {
      GST_ERROR_OBJECT (track->pad, "invalid AES-128 IV %s", params->iv);
      return FALSE;
    }
  } else {
    memset (iv, 0, 12);
    GST_WRITE_UINT32_BE (iv + 12, track->sequence);
  }

  GST_MEMDUMP ("AES-128 key", key, 16);
  GST_MEMDUMP ("AES-128 iv", iv, 16);

  EVP_CipherInit_ex (&track->aes_ctx, EVP_aes_128_cbc (), NULL,
      key, iv, AES_DECRYPT);

  return TRUE;
}

static GstBuffer *
gst_hls_track_decrypt_aes128_data (GstHlsTrack * track, GstBuffer * buffer)
{
  GstBuffer *outbuf = NULL;
  GstMapInfo map, outmap;
  gint outsize;
  gint blocksize;

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    goto map_failed;

  blocksize = EVP_CIPHER_CTX_block_size (&track->aes_ctx);
  outbuf = gst_buffer_new_allocate (NULL, map.size + blocksize, NULL);
  if (!outbuf)
    goto alloc_failed;

  if (!gst_buffer_map (outbuf, &outmap, GST_MAP_WRITE))
    goto outmap_failed;

  outsize = outmap.size;
  EVP_CipherUpdate (&track->aes_ctx,
      outmap.data, &outsize, map.data, map.size);

  gst_buffer_unmap (outbuf, &outmap);
  gst_buffer_set_size (outbuf, outsize);
  gst_buffer_unref (buffer);

  return outbuf;

outmap_failed:
  gst_buffer_unref (outbuf);
alloc_failed:
  gst_buffer_unmap (buffer, &map);
map_failed:
  gst_buffer_unref (buffer);
  return NULL;
}

static void
gst_hls_track_decrypt_aes128_finish (GstHlsTrack * track)
{
  GstBuffer *buffer;
  GstMapInfo map;
  gint blocksize;

  blocksize = EVP_CIPHER_CTX_block_size (&track->aes_ctx);
  buffer = gst_buffer_new_allocate (NULL, blocksize, NULL);
  if (!buffer)
    return;

  if (!gst_buffer_map (buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref (buffer);
    return;
  }

  EVP_CipherFinal_ex (&track->aes_ctx, map.data, &blocksize);
  gst_buffer_unmap (buffer, &map);

  if (blocksize == 0) {
    gst_buffer_unref (buffer);
    return;
  }

  gst_buffer_set_size (buffer, blocksize);
  gst_hls_track_push_buffer (track, buffer);
}

static GstFlowReturn
track_downloader_chain (GstBuffer * buffer, gpointer user_data)
{
  GstHlsTrack *track = user_data;

  if (track->key && track->key->method == GST_M3U8_KEY_METHOD_AES_128) {
    buffer = gst_hls_track_decrypt_aes128_data (track, buffer);
    if (!buffer)
      goto fail;
  }

  if (G_UNLIKELY (!track->exposed)) {
    GstCaps *caps;
    gboolean exposed;

    caps = _find_caps (buffer);
    exposed = gst_hls_track_expose (track, caps);
    gst_caps_unref (caps);

    if (!exposed)
      goto fail;

    track->exposed = TRUE;
  }

  buffer = gst_buffer_make_writable (buffer);

  GST_BUFFER_FLAGS (buffer) = 0;
  GST_BUFFER_PTS (buffer) = track->next_pts;
  GST_BUFFER_DTS (buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (buffer) = GST_CLOCK_TIME_NONE;

  track->next_pts = GST_CLOCK_TIME_NONE;

  if (track->discont) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    track->discont = FALSE;
  }

  return gst_hls_track_push_buffer (track, buffer);

fail:
  gst_buffer_unref (buffer);
  return GST_FLOW_ERROR;
}

static void
gst_hls_track_download (GstHlsTrack * track)
{
  GstM3U8Playlist *playlist;
  GstM3U8Segment *segment;
  guint64 range_start, range_end;

  playlist = gst_hls_track_get_playlist (track);

  /* find next segment to download based on sequence */
retry:
  segment = gst_m3u8_playlist_get_segment (playlist, track->sequence);
  if (!segment) {
    if (playlist->endlist) {
      GST_DEBUG_OBJECT (track->pad, "all segments downloaded, send EOS");
      goto eos;
    } else {
      gboolean update;

      if (!gst_hls_track_update_playlist (track, &update)) {
        GST_ERROR_OBJECT (track->pad, "failed to fetch stream playlist");
        goto eos;
      } else if (!update) {
        /* FIXME: is this an error ?? */
        GST_ERROR_OBJECT (track->pad, "no more segments in playlist");
        goto eos;
      } else
        goto retry;
    }
  }

  /* set discont if segment is not a continuation of the previous one */
  if (track->sequence != segment->sequence || segment->discont)
    track->discont = TRUE;

  track->sequence = segment->sequence;
  track->key = NULL;

  /* init crypto context when it does not match the previous segment */
  if (segment->key && segment->key->uri) {
    switch (segment->key->method) {
      case GST_M3U8_KEY_METHOD_NONE:
        break;

      case GST_M3U8_KEY_METHOD_AES_128:
        if (!gst_hls_track_decrypt_aes128_init (track, segment->key))
          goto eos;
        break;

      default:
        GST_ERROR_OBJECT (track->pad, "unsupported crypt method");
        goto eos;
    }

    track->key = segment->key;
  }

  /* download segment data */
  GST_DEBUG_OBJECT (track->pad, "download segment %u, offset %" G_GINT64_FORMAT
      " size %" G_GINT64_FORMAT " uri %s", segment->sequence, segment->offset,
      segment->length, segment->uri);

  range_start = segment->offset;
  range_end = segment->length < 0 ? -1 : segment->length + segment->offset;

  if (!gst_uri_downloader_stream_uri (track->downloader, segment->uri,
        range_start, range_end, track_downloader_chain, track)) {
    GST_DEBUG_OBJECT (track->pad, "failed download");
    track->discont = TRUE;
  }

  /* finish/flush crypto context */
  if (track->key && track->key->method == GST_M3U8_KEY_METHOD_AES_128)
    gst_hls_track_decrypt_aes128_finish (track);

  /* set next segment to download */
  track->sequence++;

  return;

eos:
  gst_hls_track_push_event (track, gst_event_new_eos ());
  gst_task_stop (track->task);
}

static gboolean
gst_hls_track_pad_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstHlsTrack *track;
  GstM3U8Playlist *playlist;
  gboolean res = FALSE;

  track = gst_pad_get_element_private (pad);
  playlist = gst_hls_track_get_playlist (track);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_URI:
      gst_query_set_uri (query, track->demux->client->master_playlist.uri);
      res = TRUE;
      break;

    case GST_QUERY_DURATION:
      if (playlist->endlist || playlist->type == GST_M3U8_PLAYLIST_TYPE_EVENT) {
        gst_query_set_duration (query, GST_FORMAT_TIME, playlist->duration);
        res = TRUE;
      }
      break;

    case GST_QUERY_SEEKING: {
      GstFormat format;

      gst_query_parse_seeking (query, &format, NULL, NULL, NULL);

      if (format == GST_FORMAT_TIME && (playlist->endlist ||
            playlist->type == GST_M3U8_PLAYLIST_TYPE_EVENT)) {
        gst_query_set_seeking (query, format, TRUE, 0, playlist->duration);
        res = TRUE;
      }
      break;
    }

    case GST_QUERY_LATENCY: {
      gboolean live;
      GstClockTime min, max;

      gst_query_parse_latency (query, &live, &min, &max);
      live |= !playlist->endlist &&
          playlist->type != GST_M3U8_PLAYLIST_TYPE_EVENT;
      gst_query_set_latency (query, live, min, max);
      res = TRUE;
      break;
    }

    default:
      /* do not forward query upstream, since it was only useful to
       * download the master playlist */
      break;
  }

  return res;
}

static gboolean
gst_hls_track_handle_seek_event (GstHlsTrack * track, GstEvent * event)
{
  GstM3U8Playlist *playlist;
  GSList *list;
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  GstSegment seeksegment;
  GstClockTime pos;
  gboolean snap_after;
  guint seqnum;

  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start,
      &stop_type, &stop);

  seqnum = gst_event_get_seqnum (event);

  if (seqnum == track->last_seek_seqnum) {
    GST_DEBUG_OBJECT (track->pad, "skipping already handled seek");
    return TRUE;
  }

  if (format != GST_FORMAT_TIME) {
    GST_DEBUG_OBJECT (track->pad, "can only seek in TIME");
    return FALSE;
  }

  if (rate < 0.0) {
    GST_DEBUG_OBJECT (track->pad, "reverse playback is not supported");
    return FALSE;
  }

  playlist = gst_hls_track_get_playlist (track);
  if (!playlist->endlist && playlist->type != GST_M3U8_PLAYLIST_TYPE_EVENT) {
    GST_DEBUG_OBJECT (track->pad, "cannot seek in live playlist");
    return FALSE;
  }

  GST_DEBUG_OBJECT (track->pad, "received seek event: %" GST_PTR_FORMAT, event);

  track->last_seek_seqnum = seqnum;

  gst_segment_init (&seeksegment, GST_FORMAT_TIME);
  if (playlist->endlist)
    seeksegment.duration = playlist->duration;

  gst_segment_do_seek (&seeksegment, rate, format, flags, start_type, start,
      stop_type, stop, NULL);

  GST_DEBUG_OBJECT (track->pad, "find sequence for time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (seeksegment.start));

  if (flags & GST_SEEK_FLAG_FLUSH) {
    GstEvent *flush_event = gst_event_new_flush_start ();
    GST_DEBUG_OBJECT (track->pad, "starting flush");
    gst_data_queue_set_flushing (track->queue, TRUE);
    gst_task_stop (track->task);
    gst_uri_downloader_cancel (track->downloader);
    gst_task_join (track->task);
    gst_data_queue_flush (track->queue);
    gst_event_set_seqnum (flush_event, seqnum);
    gst_pad_push_event (track->pad, flush_event);
  }

  snap_after = !!(flags & GST_SEEK_FLAG_SNAP_AFTER) &&
      !(flags & GST_SEEK_FLAG_SNAP_BEFORE);

  pos = 0;
  for (list = playlist->segments; list != NULL; list = list->next) {
    GstM3U8Segment *segment = (GstM3U8Segment *) list->data;
    gboolean clip;

    if (snap_after)
      clip = seeksegment.position <= pos;
    else
      clip = seeksegment.position >= pos &&
          seeksegment.position < pos + segment->duration;

    if (clip) {
      GST_DEBUG_OBJECT (track->pad, "found sequence %u, start time %"
          GST_TIME_FORMAT, segment->sequence, GST_TIME_ARGS (pos));
      track->sequence = segment->sequence;
      seeksegment.position = pos;
      if (flags & GST_SEEK_FLAG_KEY_UNIT) {
        seeksegment.time = pos;
        seeksegment.start = pos;
      }
      break;
    }

    pos += segment->duration;
  }

  if (flags & GST_SEEK_FLAG_FLUSH) {
    GstEvent *flush_event = gst_event_new_flush_stop (TRUE);
    GST_DEBUG_OBJECT (track->pad, "stopping flush");
    gst_data_queue_set_flushing (track->queue, FALSE);
    gst_event_set_seqnum (flush_event, seqnum);
    gst_pad_push_event (track->pad, flush_event);
  }

  GST_DEBUG_OBJECT (track->pad, "send %" GST_SEGMENT_FORMAT,
      &seeksegment);
  gst_hls_track_push_event (track, gst_event_new_segment (&seeksegment));

  track->discont = TRUE;
  track->length = 0;
  track->next_pts = seeksegment.position;

  gst_task_start (track->task);
  gst_pad_start_task (track->pad, (GstTaskFunction) gst_hls_track_dequeue,
      track, NULL);

  return TRUE;
}

static gboolean
gst_hls_track_pad_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstHlsTrack *track;
  gboolean res;

  track = gst_pad_get_element_private (pad);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      res = gst_hls_track_handle_seek_event (track, event);
      break;

    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (pad, "flush start");
      gst_task_stop (track->task);
      gst_uri_downloader_cancel (track->downloader);
      gst_data_queue_set_flushing (track->queue, TRUE);
      gst_data_queue_flush (track->queue);
      break;

    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (pad, "flush stop");
      gst_data_queue_set_flushing (track->queue, FALSE);
      break;

    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }

  return res;
}

static gboolean
gst_hls_demux_add_track (GstHlsDemux * demux, GstM3U8Stream * stream,
    GstM3U8Media * media)
{
  GstHlsTrack *track;
  GstStaticPadTemplate *static_template;
  gchar pad_name[16];
  GstM3U8MediaType media_type;

  if (media != NULL)
    media_type = media->type;
  else if (!gst_m3u8_client_guess_stream_media_type (demux->client,
        stream, &media_type)) {
    GST_DEBUG_OBJECT (demux, "cannot determine stream type");
    return FALSE;
  }

  track = g_new0 (GstHlsTrack, 1);
  track->demux = demux;
  track->stream = stream;
  track->media = media;
  track->sequence = -1;
  track->last_seek_seqnum = (guint32) -1;
  track->next_pts = GST_CLOCK_TIME_NONE;
  track->queue = gst_data_queue_new ((GstDataQueueCheckFullFunction)
      _data_queue_check_full, NULL, NULL, track);

  EVP_CIPHER_CTX_init (&track->aes_ctx);

  /* create source pad */
  switch (media_type) {
    case GST_M3U8_MEDIA_TYPE_AUDIO:
      static_template = &audio_template;
      snprintf (pad_name, sizeof (pad_name), "audio_%u",
          demux->num_audio_tracks++);
      break;

    case GST_M3U8_MEDIA_TYPE_VIDEO:
      static_template = &video_template;
      snprintf (pad_name, sizeof (pad_name), "video_%u",
          demux->num_video_tracks++);
      break;

    case GST_M3U8_MEDIA_TYPE_SUBTITLES:
      static_template = &subtitle_template;
      snprintf (pad_name, sizeof (pad_name), "subtitle_%u",
          demux->num_subtitle_tracks++);
      break;
  }

  /* setup source pad */
  track->pad = gst_pad_new_from_static_template (static_template, pad_name);
  gst_pad_set_query_function (track->pad, gst_hls_track_pad_query);
  gst_pad_set_event_function (track->pad, gst_hls_track_pad_event);
  gst_pad_set_element_private (track->pad, track);

  /* setup segment downloader */
  track->downloader = gst_uri_downloader_new ();

  /* create task for downloader */
  g_rec_mutex_init (&track->download_lock);
  track->task = gst_task_new ((GstTaskFunction) gst_hls_track_download,
      track, NULL);

  gst_task_set_lock (track->task, &track->download_lock);

  g_ptr_array_add (demux->tracks, track);

  /* expose pad */
  gst_pad_set_active (track->pad, TRUE);

  if (track->media)
    GST_INFO_OBJECT (demux, "add pad %s for media `%s' (lang: %s)", pad_name,
        track->media->name, track->media->language);
  else
    GST_INFO_OBJECT (demux, "add pad %s", pad_name);

  gst_element_add_pad (GST_ELEMENT (demux), gst_object_ref (track->pad));

  return TRUE;
}

static GstStateChangeReturn
gst_hls_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstHlsDemux *demux;
  GstStateChangeReturn ret;
  guint i;

  demux = GST_HLS_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_DEBUG_OBJECT (demux, "stopping downloads");
      gst_pad_stop_task (demux->sinkpad);
      for (i = 0; i < demux->tracks->len; i++) {
        GstHlsTrack *track = g_ptr_array_index (demux->tracks, i);
        gst_data_queue_set_flushing (track->queue, TRUE);
        gst_task_stop (track->task);
      }
      g_ptr_array_free (demux->tracks, TRUE);
      demux->tracks = NULL;
      break;

    default:
      break;
  }

  /* pass state changes to base class */
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  return ret;
}

static GstFlowReturn
gst_hls_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstHlsDemux *demux = GST_HLS_DEMUX (parent);

  if (!demux->playlist) {
    demux->playlist = buffer;
    demux->start_time = g_get_monotonic_time ();
  } else
    demux->playlist = gst_buffer_append (demux->playlist, buffer);

  return GST_FLOW_OK;
}

static gboolean
gst_hls_demux_parse_master_playlist (GstHlsDemux * demux)
{
  GstQuery *query;
  GstM3U8Stream *stream;
  GstM3U8Media *media;
  GPtrArray *group;
  guint i;
  gchar *data;
  gboolean ret;

  if (demux->playlist == NULL) {
    GST_WARNING_OBJECT (demux, "received EOS without a playlist");
    return FALSE;
  }

  demux->client = gst_m3u8_client_new ();

  query = gst_query_new_uri ();
  if (!gst_pad_peer_query (demux->sinkpad, query)) {
    GST_WARNING_OBJECT (demux, "failed to query playlist URI");
  } else {
    gst_query_parse_uri (query, &demux->client->master_playlist.uri);
    GST_INFO_OBJECT (demux, "fetched master playlist at URI: %s",
        demux->client->master_playlist.uri);
  }
  gst_query_unref (query);

  data = _buffer_to_utf8 (demux->playlist);
  gst_buffer_unref (demux->playlist);
  demux->playlist = NULL;

  ret = TRUE;
  if (!data || !gst_m3u8_client_parse_master_playlist (demux->client, data)) {
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist"), (NULL));
    ret = FALSE;
  }

  g_free (data);

  /* select stream with highest bandwidth */
  stream = gst_m3u8_client_select_stream (demux->client, 0);
  if (!stream) {
    GST_ERROR_OBJECT (demux, "failed to select stream to render");
    return FALSE;
  }

  GST_INFO_OBJECT (demux, "selected stream bandwidth: %d kbps",
      stream->bandwidth);

  /* add track for stream main rendition */
  gst_hls_demux_add_track (demux, stream, NULL);

  /* add tracks for alternate renditions in video group */
  group = gst_m3u8_variant_playlist_find_group (
      &demux->client->master_playlist, stream->video);
  if (group) {
    for (i = 0; i < group->len; i++) {
      media = g_ptr_array_index (group, i);
      if (media->uri != NULL)
        gst_hls_demux_add_track (demux, stream, media);
    }
  }

  /* add tracks for alternate renditions in audio group */
  group = gst_m3u8_variant_playlist_find_group (
      &demux->client->master_playlist, stream->audio);
  if (group) {
    for (i = 0; i < group->len; i++) {
      media = g_ptr_array_index (group, i);
      if (media->uri != NULL)
        gst_hls_demux_add_track (demux, stream, media);
    }
  }

  /* add tracks for alternate renditions in subtitles group */
  group = gst_m3u8_variant_playlist_find_group (
      &demux->client->master_playlist, stream->subtitles);
  if (group) {
    for (i = 0; i < group->len; i++) {
      media = g_ptr_array_index (group, i);
      if (media->uri != NULL)
        gst_hls_demux_add_track (demux, stream, media);
    }
  }

  gst_element_no_more_pads (GST_ELEMENT (demux));

  /* activate each track pad */
  for (i = 0; i < demux->tracks->len; i++)
    gst_hls_track_activate (g_ptr_array_index (demux->tracks, i));

  return ret;
}

static gboolean
gst_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstHlsDemux *demux = GST_HLS_DEMUX (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      if (gst_hls_demux_parse_master_playlist (demux))
        return TRUE;
      break;

    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}
