/*
 * Copyright 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
3B *     http://www.apache.org/licenses/LICENSE-2.0
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

// primary header
#include "RbduReader.h"

// library headers
#include "common/unittest.h"

namespace YamiParser {

class RbduReaderTest
    : public ::testing::Test {

};

#define RBDUREADER_TEST(name) \
    TEST_F(RbduReaderTest, name)


RBDUREADER_TEST(GetPosForEPB)
{
    const uint8_t data[] = {
        0x0, 0x0, 0x3, 0x0,
        0x0, 0x0, 0x3, 0x1,
        0x0, 0x0, 0x3, 0x2,
        0x0, 0x0, 0x3, 0x3,
        0x0, 0x0, 0x4, 0x1,
        0x0, 0x0, 0x3
    };
    const uint8_t expected[] = {
        0x0, 0x0, 0x0,
        0x0, 0x0, 0x1,
        0x0, 0x0, 0x2,
        0x0, 0x0, 0x3,
        0x0, 0x0, 0x4, 0x1,
        0x0, 0x0, 0x3
    };
    RbduReader r(&data[0], sizeof(data));
    for (uint32_t i = 0; i < sizeof(expected); i++) {
        uint32_t v;
        EXPECT_TRUE(r.read(v, 8));
        EXPECT_EQ(v, expected[i]);
    }
    EXPECT_EQ(sizeof(expected) * 8, r.getPos());
}

} // namespace YamiParser
