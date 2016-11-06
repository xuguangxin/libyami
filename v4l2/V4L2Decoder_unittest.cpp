/*
 * Copyright 2016 Intel Corporation
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

//
// The unittest header must be included before va_x11.h (which might be included
// indirectly).  The va_x11.h includes Xlib.h and X.h.  And the X headers
// define 'Bool' and 'None' preprocessor types.  Gtest uses the same names
// to define some struct placeholders.  Thus, this creates a compile conflict
// if X defines them before gtest.  Hence, the include order requirement here
// is the only fix for this right now.
//
// See bug filed on gtest at https://github.com/google/googletest/issues/371
// for more details.
//
// library headers
#include "common/unittest.h"
#include "common/common_def.h"
#include "decoder/FrameData.h"

#include <decoder/vaapidecoder_h264.h>

// primary header
#include "v4l2_wrapper.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <vector>

namespace YamiMediaCodec {

#define ASSERT_IOCTL(cmd, arg) ASSERT_EQ(0, YamiV4L2_Ioctl(fd, cmd, arg))

const uint32_t kMaxInputSize = 4 * 1024 * 1024;

static FrameData h264data[] = {
    g_avc8x8I,
    g_avc8x8P,
    g_avc8x8B,
};

struct InputBuffer {
    void* addr;
    size_t len;
};
typedef std::vector<InputBuffer> InputBuffers;

void setInputFormat(int fd, uint32_t pixelformat)
{
    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    format.fmt.pix_mp.pixelformat = pixelformat;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = kMaxInputSize;
    ASSERT_EQ(0, YamiV4L2_Ioctl(fd, VIDIOC_S_FMT, &format));
}

void createInputBuffers(int fd, uint32_t count, InputBuffers& buffers)
{

    struct v4l2_requestbuffers reqbufs;
    memset(&reqbufs, 0, sizeof(reqbufs));
    reqbufs.count = count;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    ASSERT_IOCTL(VIDIOC_REQBUFS, &reqbufs);
    EXPECT_EQ(count, reqbufs.count);
    buffers.resize(reqbufs.count);
    for (uint32_t i = 0; i < reqbufs.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane plane;
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = &plane;
        buf.length = 1;
        ASSERT_IOCTL(VIDIOC_QUERYBUF, &buf);
        void* addr = YamiV4L2_Mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, plane.m.mem_offset);
        ASSERT_NE(MAP_FAILED, addr);
        buffers[i].addr = addr;
        buffers[i].len = plane.length;
    }
}

void createOutputBuffers(int fd, uint32_t count)
{
    struct v4l2_requestbuffers reqbufs;
    memset(&reqbufs, 0, sizeof(reqbufs));
    reqbufs.count  = count;
    reqbufs.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    ASSERT_IOCTL(VIDIOC_REQBUFS, &reqbufs);
    EXPECT_EQ(count, reqbufs.count);
}

void setOutputFormat(int fd, uint32_t pixelformat)
{
    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.pixelformat = pixelformat;
    ASSERT_IOCTL(VIDIOC_S_FMT, &format);
}

void subscribeEvent(int fd)
{
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_RESOLUTION_CHANGE;
    ASSERT_IOCTL(VIDIOC_SUBSCRIBE_EVENT, &sub);
}

void sendInputBuffers(int fd, InputBuffers& inputBuffers)
{

    size_t size = N_ELEMENTS(h264data);
    ASSERT_GE(inputBuffers.size(), size);
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ASSERT_IOCTL(VIDIOC_STREAMON, &type);
    for (size_t i = 0; i < size; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane plane;
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.index = i;
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = &plane;
        plane.bytesused = h264data[i].m_size;
        buf.length = 1;
        memcpy(inputBuffers[i].addr, h264data[i].m_data, h264data[i].m_size);
        ASSERT_IOCTL(VIDIOC_QBUF, &buf);
    }
}

void streamOnOutput(int fd)
{
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ASSERT_IOCTL(VIDIOC_STREAMON, &type);
}

void waitForFormatChange(int fd)
{
    bool eventPending;
    //ASSERT_NE(0, YamiV4L2_Poll(fd, true, &eventPending));
    YamiV4L2_Poll(fd, true, &eventPending);
    ASSERT_TRUE(eventPending);

    struct v4l2_event evt;
    memset(&evt, 0, sizeof(evt));
    ASSERT_IOCTL(VIDIOC_DQEVENT, &evt);
    ASSERT_EQ(V4L2_EVENT_RESOLUTION_CHANGE, evt.type);
}

void getResolution(int fd, uint32_t& width, uint32_t& height, uint32_t& dpbSize, bool& again)
{
    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    again = false;
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (YamiV4L2_Ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        if (errno == EINVAL) {
            again = true;
        }
        return;
    }
    width = format.fmt.pix_mp.width;
    height = format.fmt.pix_mp.height;

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    ASSERT_IOCTL(VIDIOC_G_CTRL, &ctrl);
    dpbSize = ctrl.value;
}

void streamOffInput(int fd)
{
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ASSERT_IOCTL(VIDIOC_STREAMOFF, &type);
}

void streamOffOutput(int fd)
{
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ASSERT_IOCTL(VIDIOC_STREAMOFF, &type);
}

TEST(V4L2DecoderTest, APITest)
{

    //workaround
    SharedPtr<IVideoDecoder> d(new VaapiDecoderH264);
    d.reset();

    int fd = YamiV4L2_Open("decoder", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(fd, 0);

    //query caps
    struct v4l2_capability caps;
    memset(&caps, 0, sizeof(caps));
    ASSERT_IOCTL(VIDIOC_QUERYCAP, &caps);

    ASSERT_EQ(V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING, caps.capabilities);

    setInputFormat(fd, V4L2_PIX_FMT_H264);
    InputBuffers inputBuffers;
    createInputBuffers(fd, N_ELEMENTS(h264data), inputBuffers);

    setOutputFormat(fd, V4L2_PIX_FMT_NV12M);

    subscribeEvent(fd);

    sendInputBuffers(fd, inputBuffers);

    uint32_t width = 0, height = 0, dpbSize = 0;
    bool again;
    do {
        getResolution(fd, width, height, dpbSize, again);
    } while (again);
    ASSERT_GT(width, 0);
    ASSERT_GT(height, 0);
    ASSERT_GE(dpbSize, 0);

    createOutputBuffers(fd, dpbSize+3);

    streamOnOutput(fd);

    streamOffInput(fd);
    streamOffOutput(fd);

    ASSERT_EQ(0, YamiV4L2_Close(fd));
}
};
