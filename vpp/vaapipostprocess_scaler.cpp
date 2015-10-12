/*
 *  vaapipostprocess_scaler.cpp - scaler and color space convert
 *
 *  Copyright (C) 2013-2014 Intel Corporation
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

#include "vaapipostprocess_scaler.h"
#include "vaapivpppicture.h"
#include "vaapipostprocess_factory.h"
#include "common/log.h"
#include <va/va_vpp.h>

namespace YamiMediaCodec{

static bool fillRect(VARectangle& vaRect, const VideoRect& rect)
{
    vaRect.x = rect.x;
    vaRect.y = rect.y;
    vaRect.width = rect.width;
    vaRect.height = rect.height;
    return rect.x || rect.y || rect.width || rect.height;
}

static void copyVideoFrameMeta(const SharedPtr<VideoFrame>& src, const SharedPtr<VideoFrame>& dest)
{
    dest->timeStamp = src->timeStamp;
    dest->flags = src->flags;
}

VaapiPostProcessScaler::VaapiPostProcessScaler()
{
    memset(&m_alpahBleding, 0, sizeof(m_alpahBleding));
    m_alpahBleding.size = sizeof(m_alpahBleding);
}

YamiStatus
VaapiPostProcessScaler::process(const SharedPtr<VideoFrame>& src,
                                const SharedPtr<VideoFrame>& dest)
{
    if (!m_context) {
        ERROR("NO context for scaler");
        return YAMI_FAIL;
    }
    if (!src || !dest) {
        return YAMI_INVALID_PARAM;
    }
    copyVideoFrameMeta(src, dest);
    SurfacePtr surface(new VaapiSurface(m_display,(VASurfaceID)dest->surface));
    VaapiVppPicture picture(m_context, surface);
    VAProcPipelineParameterBuffer* vppParam;
    if (!picture.editVppParam(vppParam))
        return YAMI_OUT_MEMORY;
    VARectangle srcCrop, destCrop;
    if (fillRect(srcCrop, src->crop))
        vppParam->surface_region = &srcCrop;
    vppParam->surface = (VASurfaceID)src->surface;
    vppParam->surface_color_standard = VAProcColorStandardNone;

    if (fillRect(destCrop, dest->crop))
        vppParam->output_region = &destCrop;
    vppParam->output_background_color = 0x00000000;
    vppParam->output_color_standard = VAProcColorStandardNone;


    VABlendState blend;
    uint32_t flag = m_alpahBleding.flag;
    if (flag != YAMI_BLEND_NONE) {
        memset(&blend, 0, sizeof(VABlendState));
        if (flag == YAMI_BLEND_GLOBAL_ALPHA) {
            blend.flags = VA_BLEND_GLOBAL_ALPHA;
            blend.global_alpha = m_alpahBleding.globalAlpha;
        } else if (flag == YAMI_BLEND_PREMULTIPLIED_ALPHA) {
            blend.flags = VA_BLEND_PREMULTIPLIED_ALPHA;
        } else {
            return YAMI_INVALID_PARAM;
        }

        vppParam->blend_state = &blend;
    }
    return picture.process() ? YAMI_SUCCESS : YAMI_FAIL;
}

YamiStatus
VaapiPostProcessScaler::setParameters(VppParamType type, const void* params)
{
    if (!params)
        return YAMI_INVALID_PARAM;
    if (type == VppParamsAlphaBlending) {
        const VppAlphaBlending* p = (const VppAlphaBlending*)params;
        if (p->size != sizeof(*p))
            return YAMI_INVALID_PARAM;
        memcpy(&m_alpahBleding, p, sizeof(*p));
        return YAMI_SUCCESS;
    }
    return VaapiPostProcessScaler::setParameters(type, params);
}

YamiStatus
VaapiPostProcessScaler::getParameters(VppParamType type, void* params)
{
    if (!params)
        return YAMI_INVALID_PARAM;
    if (type == VppParamsAlphaBlending) {
        VppAlphaBlending* p = (VppAlphaBlending*)params;
        if (p->size != sizeof(*p))
            return YAMI_INVALID_PARAM;
        memcpy(p, &m_alpahBleding, sizeof(*p));
        return YAMI_SUCCESS;
    }
    return VaapiPostProcessScaler::getParameters(type, params);
}

const bool VaapiPostProcessScaler::s_registered =
    VaapiPostProcessFactory::register_<VaapiPostProcessScaler>(YAMI_VPP_SCALER);

}
