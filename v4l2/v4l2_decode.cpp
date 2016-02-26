/*
 *  v4l2_decode.cpp
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
#include <inttypes.h>
#include <linux/videodev2.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#ifdef ANDROID
#include <ufo/gralloc.h>
#include <ufo/graphics.h>
#endif
#include "v4l2_decode.h"
#include "interface/VideoDecoderHost.h"
#include "common/log.h"
#include "egl/egl_vaapi_image.h"

#define INT64_TO_TIMEVAL(i64, time_val) do {            \
        time_val.tv_sec = (int32_t)(i64 >> 32);         \
        time_val.tv_usec = (int32_t)(i64 & 0xffffffff); \
    } while (0)
#define TIMEVAL_TO_INT64(i64, time_val) do {            \
        i64 = time_val.tv_sec;                          \
        i64 = (i64 << 32) + time_val.tv_usec;           \
    } while(0)

V4l2Decoder::V4l2Decoder()
    : m_bindEglImage(false)
    , m_videoWidth(0)
    , m_videoHeight(0)
#ifdef ANDROID
    , m_reqBuffCnt(0)
#endif
{
    uint32_t i;
    m_memoryMode[INPUT] = V4L2_MEMORY_MMAP; // dma_buf hasn't been supported yet
    m_pixelFormat[INPUT] = V4L2_PIX_FMT_H264;
    m_bufferPlaneCount[INPUT] = 1; // decided by m_pixelFormat[INPUT]
    m_memoryMode[OUTPUT] = V4L2_MEMORY_MMAP;
    m_pixelFormat[OUTPUT] = V4L2_PIX_FMT_NV12M;
    m_bufferPlaneCount[OUTPUT] = 2;

    m_maxBufferCount[INPUT] = 8;
    m_maxBufferCount[OUTPUT] = 8;
    m_actualOutBufferCount = m_maxBufferCount[OUTPUT];

    m_inputFrames.resize(m_maxBufferCount[INPUT]);
    m_outputRawFrames.resize(m_maxBufferCount[OUTPUT]);

    for (i=0; i<m_maxBufferCount[INPUT]; i++) {
        memset(&m_inputFrames[i], 0, sizeof(VideoDecodeBuffer));
    }
    for (i=0; i<m_maxBufferCount[OUTPUT]; i++) {
        memset(&m_outputRawFrames[i], 0, sizeof(VideoFrameRawData));
    }

    m_maxBufferSize[INPUT] = 0;
    m_bufferSpace[INPUT] = NULL;
    m_maxBufferSize[OUTPUT] = 0;
    m_bufferSpace[OUTPUT] = NULL;

    m_memoryType = VIDEO_DATA_MEMORY_TYPE_DMA_BUF;
#ifdef ANDROID
    hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (hw_module_t const**)&m_pGralloc);
#endif
}

V4l2Decoder::~V4l2Decoder()
{
    if (m_bufferSpace[INPUT]) {
        delete [] m_bufferSpace[INPUT];
        m_bufferSpace[OUTPUT] = NULL;
    }
    if (m_bufferSpace[OUTPUT]) {
        delete [] m_bufferSpace[OUTPUT];
        m_bufferSpace[OUTPUT] = NULL;
    }
}

void V4l2Decoder::releaseCodecLock(bool lockable)
{
    m_decoder->releaseLock(lockable);
}

bool V4l2Decoder::start()
{
    Decode_Status status = DECODE_SUCCESS;
    if (m_started)
        return true;
    ASSERT(m_decoder);

    NativeDisplay nativeDisplay;

#if ANDROID
    nativeDisplay.type = NATIVE_DISPLAY_VA;
    nativeDisplay.handle = (intptr_t)m_vaDisplay;
#elif __ENABLE_X11__
    DEBUG("m_x11Display: %p", m_x11Display);
    if (m_x11Display) {
        nativeDisplay.type = NATIVE_DISPLAY_X11;
        nativeDisplay.handle = (intptr_t)m_x11Display;
    } else {
        nativeDisplay.type = NATIVE_DISPLAY_DRM;
        nativeDisplay.handle = m_drmfd;
    }
#else
    nativeDisplay.type = NATIVE_DISPLAY_DRM;
    nativeDisplay.handle = m_drmfd;
#endif
    m_decoder->setNativeDisplay(&nativeDisplay);

    // send codec_data if there is
    VideoConfigBuffer configBuffer;
    memset(&configBuffer, 0, sizeof(configBuffer));
    if (m_codecData.size()) {
        configBuffer.data = m_codecData.data();
        configBuffer.size = m_codecData.size();
    }
    status = m_decoder->start(&configBuffer);
    ASSERT(status == DECODE_SUCCESS);

    m_started = true;

    return true;
}

bool V4l2Decoder::stop()
{
    if (m_started)
      m_decoder->stop();

    m_started = false;
    return true;
}

bool V4l2Decoder::inputPulse(uint32_t index)
{
    Decode_Status status = DECODE_SUCCESS;

    VideoDecodeBuffer *inputBuffer = &m_inputFrames[index];

    ASSERT(index >= 0 && index < m_maxBufferCount[INPUT]);
    ASSERT(m_maxBufferSize[INPUT] > 0); // update m_maxBufferSize[INPUT] after VIDIOC_S_FMT
    ASSERT(m_bufferSpace[INPUT] || m_memoryMode[INPUT] != V4L2_MEMORY_MMAP );
    ASSERT(inputBuffer->size <= m_maxBufferSize[INPUT]);

    status = m_decoder->decode(inputBuffer);

    if (status == DECODE_FORMAT_CHANGE) {
        setCodecEvent();
        // we can continue decoding no matter what client does reconfiguration now. otherwise, a tri-state ret is required
        status = m_decoder->decode(inputBuffer);
    }

    if (!inputBuffer->size) {
        setEosState(EosStateInput);
        DEBUG("flush-debug going into flusing state");
    }

    return true; // always return true for decode; simply ignored unsupported nal
}

#if ANDROID
bool V4l2Decoder::outputPulse(uint32_t &index)
{
    SharedPtr<VideoFrame> output = m_decoder->getOutput();

    if(!output) {
        if (eosState() == EosStateInput) {
            setEosState(EosStateOutput);
            fprintf(stderr, "flush-debug flush done on OUTPUT thread\n");
        }
	return false;
    }

    m_vpp->process(output, m_videoFrames[index]);
    m_videoFrames[index]->timeStamp = output->timeStamp;
    m_videoFrames[index]->flags = output->flags;
    DEBUG("buffer index: %d, timeStamp: %" PRId64 "\n", index, output->timeStamp);
    return true;
}
#else
bool V4l2Decoder::outputPulse(uint32_t &index)
{
    SharedPtr<VideoFrame> frame;

    ASSERT(index >= 0 && index < m_maxBufferCount[OUTPUT]);
    DEBUG("index: %d", index);

    //frame = &m_outputRawFrames[index];
    if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_RAW_COPY) {
        /* if (!m_bufferSpace[OUTPUT])
            return false;
        ASSERT(frame->handle);
        frame->fourcc = VA_FOURCC_NV12;
        frame->memoryType = VIDEO_DATA_MEMORY_TYPE_RAW_COPY;*/
        ERROR("TODO: support raw copy");
        return false;
    }
    if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME || m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF) {
        if (m_eglVaapiImages.empty())
            return false;
    }


    frame = m_decoder->getOutput();

    if (!frame) {
        if (eosState() == EosStateInput) {
            setEosState(EosStateOutput);
            DEBUG("flush-debug flush done on OUTPUT thread");
        }

        if (eosState() > EosStateNormal) {
            DEBUG("seek/EOS flush, return empty buffer");
            return false;
        }
        return false;
    }

    if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME || m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF) {
        // FIXME, it introduce one GPU copy; which is not necessary in major case and should be avoided in most case
        m_eglVaapiImages[index]->blt(frame);
        if (!m_bindEglImage)
            m_eglVaapiImages[index]->exportFrame(m_memoryType, m_outputRawFrames[index]);
    }

    DEBUG("outputPulse: index=%d, timeStamp=%ld", index, m_outputRawFrames[index].timeStamp);
    return true;
}
#endif

