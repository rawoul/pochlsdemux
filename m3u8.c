/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2013 Arnaud Vrac <avrac@freebox.fr>
 *
 * m3u8.c
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

#include <stdlib.h>
#include <string.h>

#include "m3u8.h"

GST_DEBUG_CATEGORY_EXTERN (gst_hls_m3u8);
#define GST_CAT_DEFAULT gst_hls_m3u8

#define GST_M3U8_VERSION 5

typedef struct {
  GstM3U8Media *medias;
  guint n_medias;
} GstM3U8MediaGroup;

static gchar *
uri_join (const gchar * base, const gchar * uri)
{
  guint location_len;
  const gchar *p, *path;
  gchar *ret;

  if (gst_uri_is_valid (uri))
    return g_strdup (uri);

  p = strstr (base, "://");
  if (!p)
    return NULL;

  p += 3;
  while (*p && *p != '/')
    p++;

  path = p;
  location_len = path - base;

  if (uri[0] == '/') {
    ret = g_strdup_printf ("%.*s%s", location_len, base, uri);
  } else {
    guint path_len;

    p = strrchr (path, '/');
    if (!p)
      path_len = 0;
    else
      path_len = p - path;

    ret = g_strdup_printf ("%.*s/%s", location_len + path_len, base, uri);
  }

  return ret;
}

gboolean
gst_m3u8_hex_to_bin (const gchar * hex, guint8 *dest, gsize size)
{
  guint i, len;
  guint offset;

  if (!hex || !dest)
    return FALSE;

  if (hex[0] == '0' && hex[1] == 'x')
    hex += 2;

  len = strlen (hex);
  memset (dest, 0, size);

  if (size * 2 < len)
    return FALSE;

  offset = size * 2 - len;

  for (i = 0; i < len; i++) {
    gint v = g_ascii_xdigit_value (hex[i]);
    if (v < 0)
      return FALSE;

    if (offset & 1)
      dest[offset / 2] |= v;
    else
      dest[offset / 2] = v << 4;

    offset++;
  }

  return TRUE;
}

static GstM3U8Map *
gst_m3u8_map_new (void)
{
  GstM3U8Map *map;

  map = g_new (GstM3U8Map, 1);
  map->uri = NULL;
  map->offset = -1;
  map->length = -1;

  return map;
}

static void
gst_m3u8_map_free (GstM3U8Map * map)
{
  g_free (map->uri);
  g_free (map);
}

static GstM3U8Key *
gst_m3u8_key_new (void)
{
  GstM3U8Key *key;

  key = g_new0 (GstM3U8Key, 1);
  key->method = GST_M3U8_KEY_METHOD_NONE;
  key->format = GST_M3U8_KEY_FORMAT_IDENTITY;
  key->uri = NULL;

  return key;
}

static void
gst_m3u8_key_free (GstM3U8Key * key)
{
  g_free (key->uri);
  g_free (key);
}

static GstM3U8Segment *
gst_m3u8_segment_new (gchar * uri, GstClockTime duration, guint sequence)
{
  GstM3U8Segment *segment;

  segment = g_new (GstM3U8Segment, 1);
  segment->uri = uri;
  segment->duration = duration;
  segment->sequence = sequence;
  segment->offset = 0;
  segment->length = -1;
  segment->discont = FALSE;
  segment->key = NULL;
  segment->map = NULL;

  return segment;
}

static void
gst_m3u8_segment_free (GstM3U8Segment * segment)
{
  g_free (segment->uri);
  g_free (segment);
}

static void
gst_m3u8_playlist_reset (GstM3U8Playlist * playlist)
{
  playlist->version = 0;
  playlist->type = GST_M3U8_PLAYLIST_TYPE_NONE;
  playlist->endlist = FALSE;
  playlist->allow_cache = FALSE;
  playlist->media_sequence = 0;
  playlist->target_duration = GST_CLOCK_TIME_NONE;
  playlist->i_frames_only = FALSE;
  playlist->datetime = NULL;
  playlist->download_ts = GST_CLOCK_TIME_NONE;

  if (playlist->segments != NULL) {
    g_slist_free_full (playlist->segments,
        (GDestroyNotify) gst_m3u8_segment_free);
    playlist->segments = NULL;
  }

  if (playlist->maps != NULL) {
    g_slist_free_full (playlist->maps,
        (GDestroyNotify) gst_m3u8_map_free);
    playlist->maps = NULL;
  }

  if (playlist->keys != NULL) {
    g_slist_free_full (playlist->keys,
        (GDestroyNotify) gst_m3u8_key_free);
    playlist->keys = NULL;
  }

  if (playlist->datetime) {
    gst_date_time_unref (playlist->datetime);
    playlist->datetime = NULL;
  }

  if (playlist->digest) {
    g_free (playlist->digest);
    playlist->digest = NULL;
  }
}

