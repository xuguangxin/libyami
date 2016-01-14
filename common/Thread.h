/*
 *  thread.h - executor pattern, you can post and send message to thread.
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
#ifndef Thread_h
#define Thread_h

#include "condition.h"
#include "lock.h"
#include "Functional.h"

#include <string>
#include <deque>
#include <pthread.h>

namespace YamiMediaCodec {

typedef std::function<void(void)> Runnable;

class Thread {
public:
    explicit Thread(const char* name = "");
    ~Thread();
    bool start();
    //stop thread, this will waiting all post/sent job done
    void stop();
    //post job to this thread
    void post(const Runnable&);
    //send job and wait it done
    bool send(const Runnable&);
    bool isCurrent();

private:
    //thread loop
    static void* init(void*);
    void loop();
    void enqueue(const Runnable& r);
    void sentJob(const Runnable& r, bool& flag);

    std::string m_name;
    bool m_started;
    pthread_t m_thread;

    Lock m_lock;
    Condition m_cond;
    Condition m_sent;
    std::deque<Runnable> m_queue;

    static const pthread_t INVALID_ID = (pthread_t)-1;

    DISALLOW_COPY_AND_ASSIGN(Thread);
};
};

#endif
