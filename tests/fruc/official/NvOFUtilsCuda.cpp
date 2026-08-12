/*
* Copyright (c) 2018-2023 NVIDIA Corporation
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


#include "cuda.h"
#include "cuda_runtime.h"
#include "NvOF.h"
#include "NvOFCuda.h"
#include "NvOFUtils.h"

void FlowUpsample(cudaArray_t srcArray, void* srcDevPtr, uint32_t nSrcWidth, uint32_t nSrcPitch, uint32_t nSrcHeight,
                  cudaArray_t dstArray, void* dstDevPtr, uint32_t nDstWidth, uint32_t nDstPitch, uint32_t nDstHeight,
                  uint32_t nScaleFactor);
void StereoDisparityUpsample(cudaArray_t srcArray, void* srcDevPtr, uint32_t nSrcWidth, uint32_t nSrcPitch, uint32_t nSrcHeight,
                             cudaArray_t dstArray, void* dstDevPtr, uint32_t nDstWidth, uint32_t nDstPitch, uint32_t nDstHeight,
                             uint32_t nScaleFactor);

NvOFUtilsCuda::NvOFUtilsCuda(NV_OF_MODE eMode)
    : NvOFUtils(eMode)
{
}

static inline bool isCudaArray(NvOFBuffer* buffer)
{
    NvOFBufferCudaArray* cuarr = dynamic_cast<NvOFBufferCudaArray*>(buffer);
    return (cuarr) ? true : false;
}

void NvOFUtilsCuda::Upsample(NvOFBuffer* srcBuf, NvOFBuffer* dstBuf, uint32_t nScaleFactor)
{
    CUarray srcArr = nullptr, dstArr = nullptr;
    CUdeviceptr srcDevPtr = 0, dstDevPtr = 0;
    uint32_t nSrcWidth = srcBuf->getWidth();
    uint32_t nSrcHeight = srcBuf->getHeight();
    uint32_t nSrcPitch = 0;
    uint32_t nDstWidth = dstBuf->getWidth();
    uint32_t nDstHeight = dstBuf->getHeight();
    uint32_t nDstPitch = 0;

    bool srcIsCuArray = isCudaArray(srcBuf);
    if (srcIsCuArray)
    {
        NvOFBufferCudaArray* srcBufCUarray = dynamic_cast<NvOFBufferCudaArray*>(srcBuf);
        NvOFBufferCudaArray* dstBufCUarray = dynamic_cast<NvOFBufferCudaArray*>(dstBuf);
        srcArr = srcBufCUarray->getCudaArray();
        dstArr = dstBufCUarray->getCudaArray();
    }
    else
    {
        NvOFBufferCudaDevicePtr* srcBufDevPtr = dynamic_cast<NvOFBufferCudaDevicePtr*>(srcBuf);
        NvOFBufferCudaDevicePtr* dstBufDevPtr = dynamic_cast<NvOFBufferCudaDevicePtr*>(dstBuf);
        srcDevPtr = srcBufDevPtr->getCudaDevicePtr();
        dstDevPtr = dstBufDevPtr->getCudaDevicePtr();
        nSrcPitch = srcBufDevPtr->getStrideInfo().strideInfo[0].strideXInBytes;
        nDstPitch = dstBufDevPtr->getStrideInfo().strideInfo[0].strideXInBytes;
    }

    if (m_eMode == NV_OF_MODE_OPTICALFLOW)
    {
        FlowUpsample((cudaArray_t)srcArr, (void*)srcDevPtr, nSrcWidth, nSrcPitch, nSrcHeight,
                     (cudaArray_t)dstArr, (void*)dstDevPtr, nDstWidth, nDstPitch, nDstHeight,
                     nScaleFactor);
    }
    else
    {
        StereoDisparityUpsample((cudaArray_t)srcArr, (void*)srcDevPtr, nSrcWidth, nSrcPitch, nSrcHeight,
                                (cudaArray_t)dstArr, (void*)dstDevPtr, nDstWidth, nDstPitch, nDstHeight,
                                nScaleFactor);
    }
}