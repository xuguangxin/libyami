/*
 *  NativeDisplayHelper.h - help user share NativeDisplay between instance
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

#ifndef NATIVE_DISPLAY_HELPER_H_
#define NATIVE_DISPLAY_HELPER_H_

#include "VideoCommonDefs.h"

extern "C" {

typedef enum {
    YAMI_DISPLAY_AUTO,    // decided by yami
    YAMI_DISPLAY_X11,
    YAMI_DISPLAY_DRM,
    YAMI_DISPLAY_WAYLAND,
} YamiDisplayType;

typedef struct {
    YamiDisplayType type;
    intptr_t        handle;
    /* you can hold you data here */
    intptr_t        user;
    void (*free)(intptr_t user);
} YamiDisplay;

/* this function will follow following logical, take X11 for example:
 * 1. search cache for Display*, if it existed, create new NativeDisplay
 *    and set NativeDisplay.handle to related VaDisplay, increase ref count.
 * 2. if not found. we will call vaGetDisplay and vaInitialize on the Display*
 *    and cache it.
 * we think NULL is compatible with any Display*, -1 is compatible with any
 * Display* and drm fd
 */
NativeDisplay* createNativeDisplay(YamiDisplay*);

/* we hold a reference count for YamiDisplay.user, releaseNativeDisplay
 * will unref it, when the ref count equals 0, we will call YamiDisplay::free
 */
void releaseNativeDisplay(NativeDisplay*);

typedef NativeDisplay* (*createNativeDisplayFuncPtr)(const YamiDisplay*);
typedef void (*releaseNativeDisplayFuncPtr)(NativeDisplay*);

} /* extern "C"*/
#endif                          /* NATIVE_DISPLAY_HELPER_H_ */

