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

#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstducatividdec.h"

GST_BOILERPLATE (GstDucatiVidDec, gst_ducati_viddec, GstElement,
    GST_TYPE_ELEMENT);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_YUV_STRIDED ("NV12", "[ 0, max ]"))
    );

/* helper functions */

static void
engine_close (GstDucatiVidDec * self)
{
  if (self->engine) {
    Engine_close (self->engine);
    self->engine = NULL;
  }

  if (self->params) {
    dce_free (self->params);
    self->params = NULL;
  }

  if (self->dynParams) {
    dce_free (self->dynParams);
    self->dynParams = NULL;
  }

  if (self->status) {
    dce_free (self->status);
    self->status = NULL;
  }

  if (self->inBufs) {
    dce_free (self->inBufs);
    self->inBufs = NULL;
  }

  if (self->outBufs) {
    dce_free (self->outBufs);
    self->outBufs = NULL;
  }

  if (self->inArgs) {
    dce_free (self->inArgs);
    self->inArgs = NULL;
  }

  if (self->outArgs) {
    dce_free (self->outArgs);
    self->outArgs = NULL;
  }
}

static gboolean
engine_open (GstDucatiVidDec * self)
{
  gboolean ret;

  if (G_UNLIKELY (self->engine)) {
    return TRUE;
  }

  GST_DEBUG_OBJECT (self, "opening engine");

  self->engine = Engine_open ("ivahd_vidsvr", NULL, NULL);
  if (G_UNLIKELY (!self->engine)) {
    GST_ERROR_OBJECT (self, "could not create engine");
    return FALSE;
  }

  ret = GST_DUCATIVIDDEC_GET_CLASS (self)->allocate_params (self,
      sizeof (IVIDDEC3_Params), sizeof (IVIDDEC3_DynamicParams),
      sizeof (IVIDDEC3_Status), sizeof (IVIDDEC3_InArgs),
      sizeof (IVIDDEC3_OutArgs));

  return ret;
}

static void
codec_delete (GstDucatiVidDec * self)
{
  if (self->codec) {
    //XXX this crashes ducati:
    //VIDDEC3_delete(self->codec);
    self->codec = NULL;
  }

  if (self->input) {
    MemMgr_Free (self->input);
    self->input = NULL;
  }
}

static gboolean
codec_create (GstDucatiVidDec * self)
{
  gint err;
  const gchar *codec_name;

  codec_delete (self);

  if (G_UNLIKELY (!self->engine)) {
    GST_ERROR_OBJECT (self, "no engine");
    return FALSE;
  }

  /* these need to be set before VIDDEC3_create */
  self->params->maxWidth = (self->width + 15) & ~0xf;   /* round up to MB */
  self->params->maxHeight = (self->height + 15) & ~0xf; /* round up to MB */

  codec_name = GST_DUCATIVIDDEC_GET_CLASS (self)->codec_name;

  /* create codec: */
  GST_DEBUG_OBJECT (self, "creating codec: %s", codec_name);
  self->codec = VIDDEC3_create (self->engine, (char *)codec_name, self->params);

  if (!self->codec) {
    return FALSE;
  }

  err = VIDDEC3_control(self->codec, XDM_SETPARAMS, self->dynParams, self->status);
  if (err) {
    GST_ERROR_OBJECT (self, "failed XDM_SETPARAMS");
    return FALSE;
  }

#if 0
  /* not entirely sure why we need to call this here.. just copying omx.. */
  err = VIDDEC3_control(self->codec, XDM_GETBUFINFO, self->dynParams, self->status);
  if (err) {
    GST_ERROR_OBJECT (self, "failed XDM_GETBUFINFO");
    return FALSE;
  }
#endif

  /* allocate input buffer and initialize inBufs: */
  self->inBufs->numBufs = 1;
  self->input = gst_ducati_alloc_1d (self->width * self->height);
  self->inBufs->descs[0].buf = (XDAS_Int8 *) TilerMem_VirtToPhys (self->input);
  self->inBufs->descs[0].memType = XDM_MEMTYPE_RAW;

  return TRUE;
}

