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


#pragma once
#include <stdint.h>
#include <fstream>
#include <iostream>
#include <string.h>
#include <sstream>
#include <memory>
#include "FreeImage.h"
#include "nvOpticalFlowCommon.h"

/*
 * NvOFDataLoader is an abstract class for simplifying the task of loading
 * data (in the form of input frames) from one or more files and providing it
 * to the caller.
 */
class NvOFDataLoader
{
public:
    virtual uint8_t* CurrentItem() = 0;
    virtual uint32_t GetWidth() = 0;
    virtual uint32_t GetHeight() = 0;
    virtual uint32_t GetPitch() = 0;
    virtual void Next() = 0;
    virtual bool IsDone() = 0;
    virtual NV_OF_BUFFER_FORMAT GetBufferFormat() = 0;
    virtual ~NvOFDataLoader() {}
};

/*
 * Creates a data loader for the file (or files) in the path specified by
 * 'dataPath'. The kind of data loader created depends on the extension of
 * the filename(s). Currently, data loaders can be created only for files
 * with the '.yuv' or '.png' extensions.
 */
std::unique_ptr<NvOFDataLoader> CreateDataloader(const std::string& dataPath);


class NvOFDataLoaderYUV420 : public NvOFDataLoader
{
public:
    ~NvOFDataLoaderYUV420();
    uint8_t* CurrentItem() override { return m_pNv12Data.get(); }
    uint32_t GetWidth() override { return m_width; }
    uint32_t GetHeight() override { return m_height; }
    uint32_t GetPitch() override { return m_width; }
    NV_OF_BUFFER_FORMAT GetBufferFormat()   override {
        return NV_OF_BUFFER_FORMAT_NV12;
    }
    bool IsDone() override;
    void Next();
protected:
    NvOFDataLoaderYUV420(const char* szFileName);
private:
    void convertToNV12(const void* yuv420, void* nv12Output);
    void ReadYUV();
    uint32_t m_width;
    uint32_t m_height;
    std::unique_ptr<uint8_t[]> m_pFrameData;
    std::unique_ptr<uint8_t[]> m_pNv12Data;
    std::ifstream m_fpInput;
    uint32_t m_numFrames = 0;
    uint32_t m_idx = 0;
    std::vector<std::string> m_fileNames;
    uint32_t m_fileWidth;
    uint32_t m_fileHeight;
    friend std::unique_ptr<NvOFDataLoader> CreateDataloader(const std::string& dataPath);
};

class NvOFDataLoaderPNG : public NvOFDataLoader
{
public:
    uint32_t GetWidth() override { return m_width; }
    uint32_t GetHeight() override { return m_height; }
    uint32_t GetPitch() override { return m_pitch; }
    uint8_t* CurrentItem() override { return m_pFrameData.get(); }

    void Next() override;
    bool IsDone() override { return (m_idx == m_fileNames.size()); }
    NV_OF_BUFFER_FORMAT GetBufferFormat() override {
        if ((m_bpp == 32) || (m_bpp == 24))
            return NV_OF_BUFFER_FORMAT_ABGR8;
        else
            return NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    }
    ~NvOFDataLoaderPNG() {}

protected:
    NvOFDataLoaderPNG(const char* szFileName);
private:
    void InitState(const std::string& fileName);
    void ReadPNG(const std::string& fileName);
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_bpp;
    uint32_t m_pitch;
    std::unique_ptr<uint8_t[]> m_pFrameData;
    std::vector<std::string> m_fileNames;
    uint32_t m_idx = 0;
    bool m_bStatus = true;
    friend std::unique_ptr<NvOFDataLoader> CreateDataloader(const std::string& dataPath);
};

class NvOFDataLoaderFlo : public NvOFDataLoader
{
public:
    ~NvOFDataLoaderFlo() {}
    uint8_t* CurrentItem() override { return reinterpret_cast<uint8_t*>(m_pFlowFixedPoint.get()); }
    uint32_t GetWidth() override { return m_width; }
    uint32_t GetHeight() override { return m_height; }
    uint32_t GetPitch() override { return 0; }
    NV_OF_BUFFER_FORMAT GetBufferFormat()   override {
        return NV_OF_BUFFER_FORMAT_SHORT2;
    }
    bool IsDone() override { return (m_idx == m_fileNames.size()); }
    void Next();
protected:
    NvOFDataLoaderFlo(const char* szFileName, float precision = 32.0f);
private:
    void convertFloat2Fixed(const float* pfFlow, NV_OF_FLOW_VECTOR* pFixedFlow);
    void ReadFlow(const std::string& fileName);
    uint32_t m_width;
    uint32_t m_height;
    float m_precision;
    std::unique_ptr<float[]> m_pFlowFloat;
    std::unique_ptr<NV_OF_FLOW_VECTOR[]> m_pFlowFixedPoint;
    std::vector<std::string> m_fileNames;
    uint32_t m_idx = 0;
    bool m_bStatus = true;
    friend std::unique_ptr<NvOFDataLoader> CreateDataloader(const std::string& dataPath);
};