static GstM3U8Playlist *
gst_m3u8_playlist_new (void)
{
  GstM3U8Playlist *playlist;

  playlist = g_new0 (GstM3U8Playlist, 1);
  gst_m3u8_playlist_reset (playlist);

  return playlist;
}

static void
gst_m3u8_playlist_free (GstM3U8Playlist * playlist)
{
  gst_m3u8_playlist_reset (playlist);
  g_free (playlist->uri);
  g_free (playlist);
}

GstM3U8Segment *
gst_m3u8_playlist_get_segment (GstM3U8Playlist * playlist, gint sequence)
{
  GstM3U8Segment *segment;
  GSList *list;

  for (list = playlist->segments; list != NULL; list = list->next) {
    segment = (GstM3U8Segment *) list->data;
    if (segment->sequence >= sequence)
      return segment;
  }

  return NULL;
}

static GstM3U8Media *
gst_m3u8_media_new (void)
{
  GstM3U8Media *media;

  media = g_new (GstM3U8Media, 1);
  media->type = GST_M3U8_MEDIA_TYPE_VIDEO;
  media->group_id = NULL;
  media->name = NULL;
  media->language = NULL;
  media->uri = NULL;
  media->is_default = TRUE;
  media->autoselect = FALSE;
  media->forced = FALSE;
  media->playlist = NULL;

  return media;
}

static void
gst_m3u8_media_free (GstM3U8Media * media)
{
  if (media->playlist != NULL)
    gst_m3u8_playlist_free (media->playlist);

  g_free (media->group_id);
  g_free (media->name);
  g_free (media->language);
  g_free (media->uri);
  g_free (media);
}

static GstM3U8Stream *
gst_m3u8_stream_new (void)
{
  GstM3U8Stream *stream;

  stream = g_new (GstM3U8Stream, 1);
  stream->bandwidth = 0;
  stream->program_id = -1;
  stream->video_codec = GST_M3U8_MEDIA_CODEC_NONE;
  stream->audio_codec = GST_M3U8_MEDIA_CODEC_NONE;
  stream->width = 0;
  stream->height = 0;
  stream->audio = NULL;
  stream->video = NULL;
  stream->subtitles = NULL;
  stream->playlist = NULL;

  return stream;
}

static void
gst_m3u8_stream_free (GstM3U8Stream * stream)
{
  if (stream->playlist)
    gst_m3u8_playlist_free (stream->playlist);

  if (stream->audio != NULL)
    g_free (stream->audio);

  if (stream->video != NULL)
    g_free (stream->video);

  if (stream->subtitles != NULL)
    g_free (stream->subtitles);

  g_free (stream);
}

static void
gst_m3u8_stream_set_uri (GstM3U8Stream * stream, const gchar *uri)
{
  if (stream->playlist) {
    gst_m3u8_playlist_reset (stream->playlist);
    g_free (stream->playlist->uri);
  } else {
    stream->playlist = gst_m3u8_playlist_new ();
  }

  stream->playlist->uri = g_strdup (uri);
}

static void
gst_m3u8_variant_playlist_init (GstM3U8VariantPlaylist * playlist)
{
  playlist->uri = NULL;
  playlist->streams = NULL;
  playlist->i_frame_streams = NULL;
  playlist->rendition_groups = g_hash_table_new_full (g_str_hash, g_str_equal,
          (GDestroyNotify) g_ptr_array_unref, NULL);
}

static void
gst_m3u8_variant_playlist_cleanup (GstM3U8VariantPlaylist * playlist)
{
  if (playlist->streams != NULL)
    g_slist_free_full (playlist->streams,
        (GDestroyNotify) gst_m3u8_stream_free);

  if (playlist->rendition_groups != NULL)
    g_hash_table_unref (playlist->rendition_groups);

  g_free (playlist->uri);
}

GPtrArray *
gst_m3u8_variant_playlist_find_group (GstM3U8VariantPlaylist * playlist,
    const gchar * group_id)
{
  if (group_id)
    return g_hash_table_lookup (playlist->rendition_groups, group_id);
  else
    return NULL;
}

static gboolean
parse_bool (gchar * ptr, gboolean * val)
{
  if (!strcmp (ptr, "NO")) {
    *val = FALSE;
  } else if (!strcmp (ptr, "YES")) {
    *val = TRUE;
  } else {
    return FALSE;
  }
  return TRUE;
}

