// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ngraph
{
    namespace runtime
    {
        namespace reference
        {
            template <typename T,
                      typename std::enable_if<!std::is_integral<T>::value, bool>::type = true>
            void atanh(const T* arg, T* out, size_t count)
            {
                for (size_t i = 0; i < count; i++)
                {
                    out[i] = std::atanh(arg[i]);
                }
            }

            template <typename T,
                      typename std::enable_if<std::is_integral<T>::value, bool>::type = true>
            void atanh(const T* arg, T* out, size_t count)
            {
                for (size_t i = 0; i < count; i++)
                {
                    out[i] = std::roundl(std::atanh(arg[i]));
                }
            }
        } // namespace reference
    }     // namespace runtime
} // namespace ngraph
