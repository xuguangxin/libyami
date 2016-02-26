/*
 *  egl_vaapi_image.cpp - map vaapi image to egl
 *
 *  Copyright (C) 2014 Intel Corporation
 *    Author: Zhao, Halley<halley.zhao@intel.com>
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

#include "egl_util.h"
#include "egl_vaapi_image.h"
#include "common/log.h"
#include "vaapi/vaapiutils.h"
#if __ENABLE_DMABUF__
#include "libdrm/drm_fourcc.h"
#endif
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <vector>

namespace YamiMediaCodec {

EglVaapiImage::EglVaapiImage(VADisplay display, int width, int height)
    : m_display(display), m_width(width), m_height(height), m_inited(false)
    , m_acquired(false), m_eglImage(EGL_NO_IMAGE_KHR)
{

}

bool getVaFormat(VADisplay display, VAImageFormat& format);
bool EglVaapiImage::init()
{
    if (m_inited) {
        ERROR("do not init twice");
        return false;
    }
    if (!getVaFormat(m_display, m_format))
        return false;
    VAStatus vaStatus = vaCreateImage(m_display, &m_format, m_width, m_height, &m_image);
    if (!checkVaapiStatus(vaStatus, "vaCreateImage"))
        return false;
    m_inited = true;
    return true;
}

bool EglVaapiImage::acquireBufferHandle(VideoDataMemoryType memoryType)
{
    uint32_t i;
    if (m_acquired) {
        ASSERT(memoryType = m_frameInfo.memoryType);
        return true;
    }

    // FIXME, more type can be supported
    if (memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME)
        m_bufferInfo.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;
    else if (memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF)
        m_bufferInfo.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    else
        ASSERT(0);
    VAStatus vaStatus = vaAcquireBufferHandle(m_display, m_image.buf, &m_bufferInfo);
    m_frameInfo.memoryType = memoryType;
    m_frameInfo.width = m_width;
    m_frameInfo.height = m_height;
    for (i=0; i<m_image.num_planes; i++) {
        m_frameInfo.pitch[i] = m_image.pitches[i];
        m_frameInfo.offset[i] = m_image.offsets[i];
    }
    m_frameInfo.fourcc = m_image.format.fourcc;
    m_frameInfo.size = m_image.data_size; // not interest for bufferhandle
    m_frameInfo.handle = m_bufferInfo.handle;

    m_acquired = true;

    return checkVaapiStatus(vaStatus, "vaAcquireBufferHandle");
}

bool EglVaapiImage::exportFrame(VideoDataMemoryType memoryType, VideoFrameRawData &frame)
{
    if (!acquireBufferHandle(memoryType))
        return false;

    frame = m_frameInfo;
    return true;
}

EGLImageKHR EglVaapiImage::createEglImage(EGLDisplay eglDisplay, EGLContext eglContext, VideoDataMemoryType memoryType)
{
    if (m_eglImage != EGL_NO_IMAGE_KHR)
        return m_eglImage;
    if (!acquireBufferHandle(memoryType))
        return EGL_NO_IMAGE_KHR;
    //FIXME, it doesn't support planar video frame yet
    m_eglImage = createEglImageFromHandle(eglDisplay, eglContext, memoryType, m_bufferInfo.handle,
        m_width, m_height, m_image.pitches[0]);
    if (m_eglImage == EGL_NO_IMAGE_KHR) {
        ERROR("createEglImageFromHandle failed");
    }

    return m_eglImage;
}

bool EglVaapiImage::blt(const SharedPtr<VideoFrame>& src)
{
    if (!m_inited) {
        ERROR("call init before blt!");
        return false;
    }
    if (m_acquired)
        vaReleaseBufferHandle(m_display, m_image.buf);

    VAStatus vaStatus = vaGetImage(m_display, (VASurfaceID)src->surface, src->crop.x, src->crop.y, src->crop.width, src->crop.height, m_image.image_id);

    // incomplete data yet
    m_frameInfo.timeStamp = src->timeStamp;
    m_frameInfo.flags = src->flags;
    return checkVaapiStatus(vaStatus, "vaGetImage");
}

EglVaapiImage::~EglVaapiImage()
{
    if (m_inited) {
        if (m_acquired)
            vaReleaseBufferHandle(m_display, m_image.buf);
        vaDestroyImage(m_display, m_image.image_id);
    }
}

bool getVaFormat(VADisplay display, VAImageFormat& format)
{
    int num = vaMaxNumImageFormats(display);
    if (!num)
        return false;
    std::vector<VAImageFormat> vaFormats;
    vaFormats.resize(num);
    VAStatus vaStatus = vaQueryImageFormats(display, &vaFormats[0], &num);
    if (!checkVaapiStatus(vaStatus, "vaQueryImageFormats"))
        return false;
    if (vaStatus != VA_STATUS_SUCCESS) {
        ERROR("query image formats return %d", vaStatus);
        return false;
    }
    vaFormats.resize(num);
    for (size_t i = 0; i < vaFormats.size(); i++)
    {
        const VAImageFormat& fmt = vaFormats[i];
        if (fmt.fourcc == VA_FOURCC_BGRX) {
            format = fmt;
            return true;
        }
    }
    return false;
}

}//namespace YamiMediaCodec
