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
#ifndef v4l2_decode_h
#define v4l2_decode_h

#include <vector>

#include "v4l2_codecbase.h"
#include "VideoDecoderInterface.h"
#include "common/Thread.h"
#include "common/Functional.h"
#include <BufferPipe.h>

#if __ENABLE_EGL__
namespace YamiMediaCodec {
    class EglVaapiImage;
}
#endif

using namespace YamiMediaCodec;
typedef SharedPtr < IVideoDecoder > DecoderPtr;
typedef std::function<int32_t(void)> Task;
class V4l2Decoder : public V4l2CodecBase
{
  public:
    V4l2Decoder();
     ~V4l2Decoder();

    virtual int32_t ioctl(int request, void* arg);
    virtual void* mmap(void* addr, size_t length,
                         int prot, int flags, unsigned int offset);
#if __ENABLE_EGL__
    virtual int32_t useEglImage(EGLDisplay eglDisplay, EGLContext eglContext, uint32_t buffer_index, void* egl_image);
#endif

    class Output {
    public:
        Output(V4l2Decoder* decoder);
        virtual int32_t requestBuffers(uint32_t count) = 0;
        virtual void output(SharedPtr<VideoFrame>& frame) = 0;
        virtual bool isAlloccationDone() = 0;
        virtual bool isSurfaceReady() = 0;
        virtual int32_t deque(struct v4l2_buffer* buf) =  0;

    protected:
        V4l2Decoder* m_decoder;
    };
    friend class EglOutput;

  protected:
    virtual bool start();
    virtual bool stop();
    virtual bool acceptInputBuffer(struct v4l2_buffer *qbuf);
    virtual bool giveOutputBuffer(struct v4l2_buffer *dqbuf);
    virtual bool inputPulse(uint32_t index);
    virtual bool outputPulse(uint32_t &index);
    virtual bool recycleOutputBuffer(int32_t index);
    virtual bool recycleInputBuffer(struct v4l2_buffer *qbuf);
    virtual void releaseCodecLock(bool lockable);
    virtual void flush();

  private:
      int32_t onQueueBuffer(v4l2_buffer*);
      int32_t onDequeBuffer(v4l2_buffer*);
      int32_t onStreamOn(uint32_t type);
      int32_t onStreamOff(uint32_t type);
      int32_t onRequestBuffers(const v4l2_requestbuffers*);
      int32_t onSetFormat(v4l2_format*);
      int32_t onQueryBuffer(v4l2_buffer*);
      int32_t onSubscribeEvent(v4l2_event_subscription*);
      int32_t onDequeEvent(v4l2_event*);
      int32_t onGetFormat(v4l2_format*);
      int32_t onGetCtrl(v4l2_control*);
      int32_t onEnumFormat(v4l2_fmtdesc*);
      int32_t onGetCrop(v4l2_crop*);

      //jobs send to decoder thread
      void startDecoderJob();
      void getInputJob();
      void inputReadyJob();
      void getSurfaceJob();
      void outputReadyJob();
      void allocationDoneJob();
      void flushDecoderJob();



      //tasks send to decoder thread
      int32_t getFormatTask(v4l2_format*);
      int32_t getCtrlTask(v4l2_control*);

      //help functions
      int32_t sendTask(Task task);
      void post(Job job);
      VideoDecodeBuffer* peekInput();
      void consumeInput();
      bool needReallocation(const VideoFormatInfo*);


      bool m_inputOn;
      v4l2_format m_inputFormat;
      std::vector<VideoDecodeBuffer> m_inputFrames;
      std::vector<uint8_t> m_inputSpace;
      BufferPipe<uint32_t> m_in;

      bool m_outputOn;
      v4l2_format m_outputFormat;

      //decoder thread
      Thread m_thread;

      enum State {
          kUnStarted, //decoder thread is not started.
          kWaitAllocation, //wait buffer allocation
          kGetInput,
          kWaitInput,
          kGetSurface, // try get surface for decoder
          kWaitSurface, // wait for input
          kFormatChanged, // detected format change. Waiting new surfacee.
          kStopped, // stopped by use
          kError, // have a error
      };
      State m_state;
      SharedPtr<Output> m_output;
      BufferPipe<uint32_t> m_out;
      VideoFormatInfo m_lastFormat;

#if __ENABLE_WAYLAND__
    SharedPtr<VideoFrame> createVaSurface(uint32_t width, uint32_t height);
    bool mapVideoFrames(uint32_t width, uint32_t height);
    uint32_t m_reqBuffCnt;
    std::vector<SharedPtr<VideoFrame> > m_videoFrames;
#endif
    DisplayPtr m_display;
    DecoderPtr m_decoder;
    std::vector<uint8_t> m_codecData;
#if __ENABLE_EGL__
    std::vector <SharedPtr<EglVaapiImage> > m_eglVaapiImages;
#endif
};
#endif
