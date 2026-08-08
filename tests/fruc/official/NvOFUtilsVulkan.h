/*
* Copyright (c) 2022-2023 NVIDIA Corporation
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the software, and to permit persons to whom the
* software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/


#pragma once
#include <fstream>
#include <functional>
#include "NvOFUtils.h"

#define MAX_PYD_LEVEL 7

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

#define CREATE_INFO_ALIGN(_size) (((_size) + sizeof(void*) - 1) & ~((size_t)sizeof(void*) - 1))

#define _VK_GET_INSTANCE_PROC(_var, _type, _name)                                               \
    do                                                                                          \
    {                                                                                           \
        _var = (_type)vkGetInstanceProcAddr(m_instance, _name);                                 \
        if (!_var)                                                                              \
        {                                                                                       \
            NVOF_THROW_ERROR("Some vulkan entrypoints missing", NV_OF_ERR_UNSUPPORTED_FEATURE); \
        }                                                                                       \
    } while (0)
#define VK_GET_INSTANCE_PROC(_proc)  _VK_GET_INSTANCE_PROC(_proc, PFN_##_proc, #_proc)

#define _VK_GET_DEVICE_PROC(_var, _type, _name)                                                 \
    do                                                                                          \
    {                                                                                           \
        _var = (_type)vkGetDeviceProcAddr(m_device, _name);                                     \
        if (!_var)                                                                              \
        {                                                                                       \
            NVOF_THROW_ERROR("Some vulkan entrypoints missing", NV_OF_ERR_UNSUPPORTED_FEATURE); \
        }                                                                                       \
    } while (0)

#define VK_GET_DEVICE_PROC(_proc)  _VK_GET_DEVICE_PROC(_proc, PFN_##_proc, #_proc)

typedef struct _NV_OF_TIMELINE_SEMAPHORE
{
    VkSemaphore                     semaphore;
    uint64_t                        value;
} NV_OF_TIMELINE_SEMAPHORE;

namespace GNvOFUtilsVulkan
{
    VkFormat NvOFBufferFormatToVkFormat(NV_OF_BUFFER_FORMAT ofBufFormat);
    NV_OF_BUFFER_FORMAT VkFormatToNvOFBufferFormat(VkFormat vkFormat);
    uint32_t GetNumberOfPlanes(VkFormat vkFormat);
    VkOpticalFlowUsageFlagsNV GetVkOpticalFlowUsageFlag(const NV_OF_BUFFER_USAGE bufUsage);
    VkFormatFeatureFlags2 GetVkFormatFeatureFlags(const NV_OF_BUFFER_USAGE bufUsage);
}

class NvOFUtilsVulkan : public NvOFUtils
{
public:
    NvOFUtilsVulkan(): NvOFUtils(NV_OF_MODE_OPTICALFLOW) {}
    virtual void Upsample(NvOFBuffer* srcBuffer, NvOFBuffer* dstBuffer, uint32_t nScaleFactor) {}
    void TransferLayout(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t queueFamilyIndex);
    static void EnableInstanceOpticalFlow(VkInstanceCreateInfo* const pInstanceCreateInfo, size_t* pSize, VkInstanceCreateInfo* pInstanceCreateInfoOF);
    static void EnableDeviceOpticalFlow(VkInstance instance, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo* const pDeviceCreateInfo, size_t* pSize, VkDeviceCreateInfo* pDeviceCreateInfoOF);
    static void EnableResourceOpticalFlow(VkInstance instance, VkPhysicalDevice physicalDevice, const NV_OF_BUFFER_USAGE bufUsage, const NV_OF_MODE ofMode, void* const pCreateInfo, size_t* pSize, void* pCreateInfoOF);
    static bool GetOpticalFlowQueue(VkPhysicalDevice physicalDevice, uint32_t& queueFamilyIndex, uint32_t& queueIndex);
    static bool GetAppQueue(VkPhysicalDevice physicalDevice, uint32_t& queueFamilyIndex, uint32_t& queueIndex, VkQueueFlags requiredCaps = (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT), VkQueueFlags disallowCaps = 0);
protected:
    VkPipelineStageFlags getStageMask(VkImageLayout layout);
    VkAccessFlags getAccessMask(VkImageLayout layout);
};

