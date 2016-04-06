/*
 *  vaapiencoder_base.cpp - basic va encoder for video
 *
 *  Copyright (C) 2013-2014 Intel Corporation
 *    Author: Wind Yuan <feng.yuan@intel.com>
 *    Author: Xu Guangxin <guangxin.xu@intel.com>
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
#include "vaapiencoder_base.h"
#include <assert.h>
#include <stdint.h>
#include "common/common_def.h"
#include "common/utils.h"
#include "scopedlogger.h"
#include "vaapicodedbuffer.h"
#include "vaapi/vaapidisplay.h"
#include "vaapi/vaapicontext.h"
#include "vaapi/vaapisurfaceallocator.h"
#include "vaapi/vaapiutils.h"
#include "vaapi/vaapiimageutils.h"

const uint32_t MaxOutputBuffer=5;
namespace YamiMediaCodec{
VaapiEncoderBase::VaapiEncoderBase():
    m_entrypoint(VAEntrypointEncSlice),
    m_maxOutputBuffer(MaxOutputBuffer),
    m_maxCodedbufSize(0)
{
    FUNC_ENTER();
    m_externalDisplay.handle = 0,
    m_externalDisplay.type = NATIVE_DISPLAY_AUTO,

    memset(&m_videoParamCommon, 0, sizeof(m_videoParamCommon));
    m_videoParamCommon.size = sizeof(m_videoParamCommon);
    m_videoParamCommon.rawFormat = RAW_FORMAT_NV12;
    m_videoParamCommon.frameRate.frameRateNum = 30;
    m_videoParamCommon.frameRate.frameRateDenom = 1;
    m_videoParamCommon.intraPeriod = 15;
    m_videoParamCommon.ipPeriod = 1;
    m_videoParamCommon.numRefFrames = 1;
    m_videoParamCommon.rcMode = RATE_CONTROL_CQP;
    m_videoParamCommon.rcParams.initQP = 26;
    m_videoParamCommon.rcParams.minQP = 1;
    m_videoParamCommon.rcParams.maxQP = 51;
    m_videoParamCommon.rcParams.bitRate= 0;
    m_videoParamCommon.rcParams.targetPercentage= 70;
    m_videoParamCommon.rcParams.windowSize = 500;
    m_videoParamCommon.rcParams.disableBitsStuffing = 1;
    m_videoParamCommon.cyclicFrameInterval = 30;
    m_videoParamCommon.refreshType = VIDEO_ENC_NONIR;
    m_videoParamCommon.airParams.airAuto = 1;
    m_videoParamCommon.leastInputCount = 0;

    updateMaxOutputBufferCount();
}

VaapiEncoderBase::~VaapiEncoderBase()
{
    cleanupVA();
    INFO("~VaapiEncoderBase");
}

void VaapiEncoderBase::setNativeDisplay(NativeDisplay * nativeDisplay)
{
    if (!nativeDisplay || nativeDisplay->type == NATIVE_DISPLAY_AUTO)
        return;

    m_externalDisplay = *nativeDisplay;
}

Encode_Status VaapiEncoderBase::start(void)
{
    FUNC_ENTER();
    if (!initVA())
        return ENCODE_FAIL;

    return ENCODE_SUCCESS;
}

void VaapiEncoderBase::flush(void)
{
    AutoLock l(m_lock);
    m_output.clear();
}

Encode_Status VaapiEncoderBase::stop(void)
{
    FUNC_ENTER();
    cleanupVA();
    return ENCODE_SUCCESS;
}

bool VaapiEncoderBase::isBusy()
{
    AutoLock l(m_lock);
    return m_output.size() >= m_maxOutputBuffer;
}


Encode_Status VaapiEncoderBase::encode(VideoEncRawBuffer *inBuffer)
{
    FUNC_ENTER();

    if (!inBuffer)
        return ENCODE_SUCCESS;
    if (!inBuffer->data && !inBuffer->size) {
        // XXX handle EOS when there is B frames
        inBuffer->bufAvailable = true;
        return ENCODE_SUCCESS;
    }
    VideoFrameRawData frame;
    if (!fillFrameRawData(&frame, inBuffer->fourcc, width(), height(), inBuffer->data))
        return ENCODE_INVALID_PARAMS;
    inBuffer->bufAvailable = true;
    if (inBuffer->forceKeyFrame)
        frame.flags |= VIDEO_FRAME_FLAGS_KEY;
    frame.timeStamp = inBuffer->timeStamp;
    return encode(&frame);
}

Encode_Status VaapiEncoderBase::encode(VideoFrameRawData* frame)
{
    if (!frame || !frame->width || !frame->height || !frame->fourcc)
        return ENCODE_INVALID_PARAMS;

    FUNC_ENTER();

    if (isBusy())
        return ENCODE_IS_BUSY;
    SurfacePtr surface = createSurface(frame);
    if (!surface)
        return ENCODE_NO_MEMORY;
    return doEncode(surface, frame->timeStamp, frame->flags & VIDEO_FRAME_FLAGS_KEY);
}

Encode_Status VaapiEncoderBase::encode(const SharedPtr<VideoFrame>& frame)
{
    if (!frame)
        return ENCODE_INVALID_PARAMS;
    if (isBusy())
        return ENCODE_IS_BUSY;
    SurfacePtr surface = createSurface(frame);
    if (!surface)
        return ENCODE_INVALID_PARAMS;
    return doEncode(surface, frame->timeStamp, frame->flags & VIDEO_FRAME_FLAGS_KEY);
}

Encode_Status VaapiEncoderBase::getParameters(VideoParamConfigType type, Yami_PTR videoEncParams)
{
    FUNC_ENTER();
    Encode_Status ret = ENCODE_INVALID_PARAMS;
    if (!videoEncParams)
        return ret;

    DEBUG("type = 0x%08x", type);
    switch (type) {
    case VideoParamsTypeCommon: {
        VideoParamsCommon* common = (VideoParamsCommon*)videoEncParams;
        if (common->size == sizeof(VideoParamsCommon)) {
            PARAMETER_ASSIGN(*common, m_videoParamCommon);
            ret = ENCODE_SUCCESS;
        }
        break;
    }
    default:
        ret = ENCODE_SUCCESS;
        break;
    }
    return ret;
}

Encode_Status VaapiEncoderBase::setParameters(VideoParamConfigType type, Yami_PTR videoEncParams)
{
    FUNC_ENTER();
    Encode_Status ret = ENCODE_SUCCESS;
    if (!videoEncParams)
        return ret;

    DEBUG("type = 0x%08x", type);
    switch (type) {
    case VideoParamsTypeCommon: {
        VideoParamsCommon* common = (VideoParamsCommon*)videoEncParams;
        if (common->size == sizeof(VideoParamsCommon)) {
            PARAMETER_ASSIGN(m_videoParamCommon, *common);
            if(m_videoParamCommon.rcParams.bitRate > 0)
	         m_videoParamCommon.rcMode = RATE_CONTROL_CBR;
	     // Only support CQP and CBR mode now
            if (m_videoParamCommon.rcMode != RATE_CONTROL_CBR)
                m_videoParamCommon.rcMode = RATE_CONTROL_CQP;
        } else
            ret = ENCODE_INVALID_PARAMS;
        m_maxCodedbufSize = 0; // resolution may change, recalculate max codec buffer size when it is requested
        break;
    }
    case VideoConfigTypeFrameRate: {
        VideoConfigFrameRate* frameRateConfig = (VideoConfigFrameRate*)videoEncParams;
        if (frameRateConfig->size == sizeof(VideoConfigFrameRate)) {
            m_videoParamCommon.frameRate = frameRateConfig->frameRate;
        } else
            ret = ENCODE_INVALID_PARAMS;
        }
        break;
    case VideoConfigTypeBitRate: {
        VideoConfigBitRate* rcParamsConfig = (VideoConfigBitRate*)videoEncParams;
        if (rcParamsConfig->size == sizeof(VideoConfigBitRate)) {
            m_videoParamCommon.rcParams = rcParamsConfig->rcParams;
        } else
            ret = ENCODE_INVALID_PARAMS;
        }
        break;
    default:
        ret = ENCODE_INVALID_PARAMS;
        break;
    }
    INFO("bitrate: %d", bitRate());
    return ret;
}

Encode_Status VaapiEncoderBase::setConfig(VideoParamConfigType type, Yami_PTR videoEncConfig)
{
    FUNC_ENTER();
    DEBUG("type = %d", type);
    return ENCODE_SUCCESS;
}

Encode_Status VaapiEncoderBase::getConfig(VideoParamConfigType type, Yami_PTR videoEncConfig)
{
    FUNC_ENTER();
    return ENCODE_SUCCESS;
}

Encode_Status VaapiEncoderBase::getMaxOutSize(uint32_t *maxSize)
{
    FUNC_ENTER();
    *maxSize = 0;
    return ENCODE_SUCCESS;
}

#ifdef __BUILD_GET_MV__
Encode_Status VaapiEncoderBase::getMVBufferSize(uint32_t *Size)
{
    FUNC_ENTER();
    *Size = 0;
    return ENCODE_SUCCESS;
}
#endif

struct SurfaceDestroyer {
    SurfaceDestroyer(DisplayPtr display)
        : m_display(display)
    {
    }
    void operator()(VaapiSurface* surface)
    {
        VASurfaceID id = surface->getID();
        checkVaapiStatus(vaDestroySurfaces(m_display->getID(), &id, 1),
            "vaDestroySurfaces");
        delete surface;
    }

private:
    DisplayPtr m_display;
};

SurfacePtr VaapiEncoderBase::createNewSurface(uint32_t fourcc)
{
    VASurfaceAttrib attrib;
    uint32_t rtFormat;
    SurfacePtr surface;

    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = fourcc;

    switch(fourcc) {
    case VA_FOURCC_NV12:
    case VA_FOURCC_I420:
        rtFormat = VA_RT_FORMAT_YUV420;
        break;
    case VA_FOURCC_YUY2:
        rtFormat = VA_RT_FORMAT_YUV422;
        break;
    default:
        ERROR("unspported fourcc %x", fourcc);
        return surface;
    }

    VASurfaceID id;
    uint32_t width = m_videoParamCommon.resolution.width;
    uint32_t height = m_videoParamCommon.resolution.height;
    VAStatus status = vaCreateSurfaces(m_display->getID(), rtFormat, width, height,
        &id, 1, &attrib, 1);
    if (!checkVaapiStatus(status, "vaCreateSurfaces"))
        return surface;
    surface.reset(new VaapiSurface(m_display, (intptr_t)id, width, height),
        SurfaceDestroyer(m_display));
    return surface;
}

SurfacePtr VaapiEncoderBase::createSurface()
{
    SurfacePtr s;
    if (m_pool) {
        s = m_pool->alloc();
    } else {
        ERROR("BUG!: surface pool not created");
    }
    return s;
}

static bool copyImage(uint8_t* destBase,
    const uint32_t destOffsets[3], const uint32_t destPitches[3],
    const uint8_t* srcBase,
    const uint32_t srcOffsets[3], const uint32_t srcPitches[3],
    const uint32_t width[3], const uint32_t height[3], uint32_t planes)
{
    for (uint32_t i = 0; i < planes; i++) {
        uint32_t w = width[i];
        uint32_t h = height[i];
        if (w > destPitches[i] || w > srcPitches[i]) {
            ERROR("can't copy, plane = %d,  width = %d, srcPitch = %d, destPitch = %d",
                i, w, srcPitches[i], destPitches[i]);
            return false;
        }
        const uint8_t* src = srcBase + srcOffsets[i];
        uint8_t* dest = destBase + destOffsets[i];

        for (uint32_t j = 0; j < h; j++) {
            memcpy(dest, src, w);
            src += srcPitches[i];
            dest += destPitches[i];
        }
    }
    return true;
}

SurfacePtr VaapiEncoderBase::createSurface(VideoFrameRawData* frame)
{
    uint32_t fourcc = frame->fourcc;

    SurfacePtr surface = createNewSurface(fourcc);
    SurfacePtr nil;
    if (!surface)
        return nil;

    uint32_t width[3];
    uint32_t height[3];
    uint32_t planes;
    if (!getPlaneResolution(fourcc, frame->width, frame->height, width, height, planes)) {
        ERROR("invalid input format");
        return nil;
    }

    VAImage image;
    VADisplay display = m_display->getID();
    uint8_t* dest = mapSurfaceToImage(display, surface->getID(), image);
    if (!dest) {
        ERROR("map image failed");
        return nil;
    }
    uint8_t* src = reinterpret_cast<uint8_t*>(frame->handle);
    if (!copyImage(dest, image.offsets, image.pitches, src,
            frame->offset, frame->pitch, width, height, planes)) {
        ERROR("failed to copy image");
        unmapImage(display, image);
        return nil;
    }
    unmapImage(display, image);
    return surface;
}

struct SurfaceRecycler
{
    SurfaceRecycler(const SharedPtr<VideoFrame>& frame): m_frame(frame){}
    void operator()(VaapiSurface* surface) { delete surface;}
private:
    SharedPtr<VideoFrame> m_frame;
};

SurfacePtr VaapiEncoderBase::createSurface(const SharedPtr<VideoFrame>& frame)
{
    SurfacePtr surface(new VaapiSurface(m_display, (VASurfaceID)frame->surface), SurfaceRecycler(frame));
    return surface;
}

void VaapiEncoderBase::fill(VAEncMiscParameterHRD* hrd) const
{
    hrd->buffer_size = m_videoParamCommon.rcParams.bitRate * 4;
    hrd->initial_buffer_fullness = hrd->buffer_size/2;
    DEBUG("bitRate: %d, hrd->buffer_size: %d, hrd->initial_buffer_fullness: %d",
        m_videoParamCommon.rcParams.bitRate, hrd->buffer_size,hrd->initial_buffer_fullness);
}

void VaapiEncoderBase::fill(VAEncMiscParameterRateControl* rateControl) const
{
    rateControl->bits_per_second = m_videoParamCommon.rcParams.bitRate;
    rateControl->initial_qp =  m_videoParamCommon.rcParams.initQP;
    rateControl->min_qp =  m_videoParamCommon.rcParams.minQP;
    /*FIXME: where to find max_qp */
    rateControl->window_size = m_videoParamCommon.rcParams.windowSize;
    rateControl->target_percentage = m_videoParamCommon.rcParams.targetPercentage;
    rateControl->rc_flags.bits.disable_frame_skip = m_videoParamCommon.rcParams.disableFrameSkip;
    rateControl->rc_flags.bits.disable_bit_stuffing = m_videoParamCommon.rcParams.disableBitsStuffing;
}

