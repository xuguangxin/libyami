/*
 * Copyright (C) 2014 Intel Corporation. All rights reserved.
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

#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <linux/videodev2.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#if __ENABLE_WAYLAND__
#include <va/va_wayland.h>
#endif
#include "v4l2_decode.h"
#include "VideoDecoderHost.h"
#include "common/log.h"
#include "common/common_def.h"
#include "vaapi/vaapidisplay.h"

#define INT64_TO_TIMEVAL(i64, time_val)                 \
    do {                                                \
        time_val.tv_sec = (int32_t)(i64 >> 31);         \
        time_val.tv_usec = (int32_t)(i64 & 0x7fffffff); \
    } while (0)
#define TIMEVAL_TO_INT64(i64, time_val)       \
    do {                                      \
        i64 = time_val.tv_sec;                \
        i64 = (i64 << 31) + time_val.tv_usec; \
    } while (0)

//return if we have error
#define ERROR_RETURN(no) \
    do {                 \
        if (no) {        \
            errno = no;  \
            return -1;   \
        }                \
    } while (0)

#define CHECK(cond)                      \
    do {                                 \
        if (!(cond)) {                   \
            ERROR("%s is false", #cond); \
            ERROR_RETURN(EINVAL);        \
        }                                \
    } while (0)

//check condition in post messsage.
#define PCHECK(cond)                     \
    do {                                 \
        if (!(cond)) {                   \
            m_state = kError;            \
            ERROR("%s is false", #cond); \
            return;                      \
        }                                \
    } while (0)

#define SEND(func)                     \
    do {                               \
        int32_t ret_ = sendTask(func); \
        ERROR_RETURN(ret_);            \
    } while (0)

const static uint32_t kDefaultInputSize = 1024 * 1024;

using std::bind;
using std::ref;

V4l2Decoder::Output::Output(V4l2Decoder* decoder)
    : m_decoder(decoder)
{
}

#if __ENABLE_EGL__
#include "egl/egl_vaapi_image.h"
class EglOutput : public V4l2Decoder::Output {
public:
    EglOutput(V4l2Decoder* decoder)
        : V4l2Decoder::Output(decoder)
        , m_memoryType(VIDEO_DATA_MEMORY_TYPE_DRM_NAME)
    {
    }
    virtual int32_t requestBuffers(uint32_t count)
    {
        v4l2_pix_format_mplane& format = m_decoder->m_outputFormat.fmt.pix_mp;
        DisplayPtr& display = m_decoder->m_display;
        CHECK(bool(display));
        CHECK(format.width && format.height);
        m_eglVaapiImages.clear();
        for (uint32_t i = 0; i < count; i++) {
            SharedPtr<EglVaapiImage> image(new EglVaapiImage(display->getID(), format.width, format.height));
            if (!image->init()) {
                ERROR("Create egl vaapi image failed");
                m_eglVaapiImages.clear();
                return EINVAL;
            }
            m_eglVaapiImages.push_back(image);

        }
        return 0;
    }
    int32_t useEglImage(EGLDisplay eglDisplay, EGLContext eglContext, uint32_t bufferIndex, void* eglImage)
    {
        CHECK(m_memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME || m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF);
        CHECK(bufferIndex < m_eglVaapiImages.size());
        *(EGLImageKHR*)eglImage = m_eglVaapiImages[bufferIndex]->createEglImage(eglDisplay, eglContext, m_memoryType);
        return 0;
    }
    void output(SharedPtr<VideoFrame>& frame)
    {
        uint32_t index;
        if (!m_decoder->m_out.get(index)) {
            ERROR("bug: can't get index");
            return;
        }
        ASSERT(index < m_eglVaapiImages.size());
        m_eglVaapiImages[index]->blt(frame);
        m_decoder->m_out.put(index);
        m_decoder->setDeviceEvent(0);
    }
    bool isAlloccationDone()
    {
        return !m_eglVaapiImages.empty();
    }
    bool isSurfaceReady()
    {
        uint32_t index;
        return m_decoder->m_out.peek(index);
    }

    int32_t deque(struct v4l2_buffer* buf) {
        uint32_t index;
        if (!m_decoder->m_out.deque(index)) {
            ERROR_RETURN(EAGAIN);
        }
        buf->index = index;
        //chrome will use this value.
        buf->m.planes[0].bytesused = 1;
        buf->m.planes[1].bytesused = 1;
        //buf->timestamp = d->timestamp;
        return 0;
    }


private:
    std::vector<SharedPtr<EglVaapiImage> > m_eglVaapiImages;
    VideoDataMemoryType m_memoryType;
};
#endif

V4l2Decoder::V4l2Decoder()
    : m_inputOn(false)
    , m_outputOn(false)
{
    memset(&m_inputFormat, 0, sizeof(m_inputFormat));
    memset(&m_outputFormat, 0, sizeof(m_outputFormat));
    memset(&m_lastFormat, 0, sizeof(m_lastFormat));
    m_output.reset(new EglOutput(this));
    m_state = kUnStarted;
}

V4l2Decoder::~V4l2Decoder()
{

}

void V4l2Decoder::releaseCodecLock(bool lockable)
{
    m_decoder->releaseLock(lockable);
}

bool V4l2Decoder::start()
{
    return false;
}

bool V4l2Decoder::stop()
{
    return true;
}

bool V4l2Decoder::inputPulse(uint32_t index)
{

    return true; // always return true for decode; simply ignored unsupported nal
}

#if __ENABLE_WAYLAND__
bool V4l2Decoder::outputPulse(uint32_t &index)
{
    return true;
}
#elif __ENABLE_EGL__
bool V4l2Decoder::outputPulse(uint32_t &index)
{
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

    return true;
}

bool V4l2Decoder::acceptInputBuffer(struct v4l2_buffer *qbuf)
{

    return true;
}

bool V4l2Decoder::giveOutputBuffer(struct v4l2_buffer *dqbuf)
{

    return true;
}

#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 YAMI_FOURCC('V', 'P', '9', '0')
#endif

int32_t V4l2Decoder::ioctl(int command, void* arg)
{
    DEBUG("fd: %d, ioctl command: %s", m_fd[0], IoctlCommandString(command));
    switch (command) {
    case VIDIOC_QBUF: {
        struct v4l2_buffer* qbuf = static_cast<struct v4l2_buffer*>(arg);
        return onQueueBuffer(qbuf);
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer* dqbuf = static_cast<struct v4l2_buffer*>(arg);
        return onDequeBuffer(dqbuf);
    }
    case VIDIOC_STREAMON: {
        __u32 type = *((__u32*)arg);
        return onStreamOn(type);
    }
    case VIDIOC_STREAMOFF: {
        __u32 type = *((__u32*)arg);
        return onStreamOff(type);
    }
    case VIDIOC_QUERYCAP: {
        return V4l2CodecBase::ioctl(command, arg);
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers* reqbufs = static_cast<struct v4l2_requestbuffers*>(arg);
        return onRequestBuffers(reqbufs);
    }
    case VIDIOC_S_FMT: {
        struct v4l2_format* format = static_cast<struct v4l2_format*>(arg);
        return onSetFormat(format);
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
        return onQueryBuffer(buf);
    }
    case VIDIOC_SUBSCRIBE_EVENT: {
        struct v4l2_event_subscription* sub = static_cast<struct v4l2_event_subscription*>(arg);
        return onSubscribeEvent(sub);
    }
    case VIDIOC_DQEVENT: {
        // ::DequeueEvents
        struct v4l2_event* ev = static_cast<struct v4l2_event*>(arg);
        return onDequeEvent(ev);
    }
    case VIDIOC_G_FMT: {
        // ::GetFormatInfo
        struct v4l2_format* format = static_cast<struct v4l2_format*>(arg);
        return onGetFormat(format);
    }
    case VIDIOC_G_CTRL: {
        // ::CreateOutputBuffers
        struct v4l2_control* ctrl = static_cast<struct v4l2_control*>(arg);
        return onGetCtrl(ctrl);
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc* fmtdesc = static_cast<struct v4l2_fmtdesc*>(arg);
        return onEnumFormat(fmtdesc);
    }
    case VIDIOC_G_CROP: {
        struct v4l2_crop* crop = static_cast<struct v4l2_crop*>(arg);
        return onGetCrop(crop);
    }
    default: {
        ERROR("unknown ioctrl command: %d", command);
        return -1;
    }
    }
}

bool V4l2Decoder::needReallocation(const VideoFormatInfo* format)
{
    bool ret = m_lastFormat.surfaceWidth != format->surfaceWidth
        || m_lastFormat.surfaceHeight != format->surfaceHeight
        || m_lastFormat.surfaceNumber != format->surfaceNumber
        || m_lastFormat.fourcc != format->fourcc;
    m_lastFormat = *format;
    return ret;
}

VideoDecodeBuffer* V4l2Decoder::peekInput()
{
    uint32_t index;
    if (!m_in.peek(index))
        return NULL;
    ASSERT(index < m_inputFrames.size());
    VideoDecodeBuffer *inputBuffer = &(m_inputFrames[index]);
    return inputBuffer;
}

void V4l2Decoder::consumeInput()
{
    PCHECK(m_thread.isCurrent());
    uint32_t index;
    if (!m_in.get(index)) {
        ERROR("bug: can't get from input");
        return;
    }
    m_in.put(index);
    setDeviceEvent(0);
}

void V4l2Decoder::getInputJob()
{
    PCHECK(m_thread.isCurrent());
    PCHECK(bool(m_decoder));
    if (m_state != kGetInput) {
        ERROR("early out, state = %d", m_state);
        return;
    }
    VideoDecodeBuffer *inputBuffer = peekInput();
    if (!inputBuffer) {
        ERROR("early out, no input buffer");
        m_state = kWaitInput;
        return;
    }
    YamiStatus status = m_decoder->decode(inputBuffer);
    if (status == YAMI_DECODE_FORMAT_CHANGE) {
        const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
        PCHECK(outFormat);

        if (needReallocation(outFormat)) {
            m_state = kWaitAllocation;
        }
        setCodecEvent();
        ERROR("early out, format changed to %dx%d, surface size is %dx%d",
            outFormat->width, outFormat->height, outFormat->surfaceWidth, outFormat->surfaceHeight);
        return;
    }
    consumeInput();
    m_state = kGetSurface;
    post(bind(&V4l2Decoder::getSurfaceJob, this));
}

void V4l2Decoder::inputReadyJob()
{
    PCHECK(m_thread.isCurrent());
    if (m_state == kWaitInput) {
        m_state = kGetInput;
        getInputJob();
    }
}

void V4l2Decoder::getSurfaceJob()
{
    PCHECK(m_thread.isCurrent());
    PCHECK(bool(m_decoder));
    if (m_state != kGetSurface) {
        DEBUG("early out, state = %d", m_state);
        return;
    }
    while (m_output->isSurfaceReady()) {
        SharedPtr<VideoFrame> frame = m_decoder->getOutput();
        if (!frame) {
            DEBUG("early out, no frame");
            m_state = kGetInput;
            post(bind(&V4l2Decoder::getInputJob, this));
            return;
        }
        m_output->output(frame);
    }
    m_state = kWaitSurface;
}

void V4l2Decoder::outputReadyJob()
{
    PCHECK(m_thread.isCurrent());
    if (m_state == kWaitSurface) {
        m_state = kGetSurface;
        getSurfaceJob();
    }
}

void V4l2Decoder::allocationDoneJob()
{
    PCHECK(m_thread.isCurrent());
    if (m_state == kWaitAllocation) {
        m_state = kGetInput;
        getInputJob();
    }
}

int32_t V4l2Decoder::onQueueBuffer(v4l2_buffer* buf)
{
    CHECK(buf);
    uint32_t type = buf->type;
    CHECK(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        || V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        CHECK(buf->memory == V4L2_MEMORY_MMAP);
        CHECK(buf->length == 1);
        CHECK(buf->index < m_inputFrames.size());

        VideoDecodeBuffer& inputBuffer = m_inputFrames[buf->index];
        inputBuffer.size = buf->m.planes[0].bytesused; // one plane only
        if (!inputBuffer.size) // EOS
            inputBuffer.data = NULL;
        TIMEVAL_TO_INT64(inputBuffer.timeStamp, buf->timestamp);

        m_in.queue(buf->index);
        post(bind(&V4l2Decoder::inputReadyJob, this));
        return 0;
    }

    m_out.queue(buf->index);
    post(bind(&V4l2Decoder::outputReadyJob, this));
    return 0;
}

int32_t V4l2Decoder::onDequeBuffer(v4l2_buffer* buf)
{
    CHECK(buf);
    uint32_t type = buf->type;
    CHECK(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        || V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        CHECK(m_inputOn);
        uint32_t index;
        if (!m_in.deque(index)) {
            ERROR_RETURN(EAGAIN);
        }
        buf->index = index;
        return 0;
    }
    CHECK(m_outputOn);
    return m_output->deque(buf);
}
int32_t V4l2Decoder::onStreamOn(uint32_t type)
{
    CHECK(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        || V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {

        if (!m_display) {
            m_display = VaapiDisplay::create(m_nativeDisplay);
            CHECK(bool(m_display));
        }

        CHECK(!m_inputOn);
        m_inputOn = true;
        CHECK(m_thread.start());

        post(bind(&V4l2Decoder::startDecoderJob, this));
        return 0;
    }
    CHECK(!m_outputOn);
    m_outputOn = true;
    CHECK(m_output->isAlloccationDone());
    post(bind(&V4l2Decoder::allocationDoneJob, this));
    return 0;
}

void V4l2Decoder::flushDecoderJob()
{
    if (m_decoder)
        m_decoder->flush();
    m_out.clearPipe();
    m_state = kStopped;
}
int32_t V4l2Decoder::onStreamOff(uint32_t type)
{
    CHECK(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        || V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        if (m_inputOn) {
            post(bind(&V4l2Decoder::flushDecoderJob, this));
            m_thread.stop();
            m_in.clearPipe();
            m_inputOn = false;
            m_state = kUnStarted;
        }
        return 0;
    }
    m_outputOn = false;
    return 0;
}

int32_t V4l2Decoder::onRequestBuffers(const v4l2_requestbuffers* req)
{
    CHECK(req);
    uint32_t type = req->type;
    uint32_t count = req->count;
    CHECK(type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        || V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        CHECK(req->memory == V4L2_MEMORY_MMAP);
        uint32_t size = m_inputFormat.fmt.pix_mp.plane_fmt[0].sizeimage;

        m_inputFrames.resize(count);
        if (count) {
            //this means we really need allocate space
            if (!size)
                m_inputFormat.fmt.pix_mp.plane_fmt[0].sizeimage = kDefaultInputSize;
        }
        m_inputSpace.resize(count * size);
        m_inputFrames.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            VideoDecodeBuffer& frame = m_inputFrames[i];
            memset(&frame, 0, sizeof(frame));
            frame.data = &m_inputSpace[i * size];
        }
        return 0;
    }
    CHECK(req->memory == V4L2_MEMORY_MMAP);
    return m_output->requestBuffers(count);
}

int32_t V4l2Decoder::onSetFormat(v4l2_format* format)
{
    CHECK(format);
    CHECK(!m_inputOn && !m_outputOn);

    if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        uint32_t size;
        memcpy(&size, format->fmt.raw_data, sizeof(uint32_t));

        CHECK(size <= (sizeof(format->fmt.raw_data) - sizeof(uint32_t)));

        uint8_t* ptr = format->fmt.raw_data;
        ptr += sizeof(uint32_t);
        m_codecData.assign(ptr, ptr + size);
        return 0;
    }

    if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        CHECK(format->fmt.pix_mp.num_planes == 1);
        CHECK(format->fmt.pix_mp.plane_fmt[0].sizeimage);
        memcpy(&m_inputFormat, format, sizeof(*format));
        return 0;
    }

    ERROR("unknow type: %d of setting format VIDIOC_S_FMT", format->type);
    return -1;
}

int32_t V4l2Decoder::onQueryBuffer(v4l2_buffer* buf)
{
    CHECK(buf);
    CHECK(buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    CHECK(buf->memory == V4L2_MEMORY_MMAP);
    CHECK(m_inputFormat.fmt.pix_mp.num_planes == 1);

    uint32_t idx = buf->index;
    uint32_t size = m_inputFormat.fmt.pix_mp.plane_fmt[0].sizeimage;
    CHECK(size);
    buf->m.planes[0].length = size;
    buf->m.planes[0].m.mem_offset = size * idx;

    return 0;
}

int32_t V4l2Decoder::onSubscribeEvent(v4l2_event_subscription* sub)
{
    CHECK(sub->type == V4L2_EVENT_RESOLUTION_CHANGE);
    /// resolution change event is must, we always do so
    return 0;
}

int32_t V4l2Decoder::onDequeEvent(v4l2_event* ev)
{
    CHECK(ev);
    if (hasCodecEvent()) {
        ev->type = V4L2_EVENT_RESOLUTION_CHANGE;
        clearCodecEvent();
        return 0;
    }
    return -1;
}

int32_t V4l2Decoder::onGetFormat(v4l2_format* format)
{
    CHECK(format && format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    CHECK(m_inputOn);

    SEND(bind(&V4l2Decoder::getFormatTask, this, format));

    //save it.
    m_outputFormat = *format;
    return 0;
}

int32_t V4l2Decoder::onGetCtrl(v4l2_control* ctrl)
{
    CHECK(ctrl);
    CHECK(ctrl->id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);

    SEND(bind(&V4l2Decoder::getCtrlTask, this, ctrl));
    return 0;
}

int32_t V4l2Decoder::onEnumFormat(v4l2_fmtdesc* fmtdesc)
{
    uint32_t type = fmtdesc->type;
    uint32_t index = fmtdesc->index;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        CHECK(!index);
        fmtdesc->pixelformat = V4L2_PIX_FMT_NV12M;
        return 0;
    }

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        //TODO: query from libyami when capablity api is ready.
        const static uint32_t supported[] = {
            V4L2_PIX_FMT_H264,
            V4L2_PIX_FMT_VC1,
            V4L2_PIX_FMT_MPEG2,
            V4L2_PIX_FMT_JPEG,
            V4L2_PIX_FMT_VP8,
            V4L2_PIX_FMT_VP9,
        };
        CHECK(index < N_ELEMENTS(supported));
        fmtdesc->pixelformat = supported[index];
        return 0;
    }
    return -1;
}

int32_t V4l2Decoder::onGetCrop(v4l2_crop* crop)
{
    CHECK(crop->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    ASSERT(0);
    return 0;
    //return sendTask(m_decoderThread, std::bind(getCrop,crop)));
}

void V4l2Decoder::startDecoderJob()
{
    PCHECK(m_state == kUnStarted);

    if (m_decoder) {
        DEBUG("early out, start decode after seek");
        return;
    }

    const char* mime = mimeFromV4l2PixelFormat(m_inputFormat.fmt.pix_mp.pixelformat);

    m_decoder.reset(
        createVideoDecoder(mime), releaseVideoDecoder);
    if (!m_decoder) {
        ERROR("create display failed");
        m_display.reset();
        return;
    }

    YamiStatus status;
    VideoConfigBuffer config;
    memset(&config, 0, sizeof(config));
    config.width = m_inputFormat.fmt.pix_mp.width;
    config.height = m_inputFormat.fmt.pix_mp.height;
    config.data = &m_codecData[0];
    config.size = m_codecData.size();

    status = m_decoder->start(&config);
    if (status != YAMI_SUCCESS) {
        ERROR("start decoder failed");
        return;
    }
    const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
    if (outFormat) {
        //we got format now, we are waiting for surface allocation.
        m_state = kWaitAllocation;
    }
    else {
        m_state = kGetInput;
        getInputJob();
    }
}

void V4l2Decoder::post(Job job)
{
    m_thread.post(job);
}

static void taskWrapper(int32_t& ret, Task& task)
{
    ret = task();
}

int32_t V4l2Decoder::sendTask(Task task)
{
    //if send fail, we will return EINVAL;
    int32_t ret = EINVAL;
    m_thread.send(bind(taskWrapper, ref(ret), ref(task)));
    return ret;
}

int32_t V4l2Decoder::getFormatTask(v4l2_format* format)
{
    CHECK(m_thread.isCurrent());
    CHECK(format);
    CHECK(bool(m_decoder));

    const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
    if (!outFormat)
        return EINVAL;

    memset(format, 0, sizeof(*format));
    format->fmt.pix_mp.width = outFormat->width;
    format->fmt.pix_mp.height = outFormat->height;

    //TODO: add support for P010
    format->fmt.pix_mp.num_planes = 2; //for NV12
    format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;

    //we can't fill format->fmt.pix_mp.plane_fmt[0].bytesperline
    //yet, since we did not creat surface.
    return 0;
}

int32_t V4l2Decoder::getCtrlTask(v4l2_control* ctrl)
{
    CHECK(m_thread.isCurrent());
    CHECK(ctrl);
    CHECK(bool(m_decoder));

    const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
    if (!outFormat)
        return EINVAL;

    //TODO: query this from outFormat;
    ctrl->value = 0;
    return 0;
}

#if 0
int32_t V4l2Decoder::ioctl(int command, void* arg)
{
    int32_t ret = 0;
    int port = -1;

    DEBUG("fd: %d, ioctl command: %s", m_fd[0], IoctlCommandString(command));
    switch (command) {
    case VIDIOC_QBUF: {
        struct v4l2_buffer *qbuf = static_cast<struct v4l2_buffer*>(arg);
#if __ENABLE_WAYLAND__
        // FIXME, m_outputBufferCountOnInit should be reset on output buffer change (for example: resolution change)
        // it is not must to init video frame here since we don't accepted external buffer for wayland yet.
        // however, external buffer may be used in the future
        if (qbuf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && m_streamOn[OUTPUT] == false) {
            m_outputBufferCountOnInit++;
            DEBUG("m_outputBufferCountOnInit: %d", m_outputBufferCountOnInit);
            if (m_outputBufferCountOnInit == m_reqBuffCnt) {
                mapVideoFrames(m_videoWidth, m_videoHeight);
            }
        }
#endif
        if (qbuf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            DEBUG("m_outputBufferCountQBuf: %d, buffer index: %d", m_outputBufferCountQBuf, qbuf->index);
            m_outputBufferCountQBuf++;
        }
    } // no break;
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF:
    case VIDIOC_DQBUF:
    case VIDIOC_QUERYCAP:
        ret = V4l2CodecBase::ioctl(command, arg);
        if (command == VIDIOC_STREAMON) {
            m_outputBufferCountQBuf = 0;
            m_outputBufferCountPulse = 0;
            m_outputBufferCountGive = 0;
        }
        break;
    case VIDIOC_REQBUFS: {
        ret = V4l2CodecBase::ioctl(command, arg);
        ASSERT(ret == 0);
        int port = -1;
        struct v4l2_requestbuffers *reqbufs = static_cast<struct v4l2_requestbuffers *>(arg);
        GET_PORT_INDEX(port, reqbufs->type, ret);
        if (port == OUTPUT) {
#if __ENABLE_WAYLAND__
            if (reqbufs->count)
                m_reqBuffCnt = reqbufs->count;
            else
                m_videoFrames.clear();
#elif __ENABLE_EGL__
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

        ASSERT(ret == 0);
        ASSERT(buffer->memory == m_memoryMode[port]);
        ASSERT(buffer->index < m_maxBufferCount[port]);
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
            m_pixelFormat[INPUT] = format->fmt.pix_mp.pixelformat;
            m_videoWidth = format->fmt.pix_mp.width;
            m_videoHeight = format->fmt.pix_mp.height;
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
        ASSERT(format && format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ASSERT(m_decoder);

        const VideoFormatInfo* outFormat = m_decoder->getFormatInfo();
        if (format && outFormat && outFormat->width && outFormat->height) {
            format->fmt.pix_mp.num_planes = m_bufferPlaneCount[OUTPUT];
            format->fmt.pix_mp.width = outFormat->width;
            format->fmt.pix_mp.height = outFormat->height;
            // XXX assumed output format and pitch

            format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;

            format->fmt.pix_mp.plane_fmt[0].bytesperline = outFormat->width;
            format->fmt.pix_mp.plane_fmt[1].bytesperline = outFormat->width % 2 ? outFormat->width+1 : outFormat->width;
            m_videoWidth = outFormat->width;
            m_videoHeight = outFormat->height;
            m_maxBufferSize[OUTPUT] = m_videoWidth * m_videoHeight + ((m_videoWidth +1)/2*2) * ((m_videoHeight+1)/2);
        } else {
            ret = EAGAIN;
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
#endif

#define MCHECK(cond)                     \
    do {                                 \
        if (!(cond)) {                   \
            ERROR("%s is false", #cond); \
            return NULL;                 \
        }                                \
    } while (0)

void* V4l2Decoder::mmap (void* addr, size_t length,
                      int prot, int flags, unsigned int offset)
{
    MCHECK(prot == (PROT_READ | PROT_WRITE));
    MCHECK(flags == MAP_SHARED);
    uint32_t size = m_inputFormat.fmt.pix_mp.plane_fmt[0].sizeimage;
    MCHECK(size);
    MCHECK(length == size);
    MCHECK(!(offset % size));
    MCHECK(offset / size < m_inputFrames.size());
    MCHECK(offset + size <= m_inputSpace.size());

    return &m_inputSpace[offset];
}

#undef MCHECK

void V4l2Decoder::flush()
{
    if (m_decoder)
        m_decoder->flush();
}

#if __ENABLE_EGL__
int32_t V4l2Decoder::useEglImage(EGLDisplay eglDisplay, EGLContext eglContext, uint32_t bufferIndex, void* eglImage)
{
    SharedPtr<EglOutput> output = DynamicPointerCast<EglOutput>(m_output);
    if (!output) {
        ERROR("can't cast m_output to EglOutput");
        return -1;
    }
    return output->useEglImage(eglDisplay, eglContext, bufferIndex, eglImage);
}
#endif
#if __ENABLE_WAYLAND__

class VideoFrameDeleter {
public:
    VideoFrameDeleter(VADisplay display)
        : m_display(display)
    {
    }
    void operator()(VideoFrame* frame)
    {
        VASurfaceID id = (VASurfaceID)frame->surface;
        DEBUG("destroy VASurface 0x%x", id);
        vaDestroySurfaces(m_display, &id, 1);
        delete frame;
    }

private:
    VADisplay m_display;
};


SharedPtr<VideoFrame> V4l2Decoder::createVaSurface(uint32_t width, uint32_t height)
{
    SharedPtr<VideoFrame> frame;
    if (!m_display) {
        ERROR("bug: no display to create vasurface");
        return frame;
    }

    VASurfaceID id;
    VASurfaceAttrib attrib;
    uint32_t rtFormat = VA_RT_FORMAT_YUV420;
    int pixelFormat = VA_FOURCC_NV12;
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = pixelFormat;

    VAStatus vaStatus = vaCreateSurfaces(m_display->getID(), rtFormat, width, height, &id, 1, &attrib, 1);
    if (vaStatus != VA_STATUS_SUCCESS)
        return frame;

    frame.reset(new VideoFrame, VideoFrameDeleter(m_display->getID()));
    memset(frame.get(), 0, sizeof(VideoFrame));
    frame->surface = static_cast<intptr_t>(id);
    frame->crop.width = width;
    frame->crop.height = height;
    return frame;
}

bool V4l2Decoder::mapVideoFrames(uint32_t width, uint32_t height)
{
    SharedPtr<VideoFrame> frame;
    for (uint32_t i = 0; i < m_reqBuffCnt; i++) {
        frame = createVaSurface(width, height);
        if (!frame)
            return false;
        m_videoFrames.push_back(frame);
    }
    return true;
}
#endif