bool V4l2Decoder::recycleOutputBuffer(int32_t index)
{
    // FIXME, after we remove the extra vpp, renderDone() should come here
    return true;
}

bool V4l2Decoder::recycleInputBuffer(struct v4l2_buffer *dqbuf)
{
    VideoDecodeBuffer *inputBuffer = &(m_inputFrames[dqbuf->index]);
    uintptr_t *ptr = (uintptr_t*)dqbuf->m.planes[0].reserved;
    *ptr = (uintptr_t)inputBuffer->data;
    DEBUG("inputBuffer->data: %p, *ptr: %p\n", inputBuffer->data, (void*)(*ptr));

    return true;
}

bool V4l2Decoder::acceptInputBuffer(struct v4l2_buffer *qbuf)
{
    VideoDecodeBuffer *inputBuffer = &(m_inputFrames[qbuf->index]);
    ASSERT(m_maxBufferSize[INPUT] > 0);
    ASSERT(m_bufferSpace[INPUT] || m_memoryMode[INPUT] != V4L2_MEMORY_MMAP );
    ASSERT(qbuf->index >= 0 && qbuf->index < m_maxBufferCount[INPUT]);
    ASSERT(qbuf->length == 1);
    inputBuffer->size = qbuf->m.planes[0].bytesused; // one plane only
    if (!inputBuffer->size) // EOS
        inputBuffer->data = NULL;
    else {
        if (m_memoryMode[INPUT] == V4L2_MEMORY_MMAP)
            inputBuffer->data = m_bufferSpace[INPUT] + m_maxBufferSize[INPUT]*qbuf->index;
        else if (m_memoryMode[INPUT] == V4L2_MEMORY_USERPTR) {
            // FIXME, puzzle, videodev2.h uses 'unsigned long userptr', how could it support 64 bit mem address?
            uintptr_t *ptr = (uintptr_t*)qbuf->m.planes[0].reserved;
            inputBuffer->data = (uint8_t*)(*ptr);
        }
    }

    TIMEVAL_TO_INT64(inputBuffer->timeStamp, qbuf->timestamp);
    inputBuffer->flag = qbuf->flags;
    // set buffer unit-mode if possible, nal, frame?
    DEBUG("qbuf->index: %d, inputBuffer: %p, timestamp: %ld", qbuf->index, inputBuffer->data, inputBuffer->timeStamp);

    return true;
}

