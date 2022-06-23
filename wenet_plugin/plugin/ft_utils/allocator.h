/*
 * Copyright (c) 2019-2022, NVIDIA CORPORATION.  All rights reserved.
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
/**
 * Memory Allocator
 **/

#pragma once

#include "cuda_utils.h"
#include <cuda_runtime.h>
#include <unordered_map>
#include <vector>

#ifdef GOOGLE_CUDA
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#endif

#ifdef TORCH_CUDA
#include "torch/extension.h"
#include <memory>
#endif

#include "ft_utils/logger.h"

namespace fastertransformer {

enum class AllocatorType {
    CUDA,
    TF,
    TH,
    TRT
};

class IAllocator {
public:
    virtual void* malloc(size_t size, const bool is_set_zero = true) = 0;
    virtual void free(void* ptr) const = 0;
    virtual void setStream(cudaStream_t stream) = 0;

    template<typename T>
    void* reMalloc(T* ptr, size_t size, const bool is_set_zero = true)
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        void* void_ptr = (void*)ptr;
        std::string ptr_address = getAddress(void_ptr);
        if (isExist(ptr_address)) {
            if (isReMalloc(ptr_address, size)) {
                FT_LOG_DEBUG("ReMalloc the buffer %p since it is too small.", void_ptr);
                free(void_ptr);
                return malloc(size, is_set_zero);
            }
            else {
                FT_LOG_DEBUG("Reuse original buffer %p and do nothing for reMalloc.", void_ptr);
                return void_ptr;
            }
        }
        else {
            FT_LOG_DEBUG("Cannot find buffer %p, mallocing new one.", void_ptr);
            return malloc(size, is_set_zero);
        }
    }

protected:
    virtual bool isExist(std::string address) const = 0;
    virtual bool isReMalloc(std::string address, size_t size) const = 0;

    std::string getAddress(void* ptr) const
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        char address[256];
        sprintf(address, "%p", ptr);
        return std::string(address);
    }
};

template<AllocatorType AllocType_>
class Allocator;

template<>
class Allocator<AllocatorType::CUDA>: public IAllocator {
private:
    const int device_id_;
    cudaStream_t stream_ = 0;  // initialize as default stream
    std::unordered_map<std::string, std::pair<void*, size_t>>* pointer_mapping_;

    bool isExist(std::string address) const
    {
        return pointer_mapping_->count(address) > 0;
    }
    bool isReMalloc(std::string address, size_t size) const
    {
        FT_CHECK(isExist(address));
        if (pointer_mapping_->at(address).second < size) {
            return true;
        }
        else {
            return false;
        }
    }

public:
    Allocator(int device_id): device_id_(device_id)
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        pointer_mapping_ = new std::unordered_map<std::string, std::pair<void*, size_t>>();
#if defined(CUDART_VERSION) && CUDART_VERSION < 11020
        FT_LOG_WARNING(
            "Async cudaMalloc/Free is not supported before CUDA 11.2. Using Sync cudaMalloc/Free."
            "Note this may lead to hang with NCCL kernels launched in parallel; if so, try NCCL_LAUNCH_MODE=GROUP");
#else
        int device_count = 1;
        cudaGetDeviceCount(&device_count);
        cudaMemPool_t mempool;
        cudaDeviceGetMemPool(&mempool, device_id);
        cudaMemAccessDesc desc = {};
        int peer_access_available = 0;
        for (int i = 0; i < device_count; i++) {
            if (i == device_id) {
                continue;
            }
            cudaDeviceCanAccessPeer(&peer_access_available, device_id, i);
            if (!peer_access_available) {
                FT_LOG_WARNING(
                    "Device " + std::to_string(device_id) + " peer access Device " + std::to_string(i)
                    + " is not avaiable. This may lead to peer access errors when doing tensor/pipeline parallel!");
                continue;
            }
            desc.location.type = cudaMemLocationTypeDevice;
            desc.location.id = i;
            desc.flags = cudaMemAccessFlagsProtReadWrite;
            cudaMemPoolSetAccess(mempool, &desc, 1);
        }
#endif
    }

    virtual ~Allocator()
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        while (!pointer_mapping_->empty()) {
            free(pointer_mapping_->begin()->second.first);
        }
        delete pointer_mapping_;
    }

    void setStream(cudaStream_t stream)
    {
        stream_ = stream;
    }

    void* malloc(size_t size, const bool is_set_zero = true)
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        if (size == 0) {
            return nullptr;
        }
        void* ptr = nullptr;
        int o_device = 0;

        check_cuda_error(getSetDevice(device_id_, &o_device));
#if defined(CUDART_VERSION) && CUDART_VERSION >= 11020
        check_cuda_error(cudaMallocAsync(&ptr, (size_t)(ceil(size / 32.)) * 32, stream_));
#else
        check_cuda_error(cudaMalloc(&ptr, (size_t)(ceil(size / 32.)) * 32));
#endif
        check_cuda_error(getSetDevice(o_device));
        FT_LOG_DEBUG("malloc buffer %p with size %ld", ptr, size);

        pointer_mapping_->insert({getAddress(ptr), {ptr, size}});

        return ptr;
    }

    void free(void* ptr) const
    {
        FT_LOG_DEBUG(__PRETTY_FUNCTION__);
        std::string address = getAddress(ptr);
        if (ptr != nullptr) {
            int o_device = 0;

            if (pointer_mapping_->count(address)) {
                FT_LOG_DEBUG("Free buffer %s", address.c_str());
                check_cuda_error(getSetDevice(device_id_, &o_device));
#if defined(CUDART_VERSION) && CUDART_VERSION >= 11020
                check_cuda_error(cudaFreeAsync(ptr, stream_));
#else
                check_cuda_error(cudaFree(ptr));
#endif
                check_cuda_error(getSetDevice(o_device));
                pointer_mapping_->erase(address);
            }
            else {
                FT_LOG_WARNING("pointer_mapping_ does not have information of ptr at %s.", address.c_str());
            }
        }
        return;
    }
};





}  // namespace fastertransformer
