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


#ifndef vaapidecsurfacepool_h
#define vaapidecsurfacepool_h

#include "common/condition.h"
#include "common/common_def.h"
#include "common/lock.h"
#include "vaapi/vaapiptrs.h"
#include "VideoCommonDefs.h"
#include "VideoDecoderDefs.h"
#include <deque>
#include <map>
#include <vector>
#include <set>
#include <va/va.h>

namespace YamiMediaCodec{

class VaapiDecSurfacePool : public EnableSharedFromThis <VaapiDecSurfacePool>
{
public:
    static DecSurfacePoolPtr create(VideoConfigBuffer* config,
        const SharedPtr<SurfaceAllocator>& allocator);
    void getSurfaceIDs(std::vector<VASurfaceID>& ids);
    /// get a free surface
    SurfacePtr acquire();
    /// push surface to output queue
    bool output(const SurfacePtr&, int64_t timetamp);
    /// get surface from output queue
    SharedPtr<VideoFrame> getOutput();

    //flush everything in ouptut queue
    void flush();

    ~VaapiDecSurfacePool();


private:
    VaapiDecSurfacePool();
    bool init(VideoConfigBuffer* config,
        const SharedPtr<SurfaceAllocator>& allocator);

    static YamiStatus getSurface(SurfaceAllocParams* param, intptr_t* surface);
    static YamiStatus putSurface(SurfaceAllocParams* param, intptr_t surface);
    YamiStatus getSurface(intptr_t* surface);
    YamiStatus putSurface(intptr_t surface);

    //following member only change in constructor.
    std::vector<SurfacePtr> m_surfaces;

    typedef std::map<intptr_t, VaapiSurface*> SurfaceMap;
    SurfaceMap m_surfaceMap;

    //free and allocted.
    std::deque<intptr_t> m_freed;
    std::set<intptr_t> m_used;

    /* output queue*/
    typedef std::deque<SharedPtr<VideoFrame> > OutputQueue;
    OutputQueue m_output;

    Lock m_lock;

    //for external allocator
    SharedPtr<SurfaceAllocator> m_allocator;
    SurfaceAllocParams m_allocParams;

    struct SurfaceRecycler;
    struct VideoFrameRecycler;

    DISALLOW_COPY_AND_ASSIGN(VaapiDecSurfacePool);
};

} //namespace YamiMediaCodec

#endif //vaapidecsurfacepool_h