void VaapiEncoderBase::fill(VAEncMiscParameterFrameRate* frameRate) const
{
    frameRate->framerate = fps();
}

/* Generates additional control parameters */
bool VaapiEncoderBase::ensureMiscParams (VaapiEncPicture* picture)
{
    VAEncMiscParameterHRD* hrd = NULL;
    if (!picture->newMisc(VAEncMiscParameterTypeHRD, hrd))
        return false;
    if (hrd)
        fill(hrd);
    VideoRateControl mode = rateControlMode();
    if (mode == RATE_CONTROL_CBR ||
            mode == RATE_CONTROL_VBR) {
        VAEncMiscParameterRateControl* rateControl = NULL;
        if (!picture->newMisc(VAEncMiscParameterTypeRateControl, rateControl))
            return false;
        if (rateControl)
            fill(rateControl);

        VAEncMiscParameterFrameRate* frameRate = NULL;
        if (!picture->newMisc(VAEncMiscParameterTypeFrameRate, frameRate))
            return false;
        if (frameRate)
            fill(frameRate);
    }
    return true;
}

struct ProfileMapItem {
    VaapiProfile vaapiProfile;
    VAProfile    vaProfile;
};

const ProfileMapItem g_profileMap[] =
{
    {VAAPI_PROFILE_H264_BASELINE,  VAProfileH264Baseline},
    {VAAPI_PROFILE_H264_CONSTRAINED_BASELINE,VAProfileH264ConstrainedBaseline},
    {VAAPI_PROFILE_H264_MAIN, VAProfileH264Main},
    {VAAPI_PROFILE_H264_HIGH,VAProfileH264High},
    {VAAPI_PROFILE_JPEG_BASELINE,VAProfileJPEGBaseline},
    {VAAPI_PROFILE_HEVC_MAIN, VAProfileHEVCMain},
    {VAAPI_PROFILE_HEVC_MAIN10, VAProfileHEVCMain10},
};

