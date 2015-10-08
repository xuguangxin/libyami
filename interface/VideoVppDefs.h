/*
 *  VideoVppDefs.h- basic defines for video post process
 *
 *  Copyright (C) 2015 Intel Corporation
 *    Author: XuGuangxin<Guangxin.Xu@intel.com>
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

#ifndef video_vpp_defs_h
#define video_vpp_defs_h
#include "VideoCommonDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VppParamsStartUnused = 0x01000000,
    VppParamsAlphaBlending,
}VppParamType;

/* use this to disable alphablending */
#define YAMI_BLEND_NONE                   0x0
/* alpha blending flags see va_vpp.h for details */
#define YAMI_BLEND_GLOBAL_ALPHA           0x0002
/* per pixel alpha, need (RGBA) */
#define YAMI_BLEND_PREMULTIPLIED_ALPHA    0x0008

typedef struct VppAlphaBlending {
    uint32_t size;
    uint32_t flag; /* see above*/
    float    globalAlpha;
} VppAlphaBlending;

#ifdef __cplusplus
}
#endif
#endif  //video_vpp_defs_h