static gboolean
parse_int64 (gchar * ptr, gchar ** endptr, gint64 * val)
{
  gchar *end;

  errno = 0;

  *val = g_ascii_strtoll (ptr, &end, 10);
  if (endptr)
    *endptr = end;

  if (errno != 0)
    return FALSE;

  return end != ptr;
}

static gboolean
parse_int (gchar * ptr, gchar ** endptr, gint * val)
{
  gchar *end;
  gint64 v;

  errno = 0;

  v = g_ascii_strtoll (ptr, &end, 10);
  if (endptr)
    *endptr = end;

  *val = v;

  if (errno != 0)
    return FALSE;

  if (v > G_MAXINT || v < G_MININT)
    return FALSE;

  return end != ptr;
}

static gboolean
parse_double (gchar * ptr, gchar ** endptr, gdouble * val)
{
  gchar *end;

  errno = 0;

  *val = g_ascii_strtod (ptr, &end);
  if (endptr)
    *endptr = end;

  if (errno != 0)
    return FALSE;

  return end != ptr;
}

static gchar *
parse_line (gchar *data)
{
  gchar *endl;

  endl = strchr (data, '\n');
  if (endl) {
    *endl = '\0';
    if (endl > data && endl[-1] == '\r')
      endl[-1] = '\0';
    endl++;
  }

  return endl;
}

static gboolean
parse_byte_range (gchar * data, gchar ** endptr, gint64 *length, gint64 *offset)
{
  if (!parse_int64 (data, &data, length))
    return FALSE;

  if (*data == '\0') {
    *offset = -1;
  } else if (*data == '@') {
    if (!parse_int64 (data + 1, &data, offset))
      return FALSE;
  } else {
    return FALSE;
  }

  if (endptr)
    *endptr = data;

  return TRUE;
}

static gboolean
parse_resolution (gchar * data, gchar ** endptr, gint * width, gint * height)
{
  if (!parse_int (data, &data, width))
    return FALSE;

  if (*data != 'x')
    return FALSE;

  if (!parse_int (data + 1, &data, height))
    return FALSE;

  if (endptr)
    *endptr = data;

  return TRUE;
}

static gboolean
parse_attributes (gchar ** data, gchar ** a, gchar ** v)
{
  gchar *attr, *value, *equal;
  gchar *end;

  attr = *data;

  while (*attr == ' ')
    attr++;

  equal = strchr (attr, '=');
  if (!equal || equal == attr)
    return FALSE;

  value = equal + 1;
  if (*value == '"') {
    end = strchr (value + 1, '"');
    if (!end)
      return FALSE;
    end++;
  } else
    end = value;

  end = strchr (end, ',');
  if (end)
    *end++ = '\0';

  *equal = '\0';

  *a = attr;
  *v = value;
  *data = end;

  return TRUE;
}

static gboolean
strip_quotes (gchar ** data)
{
  gchar *start = *data;
  gchar *stop;

  if (*start != '"')
    return FALSE;

  stop = strchr (start + 1, '"');
  if (stop == NULL)
    return FALSE;

  *start = '\0';
  *stop = '\0';

  *data = start + 1;

  return TRUE;
}

static gboolean
parse_media_codec (const gchar * data,
    GstM3U8MediaCodec * codec, GstM3U8MediaType * type)
{
  gboolean ret = TRUE;

  if (!strcmp (data, "mp4a.40.2")) {
    *codec = GST_M3U8_MEDIA_CODEC_AAC_LC;
    *type = GST_M3U8_MEDIA_TYPE_AUDIO;
  } else if (!strcmp (data, "mp4a.40.5")) {
    *codec = GST_M3U8_MEDIA_CODEC_HE_AAC;
    *type = GST_M3U8_MEDIA_TYPE_AUDIO;
  } else if (!strcmp (data, "mp4a.40.34")) {
    *codec = GST_M3U8_MEDIA_CODEC_MP3;
    *type = GST_M3U8_MEDIA_TYPE_AUDIO;
  } else if (g_str_has_prefix (data, "mp4a")) {
    *codec = GST_M3U8_MEDIA_CODEC_GENERIC_AUDIO;
    *type = GST_M3U8_MEDIA_TYPE_AUDIO;
  } else if (!g_str_has_prefix (data, "avc1.42e0")) {
    *codec = GST_M3U8_MEDIA_CODEC_H264_BASE;
    *type = GST_M3U8_MEDIA_TYPE_VIDEO;
  } else if (!g_str_has_prefix (data, "avc1.4d40")) {
    *codec = GST_M3U8_MEDIA_CODEC_H264_MAIN;
    *type = GST_M3U8_MEDIA_TYPE_VIDEO;
  } else if (!g_str_has_prefix (data, "avc1.6400")) {
    *codec = GST_M3U8_MEDIA_CODEC_H264_HIGH;
    *type = GST_M3U8_MEDIA_TYPE_VIDEO;
  } else if (!g_str_has_prefix (data, "avc1")) {
    *codec = GST_M3U8_MEDIA_CODEC_GENERIC_H264;
    *type = GST_M3U8_MEDIA_TYPE_VIDEO;
  } else {
    ret = FALSE;
  }

  return ret;
}