VaapiProfile VaapiEncoderBase::profile() const
{
    for (size_t i = 0; i < N_ELEMENTS(g_profileMap); i++) {
        if (m_videoParamCommon.profile == g_profileMap[i].vaProfile)
            return g_profileMap[i].vaapiProfile;
    }
    return VAAPI_PROFILE_UNKNOWN;
}

void VaapiEncoderBase::cleanupVA()
{
    m_pool.reset();
    m_alloc.reset();
    m_context.reset();
    m_display.reset();
}

void unrefAllocator(SurfaceAllocator* allocator)
{
    allocator->unref(allocator);
}

bool VaapiEncoderBase::initVA()
{
    VAConfigAttrib attrib, *pAttrib = NULL;
    int32_t attribCount = 0;
    FUNC_ENTER();

    m_display = VaapiDisplay::create(m_externalDisplay);
    if (!m_display) {
        ERROR("failed to create display");
        return false;
    }

    if (RATE_CONTROL_NONE != m_videoParamCommon.rcMode) {
        attrib.type = VAConfigAttribRateControl;
        attrib.value = m_videoParamCommon.rcMode;
        pAttrib = &attrib;
        attribCount = 1;
    }
    ConfigPtr config = VaapiConfig::create(m_display, m_videoParamCommon.profile, m_entrypoint, pAttrib, attribCount);
    if (!config) {
        ERROR("failed to create config");
        return false;
    }

    m_alloc.reset(new VaapiSurfaceAllocator(m_display->getID()), unrefAllocator);

    int32_t surfaceWidth = ALIGN16(m_videoParamCommon.resolution.width);
    int32_t surfaceHeight = ALIGN16(m_videoParamCommon.resolution.height);
    m_pool = SurfacePool::create(m_display, m_alloc, YAMI_FOURCC_NV12, (uint32_t)surfaceWidth, (uint32_t)surfaceHeight,m_maxOutputBuffer);
    if (!m_pool)
        return false;

    std::vector<VASurfaceID> surfaces;
    m_pool->peekSurfaces(surfaces);

    m_context = VaapiContext::create(config,
                             surfaceWidth,
                             surfaceHeight,
                             VA_PROGRESSIVE, &surfaces[0], surfaces.size());
    if (!m_context) {
        ERROR("failed to create context");
        return false;
    }
    return true;
}

