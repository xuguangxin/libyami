/*
 * Copyright (C) 2014-2016 Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vaapiencoder_vp8.h"
#include "common/scopedlogger.h"
#include "common/common_def.h"
#include "vaapi/vaapicontext.h"
#include "vaapi/vaapidisplay.h"
#include "vaapicodedbuffer.h"
#include "vaapiencpicture.h"
#include <algorithm>
#include <vector>

namespace YamiMediaCodec{

//golden, alter, last
#define MAX_REFERECNE_FRAME 3
#define VP8_DEFAULT_QP     40
#define VP8_MAX_TEMPORAL_LAYER_NUM 3

class VaapiEncPictureVP8 : public VaapiEncPicture
{
public:
    VaapiEncPictureVP8(const ContextPtr& context, const SurfacePtr& surface,
                       int64_t timeStamp)
        : VaapiEncPicture(context, surface, timeStamp)
    {
        return;
    }

    VAGenericID getCodedBufferID()
    {
        return m_codedBuffer->getID();
    }
};

struct RefFlags
{
    /* exactly same things in VAEncPictureParameterBufferVP8 */
    uint32_t refresh_golden_frame           : 1;
    uint32_t refresh_alternate_frame        : 1;
    uint32_t refresh_last                   : 1;
    uint32_t copy_buffer_to_golden          : 2;
    uint32_t copy_buffer_to_alternate       : 2;
    uint32_t no_ref_last                    : 1;
    uint32_t no_ref_gf                      : 1;
    uint32_t no_ref_arf                     : 1;

    RefFlags()
    {
        memset(this, 0, sizeof(RefFlags));

    }
};

class Vp8Encoder {
public:
    virtual void getRefFlags(RefFlags&, uint8_t temporalLayer) const = 0;
    virtual void getLayerIds(std::vector<uint32_t>& ids) const = 0;
    virtual bool getErrorResilient() const = 0;
    virtual bool getRefreshEntropyProbs() const = 0;
    virtual uint8_t getTemporalLayer(uint32_t frameNum) const = 0;
};

class Vp8EncoderNormal : public Vp8Encoder {
public:
    void getRefFlags(RefFlags&, uint8_t temporalLayer) const;
    void getLayerIds(std::vector<uint32_t>& ids) const { ASSERT(0 && "not suppose call this"); }
    bool getErrorResilient() const { return false; }
    bool getRefreshEntropyProbs() const { return false; }
    uint8_t getTemporalLayer(uint32_t frameNum) const { return 0; }
};

void Vp8EncoderNormal::getRefFlags(RefFlags& refFlags, uint8_t temporalLayer) const
{
    refFlags.refresh_last = 1;
    refFlags.refresh_golden_frame = 0;
    refFlags.copy_buffer_to_golden = 1;
    refFlags.refresh_alternate_frame = 0;
    refFlags.copy_buffer_to_alternate = 2;

}
class Vp8EncoderSvct : public Vp8Encoder {
public:
    Vp8EncoderSvct(const VideoParamsCommon& common);

    void getRefFlags(RefFlags&, uint8_t temporalLayer) const;
    void getLayerIds(std::vector<uint32_t>& ids) const;

    bool getErrorResilient() const { return true; }
    bool getRefreshEntropyProbs() const { return false; }
    uint8_t getTemporalLayer(uint32_t frameNum) const { return m_tempLayerIDs[frameNum % m_periodicity]; }

private:
    void printRatio();
    void printLayerIDs();

    bool calculateFramerateRatio();
    bool calculatePeriodicity();
    bool calculateLayerIDs();
    uint32_t calculateGCD(uint32_t* gcdArray, uint32_t num);

    VideoFrameRate m_framerates[VP8_MAX_TEMPORAL_LAYER_NUM];
    uint32_t m_framerateRatio[VP8_MAX_TEMPORAL_LAYER_NUM];
    uint32_t m_layerBitRate[VP8_MAX_TEMPORAL_LAYER_NUM];
    uint32_t m_periodicity;
    std::vector<uint32_t> m_tempLayerIDs;
    uint8_t m_layerNum;
};



