/*
 *  vaapidecoder_vp9.cpp - vp9 decoder
 *
 *  Copyright (C) 2013-2014 Intel Corporation
 *    Author: XuGuangxin<Guangxin.Xu@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#ifdef ANDROID
#include <functional>
#else
#include <tr1/functional>
#endif
#include "common/log.h"
#include "vaapidecoder_vp9.h"
#include "vaapidecoder_factory.h"

namespace YamiMediaCodec{
typedef VaapiDecoderVP9::PicturePtr PicturePtr;

VaapiDecoderVP9::VaapiDecoderVP9()
{
    m_parser.reset(vp9_parser_new(), vp9_parser_free);
    m_reference.resize(VP9_REF_FRAMES);
}

VaapiDecoderVP9::~VaapiDecoderVP9()
{
    stop();
}


Decode_Status VaapiDecoderVP9::start(VideoConfigBuffer * buffer)
{
    DEBUG("VP9: start() buffer size: %d x %d", buffer->width,
          buffer->height);

    buffer->profile = VAProfileVP9Profile0;
    //8 reference frame + extra number
    buffer->surfaceNumber = 8 + VP9_EXTRA_SURFACE_NUMBER;


    DEBUG("disable native graphics buffer");
    buffer->flag &= ~USE_NATIVE_GRAPHIC_BUFFER;
    m_configBuffer = *buffer;
    m_configBuffer.data = NULL;
    m_configBuffer.size = 0;

    if (m_configBuffer.width && m_configBuffer.height) {
        m_configBuffer.surfaceWidth = ALIGN8(m_configBuffer.width);
        m_configBuffer.surfaceHeight = ALIGN32(m_configBuffer.height);
        Decode_Status status = VaapiDecoderBase::start(&m_configBuffer);
        if (status != DECODE_SUCCESS)
            return status;
    }

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderVP9::reset(VideoConfigBuffer * buffer)
{
    DEBUG("VP9: reset()");
    return VaapiDecoderBase::reset(buffer);
}

void VaapiDecoderVP9::stop(void)
{
    DEBUG("VP9: stop()");
    flush();
    VaapiDecoderBase::stop();
}

void VaapiDecoderVP9::flush(void)
{
    m_parser.reset(vp9_parser_new(), vp9_parser_free);
    m_reference.clear();
    m_reference.resize(VP9_REF_FRAMES);
    VaapiDecoderBase::flush();
}


Decode_Status VaapiDecoderVP9::ensureContext(const Vp9FrameHdr* hdr)
{
    // only reset va context when there is a larger frame
    if (m_configBuffer.width < hdr->width
        || m_configBuffer.height <  hdr->height) {
        INFO("frame size changed, reconfig codec. orig size %d x %d, new size: %d x %d",
                m_configBuffer.width, m_configBuffer.height, hdr->width, hdr->height);
        Decode_Status status = VaapiDecoderBase::terminateVA();
        if (status != DECODE_SUCCESS)
            return status;
        m_configBuffer.width = hdr->width;
        m_configBuffer.height = hdr->height;
        m_configBuffer.surfaceWidth = ALIGN8(hdr->width);
        m_configBuffer.surfaceHeight = ALIGN32(hdr->height);
        status = VaapiDecoderBase::start(&m_configBuffer);
        if (status != DECODE_SUCCESS)
            return status;
        return DECODE_FORMAT_CHANGE;
    } else if ((m_videoFormatInfo.width != hdr->width
        || m_videoFormatInfo.height != hdr->height) &&
        (!hdr->show_existing_frame)) {
        // notify client of resolution change, no need to reset hw context
            INFO("frame size changed, reconfig codec. orig size %d x %d, new size: %d x %d\n", m_videoFormatInfo.width, m_videoFormatInfo.height, hdr->width, hdr->height);
            m_videoFormatInfo.width = hdr->width;
            m_videoFormatInfo.height = hdr->height;
            return DECODE_FORMAT_CHANGE;
    }
    return DECODE_SUCCESS;

}

bool VaapiDecoderVP9::fillReference(VADecPictureParameterBufferVP9* param, const Vp9FrameHdr* hdr)
{
#define FILL_REFERENCE(vafield, ref) \
    do { \
        int idx = ref - VP9_LAST_FRAME; \
        if (!m_reference[idx]) { \
            ERROR("reference to %d is invalid", idx); \
            return false; \
        } \
        param->pic_fields.bits.vafield = hdr->ref_frame_indices[idx]; \
        param->pic_fields.bits.vafield##_sign_bias = hdr->ref_frame_sign_bias[idx]; \
    } while (0)


    if (hdr->frame_type == VP9_KEY_FRAME) {
        m_reference.clear();
        m_reference.resize(VP9_REF_FRAMES);
    } else {
        FILL_REFERENCE(last_ref_frame, VP9_LAST_FRAME);
        FILL_REFERENCE(golden_ref_frame, VP9_GOLDEN_FRAME);
        FILL_REFERENCE(alt_ref_frame, VP9_ALTREF_FRAME);
    }
    for (size_t i = 0; i < m_reference.size(); i++) {
        SurfacePtr& surface = m_reference[i];
        param->reference_frames[i] = surface.get() ? surface->getID():VA_INVALID_SURFACE;
    }
    return true;
#undef FILL_REFERENCE
}

void VaapiDecoderVP9::updateReference(const PicturePtr& picture, const Vp9FrameHdr* hdr)
{
    uint8_t flag  = 1;
    uint8_t refresh_frame_flags;
    if (hdr->frame_type == VP9_KEY_FRAME) {
        refresh_frame_flags = 0xff;
    } else {
        refresh_frame_flags = hdr->refresh_frame_flags;
    }

    for (int i = 0; i < VP9_REF_FRAMES; i++) {
        if (refresh_frame_flags & flag) {
            m_reference[i] = picture->getSurface();
        }
        flag <<= 1;
    }
}

bool VaapiDecoderVP9::ensurePicture(const PicturePtr& picture, const Vp9FrameHdr* hdr)
{
    VADecPictureParameterBufferVP9* param;
    if (!picture->editPicture(param))
        return false;
    param->frame_width = hdr->width;
    param->frame_height = hdr->height;
    if (!fillReference(param, hdr))
        return false;


#define FILL_PIC_FIELD(field) param->pic_fields.bits.field = hdr->field;
    FILL_PIC_FIELD(subsampling_x)
    FILL_PIC_FIELD(subsampling_y)
    FILL_PIC_FIELD(frame_type)
    FILL_PIC_FIELD(show_frame)
    FILL_PIC_FIELD(error_resilient_mode)
    FILL_PIC_FIELD(intra_only)
    FILL_PIC_FIELD(allow_high_precision_mv)
    FILL_PIC_FIELD(mcomp_filter_type)
    FILL_PIC_FIELD(frame_parallel_decoding_mode)
    FILL_PIC_FIELD(reset_frame_context)
    FILL_PIC_FIELD(refresh_frame_context)
    FILL_PIC_FIELD(frame_context_idx)
#undef FILL_PIC_FIELD

    param->pic_fields.bits.segmentation_enabled = hdr->segmentation.enabled;
    param->pic_fields.bits.segmentation_temporal_update = hdr->segmentation.temporal_update;
    param->pic_fields.bits.segmentation_update_map = hdr->segmentation.update_map;
    param->pic_fields.bits.lossless_flag = m_parser->lossless_flag;

    param->filter_level = hdr->loopfilter.filter_level;
    param->sharpness_level = hdr->loopfilter.sharpness_level;


#define FILL_FIELD(field) param->field = hdr->field;
    FILL_FIELD(log2_tile_rows);
    FILL_FIELD(log2_tile_columns);
    FILL_FIELD(frame_header_length_in_bytes)
    FILL_FIELD(first_partition_size)
#undef FILL_FIELD
    assert(sizeof(param->mb_segment_tree_probs) == sizeof(m_parser->mb_segment_tree_probs));
    assert(sizeof(param->segment_pred_probs) == sizeof(m_parser->segment_pred_probs));
    memcpy(param->mb_segment_tree_probs, m_parser->mb_segment_tree_probs, sizeof(m_parser->mb_segment_tree_probs));
    memcpy(param->segment_pred_probs, m_parser->segment_pred_probs, sizeof(m_parser->segment_pred_probs));

    return true;
}

bool VaapiDecoderVP9::ensureSlice(const PicturePtr& picture, const void* data, int size)
{
#define FILL_FIELD(field) vaseg.field = seg.field;

    VASliceParameterBufferVP9* slice;
    if (!picture->newSlice(slice, data, size))
        return false;
    for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
        VASegmentParameterVP9& vaseg = slice->seg_param[i];
        Vp9Segmentation& seg = m_parser->segmentation[i];
        memcpy(vaseg.filter_level, seg.filter_level, sizeof(seg.filter_level));
        FILL_FIELD(luma_ac_quant_scale)
        FILL_FIELD(luma_dc_quant_scale)
        FILL_FIELD(chroma_ac_quant_scale)
        FILL_FIELD(chroma_dc_quant_scale)

        vaseg.segment_flags.fields.segment_reference_skipped = seg.reference_skip;
        vaseg.segment_flags.fields.segment_reference_enabled = seg.reference_frame_enabled;
        vaseg.segment_flags.fields.segment_reference = seg.reference_frame;

    }
#undef FILL_FIELD
    return true;
}

Decode_Status VaapiDecoderVP9::decode(const Vp9FrameHdr* hdr, const uint8_t* data, uint32_t size, uint64_t timeStamp)
{


    Decode_Status ret;
    ret = ensureContext(hdr);
    if (ret != DECODE_SUCCESS)
        return ret;

    PicturePtr picture = createPicture(timeStamp);
    if (!picture)
        return DECODE_MEMORY_FAIL;

    if (hdr->show_existing_frame) {
        SurfacePtr& surface = m_reference[hdr->frame_to_show];
        if (!surface) {
            ERROR("frame to show is invalid, idx = %d", hdr->frame_to_show);
            return DECODE_SUCCESS;
        }
        picture->setSurface(surface);
        return outputPicture(picture);
    }

    if (!picture->getSurface()->setCrop(0, 0, hdr->width, hdr->height)) {
        ERROR("resize to %dx%d failed", hdr->width, hdr->height);
        return DECODE_MEMORY_FAIL;
    }

    if (!ensurePicture(picture, hdr))
        return DECODE_FAIL;
    if (!ensureSlice(picture, data, size))
        return DECODE_FAIL;
    ret = picture->decode();
    if (ret != DECODE_SUCCESS)
        return ret;
    updateReference(picture, hdr);
    if (hdr->show_frame)
        return outputPicture(picture);
    return DECODE_SUCCESS;
}

static bool parse_super_frame(std::vector<uint32_t>& frameSize, const uint8_t* data, const size_t size)
{
    if (!data || !size)
        return false;
    uint8_t marker;
    marker =  *(data + size - 1);
    if ((marker & 0xe0) != 0xc0) {
        frameSize.push_back(size);
        return true;
    }
    const uint32_t frames = (marker & 0x7) + 1;
    const uint32_t mag = ((marker >> 3) & 0x3) + 1;
    const size_t indexSz = 2 + mag * frames;
    if (size < indexSz)
        return false;
    data += size - indexSz;
    const uint8_t marker2 = *data++;
    if (marker != marker2)
        return false;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t sz = 0;
        for (uint32_t j = 0; j < mag; j++) {
            sz |= (*data++) << (j * 8);
        }
        frameSize.push_back(sz);
    }
    return true;
}

Decode_Status VaapiDecoderVP9::decode(VideoDecodeBuffer * buffer)
{
    Decode_Status status;
    if (!buffer)
        return DECODE_INVALID_DATA;
    uint8_t* data = buffer->data;
    size_t  size = buffer->size;
    uint8_t* end = data + size;
    std::vector<uint32_t> frameSize;
    if (!parse_super_frame(frameSize, data, size))
        return DECODE_INVALID_DATA;
    for (size_t i = 0; i < frameSize.size(); i++) {
        uint32_t sz = frameSize[i];
        if (data + sz > end)
            return DECODE_INVALID_DATA;
        status = decode(data, sz, buffer->timeStamp);
        if (status != DECODE_SUCCESS)
            return status;
        data += sz;
    }
    return DECODE_SUCCESS;

}

Decode_Status VaapiDecoderVP9::decode(const uint8_t* data, uint32_t size, uint64_t timeStamp)
{
    Vp9FrameHdr hdr;
    if (!m_parser)
        return DECODE_MEMORY_FAIL;
    if (vp9_parse_frame_header(m_parser.get(), &hdr, data, size) != VP9_PARSER_OK)
        return DECODE_INVALID_DATA;
    if (hdr.first_partition_size + hdr.frame_header_length_in_bytes > size)
        return DECODE_INVALID_DATA;
    return decode(&hdr, data, size, timeStamp);
}

const bool VaapiDecoderVP9::s_registered =
    VaapiDecoderFactory::register_<VaapiDecoderVP9>(YAMI_MIME_VP9);

}