static gboolean
gst_m3u8_variant_playlist_add_media (GstM3U8VariantPlaylist * playlist,
    GstM3U8Media *media)
{
  GstM3U8Media *first;
  GPtrArray *group;

  group = g_hash_table_lookup (playlist->rendition_groups, media->group_id);
  if (group == NULL) {
    group = g_ptr_array_new_with_free_func ((GDestroyNotify)
        gst_m3u8_media_free);
    g_hash_table_insert (playlist->rendition_groups, media->group_id, group);
  } else {
    first = g_ptr_array_index (group, 0);
    if (first->type != media->type)
      return FALSE;
  }

  g_ptr_array_add (group, media);

  if (media->uri) {
    media->playlist = gst_m3u8_playlist_new ();
    media->playlist->uri = g_strdup (media->uri);
  }

  return TRUE;
}

static gboolean
gst_m3u8_variant_playlist_parse (GstM3U8VariantPlaylist * playlist,
    gchar * data)
{
  GstM3U8Stream *stream;
  gchar *next;
  gboolean error;

  next = parse_line (data);
  if (strcmp (data, "#EXTM3U") != 0) {
    GST_ERROR ("data doesn't start with #EXTM3U");
    return FALSE;
  }

  stream = NULL;
  error = FALSE;

  for (data = next; next != NULL; data = next) {
    next = parse_line (data);
    if (*data == '\0')
      continue;

    GST_TRACE ("parsing `%s'", data);

    if (*data != '#') {
      GstM3U8Playlist *media_playlist;

      if (stream == NULL) {
        GST_DEBUG ("got URI line without EXT-X-STREAM-INF, dropping `%s'",
            data);
        continue;
      }

      media_playlist = gst_m3u8_playlist_new ();
      media_playlist->uri = uri_join (playlist->uri, data);

      stream->playlist = media_playlist;
      stream = NULL;

    } else if (g_str_has_prefix (data, "#EXT-X-VERSION:")) {
      gint version;

      if (parse_int (data + 15, NULL, &version)) {
        playlist->version = version;
        if (playlist->version > GST_M3U8_VERSION) {
          GST_ERROR ("unsupported playlist version %d", playlist->version);
          error = TRUE;
          break;
        }
      }

    } else if (g_str_has_prefix (data, "#EXT-X-MEDIA:")) {
      gchar *v, *a;
      GstM3U8Media *media;

      media = gst_m3u8_media_new ();
      media->type = -1;
      data += 13;

      while (data && parse_attributes (&data, &a, &v)) {
        if (!strcmp (a, "TYPE")) {
          if (!strcmp (v, "AUDIO"))
            media->type = GST_M3U8_MEDIA_TYPE_AUDIO;
          else if (!strcmp (v, "VIDEO"))
            media->type = GST_M3U8_MEDIA_TYPE_VIDEO;
          else if (!strcmp (v, "SUBTITLES"))
            media->type = GST_M3U8_MEDIA_TYPE_SUBTITLES;
        } else if (!strcmp (a, "GROUP-ID") && !media->group_id) {
          if (strip_quotes (&v)) {
            g_free (media->group_id);
            media->group_id = g_strdup (v);
          }
        } else if (!strcmp (a, "NAME")) {
          if (strip_quotes (&v)) {
            g_free (media->name);
            media->name = g_strdup (v);
          }
        } else if (!strcmp (a, "LANGUAGE")) {
          if (strip_quotes (&v)) {
            g_free (media->language);
            media->language = g_strdup (v);
          }
        } else if (!strcmp (a, "DEFAULT")) {
          if (!parse_bool (v, &media->is_default))
            GST_WARNING ("invalid DEFAULT value");
        } else if (!strcmp (a, "AUTOSELECT")) {
          if (!parse_bool (v, &media->autoselect))
            GST_WARNING ("invalid AUTOSELECT value");
        } else if (!strcmp (a, "FORCED")) {
          if (!parse_bool (v, &media->forced))
            GST_WARNING ("invalid FORCED value");
        } else if (!strcmp (a, "URI")) {
          if (strip_quotes (&v)) {
            g_free (media->uri);
            media->uri = uri_join (playlist->uri, v);
          }
        }
      }

      if (media->type == (GstM3U8MediaType) -1) {
        GST_WARNING ("media with no type, ignoring");
        gst_m3u8_media_free (media);
        continue;
      }

      if (media->group_id == NULL) {
        GST_WARNING ("media with no group id, ignoring");
        gst_m3u8_media_free (media);
        continue;
      }

      if (!gst_m3u8_variant_playlist_add_media (playlist, media)) {
        GST_WARNING ("invalid media for group %s, ignoring", media->group_id);
        gst_m3u8_media_free (media);
        continue;
      }

    } else if (g_str_has_prefix (data, "#EXT-X-STREAM-INF:") ||
        g_str_has_prefix (data, "#EXT-X-I-FRAME-STREAM-INF:")) {
      gchar *v, *a;

      if (stream != NULL) {
        GST_WARNING ("dropping stream with no URI");
        gst_m3u8_stream_free (stream);
      }

      stream = gst_m3u8_stream_new ();

      stream->i_frames_only = g_str_has_prefix (data,
          "#EXT-X-I-FRAME-STREAM-INF:");

      data += stream->i_frames_only ? 26 : 18;

      while (data && parse_attributes (&data, &a, &v)) {
        if (!strcmp (a, "BANDWIDTH")) {
          if (!parse_int (v, NULL, &stream->bandwidth))
            GST_WARNING ("invalid stream bandwidth `%s'", v);

        } else if (!strcmp (a, "PROGRAM-ID")) {
          if (!parse_int (v, NULL, &stream->program_id))
            GST_WARNING ("invalid stream program id `%s'", v);

        } else if (!strcmp (a, "CODECS")) {
          if (strip_quotes (&v)) {
            gchar **codecs;
            gint i;

            codecs = g_strsplit (v, ",", 3);

            for (i = 0; i < 3 && codecs[i] != NULL; i++) {
              GstM3U8MediaType type;
              GstM3U8MediaCodec codec;

              if (parse_media_codec (g_strstrip (codecs[i]), &codec, &type)) {
                if (type == GST_M3U8_MEDIA_TYPE_AUDIO)
                  stream->audio_codec = codec;
                else if (type == GST_M3U8_MEDIA_TYPE_VIDEO)
                  stream->video_codec = codec;
              }
            }
            g_strfreev (codecs);
          }

        } else if (!strcmp (a, "RESOLUTION")) {
          if (!parse_resolution (v, NULL, &stream->width, &stream->height))
            GST_WARNING ("invalid stream resolution `%s'", v);

        } else if (!strcmp (a, "VIDEO")) {
          if (strip_quotes (&v)) {
            g_free (stream->video);
            stream->video = g_strdup (v);
          }

        } else if (!stream->i_frames_only && !strcmp (a, "AUDIO")) {
          if (strip_quotes (&v)) {
            g_free (stream->audio);
            stream->audio = g_strdup (v);
          }

        } else if (!stream->i_frames_only && !strcmp (a, "SUBTITLES")) {
          if (strip_quotes (&v)) {
            g_free (stream->subtitles);
            stream->subtitles = g_strdup (v);
          }

        } else if (stream->i_frames_only && !strcmp (a, "URI")) {
          if (strip_quotes (&v))
            gst_m3u8_stream_set_uri (stream, uri_join (playlist->uri, v));
        }
      }

      if (stream->i_frames_only) {
        playlist->i_frame_streams =
            g_slist_prepend (playlist->i_frame_streams, stream);
        stream = NULL;
      } else {
        playlist->streams = g_slist_prepend (playlist->streams, stream);
      }

    } else {
      GST_LOG ("ignoring unsupported tag `%s'", data);
    }
  }

  if (stream != NULL) {
    GST_WARNING ("dropping stream with no URI");
    gst_m3u8_stream_free (stream);
  }

  if (error) {
    gst_m3u8_variant_playlist_cleanup (playlist);
  } else {
    playlist->streams = g_slist_reverse (playlist->streams);
    playlist->i_frame_streams = g_slist_reverse (playlist->i_frame_streams);
  }

  return TRUE;
}

