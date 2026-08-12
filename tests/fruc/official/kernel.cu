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


#include <cuda_runtime.h>
#include <stdio.h>

typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;
typedef   signed short  int16_t;
typedef   signed int    int32_t;

#define BLOCKDIM_X 32
#define BLOCKDIM_Y 16

// data required to do 2x upsampling.  Same can be used for 4x upsampling also
#define SMEM_COLS  ((BLOCKDIM_X)/2)
#define SMEM_ROWS  ((BLOCKDIM_Y)/2)

static const char *_cudaGetErrorEnum(cudaError_t error) {
    return cudaGetErrorName(error);
}

template <typename T>
void check(T result, char const *const func, const char *const file,
    int const line) {
    if (result) {
        fprintf(stderr, "CUDA error at %s:%d code=%d(%s) \"%s\" \n", file, line,
            static_cast<uint32_t>(result), _cudaGetErrorEnum(result), func);
        // Make sure we call CUDA Device Reset before exiting
        exit(EXIT_FAILURE);
    }
}
#define checkCudaErrors(val) check((val), #val, __FILE__, __LINE__)

template <class T>
static __device__ void ReadArrayData(cudaSurfaceObject_t srcSurf, uint32_t x0, uint32_t y0, uint32_t src_w, uint32_t src_h,
                                     T src[][SMEM_COLS], uint32_t i, uint32_t j)
{
    surf2Dread<T>(&src[j][i], srcSurf, x0 * sizeof(T), y0, cudaBoundaryModeClamp);
}

template <class T>
static __device__ void ReadDevPtrData(void* devptr, uint32_t x0, uint32_t y0, uint32_t src_w, uint32_t src_h, uint32_t src_pitch,
                                      T src[][SMEM_COLS], uint32_t i, uint32_t j)
{
    uint32_t shift = (sizeof(T) == sizeof(int32_t)) ? 2 : 1;
    src[j][i] = *(T*)((uint8_t*)devptr + y0 * src_pitch + (x0 << shift));
}


extern "C"
__global__ void NearestNeighborFlowKernel(cudaSurfaceObject_t srcSurf, void* srcDevPtr, uint32_t src_w, uint32_t src_pitch, uint32_t src_h,
                                          cudaSurfaceObject_t dstSurf, void* dstDevPtr, uint32_t dst_w, uint32_t dst_pitch, uint32_t dst_h,
                                          uint32_t nScaleFactor)
{
    int x = blockDim.x * blockIdx.x + threadIdx.x;
    int y = blockDim.y * blockIdx.y + threadIdx.y;

    int x0 = x / nScaleFactor;
    int y0 = y / nScaleFactor;

    __shared__ short2 src[SMEM_ROWS][SMEM_COLS];

    int i = threadIdx.x / nScaleFactor;
    int j = threadIdx.y / nScaleFactor;

    if ((x % nScaleFactor == 0) && (y % nScaleFactor == 0))
    {
        if (srcDevPtr == NULL)
        {
            ReadArrayData<short2>(srcSurf, x0, y0, src_w, src_h, src, i, j);
        }
        else
        {
            ReadDevPtrData<short2>(srcDevPtr, x0, y0, src_w, src_h, src_pitch, src, i, j);
        }
    }
    __syncthreads();

    if (x < dst_w && y < dst_h)
    {
        if (dstDevPtr == NULL)
        {
            surf2Dwrite<short2>(src[j][i], dstSurf, x * sizeof(short2), y, cudaBoundaryModeClamp);
        }
        else
        {
            *(short2*)((uint8_t*)dstDevPtr + y * dst_pitch + (x << 2)) = src[j][i];
        }
    }
}

void FlowUpsample(cudaArray_t srcArray, void* srcDevPtr, uint32_t nSrcWidth, uint32_t nSrcPitch, uint32_t nSrcHeight,
                  cudaArray_t dstArray, void* dstDevPtr, uint32_t nDstWidth, uint32_t nDstPitch, uint32_t nDstHeight,
                  uint32_t nScaleFactor)
{
    if (srcDevPtr == 0 && dstDevPtr == 0)
    {
        cudaSurfaceObject_t srcSurfObj;
        cudaResourceDesc srcSurfRes;
        memset(&srcSurfRes, 0, sizeof(cudaResourceDesc));
        srcSurfRes.resType = cudaResourceTypeArray;
        srcSurfRes.res.array.array = srcArray;
        checkCudaErrors(cudaCreateSurfaceObject(&srcSurfObj, &srcSurfRes));

        cudaSurfaceObject_t dstSurfObj;
        cudaResourceDesc dstSurfRes;
        memset(&dstSurfRes, 0, sizeof(cudaResourceDesc));
        dstSurfRes.resType = cudaResourceTypeArray;
        dstSurfRes.res.array.array = dstArray;
        checkCudaErrors(cudaCreateSurfaceObject(&dstSurfObj, &dstSurfRes));

        dim3 blockDim(BLOCKDIM_X, BLOCKDIM_Y);
        dim3 gridDim((nDstWidth + blockDim.x - 1) / blockDim.x, (nDstHeight + blockDim.y - 1) / blockDim.y);
        NearestNeighborFlowKernel << <gridDim, blockDim >> > (srcSurfObj, srcDevPtr, nSrcWidth, nSrcPitch, nSrcHeight,
            dstSurfObj, dstDevPtr, nDstWidth, nDstPitch, nDstHeight,
            nScaleFactor);
        checkCudaErrors(cudaGetLastError());

        checkCudaErrors(cudaDestroySurfaceObject(srcSurfObj));
        checkCudaErrors(cudaDestroySurfaceObject(dstSurfObj));
    }
    else
    {
        dim3 blockDim(BLOCKDIM_X, BLOCKDIM_Y);
        dim3 gridDim((nDstWidth + blockDim.x - 1) / blockDim.x, (nDstHeight + blockDim.y - 1) / blockDim.y);
        NearestNeighborFlowKernel << <gridDim, blockDim >> > (0, srcDevPtr, nSrcWidth, nSrcPitch, nSrcHeight,
            0, dstDevPtr, nDstWidth, nDstPitch, nDstHeight,
            nScaleFactor);

        checkCudaErrors(cudaGetLastError());
    }
}

