/*
 *  vaapidecoder_h265.h
 *
 *  Copyright (C) 2014 Intel Corporation
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

#ifndef vaapidecoder_h265_h
#define vaapidecoder_h265_h

#include "vaapidecoder_base.h"
#include "vaapidecpicture.h"

extern "C" {
typedef struct _H265Parser                   H265Parser;
typedef struct _H265NalUnit                  H265NalUnit;
typedef struct _H265VPS                      H265VPS;
typedef struct _H265SPS                      H265SPS;
typedef struct _H265PPS                      H265PPS;
typedef struct _VASliceParameterBufferHEVC VASliceParameterBufferHEVC;
}


namespace YamiMediaCodec{
enum {
    H265_EXTRA_SURFACE_NUMBER = 5,
};

class VaapiDecPictureH265;
class VaapiDecoderH265:public VaapiDecoderBase {
public:
    typedef SharedPtr<VaapiDecPictureH265> PicturePtr;
    VaapiDecoderH265();
    virtual ~VaapiDecoderH265();
    virtual Decode_Status start(VideoConfigBuffer* );
    virtual Decode_Status decode(VideoDecodeBuffer*);

private:
    Decode_Status decodeNalu(H265NalUnit* nalu);
    Decode_Status decodeParamSet(H265NalUnit* nalu);
    Decode_Status decodeSlice(H265NalUnit* nalu);

    Decode_Status ensureContext(const H265SPS*);
    bool fillPicture(const PicturePtr& , const H265SliceHdr*);
    bool fillSlice(const PicturePtr&, const H265SliceHdr*, const H265NalUnit*);
    bool fillIqMatrix(const PicturePtr&, const H265SliceHdr*);
    bool fillPredWeightTable(VASliceParameterBufferHEVC*, const H265SliceHdr*);
    bool fillReference(const PicturePtr& picture,
            VASliceParameterBufferHEVC*, const H265SliceHdr*);

    PicturePtr createPicture(const H265SliceHdr* const, const H265NalUnit* const nalu);
    void getPoc(const PicturePtr&,const H265SliceHdr* const,
            const H265NalUnit* const);
    Decode_Status decodeCurrent();
    Decode_Status outputPicture(const PicturePtr& picture);

    H265Parser* m_parser;
    PicturePtr  m_current;
    uint16_t    m_prevPicOrderCntMsb;
    int32_t     m_prevPicOrderCntLsb;
    bool        m_newStream;
    static const bool s_registered; // VaapiDecoderFactory registration result
};

};

#endif

