/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ION_TEST_FIXTURE_H_
#define ION_TEST_FIXTURE_H_

#include <gtest/gtest.h>
#include <vector>
#include "ion_4.12.h"

using ::testing::Test;

class IonTest : public virtual Test {
  public:
    IonTest();
    virtual ~IonTest(){};
    virtual void SetUp();
    virtual void TearDown();
    int ionfd;
    std::vector<struct ion_heap_data> ion_heaps;
};

#endif /* ION_TEST_FIXTURE_H_ */
