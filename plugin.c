#include <gst/gst.h>

#include "gsthlsdemux.h"

GST_DEBUG_CATEGORY (gst_hls_m3u8);

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_hls_m3u8, "m3u8",
      (GST_DEBUG_BOLD | GST_DEBUG_FG_GREEN), "M3U8 playlist");

  if (!gst_element_register (plugin, "pochlsdemux", GST_RANK_PRIMARY + 1,
          GST_TYPE_HLS_DEMUX))
    return FALSE;

  return TRUE;
}

#define PACKAGE "HLSPOC"

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    hls, "HLS elements", plugin_init,
    "1.0", "LGPL", "HLS POC", "http://github.com/rawoul");
