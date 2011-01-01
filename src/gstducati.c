/*
 * GStreamer
 * Copyright (c) 2010, Texas Instruments Incorporated
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstducati.h"
#include "gstducatih264dec.h"
#include "gstducatimpeg4dec.h"
#include "gstducatimpeg2dec.h"
#include "gstducativc1dec.h"
#include "gstducativp6dec.h"
#include "gstducativp7dec.h"
#include "gstducatirvdec.h"

GST_DEBUG_CATEGORY (gst_ducati_debug);

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_ducati_debug, "ducati", 0, "ducati");

  /* TODO .. find some way to reasonably detect if the corresponding
   * codecs are actually available..
   */
  return gst_element_register (plugin, "ducatih264dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIH264DEC) &&
      gst_element_register (plugin, "ducatimpeg4dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIMPEG4DEC) &&
      gst_element_register (plugin, "ducatimpeg2dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIMPEG2DEC) &&
      gst_element_register (plugin, "ducativc1dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIVC1DEC) &&
      gst_element_register (plugin, "ducativp6dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIVP6DEC) &&
      gst_element_register (plugin, "ducativp7dec", GST_RANK_PRIMARY, GST_TYPE_DUCATIVP7DEC) &&
      gst_element_register (plugin, "ducatirvdec", GST_RANK_PRIMARY, GST_TYPE_DUCATIRVDEC);
}

void *
gst_ducati_alloc_1d (gint sz)
{
  MemAllocBlock block = {
    .pixelFormat = PIXEL_FMT_PAGE,
    .dim.len = sz,
  };
  return MemMgr_Alloc (&block, 1);
}

void *
gst_ducati_alloc_2d (gint width, gint height, guint * sz)
{
  MemAllocBlock block[] = { {
          .pixelFormat = PIXEL_FMT_8BIT,
          .dim = {.area = {
                      .width = width,
                      .height = ALIGN2 (height, 1),
                  }},
          .stride = 4096
      }, {
        .pixelFormat = PIXEL_FMT_16BIT,
        .dim = {.area = {
                    .width = width,
                    .height = ALIGN2 (height, 1) / 2,
                }},
        .stride = 4096
      }
  };
  if (sz) {
    *sz = (4096 * ALIGN2 (height, 1) * 3) / 2;
  }
  return MemMgr_Alloc (block, 2);
}

XDAS_Int16
gst_ducati_get_mem_type (SSPtr paddr)
{
  if ((0x60000000 <= paddr) && (paddr < 0x68000000))
    return XDM_MEMTYPE_TILED8;
  if ((0x68000000 <= paddr) && (paddr < 0x70000000))
    return XDM_MEMTYPE_TILED16;
  if ((0x70000000 <= paddr) && (paddr < 0x78000000))
    return XDM_MEMTYPE_TILED32;
  if ((0x78000000 <= paddr) && (paddr < 0x80000000))
    return XDM_MEMTYPE_RAW;
  return -1;
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#  define PACKAGE "ducati"
#endif

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "ducati",
    "Hardware accellerated codecs for OMAP4",
    plugin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
