/*
 *  simpleplayer.cpp - simpleplayer to demo API usage, no tricks
 *
 *  Copyright (C) 2013-2014 Intel Corporation
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

#include "decodeinput.h"
#include "common/log.h"
#include "common/utils.h"
#include "VideoDecoderHost.h"
#include "NativeDisplayHelper.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <va/va_x11.h>

using namespace YamiMediaCodec;

class SimplePlayer
{
public:
    bool init(int argc, char** argv)
    {
        if (argc != 2) {
            printf("usage: simpleplayer xxx.264\n");
            return false;
        }
        m_input.reset(DecodeInput::create(argv[1]));
        if (!m_input) {
            fprintf(stderr, "failed to open %s", argv[1]);
            return false;
        }

        //init decoder
        m_decoder.reset(createVideoDecoder(m_input->getMimeType()), releaseVideoDecoder);
        if (!m_decoder) {
            fprintf(stderr, "failed create decoder for %s", m_input->getMimeType());
            return false;
        }

        if (!initDisplay()) {
            return false;
        }
        //set native display
        m_decoder->setNativeDisplay(m_nativeDisplay.get());
        return true;
    }
    bool run()
    {
        VideoConfigBuffer configBuffer;
        memset(&configBuffer, 0, sizeof(configBuffer));
        configBuffer.profile = VAProfileNone;
        const string codecData = m_input->getCodecData();
        if (codecData.size()) {
            configBuffer.data = (uint8_t*)codecData.data();
            configBuffer.size = codecData.size();
        }

        Decode_Status status = m_decoder->start(&configBuffer);
        assert(status == DECODE_SUCCESS);

        VideoDecodeBuffer inputBuffer;
        while (m_input->getNextDecodeUnit(inputBuffer)) {
            status = m_decoder->decode(&inputBuffer);
            if (DECODE_FORMAT_CHANGE == status) {
                //drain old buffers
                renderOutputs();
                const VideoFormatInfo *formatInfo = m_decoder->getFormatInfo();
                resizeWindow(formatInfo->width, formatInfo->height);
                //resend the buffer
                status = m_decoder->decode(&inputBuffer);
            }
            if(status == DECODE_SUCCESS) {
                renderOutputs();
            } else {
                ERROR("decode error status = %d", status);
                break;
            }
        }
        m_decoder->stop();
        return true;
    }
    SimplePlayer():m_window(0), m_width(0), m_height(0) {}
    ~SimplePlayer()
    {
        m_nativeDisplay.reset();
        if (m_window) {
            XDestroyWindow(m_display.get(), m_window);
        }
    }
private:
    void renderOutputs()
    {
        VAStatus status = VA_STATUS_SUCCESS;
        do {
            SharedPtr<VideoFrame> frame = m_decoder->getOutput();
            if (!frame)
                break;
            status = vaPutSurface(m_vaDisplay, (VASurfaceID)frame->surface,
                m_window, 0, 0, m_width, m_height, 0, 0, m_width, m_height,
                NULL, 0, 0);
            if (status != VA_STATUS_SUCCESS) {
                ERROR("vaPutSurface return %d", status);
                break;
            }
        } while (1);
    }
    bool initDisplay()
    {
        Display* display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr, "Failed to XOpenDisplay \n");
            return false;
        }
        m_display.reset(display, XCloseDisplay);

        YamiDisplay yamiDisplay;
        memset(&yamiDisplay, 0, sizeof(yamiDisplay));
        yamiDisplay.type = YAMI_DISPLAY_X11;
        yamiDisplay.handle = (intptr_t)display;
        m_nativeDisplay.reset(createNativeDisplay(&yamiDisplay), releaseNativeDisplay);
        if (!m_nativeDisplay) {
            fprintf(stderr, "Failed to createNativeDisplay \n");
            return false;
        }
        m_vaDisplay = (VADisplay)m_nativeDisplay->handle;
        return true;
    }
    void resizeWindow(int width, int height)
    {
        Display* display = m_display.get();
        if (m_window) {
        //todo, resize window;
        } else {
            DefaultScreen(display);

            XSetWindowAttributes x11WindowAttrib;
            x11WindowAttrib.event_mask = KeyPressMask;
            m_window = XCreateWindow(display, DefaultRootWindow(display),
                0, 0, width, height, 0, CopyFromParent, InputOutput,
                CopyFromParent, CWEventMask, &x11WindowAttrib);
            XMapWindow(display, m_window);
        }
        XSync(display, false);
        {
            DEBUG("m_window=%lu", m_window);
            XWindowAttributes wattr;
            XGetWindowAttributes(display, m_window, &wattr);
        }
        m_width = width;
        m_height = height;
    }
    SharedPtr<Display> m_display;
    SharedPtr<NativeDisplay> m_nativeDisplay;
    VADisplay m_vaDisplay;
    Window   m_window;
    SharedPtr<IVideoDecoder> m_decoder;
    SharedPtr<DecodeInput> m_input;
    int m_width, m_height;
};

int main(int argc, char** argv)
{

    SimplePlayer player;
    if (!player.init(argc, argv)) {
        ERROR("init player failed with %s", argv[1]);
        return -1;
    }
    if (!player.run()){
        ERROR("run simple player failed");
        return -1;
    }
    printf("play file done\n");
    return  0;

}