static XDAS_Int32
codec_prepare_outbuf (GstDucatiVidDec * self, GstBuffer * buf)
{
  XDAS_Int16 y_type, uv_type;
  guint8 *y_vaddr, *uv_vaddr;
  SSPtr y_paddr, uv_paddr;

  y_vaddr = GST_BUFFER_DATA (buf);
  uv_vaddr = y_vaddr + self->stride * self->padded_height;

  y_paddr = TilerMem_VirtToPhys (y_vaddr);
  uv_paddr = TilerMem_VirtToPhys (uv_vaddr);

  y_type = gst_ducati_get_mem_type (y_paddr);
  uv_type = gst_ducati_get_mem_type (uv_paddr);

  if ((y_type < 0) || (uv_type < 0)) {
    return 0;
  }

  if (!self->outBufs->numBufs) {
    /* initialize output buffer type */
    self->outBufs->numBufs = 2;
    self->outBufs->descs[0].memType = y_type;
    self->outBufs->descs[0].bufSize.tileMem.width = self->padded_width;
    self->outBufs->descs[0].bufSize.tileMem.height = self->padded_height;
    self->outBufs->descs[1].memType = uv_type;
    /* note that UV interleaved width is same a Y: */
    self->outBufs->descs[1].bufSize.tileMem.width = self->padded_width;
    self->outBufs->descs[1].bufSize.tileMem.height = self->padded_height / 2;
  } else {
    /* verify output buffer type matches what we've already given
     * to the codec
     */
    // TODO
  }

  self->outBufs->descs[0].buf = (XDAS_Int8 *)y_paddr;
  self->outBufs->descs[1].buf = (XDAS_Int8 *)uv_paddr;

  return (XDAS_Int32) buf;      // XXX use lookup table
}

static GstBuffer *
codec_get_outbuf (GstDucatiVidDec * self, XDAS_Int32 id)
{
  GstBuffer *buf = (GstBuffer *) id;    // XXX use lookup table
  if (buf) {
    gst_buffer_ref (buf);
  }
  return buf;
}

static void
codec_unlock_outbuf (GstDucatiVidDec * self, XDAS_Int32 id)
{
  GstBuffer *buf = (GstBuffer *) id;    // XXX use lookup table
  if (buf) {
    gst_buffer_unref (buf);
  }
}

static gboolean
codec_flush (GstDucatiVidDec * self)
{
  if (G_UNLIKELY (!self->codec)) {
    GST_WARNING_OBJECT (self, "no codec");
    return TRUE;
  }

  GST_WARNING_OBJECT (self, "TODO");

  return FALSE;
}

/* GstDucatiVidDec vmethod default implementations */