Encode_Status VaapiEncoderBase::checkEmpty(VideoEncOutputBuffer *outBuffer, bool *outEmpty)
{
    bool isEmpty;
    FUNC_ENTER();
    if (!outBuffer)
        return ENCODE_INVALID_PARAMS;

    AutoLock l(m_lock);
    isEmpty = m_output.empty();
    INFO("output queue size: %zu\n", m_output.size());

    *outEmpty = isEmpty;

    if (isEmpty) {
        if (outBuffer->format == OUTPUT_CODEC_DATA)
           return getCodecConfig(outBuffer);
        return ENCODE_BUFFER_NO_MORE;
    }
    return ENCODE_SUCCESS;
}

void VaapiEncoderBase::getPicture(PicturePtr &outPicture)
{
    outPicture = m_output.front();
    outPicture->sync();
}

Encode_Status VaapiEncoderBase::checkCodecData(VideoEncOutputBuffer * outBuffer)
{
    if (outBuffer->format != OUTPUT_CODEC_DATA) {
        AutoLock l(m_lock);
        m_output.pop_front();
    }
    return ENCODE_SUCCESS;
}

#ifndef __BUILD_GET_MV__
Encode_Status VaapiEncoderBase::getOutput(VideoEncOutputBuffer * outBuffer, bool withWait)
{
    bool isEmpty;
    PicturePtr picture;
    Encode_Status ret;
    FUNC_ENTER();
    ret = checkEmpty(outBuffer, &isEmpty);
    if (isEmpty)
        return ret;

    getPicture(picture);
    ret = picture->getOutput(outBuffer);
    if (ret != ENCODE_SUCCESS)
        return ret;

    checkCodecData(outBuffer);
    return ENCODE_SUCCESS;
}

#else

Encode_Status VaapiEncoderBase::getOutput(VideoEncOutputBuffer * outBuffer, VideoEncMVBuffer * MVBuffer, bool withWait)
{
    void *data = NULL;
    uint32_t mappedSize;
    bool isEmpty;
    PicturePtr picture;
    Encode_Status ret;
    FUNC_ENTER();

    ret = checkEmpty(outBuffer, &isEmpty);
    if (isEmpty)
        return ret;
    getPicture(picture);

    ret = picture->getOutput(outBuffer);
    if (ret != ENCODE_SUCCESS)
        return ret;
    if (!picture->editMVBuffer(data, &mappedSize))
        return ret;
    if (data)
        memcpy(MVBuffer->data, data, mappedSize);
    checkCodecData(outBuffer);
    return ENCODE_SUCCESS;
}

#endif

Encode_Status VaapiEncoderBase::getCodecConfig(VideoEncOutputBuffer * outBuffer)
{
    ASSERT(outBuffer && (outBuffer->format == OUTPUT_CODEC_DATA));
    outBuffer->dataSize  = 0;
    return ENCODE_SUCCESS;
}

}
