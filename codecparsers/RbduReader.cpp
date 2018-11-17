/*
 * Copyright 2016 Intel Corporation
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

#include "RbduReader.h"
#include <assert.h>

namespace YamiParser {

RbduReader::RbduReader(const uint8_t* pdata, uint32_t size)
    : EpbReader(pdata, size)
    , m_idx(0)
{
}

//table 255
bool RbduReader::isEmulationPreventionByte(const uint8_t* p)
{
    //       0x0 0x0 0x3 0x?
    //m_idx    0   1   2   3
    uint8_t v = *p;
    const uint8_t* end = m_stream + m_size;
    const uint8_t* next = p + 1;

    if (v == 0x3) {
        if (m_idx == 2 && next < end && *end <= 0x3) {
            m_idx++;
            return true;
        }
        m_idx = 0;
        return false;
    }
    if (v != 0 || m_idx == 3) {
        m_idx = 0;
        return false;
    }

    if (m_idx < 2)
        m_idx++;
    return false;
}

} /*namespace YamiParser*/
