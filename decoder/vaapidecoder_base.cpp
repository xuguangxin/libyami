/*
 * Copyright (C) 2013-2014 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vaapidecoder_base.h"
#include "common/log.h"
#include "vaapi/vaapisurfaceallocator.h"
#include "vaapi/vaapicontext.h"
#include "vaapi/vaapidisplay.h"
#include "vaapi/VaapiUtils.h"
#include "vaapidecsurfacepool.h"
#include <string.h>
#include <stdlib.h> // for setenv
#include <va/va_backend.h>

namespace YamiMediaCodec{
typedef VaapiDecoderBase::PicturePtr PicturePtr;

inline void unrefAllocator(SurfaceAllocator* allocator)
{
    allocator->unref(allocator);
}

VaapiDecoderBase::VaapiDecoderBase()
    : m_VAStarted(false)
    , m_currentPTS(INVALID_PTS)
{
    INFO("base: construct()");
    m_externalDisplay.handle = 0,
    m_externalDisplay.type = NATIVE_DISPLAY_AUTO,
    memset(&m_videoFormatInfo, 0, sizeof(VideoFormatInfo));
    memset(&m_configBuffer, 0, sizeof(m_configBuffer));
    m_configBuffer.fourcc = YAMI_FOURCC_NV12;
}

VaapiDecoderBase::~VaapiDecoderBase()
{
    INFO("base: deconstruct()");
    stop();
}

PicturePtr VaapiDecoderBase::createPicture(int64_t timeStamp /* , VaapiPictureStructure structure = VAAPI_PICTURE_STRUCTURE_FRAME */)
{
    PicturePtr picture;
    /*accquire one surface from m_surfacePool in base decoder  */
    SurfacePtr surface = createSurface();
    if (!surface) {
        DEBUG("create surface failed");
        return picture;
    }

    picture.reset(new VaapiDecPicture(m_context, surface, timeStamp));
    return picture;
}

YamiStatus VaapiDecoderBase::start(VideoConfigBuffer* buffer)
{
    YamiStatus status;

    INFO("base: start()");

    if (buffer == NULL) {
        return YAMI_DECODE_INVALID_DATA;
    }

    m_configBuffer = *buffer;
    m_configBuffer.data = NULL;
    m_configBuffer.size = 0;

    m_videoFormatInfo.width = buffer->width;
    m_videoFormatInfo.height = buffer->height;
    m_videoFormatInfo.surfaceWidth = buffer->surfaceWidth;
    m_videoFormatInfo.surfaceHeight = buffer->surfaceHeight;
    m_videoFormatInfo.surfaceNumber = buffer->surfaceNumber;
    if (!m_configBuffer.fourcc) {
        /* This just a workaround, user usually memset the VideoConfigBuffer to zero, and we will copy it to m_configBuffer.
           We need remove fields only for internal user from VideoConfigBuffer
           i.e., following thing should removed:
            int32_t surfaceWidth;
            int32_t surfaceHeight;
            int32_t frameRate;
            int32_t surfaceNumber;
            VAProfile profile;
            uint32_t flag;
            uint32_t fourcc;
        */
        m_videoFormatInfo.fourcc = m_configBuffer.fourcc = YAMI_FOURCC_NV12;
    }
    else {
        m_videoFormatInfo.fourcc = m_configBuffer.fourcc;
    }

    status = setupVA(buffer->surfaceNumber, buffer->profile);
    if (status != YAMI_SUCCESS)
        return status;

    DEBUG
        ("m_videoFormatInfo video size: %d x %d, m_videoFormatInfo surface size: %d x %d",
         m_videoFormatInfo.width, m_videoFormatInfo.height,
         m_videoFormatInfo.surfaceWidth, m_videoFormatInfo.surfaceHeight);

#ifdef __ENABLE_DEBUG__
    renderPictureCount = 0;
#endif
    return YAMI_SUCCESS;
}

YamiStatus VaapiDecoderBase::reset(VideoConfigBuffer* buffer)
{
    YamiStatus status;

    INFO("base: reset()");
    if (buffer == NULL) {
        return YAMI_DECODE_INVALID_DATA;
    }

    flush();

    status = terminateVA();
    if (status != YAMI_SUCCESS)
        return status;

    status = start(buffer);
    if (status != YAMI_SUCCESS)
        return status;

    return YAMI_SUCCESS;
}

void VaapiDecoderBase::stop(void)
{
    INFO("base: stop()");
    terminateVA();

    m_currentPTS = INVALID_PTS;

    m_videoFormatInfo.valid = false;
}

void VaapiDecoderBase::flush(void)
{

    INFO("base: flush()");
    if (m_surfacePool) {
        m_surfacePool->flush();
    }

    m_currentPTS = INVALID_PTS;
}

