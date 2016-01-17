/*
 *  surfacepoolfifo.cpp - default surface pool, if external pool not set.
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

#include "surfacepoolfifo.h"

#include "common/log.h"
#include "common/lock.h"
#include "interface/VideoCommonDefs.h"

#include <deque>
#include <set>
#include <stdint.h>

namespace YamiMediaCodec{

class SurfacePoolFifo : public SurfacePool
{
public:
    SurfacePoolFifo(intptr_t* surfaces, uint32_t size);

    YamiStatus doAlloc(intptr_t* surface)
    {
        AutoLock lock(m_lock);
        if (m_free.empty())
            return YAMI_OUT_MEMORY;
        *surface = m_free.front();
        m_free.pop_front();
        m_allocated.insert(*surface);
        return YAMI_SUCCESS;
    }
    YamiStatus doRecycle(intptr_t surface)
    {
        AutoLock lock(m_lock);
        std::set<intptr_t>::iterator it;
        it = m_allocated.find(surface);
        if (it == m_allocated.end())
            return YAMI_INVALID_PARAM;
        m_free.push_back(surface);
        m_allocated.erase(surface);
        return YAMI_SUCCESS;
    }
    ~SurfacePoolFifo()
    {
        if (!m_allocated.empty()) {
            ERROR("bug: have %d surface leaked", (int)m_allocated.size());
        }
    }
private:
    Lock m_lock;
    std::deque<intptr_t> m_free;
    std::set<intptr_t>   m_allocated;
};

static YamiStatus allocSurface(SurfacePool* thiz, intptr_t* surface)
{
    if (!thiz || !surface)
        return YAMI_INVALID_PARAM;
    SurfacePoolFifo* fifo = (SurfacePoolFifo*)thiz;
    return fifo->doAlloc(surface);
}

static YamiStatus recycleSurface(SurfacePool* thiz, intptr_t surface)
{
    if (!thiz)
        return YAMI_INVALID_PARAM;
    SurfacePoolFifo* fifo = (SurfacePoolFifo*)thiz;
    return fifo->doRecycle(surface);
}

SurfacePoolFifo::SurfacePoolFifo(intptr_t* surfaces, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        m_free.push_back(surfaces[i]);
    }
    this->alloc = allocSurface;
    this->recycle = recycleSurface;
}

SurfacePool* createSurfacePoolFifo(intptr_t* surfaces, uint32_t size)
{
    return new SurfacePoolFifo(surfaces, size);

}
void releaseSurfacePoolFifo(SurfacePool* pool)
{
    if (pool) {
        SurfacePoolFifo* fifo = (SurfacePoolFifo*)pool;
        delete fifo;
    }
}

} //YamiMediaCodec

