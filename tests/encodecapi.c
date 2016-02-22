/*
 *  h264_encode.cpp - h264 encode test
 *
 *  Copyright (C) 2011-2014 Intel Corporation
 *    Author: Cong Zhong<congx.zhong@intel.com>
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "common/log.h"
#include "capi/VideoEncoderCapi.h"
#include "encodeInputCapi.h"
#include "VideoEncoderDefs.h"
#include "encodehelp.h"

int main(int argc, char** argv)
{
    EncodeHandler encoder = NULL;
    uint32_t maxOutSize = 0;
    EncodeInputHandler input;
    EncodeOutputHandler output;
    Encode_Status status;
    VideoFrameRawData inputBuffer;
    VideoEncOutputBuffer outputBuffer;
    int encodeFrameCount = 0;

    if (!process_cmdline(argc, argv))
        return -1;

    DEBUG("inputFourcc: %.4s", (char*)(&inputFourcc));
    input = createEncodeInput(inputFileName, inputFourcc, videoWidth, videoHeight);

    if (!input) {
        fprintf (stderr, "fail to init input stream\n");
        return -1;
    }

    videoWidth = getInputWidth(input);
    videoHeight = getInputHeight(input);

    output = createEncodeOutput(outputFileName, videoWidth, videoHeight);
    if(!output) {
      fprintf(stderr, "fail to init ourput stream\n");
      return -1;
    }

    encoder = createEncoder(getOutputMimeType(output));
    assert(encoder != NULL);

    NativeDisplay nativeDisplay;
    nativeDisplay.type = NATIVE_DISPLAY_DRM;
    nativeDisplay.handle = 0;
    encodeSetNativeDisplay(encoder, &nativeDisplay);

    //configure encoding parameters
    VideoParamsCommon encVideoParams;
    encVideoParams.size = sizeof(VideoParamsCommon);
    encodeGetParameters(encoder, VideoParamsTypeCommon, &encVideoParams);
    setEncoderParameters(&encVideoParams);
    encVideoParams.size = sizeof(VideoParamsCommon);
    encodeSetParameters(encoder, VideoParamsTypeCommon, &encVideoParams);

    VideoConfigAVCStreamFormat streamFormat;
    streamFormat.size = sizeof(VideoConfigAVCStreamFormat);
    streamFormat.streamFormat = AVC_STREAM_FORMAT_ANNEXB;
    encodeSetParameters(encoder, VideoConfigTypeAVCStreamFormat, &streamFormat);

    status = encodeStart(encoder);
    assert(status == ENCODE_SUCCESS);

    //init output buffer
    encodeGetMaxOutSize(encoder, &maxOutSize);

    if (!createOutputBuffer(&outputBuffer, maxOutSize)) {
        fprintf (stderr, "fail to init input stream\n");
        return -1;
    }

    while (!encodeInputIsEOS(input))
    {
        memset(&inputBuffer, 0, sizeof(inputBuffer));
        if (getOneFrameInput(input, &inputBuffer)){
            status = encodeEncodeRawData(encoder, &inputBuffer);
            recycleOneFrameInput(input, &inputBuffer);
        }
        else
            break;

        //get the output buffer
        do {
            status = encodeGetOutput(encoder, &outputBuffer, false);
            if (status == ENCODE_SUCCESS
              && !writeOutput(output, outputBuffer.data, outputBuffer.dataSize))
                assert(0);
        } while (status != ENCODE_BUFFER_NO_MORE);

        if (frameCount &&  encodeFrameCount++ > frameCount)
            break;
    }

    // drain the output buffer
    do {
       status = encodeGetOutput(encoder, &outputBuffer, true);
       if (status == ENCODE_SUCCESS
           && !writeOutput(output, outputBuffer.data, outputBuffer.dataSize))
           assert(0);
    } while (status != ENCODE_BUFFER_NO_MORE);

    encodeStop(encoder);
    releaseEncoder(encoder);
    releaseEncodeInput(input);
    releaseEncodeOutput(output);
    free(outputBuffer.data);

    fprintf(stderr, "encode done\n");
    return 0;
}