static gboolean
gst_ducati_viddec_allocate_params (GstDucatiVidDec * self, gint params_sz,
    gint dynparams_sz, gint status_sz, gint inargs_sz, gint outargs_sz)
{

  /* allocate params: */
  self->params = dce_alloc (params_sz);
  if (G_UNLIKELY (!self->params)) {
    return FALSE;
  }
  self->params->size = params_sz;
  self->params->maxFrameRate = 30000;
  //h264, mpeg4:
  //note mpeg4 and h263 are same..
  self->params->maxBitRate = 10000000;
  //vc1:
  //self->params->maxBitRate = 45000000;

  self->params->dataEndianness = XDM_BYTE;
  self->params->forceChromaFormat = XDM_YUV_420SP;
  self->params->operatingMode = IVIDEO_DECODE_ONLY;

  //mpeg4:
  //self->params->displayDelay = IVIDDEC3_DISPLAY_DELAY_1;

  //vc1:
  //self->params->displayDelay = IVIDDEC3_DISPLAY_DELAY_1;


  self->params->displayBufsMode = IVIDDEC3_DISPLAYBUFS_EMBEDDED;
  self->params->inputDataMode = IVIDEO_ENTIREFRAME;
  self->params->outputDataMode = IVIDEO_ENTIREFRAME;
  self->params->numInputDataUnits = 0;
  self->params->numOutputDataUnits = 0;

  //vp6, vp7:
  //self->params->numInputDataUnits = 1;
  //self->params->numOutputDataUnits = 1;

  self->params->metadataType[0] = IVIDEO_METADATAPLANE_NONE;
  self->params->metadataType[1] = IVIDEO_METADATAPLANE_NONE;
  self->params->metadataType[2] = IVIDEO_METADATAPLANE_NONE;
  self->params->errorInfoMode = IVIDEO_ERRORINFO_OFF;

  //mpeg4:
  //((IMPEG4VDEC_Params *) self->params)->outloopDeBlocking = 0;
  //((IMPEG4VDEC_Params *) self->params)->sorensonSparkStream = 0;
  //((IMPEG4VDEC_Params *) self->params)->ErrorConcealmentON = 1;


  /* allocate dynParams: */
  self->dynParams = dce_alloc (dynparams_sz);
  if (G_UNLIKELY (!self->dynParams)) {
    return FALSE;
  }
  self->dynParams->size = dynparams_sz;
  self->dynParams->decodeHeader = XDM_DECODE_AU;
  self->dynParams->displayWidth = 0;
  self->dynParams->frameSkipMode = IVIDEO_NO_SKIP;
  self->dynParams->newFrameFlag = XDAS_TRUE;

  //mpeg4:
  //self->dynParams->lateAcquireArg = IRES_HDVICP2_UNKNOWNLATEACQUIREARG;

  /* allocate status: */
  self->status = dce_alloc (status_sz);
  if (G_UNLIKELY (!self->status)) {
    return FALSE;
  }
  self->status->size = status_sz;

  /* allocate inBufs/outBufs: */
  self->inBufs = dce_alloc (sizeof (XDM2_BufDesc));
  self->outBufs = dce_alloc (sizeof (XDM2_BufDesc));
  if (G_UNLIKELY (!self->inBufs) || G_UNLIKELY (!self->outBufs)) {
    return FALSE;
  }

  /* allocate inArgs/outArgs: */
  self->inArgs = dce_alloc (inargs_sz);
  self->outArgs = dce_alloc (outargs_sz);
  if (G_UNLIKELY (!self->inArgs) || G_UNLIKELY (!self->outArgs)) {
    return FALSE;
  }
  self->inArgs->size = inargs_sz;
  self->outArgs->size = outargs_sz;
}

static GstBuffer *
gst_ducati_viddec_push_input (GstDucatiVidDec * self, GstBuffer * buf)
{
  /* just copy entire buffer */
  self->inArgs->numBytes = GST_BUFFER_SIZE (buf);
  self->inBufs->descs[0].bufSize.bytes = self->inArgs->numBytes;
  GST_DEBUG_OBJECT (self, "push: %d bytes)", self->inArgs->numBytes);
  memcpy (self->input, GST_BUFFER_DATA (buf), self->inArgs->numBytes);
  gst_buffer_unref (buf);
  return NULL;
}

/* GstElement vmethod implementations */