extern "C"
__global__ void NearestNeighborDispKernel(cudaSurfaceObject_t srcSurf, void* srcDevPtr, uint32_t src_w, uint32_t src_h, uint32_t src_pitch,
                                          cudaSurfaceObject_t dstSurf, void* dstDevPtr, uint32_t dst_w, uint32_t dst_h, uint32_t dst_pitch,
                                          uint32_t nScaleFactor)
{
    int x = blockDim.x * blockIdx.x + threadIdx.x;
    int y = blockDim.y * blockIdx.y + threadIdx.y;

    int x0 = x / nScaleFactor;
    int y0 = y / nScaleFactor;

    __shared__ short src[SMEM_ROWS][SMEM_COLS];

    int i = threadIdx.x / nScaleFactor;
    int j = threadIdx.y / nScaleFactor;

    if (srcDevPtr == nullptr)
    {
        ReadArrayData<short>(srcSurf, x0, y0, src_w, src_h, src, i, j);
    }
    else
    {
        ReadDevPtrData<short>(srcDevPtr, x0, y0, src_w, src_h, src_pitch, src, i, j);
    }

    __syncthreads();

    if (x < dst_w && y < dst_h)
    {
        if (srcDevPtr == nullptr)
        {
            surf2Dwrite<short>(src[j][i], dstSurf, x * sizeof(short), y, cudaBoundaryModeClamp);
        }
        else
        {
            *(short*)((uint8_t*)dstDevPtr + y * dst_pitch + (x << 1)) = src[j][i];
        }
    }
}

void StereoDisparityUpsample(cudaArray_t srcArray, void* srcDevPtr, uint32_t nSrcWidth, uint32_t nSrcPitch, uint32_t nSrcHeight,
    cudaArray_t dstArray, void* dstDevPtr, uint32_t nDstWidth, uint32_t nDstPitch, uint32_t nDstHeight, uint32_t nScaleFactor)
{
    if (srcDevPtr == nullptr && dstDevPtr == nullptr)
    {
        cudaSurfaceObject_t srcSurfObj;
        cudaResourceDesc srcSurfRes;
        memset(&srcSurfRes, 0, sizeof(cudaResourceDesc));
        srcSurfRes.resType = cudaResourceTypeArray;
        srcSurfRes.res.array.array = srcArray;
        checkCudaErrors(cudaCreateSurfaceObject(&srcSurfObj, &srcSurfRes));

        cudaSurfaceObject_t dstSurfObj;
        cudaResourceDesc dstSurfRes;
        memset(&dstSurfRes, 0, sizeof(cudaResourceDesc));
        dstSurfRes.resType = cudaResourceTypeArray;
        dstSurfRes.res.array.array = dstArray;
        checkCudaErrors(cudaCreateSurfaceObject(&dstSurfObj, &dstSurfRes));

        dim3 blockDim(BLOCKDIM_X, BLOCKDIM_Y);
        dim3 gridDim((nDstWidth + blockDim.x - 1) / blockDim.x, (nDstHeight + blockDim.y - 1) / blockDim.y);
        NearestNeighborDispKernel << <gridDim, blockDim >> > (srcSurfObj, nullptr, nSrcWidth, nSrcHeight, nSrcPitch,
                                                              dstSurfObj, nullptr, nDstWidth, nDstHeight, nDstPitch,
                                                              nScaleFactor);

        checkCudaErrors(cudaGetLastError());

        checkCudaErrors(cudaDestroySurfaceObject(srcSurfObj));
        checkCudaErrors(cudaDestroySurfaceObject(dstSurfObj));
    }
    else
    {
        dim3 blockDim(BLOCKDIM_X, BLOCKDIM_Y);
        dim3 gridDim((nDstWidth + blockDim.x - 1) / blockDim.x, (nDstHeight + blockDim.y - 1) / blockDim.y);
        NearestNeighborDispKernel << <gridDim, blockDim >> > (0, srcDevPtr, nSrcWidth, nSrcHeight, nSrcPitch,
                                                              0, dstDevPtr, nDstWidth, nDstHeight, nDstPitch,
                                                              nScaleFactor);

        checkCudaErrors(cudaGetLastError());
    }
}