static void
gst_m3u8_playlist_process (GstM3U8Playlist * playlist)
{
  GstClockTime max_duration;
  guint n_segments;
  GSList *l;

  n_segments = 0;
  max_duration = playlist->target_duration;
  playlist->duration = 0;

  for (l = playlist->segments; l; l = l->next) {
    GstM3U8Segment *segment = l->data;
    segment->sequence += playlist->media_sequence;
    playlist->duration += segment->duration;
    if (max_duration < segment->duration)
      max_duration = ((segment->duration / GST_SECOND) + 1) * GST_SECOND;
    n_segments++;
  }

  if (max_duration > playlist->target_duration) {
    GST_WARNING ("fixing target duration to %" GST_TIME_FORMAT,
        GST_TIME_ARGS (max_duration));
    playlist->target_duration = max_duration;
  }

  GST_DEBUG ("playlist version:%d type:%d endlist:%d allow-cache:%d "
      "iframes-only:%d target-duration:%" G_GINT64_FORMAT
      " media-sequence:%d, %u segments, total duration %" GST_TIME_FORMAT,
      playlist->version, playlist->type, playlist->endlist,
      playlist->allow_cache, playlist->i_frames_only,
      GST_TIME_AS_SECONDS (playlist->target_duration),
      playlist->media_sequence, n_segments,
      GST_TIME_ARGS (playlist->duration));
}