static gboolean
gst_ducati_viddec_set_caps (GstPad * pad, GstCaps * caps)
{
  gboolean ret = TRUE;
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (gst_pad_get_parent (pad));
  GstStructure *s;

  g_return_val_if_fail (caps, FALSE);
  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);

  s = gst_caps_get_structure (caps, 0);

  if (pad == self->sinkpad) {
    gint width, height, frn, frd;
    GST_INFO_OBJECT (self, "setcaps (sink): %" GST_PTR_FORMAT, caps);

    if (gst_structure_get_int (s, "width", &width) &&
        gst_structure_get_int (s, "height", &height) &&
        gst_structure_get_fraction (s, "framerate", &frn, &frd)) {
      GstCaps *outcaps;

      /* ok, these caps seem sane.. grab the required values and construct
       * appropriate output caps
       */
      self->width = width;
      self->height = height;
      self->stride = 4096;      /* TODO: don't hardcode */

      /* update output/padded sizes:
       */
      GST_DUCATIVIDDEC_GET_CLASS (self)->update_buffer_size (self);

      self->outsize =
          GST_ROUND_UP_2 (self->stride * self->padded_height * 3) / 2;

      outcaps = gst_caps_new_simple ("video/x-raw-yuv-strided",
          "rowstride", G_TYPE_INT, self->stride,
          "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('N','V','1','2'),
          "width", G_TYPE_INT, self->padded_width,
          "height", G_TYPE_INT, self->padded_height,
          "framerate", GST_TYPE_FRACTION, frn, frd,
          NULL);

      GST_DEBUG_OBJECT (self, "outcaps: %" GST_PTR_FORMAT, outcaps);

      ret = gst_pad_set_caps (self->srcpad, outcaps);
      gst_caps_unref (outcaps);

      if (!ret) {
        GST_WARNING_OBJECT (self, "failed to set caps");
        return FALSE;
      }
    } else {
      GST_WARNING_OBJECT (self, "missing required fields");
      return FALSE;
    }
  } else {
    GST_INFO_OBJECT (self, "setcaps (src): %" GST_PTR_FORMAT, caps);
    // XXX check to make sure caps are ok.. keep track if we
    // XXX need to handle unstrided buffers..
    GST_WARNING_OBJECT (self, "TODO");
  }

  gst_object_unref (self);

  return gst_pad_set_caps (pad, caps);
}

