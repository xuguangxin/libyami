/*
 *  vppinputasync.cpp - threaded vpp input
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
#include "encodeoutputasync.h"

EncodeOutputAsync::EncodeOutputAsync()
    :m_cond(m_lock)
    ,m_queueSize(0)
    ,m_quit(false)
{
}


SharedPtr<EncodeOutputAsync>
EncodeOutputAsync::create(const SharedPtr<EncodeOutput>& output, uint32_t queueSize, uint32_t maxOutSize)
{
    SharedPtr<EncodeOutputAsync> nil;
    if (!output || !queueSize)
        return nil;
    SharedPtr<EncodeOutputAsync> async(new EncodeOutputAsync());
    if (!async->init(output, queueSize, maxOutSize)) {
        ERROR("init EncodeOutputAsync failed");
        return nil;
    }
    return async;
}

void* EncodeOutputAsync::start(void* async)
{
    EncodeOutputAsync* input = (EncodeOutputAsync*)async;
    input->loop();
    return NULL;
}

void EncodeOutputAsync::loop()
{
    while (1) {
        SharedPtr<EncodedBuffer> encoded;
        {
            AutoLock lock(m_lock);
            while (m_queue.empty()) {
                if (m_quit)
                    return;
                m_cond.wait();
            }
            encoded = m_queue.front();
            m_queue.pop_front();
        }
        Encode_Status status = encoded->getOutput(&m_outputBuffer);
        if (status == ENCODE_SUCCESS
            && !m_output->write(m_outputBuffer.data, m_outputBuffer.dataSize)) {
            ERROR("write to file failed");
        }
   }
}

void EncodeOutputAsync::initOuputBuffer(uint32_t maxOutSize)
{
    m_buffer.resize(maxOutSize);
    m_outputBuffer.bufferSize = maxOutSize;
    m_outputBuffer.format = OUTPUT_EVERYTHING;
    m_outputBuffer.data = &m_buffer[0];

}

bool EncodeOutputAsync::init(const SharedPtr<EncodeOutput>& output, uint32_t queueSize, uint32_t maxOutSize)
{
    m_output = output;
    m_queueSize = queueSize;
    initOuputBuffer(maxOutSize);
    if (pthread_create(&m_thread, NULL, start, this)) {
        ERROR("create thread failed");
        return false;
    }
    return true;

}

bool EncodeOutputAsync::write(const SharedPtr<EncodedBuffer>& encoded)
{
    AutoLock lock(m_lock);
    while (m_queue.size() > m_queueSize) {
        m_cond.wait();
    }
    m_queue.push_back(encoded);
    m_cond.signal();
    return true;
}

EncodeOutputAsync::~EncodeOutputAsync()
{
    {
        AutoLock lock(m_lock);
        m_quit = true;
        m_cond.signal();
    }
    pthread_join(m_thread, NULL);
}