gboolean
gst_m3u8_playlist_update (GstM3U8Playlist * playlist, gchar * data,
    gboolean * updated)
{
  gchar *digest;
  gchar *next;
  gboolean bval;
  gdouble fval;
  gint ival;
  GstM3U8Key *key;
  GstM3U8Map *map;
  GstClockTime duration;
  gint64 offset, length;
  gboolean discont;
  guint sequence;
  gboolean error;

  next = parse_line (data);
  if (strcmp (data, "#EXTM3U") != 0) {
    GST_WARNING ("data doesn't start with #EXTM3U");
    if (updated)
      *updated = FALSE;
    return FALSE;
  }

  /* check if the data changed since last update */
  digest = g_compute_checksum_for_string (G_CHECKSUM_MD5, next, -1);
  if (!g_strcmp0 (playlist->digest, digest)) {
    GST_DEBUG ("playlist is the same as previous one");
    if (updated)
      *updated = FALSE;
    g_free (digest);
    return TRUE;
  }

  gst_m3u8_playlist_reset (playlist);
  playlist->digest = digest;
  playlist->download_ts = gst_util_get_timestamp ();

  duration = GST_CLOCK_TIME_NONE;
  sequence = 0;
  offset = 0;
  length = -1;
  discont = FALSE;
  key = NULL;
  map = NULL;
  error = FALSE;

  for (data = next; data != NULL; data = next) {
    next = parse_line (data);

    if (*data == '\0')
      continue;

    GST_TRACE ("parsing `%s'", data);

    if (*data != '#') {
      GstM3U8Segment *segment;

      if (duration == GST_CLOCK_TIME_NONE) {
        GST_DEBUG ("got URI line without EXTINF, dropping `%s'", data);
        continue;
      }

      segment = gst_m3u8_segment_new (uri_join (playlist->uri, data),
          duration, sequence++);

      if (length != -1) {
        segment->length = length;
        segment->offset = offset;
        offset += length;
      }

      segment->discont = discont;
      segment->key = key;
      segment->map = map;

      playlist->segments = g_slist_prepend (playlist->segments, segment);

      length = -1;
      duration = GST_CLOCK_TIME_NONE;
      discont = FALSE;

    } else if (!strcmp (data, "#EXT-X-ENDLIST")) {
      playlist->endlist = TRUE;

    } else if (g_str_has_prefix (data, "#EXT-X-VERSION:")) {
      if (parse_int (data + 15, NULL, &ival)) {
        playlist->version = ival;
        if (playlist->version > GST_M3U8_VERSION) {
          GST_ERROR ("unsupported playlist version %d", playlist->version);
          error = TRUE;
          break;
        }
      }

    } else if (g_str_has_prefix (data, "#EXT-X-PLAYLIST-TYPE:")) {
      if (!strcmp (data + 21, "VOD"))
        playlist->type = GST_M3U8_PLAYLIST_TYPE_VOD;
      else if (!strcmp (data + 21, "EVENT"))
        playlist->type = GST_M3U8_PLAYLIST_TYPE_EVENT;

    } else if (g_str_has_prefix (data, "#EXT-X-TARGETDURATION:")) {
      if (parse_int (data + 22, NULL, &ival))
        playlist->target_duration = ival * GST_SECOND;

    } else if (g_str_has_prefix (data, "#EXT-X-MEDIA-SEQUENCE:")) {
      if (parse_int (data + 22, NULL, &ival))
        playlist->media_sequence = ival;

    } else if (!strcmp (data, "#EXT-X-DISCONTINUITY")) {
       discont = TRUE;
       map = NULL;

    } else if (!strcmp (data, "#EXT-X-I-FRAMES-ONLY")) {
       playlist->i_frames_only = TRUE;

    } else if (g_str_has_prefix (data, "#EXT-X-PROGRAM-DATE-TIME:")) {
      if (playlist->datetime)
        gst_date_time_unref (playlist->datetime);
      playlist->datetime = gst_date_time_new_from_iso8601_string (data + 25);

    } else if (g_str_has_prefix (data, "#EXT-X-ALLOW-CACHE:")) {
      if (parse_bool (data + 19, &bval))
        playlist->allow_cache = bval;

    } else if (g_str_has_prefix (data, "#EXT-X-MAP:")) {
      GstM3U8Map *map;
      gchar *v, *a;

      map = gst_m3u8_map_new ();
      data += 11;

      while (data && parse_attributes (&data, &a, &v)) {
        if (!strcmp (a, "URI")) {
          if (strip_quotes (&v)) {
            g_free (map->uri);
            map->uri = uri_join (playlist->uri, v);
          }

        } else if (!strcmp (a, "BYTERANGE")) {
          if (strip_quotes (&v)) {
            if (!parse_byte_range (v, NULL, &map->length, &map->offset))
              GST_WARNING ("invalid map byte-range `%s'", v);
          }
        }
      }

      playlist->maps = g_slist_prepend (playlist->maps, map);

    } else if (g_str_has_prefix (data, "#EXT-X-KEY:")) {
      gchar *v, *a;

      key = gst_m3u8_key_new ();
      data += 11;

      while (data && parse_attributes (&data, &a, &v)) {
        if (!strcmp (a, "METHOD")) {
          if (!strcmp (v, "NONE"))
            key->method = GST_M3U8_KEY_METHOD_NONE;
          else if (!strcmp (v, "AES-128"))
            key->method = GST_M3U8_KEY_METHOD_AES_128;
          else if (!strcmp (v, "SAMPLE-AES"))
            key->method = GST_M3U8_KEY_METHOD_SAMPLE_AES;
          else
            key->method = GST_M3U8_KEY_METHOD_UNKNOWN;

        } else if (!strcmp (a, "URI")) {
          if (strip_quotes (&v)) {
            g_free (key->uri);
            key->uri = uri_join (playlist->uri, v);
          }

        } else if (!strcmp (a, "IV")) {
          g_free (key->iv);
          key->iv = g_ascii_strdown (v, -1);

        } else if (!strcmp (a, "KEYFORMAT")) {
          if (strip_quotes (&v)) {
            if (!strcmp (v, "identity"))
              key->format = GST_M3U8_KEY_FORMAT_IDENTITY;
            else
              key->format = GST_M3U8_KEY_FORMAT_UNKNOWN;
          }
        } else if (!strcmp (a, "KEYFORMATVERSIONS")) {
          GST_DEBUG ("ignoring KEYFORMATVERSIONS attribute: `%s'", v);
        }
      }

      playlist->keys = g_slist_prepend (playlist->keys, key);

    } else if (g_str_has_prefix (data, "#EXTINF:")) {
      if (!parse_double (data + 8, NULL, &fval)) {
        GST_WARNING ("can't read EXTINF duration");
        continue;
      }

      duration = fval * (gdouble) GST_SECOND;

    } else if (g_str_has_prefix (data, "#EXT-X-BYTERANGE:")) {
      gint64 range_offset;

      if (!parse_byte_range (data + 17, NULL, &length, &range_offset)) {
        GST_WARNING ("invalid byte-range `%s'", data + 17);
        continue;
      }

      if (range_offset != -1)
        offset = range_offset;

    } else {
      GST_LOG ("ignoring unsupported tag `%s'", data);
    }
  }

  if (error) {
    gst_m3u8_playlist_reset (playlist);
  } else {
    playlist->segments = g_slist_reverse (playlist->segments);
    gst_m3u8_playlist_process (playlist);
  }

  if (updated)
    *updated = TRUE;

  return !error;
}