static gboolean
gst_ducati_viddec_query (GstPad * pad, GstQuery * query)
{
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (GST_OBJECT_PARENT (pad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_BUFFERS:
      GST_DEBUG_OBJECT (self, "min buffers: %d", self->min_buffers);
      gst_query_set_buffers_count (query, self->min_buffers);

      GST_DEBUG_OBJECT (self, "min dimensions: %dx%d",
          self->padded_width, self->padded_height);
      gst_query_set_buffers_dimensions (query,
          self->padded_width, self->padded_height);
      return TRUE;
    default:
      return FALSE;
  }
}

static GstFlowReturn
gst_ducati_viddec_chain (GstPad * pad, GstBuffer * buf)
{
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (GST_OBJECT_PARENT (pad));
  GstFlowReturn ret;
  Int32 err;
  gint i;
  GstBuffer *outbuf = NULL;

  if (G_UNLIKELY (!self->engine)) {
    GST_ERROR_OBJECT (self, "no engine");
    return GST_FLOW_ERROR;
  }

  /* do this before creating codec to ensure reverse caps negotiation
   * happens first:
   */
  ret = gst_pad_alloc_buffer_and_set_caps (self->srcpad, 0, self->outsize,
      GST_PAD_CAPS (self->srcpad), &outbuf);

  if (ret != GST_FLOW_OK) {
    /* TODO: if we had our own buffer class, we could allocate our own
     * output buffer from TILER...
     */
 GST_WARNING_OBJECT (self, "ret=%d", ret);
    GST_WARNING_OBJECT (self, "TODO: allocate output TILER buffer");
    return ret;
  }

  if (G_UNLIKELY (!self->codec)) {
    if (!codec_create (self)) {
      GST_ERROR_OBJECT (self, "could not create codec");
      return GST_FLOW_ERROR;
    }
  }

  GST_BUFFER_TIMESTAMP (outbuf) = GST_BUFFER_TIMESTAMP (buf);
  GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (buf);

  self->inArgs->inputID = codec_prepare_outbuf (self, outbuf);
  if (!self->inArgs->inputID) {
    GST_ERROR_OBJECT (self, "could not prepare output buffer");
    return GST_FLOW_ERROR;
  }

  buf = GST_DUCATIVIDDEC_GET_CLASS (self)->push_input (self, buf);

  if (buf) {
    // XXX
    GST_WARNING_OBJECT (self, "TODO.. can't push more than one.. need loop");
    gst_buffer_unref (buf);
    buf = NULL;
  }

  //XXX t = mark (NULL);
  err = VIDDEC3_process (self->codec,
      self->inBufs, self->outBufs,
      self->inArgs, self->outArgs);
  //XXX GST_DEBUG_OBJECT (self, "processed returned in: %dus", mark (&t));
  if (err) {
    GST_ERROR_OBJECT (self, "process returned error: %d %08x",
        err, self->outArgs->extendedError);
    return GST_FLOW_ERROR;
  }

  for (i = 0; self->outArgs->outputID[i]; i++) {
#if 0
    /* calculate offset to region of interest */
    XDM_Rect *r = &(outArgs->displayBufs.bufDesc[0].activeFrameRegion);
    int yoff = (r->topLeft.y * 4096) + r->topLeft.x;
    int uvoff = (r->topLeft.y * 4096 / 2) + r->topLeft.x;
    // XXX do something..
#endif

    outbuf = codec_get_outbuf (self, self->outArgs->outputID[i]);
    gst_pad_push (self->srcpad, outbuf);
  }

  for (i = 0; self->outArgs->freeBufID[i]; i++) {
    codec_unlock_outbuf (self, self->outArgs->freeBufID[i]);
  }

  if (self->outArgs->outBufsInUseFlag) {
    GST_WARNING_OBJECT (self, "TODO... outBufsInUseFlag");      // XXX
  }

  return GST_FLOW_OK;
}

static gboolean
gst_ducati_viddec_event (GstPad * pad, GstEvent * event)
{
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (GST_OBJECT_PARENT (pad));
  gboolean ret = TRUE;

  GST_INFO_OBJECT (self, "begin: event=%s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
    case GST_EVENT_FLUSH_STOP:
      if (!codec_flush (self)) {
        GST_ERROR_OBJECT (self, "could not flush");
        return FALSE;
      }
      /* fall-through */
    default:
      ret = gst_pad_push_event (self->srcpad, event);
      break;
  }

  GST_LOG_OBJECT (self, "end");

  return ret;
}

static GstStateChangeReturn
gst_ducati_viddec_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (element);

  GST_INFO_OBJECT (self, "begin: changing state %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!engine_open (self)) {
        GST_ERROR_OBJECT (self, "could not open");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    goto leave;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      codec_delete (self);
      engine_close (self);
      break;
    default:
      break;
  }

leave:
  GST_LOG_OBJECT (self, "end");

  return ret;
}

/* GObject vmethod implementations */

static void
gst_ducati_viddec_finalize (GObject * obj)
{
  GstDucatiVidDec *self = GST_DUCATIVIDDEC (obj);

  codec_delete (self);
  engine_close (self);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_ducati_viddec_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
}

static void
gst_ducati_viddec_class_init (GstDucatiVidDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_ducati_viddec_finalize;
  gstelement_class->change_state = gst_ducati_viddec_change_state;

  klass->allocate_params =
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_allocate_params);
  klass->push_input =
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_push_input);
}

static void
gst_ducati_viddec_init (GstDucatiVidDec * self, GstDucatiVidDecClass * klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  self->sinkpad = gst_pad_new_from_template (
      gst_element_class_get_pad_template (gstelement_class, "sink"), "sink");
  gst_pad_set_setcaps_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_set_caps));
  gst_pad_set_chain_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_chain));
  gst_pad_set_event_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_event));

  self->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_pad_set_setcaps_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_ducati_viddec_set_caps));
  gst_pad_set_query_function (self->srcpad,
          GST_DEBUG_FUNCPTR (gst_ducati_viddec_query));

  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
}