Vp8EncoderSvct::Vp8EncoderSvct(const VideoParamsCommon& common)
{
    uint32_t gcd;
    uint32_t i;
    uint32_t layers = common.temporalLayers.numLayers;
    assert(layers > 0 && layers < VP8_MAX_TEMPORAL_LAYER_NUM);
    m_layerNum = layers + 1;
    memset(m_layerBitRate, 0, sizeof(m_layerBitRate));
    memset(m_framerateRatio, 0, sizeof(m_framerateRatio));
    m_layerBitRate[0] = common.rcParams.bitRate;
    m_framerates[0] = common.frameRate;
    for (i = 1; i < m_layerNum; i++) {
        m_framerates[i] = common.temporalLayers.frameRate[i - 1];
        m_layerBitRate[i] = common.temporalLayers.bitRate[i - 1];
    }
    for (i = 0; i < m_layerNum; i++) {
        gcd = std::__gcd(m_framerates[i].frameRateNum, m_framerates[i].frameRateDenom);
        m_framerates[i].frameRateNum /= gcd;
        m_framerates[i].frameRateDenom /= gcd;
    }

    calculatePeriodicity();
    calculateLayerIDs();
    printRatio();
    printLayerIDs();
}

void Vp8EncoderSvct::getLayerIds(std::vector<uint32_t>& ids) const
{
    ids = m_tempLayerIDs;
}

void Vp8EncoderSvct::getRefFlags(RefFlags& refFlags, uint8_t temporalLayer) const
{
    switch (temporalLayer) {
    case 2:
        refFlags.refresh_alternate_frame = 1;
        break;
    case 1:
        refFlags.refresh_golden_frame = 1;
        refFlags.no_ref_arf = 1;
        break;
    case 0:
        refFlags.refresh_last = 1;
        refFlags.no_ref_gf = 1;
        refFlags.no_ref_arf = 1;
        break;
    default:
        ERROR("temporal layer %d is out of the range[0, 2].", temporalLayer);
        break;
    }
}

bool Vp8EncoderSvct::calculateFramerateRatio()
{
    int32_t numerator;
    //Reduct fractions to a common denominator, then get the numerators
    //into m_framerateRatio.
    for (uint8_t i = 0; i < m_layerNum; i++) {
        numerator = 1;
        for (uint8_t j = 0; j < m_layerNum; j++)
            if (j != i)
                numerator *= m_framerates[j].frameRateDenom;
            else
                numerator *= m_framerates[j].frameRateNum;
        m_framerateRatio[i] = numerator;
    }

    //Divide m_framerateRatio by Greatest Common Divisor.
    uint32_t gcd = calculateGCD(m_framerateRatio, m_layerNum);
    if ((gcd != 0) && (gcd != 1))
        for (uint8_t i = 0; i < m_layerNum; i++)
            m_framerateRatio[i] /= gcd;

    return true;
}

bool Vp8EncoderSvct::calculatePeriodicity()
{
    if (!m_framerateRatio[0])
        calculateFramerateRatio();

    m_periodicity = m_framerateRatio[m_layerNum - 1];

    return true;
}
bool Vp8EncoderSvct::calculateLayerIDs()
{
    uint32_t layer = 0;
    uint32_t m_frameNum[VP8_MAX_TEMPORAL_LAYER_NUM];
    uint32_t m_frameNumAssigned[VP8_MAX_TEMPORAL_LAYER_NUM];

    memset(m_frameNumAssigned, 0, sizeof(m_frameNumAssigned));
    m_frameNum[0] = m_framerateRatio[0];
    for (uint32_t i = 1; i < m_layerNum; i++)
        m_frameNum[i] = m_framerateRatio[i] - m_framerateRatio[i - 1];

    for (uint32_t i = 0; i < m_periodicity; i++)
        for (layer = 0; layer < m_layerNum; layer++)
            if (!(i % (m_periodicity / m_framerateRatio[layer])))
                if (m_frameNumAssigned[layer] < m_frameNum[layer]) {
                    m_tempLayerIDs.push_back(layer);
                    m_frameNumAssigned[layer]++;
                    break;
                }

    return true;
}

//GCD: Greatest Common Divisor;
//return:
//     0: failure;
//  else: successful;
uint32_t Vp8EncoderSvct::calculateGCD(uint32_t* gcdArray, uint32_t num)
{
    if (!gcdArray || !num)
        return 0;
    std::vector<uint32_t> gcdVector;
    uint32_t gcdNotTheSame = 0;
    uint32_t min = 0;

    //copy gcdArray, and find out the min of gcdArray.
    gcdVector.push_back(gcdArray[0]);
    min = gcdVector[0];
    for (uint8_t i = 1; i < num; i++) {
        gcdVector.push_back(gcdArray[i]);
        if (min > gcdVector[i])
            min = gcdVector[i];
    }
    //if gcdArray contains 0, would fail;
    if (!min)
        return min;

    //if the GCDs(greatest common divisor) of min and each element in gcdVector
    //are the same, we get the GCD.
    while (min != 1) {
        gcdNotTheSame = 0;
        for (uint8_t i = 0; i < num; i++) {
            //calculate the GCD of min and gcdVector[i], and refresh gcdTemp[i]
            //with the new GCD.
            gcdVector[i] = std::__gcd(min, gcdVector[i]);
            if (i > 1)
                if (gcdVector[i] != gcdVector[i - 1])
                    gcdNotTheSame = 1;
            if (min > gcdVector[i])
                min = gcdVector[i];
        }
        if (!gcdNotTheSame)
            break;
    }

    return min;
}