static gboolean
gst_m3u8_is_variant_playlist (const gchar * data)
{
  /* A variant playlist must have at least one EXT-X-STREAM-INF */
  return strstr (data, "#EXT-X-STREAM-INF:") != NULL;
}

GstM3U8Client *
gst_m3u8_client_new (void)
{
  GstM3U8Client *client;

  client = g_new (GstM3U8Client, 1);
  client->stream = NULL;

  gst_m3u8_variant_playlist_init (&client->master_playlist);

  return client;
}

void
gst_m3u8_client_free (GstM3U8Client * client)
{
  g_return_if_fail (client != NULL);

  gst_m3u8_variant_playlist_cleanup (&client->master_playlist);
  g_free (client);
}

gboolean
gst_m3u8_client_parse_master_playlist (GstM3U8Client * client,
    gchar * data)
{
  g_return_val_if_fail (client != NULL, FALSE);

  if (!gst_m3u8_is_variant_playlist (data)) {
    GstM3U8Stream *stream;

    /* If if's a rendition playlist create a dummy stream and add it to
     * the variant playlist */
    GST_DEBUG ("parsing rendition playlist");
    stream = gst_m3u8_stream_new ();
    stream->playlist = gst_m3u8_playlist_new ();
    stream->playlist->uri = g_strdup (client->master_playlist.uri);

    if (!gst_m3u8_playlist_update (stream->playlist, data, NULL)) {
      gst_m3u8_stream_free (stream);
      return FALSE;
    }

    client->master_playlist.streams =
        g_slist_prepend (client->master_playlist.streams, stream);

  } else {
    /* Parse the variant playlist */
    GST_DEBUG ("parsing variant playlist");
    if (!gst_m3u8_variant_playlist_parse (&client->master_playlist, data))
      return FALSE;
  }

  return TRUE;
}

