/*
 *  thread.cpp - executor pattern, you can post and send message to thread.
 *
 *  Copyright (C) 2016 Intel Corporation
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

#include "Thread.h"
#include "Functional.h"
#include "log.h"

#include <assert.h>

namespace YamiMediaCodec {

Thread::Thread(const char* name)
    : m_name(name)
    , m_started(false)
    , m_thread(INVALID_ID)
    , m_cond(m_lock)
    , m_sent(m_lock)
{
}

bool Thread::start()
{
    AutoLock lock(m_lock);
    if (m_started)
        return false;
    if (pthread_create(&m_thread, NULL, init, this)) {
        ERROR("create thread %s failed", m_name.c_str());
        m_thread = INVALID_ID;
        return false;
    }
    m_started = true;
    return true;
}

void* Thread::init(void* thread)
{
    Thread* t = (Thread*)thread;
    t->loop();
    return NULL;
}

void Thread::loop()
{
    while (1) {
        AutoLock lock(m_lock);
        if (m_queue.empty()) {
            m_cond.wait();
            if (!m_started)
                return;
        }
        else {
            Runnable& r = m_queue.front();
            m_lock.release();
            r();
            m_lock.acquire();
            m_queue.pop_front();
        }
    }
}

void Thread::enqueue(const Runnable& r)
{
    m_queue.push_back(r);
    m_cond.signal();
}

void Thread::post(const Runnable& r)
{
    AutoLock lock(m_lock);
    if (!m_started) {
        ERROR("%s: post job after stop()", m_name.c_str());
        return;
    }
    enqueue(r);
}

void Thread::sentJob(const Runnable& r, bool& flag)
{
    r();
    AutoLock lock(m_lock);
    flag = true;
    m_sent.broadcast();
}

bool Thread::send(const Runnable& c)
{
    bool flag = false;

    AutoLock lock(m_lock);
    if (!m_started) {
        ERROR("%s: sent job after stop()", m_name.c_str());
        return false;
    }
    enqueue(std::bind(&Thread::sentJob, this, std::ref(c), std::ref(flag)));
    //wait for result;
    while (!flag) {
        m_sent.wait();
    }
    return true;
}

void Thread::stop()
{
    {
        AutoLock lock(m_lock);
        if (!m_started)
            return;
        m_started = false;
        m_cond.signal();
    }
    pthread_join(m_thread, NULL);
    m_thread = INVALID_ID;
    assert(m_queue.empty());
}

bool Thread::isCurrent()
{
    return pthread_self() == m_thread;
}

Thread::~Thread()
{
    stop();
}
}