bool V4l2Decoder::giveOutputBuffer(struct v4l2_buffer *dqbuf)
{
    int i;
    ASSERT(dqbuf);
    // for the buffers within range of [m_actualOutBufferCount, m_maxBufferCount[OUTPUT]]
    // there are not used in reality, but still be returned back to client during flush (seek/eos)
    ASSERT(dqbuf->index >= 0 && dqbuf->index < m_maxBufferCount[OUTPUT]);

    dqbuf->flags = m_outputRawFrames[dqbuf->index].flags;
    if (!m_bindEglImage) {
        // FIXME: m_bufferPlaneCount[OUTPUT] doesn't match current RGBX output, it is a bug
        for (i=0; i< 1; i++) {
            // FIXME, a better field to hold drm/dma handle
            dqbuf->m.planes[i].reserved[0] = m_outputRawFrames[dqbuf->index].handle;
            dqbuf->m.planes[i].reserved[1] = m_outputRawFrames[dqbuf->index].pitch[i];
            dqbuf->m.planes[i].data_offset = m_outputRawFrames[dqbuf->index].offset[i];
        }
    }

    // simple set size data to satify chrome even in texture mode
    dqbuf->m.planes[0].bytesused = m_videoWidth * m_videoHeight;
    dqbuf->m.planes[1].bytesused = m_videoWidth * m_videoHeight/2;
#if ANDROID
    INT64_TO_TIMEVAL(m_videoFrames[dqbuf->index]->timeStamp, dqbuf->timestamp);
    dqbuf->flags = m_videoFrames[dqbuf->index]->flags;
#else
    INT64_TO_TIMEVAL(m_outputRawFrames[dqbuf->index].timeStamp, dqbuf->timestamp);
    dqbuf->flags = m_outputRawFrames[dqbuf->index].flags;
#endif
    DEBUG("deque buffer index: %d, timeStamp: (%ld, %ld)\n",
        dqbuf->index,  dqbuf->timestamp.tv_sec, dqbuf->timestamp.tv_usec);

    return true;
}