struct BufferRecycler
{
    BufferRecycler(const DecSurfacePoolPtr&  pool, VideoRenderBuffer* buffer)
        :m_pool(pool), m_buffer(buffer)
    {
    }
    void operator()(VideoFrame* frame)
    {
        m_pool->recycle(m_buffer);
        delete frame;
    }

private:
    DecSurfacePoolPtr  m_pool;
    VideoRenderBuffer* m_buffer;
};

SharedPtr<VideoFrame> VaapiDecoderBase::getOutput()
{
    SharedPtr<VideoFrame> frame;
    DecSurfacePoolPtr pool = m_surfacePool;
    if (!pool)
        return frame;
    VideoRenderBuffer *buffer = pool->getOutput();
    if (buffer) {
        frame.reset(new VideoFrame, BufferRecycler(pool, buffer));
        memset(frame.get(), 0, sizeof(VideoFrame));
        frame->surface = (intptr_t)buffer->surface;
        frame->timeStamp = buffer->timeStamp;
        frame->crop = buffer->crop;
        frame->fourcc = buffer->fourcc;
    }
    return frame;
}

const VideoFormatInfo *VaapiDecoderBase::getFormatInfo(void)
{
    INFO("base: getFormatInfo()");

    if (!m_VAStarted)
        return NULL;

    return &m_videoFormatInfo;
}

YamiStatus
VaapiDecoderBase::setupVA(uint32_t numSurface, VAProfile profile)
{
    INFO("base: setup VA");

    if (m_VAStarted) {
        return YAMI_SUCCESS;
    }

    if (m_display) {
        WARNING("VA is partially started.");
        return YAMI_FAIL;
    }

    m_display = VaapiDisplay::create(m_externalDisplay);

    if (!m_display) {
        ERROR("failed to create display");
        return YAMI_FAIL;
    }

    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;


    ConfigPtr config = VaapiConfig::create(m_display, profile, VAEntrypointVLD,&attrib, 1);
    if (!config) {
        ERROR("failed to create config");
        return YAMI_FAIL;
    }

    if (!m_externalAllocator) {
        //use internal allocator
        m_allocator.reset(new VaapiSurfaceAllocator(m_display->getID()), unrefAllocator);
    } else {
        m_allocator = m_externalAllocator;
    }

    m_configBuffer.surfaceNumber = numSurface;
    m_surfacePool = VaapiDecSurfacePool::create(m_display, &m_configBuffer, m_allocator);
    DEBUG("surface pool is created");
    if (!m_surfacePool)
        return YAMI_FAIL;
    std::vector<VASurfaceID> surfaces;
    m_surfacePool->getSurfaceIDs(surfaces);
    if (surfaces.empty())
        return YAMI_FAIL;
    int size = surfaces.size();
    m_context = VaapiContext::create(config,
                                       m_videoFormatInfo.width,
                                       m_videoFormatInfo.height,
                                       0, &surfaces[0], size);

    if (!m_context) {
        ERROR("create context failed");
        return YAMI_FAIL;
    }

    m_videoFormatInfo.surfaceWidth = m_videoFormatInfo.width;
    m_videoFormatInfo.surfaceHeight = m_videoFormatInfo.height;

    m_VAStarted = true;
    return YAMI_SUCCESS;
}

YamiStatus VaapiDecoderBase::terminateVA(void)
{
    INFO("base: terminate VA");
    m_surfacePool.reset();
    m_allocator.reset();
    DEBUG("surface pool is reset");
    m_context.reset();
    m_display.reset();

    m_VAStarted = false;
    return YAMI_SUCCESS;
}

void VaapiDecoderBase::setNativeDisplay(NativeDisplay * nativeDisplay)
{
    if (!nativeDisplay || nativeDisplay->type == NATIVE_DISPLAY_AUTO)
        return;

    m_externalDisplay = *nativeDisplay;
}

void VaapiDecoderBase::releaseLock(bool lockable)
{
    if (!m_surfacePool)
        return;

    m_surfacePool->setWaitable(lockable);
}

void VaapiDecoderBase::setAllocator(SurfaceAllocator* allocator)
{
    m_externalAllocator.reset(allocator,unrefAllocator);
}

SurfacePtr VaapiDecoderBase::createSurface()
{
    SurfacePtr surface;
    if (m_surfacePool) {
        surface = m_surfacePool->acquireWithWait();
    }
    return surface;
}

YamiStatus VaapiDecoderBase::outputPicture(const PicturePtr& picture)
{
    //TODO: reorder poc
    return m_surfacePool->output(picture->getSurface(),
               picture->m_timeStamp)
        ? YAMI_SUCCESS
        : YAMI_FAIL;
}

VADisplay VaapiDecoderBase::getDisplayID()
{
    if (!m_display)
        return NULL;
    return m_display->getID();
}

} //namespace YamiMediaCodec