void Vp8EncoderSvct::printRatio()
{
    DEBUG("ratio: \n");
    for (uint8_t i = 0; i < m_layerNum; i++) {
        if (i != 0)
            DEBUG(" : ");
        DEBUG("%d", m_framerateRatio[i]);
    }
    DEBUG("\n");

    return;
}

void Vp8EncoderSvct::printLayerIDs()
{
    DEBUG("LayerIDs: \n");
    DEBUG(" frame number: ");
    for (uint8_t i = 0; i < m_periodicity; i++) {
        DEBUG("%2d ", i);
    }
    DEBUG("\n");
    DEBUG("frame layerid: ");
    for (uint8_t i = 0; i < m_periodicity; i++) {
        DEBUG("%2d ", m_tempLayerIDs[i]);
    }
    DEBUG("\n");

    return;
}
VaapiEncoderVP8::VaapiEncoderVP8():
	m_frameCount(0),
	m_qIndex(VP8_DEFAULT_QP)
{
    m_videoParamCommon.profile = VAProfileVP8Version0_3;
    m_videoParamCommon.rcParams.minQP = 9;
    m_videoParamCommon.rcParams.maxQP = 127;
    m_videoParamCommon.rcParams.initQP = VP8_DEFAULT_QP;
}

VaapiEncoderVP8::~VaapiEncoderVP8()
{
}

YamiStatus VaapiEncoderVP8::getMaxOutSize(uint32_t* maxSize)
{
    FUNC_ENTER();
    *maxSize = m_maxCodedbufSize;
    return YAMI_SUCCESS;
}

//if the context is very complex and the quantization value is very small,
//the coded slice data will be very close to the limitation value width() * height() * 3 / 2.
//And the coded bitstream (slice_data + frame headers) will more than width() * height() * 3 / 2.
//so we add VP8_HEADER_MAX_SIZE to m_maxCodedbufSize to make sure it's not overflow.
#define VP8_HEADER_MAX_SIZE 0x4000

void VaapiEncoderVP8::resetParams()
{
    m_maxCodedbufSize = width() * height() * 3 / 2 + VP8_HEADER_MAX_SIZE;
    if (ipPeriod() == 0)
        m_videoParamCommon.intraPeriod = 1;
    if (m_videoParamCommon.temporalLayers.numLayers > 0) {
        m_encoder.reset(new Vp8EncoderSvct(m_videoParamCommon));
    } else {
        m_encoder.reset(new Vp8EncoderNormal());
    }
}

YamiStatus VaapiEncoderVP8::start()
{
    FUNC_ENTER();
    resetParams();
    return VaapiEncoderBase::start();
}

void VaapiEncoderVP8::flush()
{
    FUNC_ENTER();
    m_frameCount = 0;
    m_last.reset();
    m_golden.reset();
    m_alt.reset();
    VaapiEncoderBase::flush();
}

YamiStatus VaapiEncoderVP8::stop()
{
    flush();
    return VaapiEncoderBase::stop();
}

YamiStatus VaapiEncoderVP8::setParameters(VideoParamConfigType type, Yami_PTR videoEncParams)
{
    YamiStatus status = YAMI_SUCCESS;
    FUNC_ENTER();
    if (!videoEncParams)
        return YAMI_INVALID_PARAM;

    switch (type) {
    default:
        status = VaapiEncoderBase::setParameters(type, videoEncParams);
        break;
    }
    return status;
}

YamiStatus VaapiEncoderVP8::getParameters(VideoParamConfigType type, Yami_PTR videoEncParams)
{
    FUNC_ENTER();
    if (!videoEncParams)
        return YAMI_INVALID_PARAM;

    // TODO, update video resolution basing on hw requirement
    return VaapiEncoderBase::getParameters(type, videoEncParams);
}

