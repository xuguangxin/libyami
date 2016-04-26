/*
 *  encodeoutputasync.h - threaded vpp input
 *
 *  Copyright (C) 2015 Intel Corporation
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
#ifndef encodeoutputasync_h
#define encodeoutputasync_h
#include "common/condition.h"
#include "common/lock.h"
#include <deque>

#include "vppinputoutput.h"
#include "encodeinput.h"

class EncodeOutputAsync
{
public:

    bool write(const SharedPtr<EncodedBuffer>& encoded);

    static SharedPtr<EncodeOutputAsync>
    create(const SharedPtr<EncodeOutput>& output, uint32_t queueSize, uint32_t maxOutSize);

    EncodeOutputAsync();
    virtual ~EncodeOutputAsync();


private:
    bool init(const SharedPtr<EncodeOutput>& output, uint32_t queueSize, uint32_t maxOutSize);
    static void* start(void* async);
    void loop();
    void initOuputBuffer(uint32_t maxOutSize);

    Condition  m_cond;
    Lock       m_lock;

    VideoEncOutputBuffer m_outputBuffer;
    std::vector<uint8_t> m_buffer;
    SharedPtr<EncodeOutput> m_output;

    typedef std::deque<SharedPtr<EncodedBuffer> > EncodedQueue;
    EncodedQueue m_queue;
    uint32_t   m_queueSize;

    pthread_t  m_thread;
    bool       m_quit;
};
#endif //encodeoutputasync_h