GstM3U8Stream *
gst_m3u8_client_select_stream (GstM3U8Client * client, gint max_bitrate)
{
  GstM3U8Stream *stream, *lq_stream;
  GSList *l;

  lq_stream = NULL;
  stream = NULL;

  if (max_bitrate <= 0)
    max_bitrate = G_MAXINT;

  for (l = client->master_playlist.streams; l != NULL; l = l->next) {
    GstM3U8Stream *s = (GstM3U8Stream *) l->data;

    if (!lq_stream || s->bandwidth < lq_stream->bandwidth)
      lq_stream = s;

    if (s->bandwidth < max_bitrate &&
        (!stream || s->bandwidth > stream->bandwidth))
      stream = s;
  }

  if (!stream)
    stream = lq_stream;

  client->stream = stream;

  return stream;
}

gboolean
gst_m3u8_client_guess_stream_media_type (GstM3U8Client * client,
    GstM3U8Stream * stream, GstM3U8MediaType * media_type)
{
  GstM3U8MediaType type;
  GstM3U8Media *media;
  GPtrArray *group;
  gboolean has_audio;
  gboolean has_video;
  guint i;
  gboolean ret = TRUE;

  /* First rely on media groups to know if a known media type uses the
   * stream playlist. This is better than relying on codecs information. */

  has_video = FALSE;
  group = gst_m3u8_variant_playlist_find_group (&client->master_playlist,
      stream->video);
  if (group) {
    for (i = 0; i < group->len; i++) {
      media = g_ptr_array_index (group, i);
      if (!media->uri) {
        has_video = TRUE;
        break;
      }
    }
  }

  if (has_video) {
    type = GST_M3U8_MEDIA_TYPE_VIDEO;
    goto out;
  }

  has_audio = FALSE;
  group = gst_m3u8_variant_playlist_find_group (&client->master_playlist,
      stream->audio);
  if (group) {
    for (i = 0; i < group->len; i++) {
      media = g_ptr_array_index (group, i);
      if (!media->uri) {
        has_audio = TRUE;
        break;
      }
    }
  }

  if (!has_audio) {
    type = GST_M3U8_MEDIA_TYPE_VIDEO;
    goto out;
  }

  /* Then rely on codec information if set */
  if (stream->video_codec != GST_M3U8_MEDIA_CODEC_NONE) {
    type = GST_M3U8_MEDIA_TYPE_VIDEO;
    goto out;
  }

  if (stream->video_codec == GST_M3U8_MEDIA_CODEC_NONE &&
      stream->audio_codec != GST_M3U8_MEDIA_CODEC_NONE) {
    type = GST_M3U8_MEDIA_TYPE_AUDIO;
    goto out;
  }

  /* Check if resolution is set */
  if (stream->width != 0 && stream->height != 0) {
    type = GST_M3U8_MEDIA_TYPE_VIDEO;
    goto out;
  }

  ret = FALSE;

out:
  if (ret && media_type)
    *media_type = type;

  return ret;
}