YamiStatus VaapiEncoderVP8::doEncode(const SurfacePtr& surface, uint64_t timeStamp, bool forceKeyFrame)
{
    YamiStatus ret;
    if (!surface)
        return YAMI_INVALID_PARAM;

    PicturePtr picture(new VaapiEncPictureVP8(m_context, surface, timeStamp));

    if (!(m_frameCount % keyFramePeriod()) || forceKeyFrame)
        picture->m_type = VAAPI_PICTURE_I;
    else
        picture->m_type = VAAPI_PICTURE_P;

    picture->m_temporalID = m_encoder->getTemporalLayer(m_frameCount % keyFramePeriod());
    m_frameCount++;

    m_qIndex = (initQP() > minQP() && initQP() < maxQP()) ? initQP() : VP8_DEFAULT_QP;

    CodedBufferPtr codedBuffer = VaapiCodedBuffer::create(m_context, m_maxCodedbufSize);
    if (!codedBuffer)
        return YAMI_OUT_MEMORY;
    picture->m_codedBuffer = codedBuffer;
    codedBuffer->setFlag(ENCODE_BUFFERFLAG_ENDOFFRAME);
    INFO("picture->m_type: 0x%x\n", picture->m_type);
    if (picture->m_type == VAAPI_PICTURE_I) {
        codedBuffer->setFlag(ENCODE_BUFFERFLAG_SYNCFRAME);
    }
    ret = encodePicture(picture);
    if (ret != YAMI_SUCCESS) {
        return ret;
    }
    output(picture);
    return YAMI_SUCCESS;
}


bool VaapiEncoderVP8::fill(VAEncSequenceParameterBufferVP8* seqParam) const
{
    seqParam->frame_width = width();
    seqParam->frame_height = height();
    seqParam->bits_per_second = bitRate();
    seqParam->intra_period = intraPeriod();
    seqParam->error_resilient = m_encoder->getErrorResilient();
    return true;
}

void VaapiEncoderVP8::fill(VAEncPictureParameterBufferVP8* picParam, const RefFlags& refFlags) const
{
    picParam->pic_flags.bits.refresh_golden_frame = refFlags.refresh_golden_frame;
    picParam->pic_flags.bits.refresh_alternate_frame = refFlags.refresh_alternate_frame;
    picParam->pic_flags.bits.refresh_last = refFlags.refresh_last;
    picParam->pic_flags.bits.copy_buffer_to_golden = refFlags.copy_buffer_to_golden;
    picParam->pic_flags.bits.copy_buffer_to_alternate = refFlags.copy_buffer_to_alternate;
    picParam->ref_flags.bits.no_ref_last = refFlags.no_ref_last;
    picParam->ref_flags.bits.no_ref_gf = refFlags.no_ref_gf;
    picParam->ref_flags.bits.no_ref_arf = refFlags.no_ref_arf;
}
/* Fills in VA picture parameter buffer */
bool VaapiEncoderVP8::fill(VAEncPictureParameterBufferVP8* picParam, const PicturePtr& picture,
                            const SurfacePtr& surface, RefFlags& refFlags) const
 {
     picParam->reconstructed_frame = surface->getID();
     if (picture->m_type == VAAPI_PICTURE_P) {
         picParam->pic_flags.bits.frame_type = 1;
        picParam->ref_arf_frame = m_alt->getID();
        picParam->ref_gf_frame = m_golden->getID();
        picParam->ref_last_frame = m_last->getID();

        m_encoder->getRefFlags(refFlags, picture->m_temporalID);
        fill(picParam, refFlags);
    } else {
        picParam->ref_last_frame = VA_INVALID_SURFACE;
        picParam->ref_gf_frame = VA_INVALID_SURFACE;
        picParam->ref_arf_frame = VA_INVALID_SURFACE;
    }

    picParam->coded_buf = picture->getCodedBufferID();
    picParam->ref_flags.bits.temporal_id = picture->m_temporalID;

    picParam->pic_flags.bits.show_frame = 1;
    /*TODO: multi partition*/
    picParam->pic_flags.bits.num_token_partitions = 0;
    //REMOVE THIS
    picParam->pic_flags.bits.refresh_entropy_probs = 0;
    /*pic_flags end */
    for (int i = 0; i < 4; i++) {
        picParam->loop_filter_level[i] = 19;
    }

    picParam->clamp_qindex_low = minQP();
    picParam->clamp_qindex_high = maxQP();
    return TRUE;
}