#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 YAMI_FOURCC('V', 'P', '9', '0')
#endif

int32_t V4l2Decoder::ioctl(int command, void* arg)
{
    int32_t ret = 0;
    int port = -1;

    DEBUG("fd: %d, ioctl command: %s", m_fd[0], IoctlCommandString(command));
    switch (command) {
    case VIDIOC_QBUF: {
#ifdef ANDROID
        struct v4l2_buffer *qbuf = static_cast<struct v4l2_buffer*>(arg);
        static uint32_t bufferCount = 0;
        if(qbuf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
           m_streamOn[OUTPUT] == false) {
            m_winBuff.push_back((ANativeWindowBuffer*)(qbuf->m.userptr));
            bufferCount++;
            if (bufferCount == m_reqBuffCnt)
                mapVideoFrames();
        }
#endif
    }
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF:
    case VIDIOC_DQBUF:
    case VIDIOC_QUERYCAP:
        ret = V4l2CodecBase::ioctl(command, arg);
        break;
    case VIDIOC_REQBUFS: {
        ret = V4l2CodecBase::ioctl(command, arg);
        ASSERT(ret == 0);
        int port = -1;
        struct v4l2_requestbuffers *reqbufs = static_cast<struct v4l2_requestbuffers *>(arg);
        GET_PORT_INDEX(port, reqbufs->type, ret);
        if (port == OUTPUT) {
        #if ANDROID
            if (reqbufs->count)
                m_reqBuffCnt = reqbufs->count;
            else
                m_videoFrames.clear();
        #else
            if (!reqbufs->count) {
                m_eglVaapiImages.clear();
            } else {
                const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
                ASSERT(outFormat && outFormat->width && outFormat->height);
                ASSERT(m_eglVaapiImages.empty());
                for (uint32_t i = 0; i < reqbufs->count; i++) {
                    SharedPtr<EglVaapiImage> image(
                                                   new EglVaapiImage(m_decoder->getDisplayID(), outFormat->width, outFormat->height));
                    if (!image->init()) {
                        ERROR("Create egl vaapi image failed");
                        ret  = -1;
                        break;
                    }
                    m_eglVaapiImages.push_back(image);
                }
            }
        #endif
        }
        break;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *buffer = static_cast<struct v4l2_buffer*>(arg);
        GET_PORT_INDEX(port, buffer->type, ret);

        ASSERT(buffer->memory == m_memoryMode[port]);
        ASSERT(buffer->index >= 0 && buffer->index < m_maxBufferCount[port]);
        ASSERT(buffer->length == m_bufferPlaneCount[port]);
        ASSERT(m_maxBufferSize[port] > 0);

        if (port == INPUT) {
            ASSERT(buffer->memory == V4L2_MEMORY_MMAP);
            buffer->m.planes[0].length = m_maxBufferSize[INPUT];
            buffer->m.planes[0].m.mem_offset = m_maxBufferSize[INPUT] * buffer->index;
        } else if (port == OUTPUT) {
            ASSERT(m_maxBufferSize[INPUT] && m_maxBufferCount[INPUT]);
            // plus input buffer space size, it will be minused in mmap
            buffer->m.planes[0].m.mem_offset =  m_maxBufferSize[OUTPUT] * buffer->index;
            buffer->m.planes[0].m.mem_offset += m_maxBufferSize[INPUT] * m_maxBufferCount[INPUT];
            buffer->m.planes[0].length = m_videoWidth * m_videoHeight;
            buffer->m.planes[1].m.mem_offset = buffer->m.planes[0].m.mem_offset + buffer->m.planes[0].length;
            buffer->m.planes[1].length = ((m_videoWidth+1)/2*2) * ((m_videoHeight+1)/2);
        }
    }
    break;
    case VIDIOC_S_FMT: {
        struct v4l2_format *format = static_cast<struct v4l2_format *>(arg);
        ASSERT(!m_streamOn[INPUT] && !m_streamOn[OUTPUT]);
        if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            // ::Initialize
            uint32_t size;
            memcpy(&size, format->fmt.raw_data, sizeof(uint32_t));
            if(size <= (sizeof(format->fmt.raw_data)-sizeof(uint32_t))) {
                uint8_t *ptr = format->fmt.raw_data;
                ptr += sizeof(uint32_t);
                m_codecData.assign(ptr, ptr + size);
            } else {
                ret = -1;
                ERROR("unvalid codec size");
            }
            //ASSERT(format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M);
        } else if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            // ::CreateInputBuffers
            ASSERT(format->fmt.pix_mp.num_planes == 1);
            ASSERT(format->fmt.pix_mp.plane_fmt[0].sizeimage);
            m_codecData.clear();
            m_decoder.reset(
                createVideoDecoder(mimeFromV4l2PixelFormat(format->fmt.pix_mp.pixelformat)),
                releaseVideoDecoder);
            ASSERT(m_decoder);
            if (!m_decoder) {
                ret = -1;
            }

            m_maxBufferSize[INPUT] = format->fmt.pix_mp.plane_fmt[0].sizeimage;
        } else {
            ret = -1;
            ERROR("unknow type: %d of setting format VIDIOC_S_FMT", format->type);
        }
    }
    break;
    case VIDIOC_SUBSCRIBE_EVENT: {
        // ::Initialize
        struct v4l2_event_subscription *sub = static_cast<struct v4l2_event_subscription*>(arg);
        ASSERT(sub->type == V4L2_EVENT_RESOLUTION_CHANGE);
        // resolution change event is must, we always do so
    }
    break;
    case VIDIOC_DQEVENT: {
        // ::DequeueEvents
        struct v4l2_event *ev = static_cast<struct v4l2_event*>(arg);
        // notify resolution change
        if (hasCodecEvent()) {
            ev->type = V4L2_EVENT_RESOLUTION_CHANGE;
            clearCodecEvent();
        } else
            ret = -1;
    }
    break;
    case VIDIOC_G_FMT: {
        // ::GetFormatInfo
        struct v4l2_format* format = static_cast<struct v4l2_format*>(arg);
        ASSERT(format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ASSERT(m_decoder);

        DEBUG();
        const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
        if (format && outFormat && outFormat->width && outFormat->height) {
            format->fmt.pix_mp.num_planes = m_bufferPlaneCount[OUTPUT];
            format->fmt.pix_mp.width = outFormat->width;
            format->fmt.pix_mp.height = outFormat->height;
            // XXX assumed output format and pitch
#ifdef ANDROID
            format->fmt.pix_mp.pixelformat = HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL;
#else
            format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
#endif
            format->fmt.pix_mp.plane_fmt[0].bytesperline = outFormat->width;
            format->fmt.pix_mp.plane_fmt[1].bytesperline = outFormat->width % 2 ? outFormat->width+1 : outFormat->width;
            m_videoWidth = outFormat->width;
            m_videoHeight = outFormat->height;
            m_maxBufferSize[OUTPUT] = m_videoWidth * m_videoHeight + ((m_videoWidth +1)/2*2) * ((m_videoHeight+1)/2);
        } else {
            ret = -1;
            // chromeos accepts EINVAL as not enough input data yet, will try it again.
            errno = EINVAL;
        }
      }
    break;
    case VIDIOC_G_CTRL: {
        // ::CreateOutputBuffers
        struct v4l2_control* ctrl = static_cast<struct v4l2_control*>(arg);
        ASSERT(ctrl->id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
        ASSERT(m_decoder);
        // VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
        ctrl->value = 0; // no need report dpb size, we hold all buffers in decoder.
    }
    break;
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *fmtdesc = static_cast<struct v4l2_fmtdesc *>(arg);
        if ((fmtdesc->index == 0) && (fmtdesc->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
            fmtdesc->pixelformat = V4L2_PIX_FMT_NV12M;
        } else if ((fmtdesc->index == 0) && (fmtdesc->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
            fmtdesc->pixelformat = V4L2_PIX_FMT_VP8;
        } else if ((fmtdesc->index == 1) && (fmtdesc->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
            fmtdesc->pixelformat = V4L2_PIX_FMT_VP9;
        } else {
            ret = -1;
        }
    }
    break;
    case VIDIOC_G_CROP: {
        struct v4l2_crop* crop= static_cast<struct v4l2_crop *>(arg);
        ASSERT(crop->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ASSERT(m_decoder);
        const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
        if (outFormat && outFormat->width && outFormat->height) {
            crop->c.left =  0;
            crop->c.top  =  0;
            crop->c.width  = outFormat->width;
            crop->c.height = outFormat->height;
        } else {
            ret = -1;
        }
    }
    break;
    default:
        ret = -1;
        ERROR("unknown ioctrl command: %d", command);
    break;
    }

    if (ret == -1 && errno != EAGAIN) {
        // ERROR("ioctl failed");
        WARNING("ioctl command: %s failed", IoctlCommandString(command));
    }

    return ret;
}

/**
 * in order to distinguish input/output buffer, output is <assumed> after input buffer.
 * additional m_maxBufferSize[INPUT] && m_maxBufferCount[INPUT] is added to input buffer offset in VIDIOC_QUERYBUF
 * then minus back in mmap.
*/
void* V4l2Decoder::mmap (void* addr, size_t length,
                      int prot, int flags, unsigned int offset)
{
    uint32_t i;
    ASSERT((prot == PROT_READ) | PROT_WRITE);
    ASSERT(flags == MAP_SHARED);

    ASSERT(m_maxBufferSize[INPUT] && m_maxBufferCount[INPUT]);

    if (offset < m_maxBufferSize[INPUT] * m_maxBufferCount[INPUT]) { // assume it is input buffer
        if (!m_bufferSpace[INPUT]) {
            m_bufferSpace[INPUT] = new uint8_t[m_maxBufferSize[INPUT] * m_maxBufferCount[INPUT]];
            for (i=0; i<m_maxBufferCount[INPUT]; i++) {
                m_inputFrames[i].data = m_bufferSpace[INPUT] + m_maxBufferSize[INPUT]*i;
                m_inputFrames[i].size = m_maxBufferSize[INPUT];
            }
        }
        ASSERT(m_bufferSpace[INPUT]);
        return m_bufferSpace[INPUT] + offset;
    } else { // it is output buffer
        offset -= m_maxBufferSize[INPUT] * m_maxBufferCount[INPUT];
        ASSERT(offset <= m_maxBufferSize[OUTPUT] * m_maxBufferCount[OUTPUT]);
        if (!m_bufferSpace[OUTPUT]) {
            m_bufferSpace[OUTPUT] = new uint8_t[m_maxBufferSize[OUTPUT] * m_maxBufferCount[OUTPUT]];
            for (i=0; i<m_maxBufferCount[OUTPUT]; i++) {
                m_outputRawFrames[i].handle = (intptr_t)(m_bufferSpace[OUTPUT] + m_maxBufferSize[OUTPUT]*i);
                m_outputRawFrames[i].size = m_maxBufferSize[OUTPUT];
                m_outputRawFrames[i].width = m_videoWidth;
                m_outputRawFrames[i].height = m_videoHeight;

                // m_outputRawFrames[i].fourcc = VA_FOURCC_NV12;
                m_outputRawFrames[i].offset[0] = 0;
                m_outputRawFrames[i].offset[1] = m_videoWidth * m_videoHeight;
                m_outputRawFrames[i].pitch[0] = m_videoWidth;
                m_outputRawFrames[i].pitch[1] = m_videoWidth % 2 ? m_videoWidth+1 : m_videoWidth;
            }
        }
        ASSERT(m_bufferSpace[OUTPUT]);
        return m_bufferSpace[OUTPUT] + offset;
    }
}

void V4l2Decoder::flush()
{
    if (m_decoder)
        m_decoder->flush();
}

#if ANDROID
SharedPtr<VideoFrame> V4l2Decoder::createVaSurface(const ANativeWindowBuffer* buf)
{
    SharedPtr<VideoFrame> frame;

    intel_ufo_buffer_details_t info;
    memset(&info, 0, sizeof(info));
    *reinterpret_cast<uint32_t*>(&info) = sizeof(info);

    int err = 0;
    if (m_pGralloc)
        err = m_pGralloc->perform(m_pGralloc, INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_INFO, (buffer_handle_t)buf->handle, &info);

    if (0 != err || !m_pGralloc) {
        fprintf(stderr, "create vaSurface failed\n");
        return frame;
    }

    VASurfaceAttrib attrib;
    memset(&attrib, 0, sizeof(attrib));

    VASurfaceAttribExternalBuffers external;
    memset(&external, 0, sizeof(external));

    external.pixel_format = VA_FOURCC_NV12;
    external.width = buf->width;
    external.height = buf->height;
    external.pitches[0] = info.pitch;
    external.num_planes = 2;
    external.num_buffers = 1;
    uint8_t* handle = (uint8_t*)buf->handle;
    external.buffers = (long unsigned int*)&handle; //graphic handel
    external.flags = VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC;

    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
    attrib.value.type = VAGenericValueTypePointer;
    attrib.value.value.p = &external;

    VASurfaceID id;
    VAStatus vaStatus = vaCreateSurfaces(m_vaDisplay, VA_RT_FORMAT_YUV420,
                        buf->width, buf->height, &id, 1, &attrib, 1);
    if (vaStatus != VA_STATUS_SUCCESS)
        return frame;

    frame.reset(new VideoFrame);
    memset(frame.get(), 0, sizeof(VideoFrame));

    frame->surface = static_cast<intptr_t>(id);
    frame->crop.width = buf->width;
    frame->crop.height = buf->height;

    return frame;
}

bool V4l2Decoder::mapVideoFrames()
{
    SharedPtr<VideoFrame> frame;

    for (uint32_t i = 0; i < m_winBuff.size(); i++) {
        frame = createVaSurface(m_winBuff[i]);
        if (!frame)
            return false;
        m_videoFrames.push_back(frame);
    }
    return true;
}
#else
int32_t V4l2Decoder::useEglImage(EGLDisplay eglDisplay, EGLContext eglContext, uint32_t bufferIndex, void* eglImage)
{
    m_bindEglImage = true;
    ASSERT(m_memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME || m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF);
    ASSERT(bufferIndex<m_eglVaapiImages.size());
    bufferIndex = std::min(bufferIndex, m_actualOutBufferCount-1);
    *(EGLImageKHR*)eglImage = m_eglVaapiImages[bufferIndex]->createEglImage(eglDisplay, eglContext, m_memoryType);

    return 0;
}
#endif
