/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * m3u8.h:
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

#ifndef M3U8_H_
# define M3U8_H_

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstM3U8Map GstM3U8Map;
typedef struct _GstM3U8Key GstM3U8Key;
typedef struct _GstM3U8Segment GstM3U8Segment;
typedef struct _GstM3U8Playlist GstM3U8Playlist;
typedef struct _GstM3U8Media GstM3U8Media;
typedef struct _GstM3U8Stream GstM3U8Stream;
typedef struct _GstM3U8VariantPlaylist GstM3U8VariantPlaylist;
typedef struct _GstM3U8Rendition GstM3U8Rendition;
typedef struct _GstM3U8Client GstM3U8Client;

typedef enum
{
  GST_M3U8_PLAYLIST_TYPE_NONE,
  GST_M3U8_PLAYLIST_TYPE_VOD,
  GST_M3U8_PLAYLIST_TYPE_EVENT,
} GstM3U8PlaylistType;

typedef enum
{
  GST_M3U8_MEDIA_TYPE_AUDIO,
  GST_M3U8_MEDIA_TYPE_VIDEO,
  GST_M3U8_MEDIA_TYPE_SUBTITLES,
} GstM3U8MediaType;

typedef enum
{
  GST_M3U8_MEDIA_CODEC_NONE,
  GST_M3U8_MEDIA_CODEC_GENERIC_AUDIO,            /* mp4a */
  GST_M3U8_MEDIA_CODEC_AAC_LC,                   /* mp4a.40.2 */
  GST_M3U8_MEDIA_CODEC_HE_AAC,                   /* mp4a.40.5 */
  GST_M3U8_MEDIA_CODEC_MP3,                      /* mp4a.40.34 */
  GST_M3U8_MEDIA_CODEC_GENERIC_H264,             /* avc1 */
  GST_M3U8_MEDIA_CODEC_H264_BASE,                /* avc1.42e0XX */
  GST_M3U8_MEDIA_CODEC_H264_MAIN,                /* avc1.4d40XX */
  GST_M3U8_MEDIA_CODEC_H264_HIGH,                /* avc1.6400XX */
} GstM3U8MediaCodec;

typedef enum
{
  GST_M3U8_KEY_METHOD_NONE,
  GST_M3U8_KEY_METHOD_AES_128,
  GST_M3U8_KEY_METHOD_SAMPLE_AES,
  GST_M3U8_KEY_METHOD_UNKNOWN,
} GstM3U8KeyMethod;

typedef enum
{
  GST_M3U8_KEY_FORMAT_IDENTITY,
  GST_M3U8_KEY_FORMAT_UNKNOWN,
} GstM3U8KeyFormat;

struct _GstM3U8Map               /* EXT-X-MAP */
{
  gchar *uri;
  gint64 offset;
  gint64 length;
};

struct _GstM3U8Key               /* EXT-X-KEY */
{
  GstM3U8KeyMethod method;       /* .METHOD */
  GstM3U8KeyFormat format;       /* .KEYFORMAT */
  gchar *uri;                    /* .URI */
  gchar *iv;                     /* .IV */
};

struct _GstM3U8Segment           /* EXTINF */
{
  gchar *uri;
  GstClockTime duration;
  gint64 offset;                /* EXT-X-BYTERANGE start */
  gint64 length;                /* EXT-X-BYTERANGE length */
  gint sequence;
  gboolean discont;

  GstM3U8Map *map;
  GstM3U8Key *key;
};

struct _GstM3U8Playlist
{
  gchar *uri;
  gint version;                  /* EXT-X-VERSION */
  GstM3U8PlaylistType type;      /* EXT-X-PLAYLIST-TYPE */
  gboolean endlist;              /* EXT-X-ENDLIST */
  gboolean allow_cache;          /* EXT-X-ALLOWCACHE */
  gboolean i_frames_only;        /* EXT-X-I-FRAMES-ONLY */
  guint media_sequence;          /* EXT-X-MEDIA-SEQUENCE */
  GstDateTime *datetime;         /* EXT-X-PROGRAM-DATE-TIME */
  GstClockTime target_duration;  /* EXT-X-TARGETDURATION */
  GstClockTime download_ts;
  GstClockTime duration;

  GSList *maps;
  GSList *keys;
  GSList *segments;

  gchar *digest;
};

struct _GstM3U8Media             /* EXT-X-MEDIA */
{
  GstM3U8MediaType type;         /* .TYPE */
  gchar *group_id;               /* .GROUP-ID */
  gchar *name;                   /* .NAME */
  gchar *language;               /* .LANGUAGE */
  gchar *uri;                    /* .URI */
  gboolean is_default;           /* .DEFAULT */
  gboolean autoselect;           /* .AUTOSELECT */
  gboolean forced;               /* .FORCED */

  GstM3U8Playlist *playlist;
};

struct _GstM3U8Stream            /* EXT-X-STREAM-INF */
{                                /* or EXT-X-I-FRAME-STREAM-INF */
  gboolean i_frames_only;
  gint bandwidth;                /* .BANDWIDTH */
  gint program_id;               /* .PROGRAM-ID */
  GstM3U8MediaCodec video_codec; /* .CODECS */
  GstM3U8MediaCodec audio_codec; /* .CODECS */
  gint width;                    /* .RESOLUTION */
  gint height;                   /* .RESOLUTION */
  gchar *audio;                  /* .AUDIO */
  gchar *video;                  /* .VIDEO */
  gchar *subtitles;              /* .SUBTITLES */

  GstM3U8Playlist *playlist;
};

struct _GstM3U8VariantPlaylist
{
  gchar *uri;
  gint version;                  /* EXT-X-VERSION */
  GSList *streams;               /* list of GstM3U8Stream */
  GSList *i_frame_streams;       /* list of GstM3U8Stream */
  GHashTable *rendition_groups;  /* Group-ID -> GPtrArray[GstM3U8Media] */
};

struct _GstM3U8Client
{
  GstM3U8VariantPlaylist master_playlist;
  GstM3U8Stream *stream;         /* selected stream */
};

gboolean gst_m3u8_hex_to_bin (const gchar * hex, guint8 *dest, gsize size);

GstM3U8Client *gst_m3u8_client_new (void);
void gst_m3u8_client_free (GstM3U8Client * client);

gboolean gst_m3u8_client_parse_master_playlist (GstM3U8Client * client,
    gchar * data);

gboolean gst_m3u8_playlist_update (GstM3U8Playlist * playlist, gchar * data,
    gboolean * updated);

GstM3U8Segment *gst_m3u8_playlist_get_segment (GstM3U8Playlist * playlist,
    gint sequence);

GstM3U8Stream *gst_m3u8_client_select_stream (GstM3U8Client * client,
    gint max_bitrate);

gboolean gst_m3u8_client_guess_stream_media_type (GstM3U8Client * client,
    GstM3U8Stream * stream, GstM3U8MediaType * media_type);

GPtrArray *gst_m3u8_variant_playlist_find_group (GstM3U8VariantPlaylist * pl,
    const gchar * group_id);

G_END_DECLS

#endif /* M3U8_H_ */