bool VaapiEncoderVP8::fill(VAQMatrixBufferVP8* qMatrix) const
{
    size_t i;

    for (i = 0; i < N_ELEMENTS(qMatrix->quantization_index); i++) {
        qMatrix->quantization_index[i] = m_qIndex;
    }

    for (i = 0; i < N_ELEMENTS(qMatrix->quantization_index_delta); i++) {
        qMatrix->quantization_index_delta[i] = 0;
    }

    return true;
}


bool VaapiEncoderVP8::ensureSequence(const PicturePtr& picture)
{
    if (picture->m_type != VAAPI_PICTURE_I)
        return true;

    VAEncSequenceParameterBufferVP8* seqParam;
    if (!picture->editSequence(seqParam) || !fill(seqParam)) {
        ERROR("failed to create sequence parameter buffer (SPS)");
        return false;
    }
    return true;
}

bool VaapiEncoderVP8::ensurePicture (const PicturePtr& picture,
                                     const SurfacePtr& surface,
                                     RefFlags& refFlags)
{
    VAEncPictureParameterBufferVP8 *picParam;

    if (!picture->editPicture(picParam) || !fill(picParam, picture, surface, refFlags)) {
        ERROR("failed to create picture parameter buffer (PPS)");
        return false;
    }
    return true;
}

bool VaapiEncoderVP8::ensureQMatrix (const PicturePtr& picture)
{
    VAQMatrixBufferVP8 *qMatrix;

    if (!picture->editQMatrix(qMatrix) || !fill(qMatrix)) {
        ERROR("failed to create qMatrix");
        return false;
    }
    return true;
}

/* Section 9.7 */
const SurfacePtr& VaapiEncoderVP8::referenceUpdate(
    const SurfacePtr& to, const SurfacePtr& from,
    const SurfacePtr& recon, bool refresh, uint32_t copy) const
{
    if (refresh)
        return recon;
    switch (copy) {
        case 0:
            return to;
        case 1:
            return m_last;
        case 2:
            return from;
        default:
            ASSERT(0 && "invalid copy to flags");
            return to;
    }

}

bool VaapiEncoderVP8::referenceListUpdate (const PicturePtr& pic, const SurfacePtr& recon, const RefFlags& refFlags)
{
    if (VAAPI_PICTURE_I == pic->m_type) {
        m_last = recon;
        m_golden = recon;
        m_alt = recon;
    }
    else {
        /* 9.7 and 9.8 */
        m_golden = referenceUpdate(m_golden, m_alt, recon,
            refFlags.refresh_golden_frame, refFlags.copy_buffer_to_golden);
        m_alt = referenceUpdate(m_alt, m_golden, recon,
            refFlags.refresh_alternate_frame, refFlags.copy_buffer_to_alternate);
        if (refFlags.refresh_last)
            m_last = recon;
    }
    return true;
}
/* Generates additional control parameters */
bool VaapiEncoderVP8::ensureMiscParams(VaapiEncPicture* picture)
{
    if (!VaapiEncoderBase::ensureMiscParams(picture))
        return false;

    VideoRateControl mode = rateControlMode();
    if (mode == RATE_CONTROL_CBR || mode == RATE_CONTROL_VBR) {
        if (m_videoParamCommon.temporalLayers.numLayers > 0) {
            VAEncMiscParameterTemporalLayerStructure* layerParam = NULL;
            if (!picture->newMisc(VAEncMiscParameterTypeTemporalLayerStructure,
                    layerParam))
                return false;

            std::vector<uint32_t> ids;
            m_encoder->getLayerIds(ids);
            if (layerParam) {
                layerParam->number_of_layers = m_videoParamCommon.temporalLayers.numLayers + 1;
                layerParam->periodicity = ids.size();
                std::copy(ids.begin(), ids.end(), layerParam->layer_id);
            }

        }
    }
    return true;
}

YamiStatus VaapiEncoderVP8::encodePicture(const PicturePtr& picture)
{
    YamiStatus ret = YAMI_FAIL;
    SurfacePtr reconstruct = createSurface();
    if (!reconstruct)
        return ret;

    if (!ensureSequence (picture))
        return ret;

    if (!ensureMiscParams (picture.get()))
        return ret;

    RefFlags refFlags;
    if (!ensurePicture(picture, reconstruct, refFlags))
        return ret;

    if (!ensureQMatrix(picture))
        return ret;

    if (!picture->encode())
        return ret;

    if (!referenceListUpdate(picture, reconstruct, refFlags))
        return ret;

    return YAMI_SUCCESS;
}

}
