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
#include <memory>
#include <math.h>
#include <string>
#include <array>
#include <chrono>
#define NOMINMAX
#include "nvOpticalFlowCommon.h"


// first four bytes, should be the same in little endian
#define TAG_FLOAT 202021.25  // check for this when READING the file
#define TAG_STRING "PIEH"    // use this when WRITING the file
#define HEADER_SIZE (strlen(TAG_STRING)+sizeof(int)*2)
/*
 * NvOFFileWriter simplifies the task of writing the output from NVOFAPI into
 * a file.
 */
class NvOFFileWriter
{
public:
    NvOFFileWriter(uint32_t width,
        uint32_t height,
        float precision = 32.0f);

    virtual ~NvOFFileWriter() {}

    /*
     * Creates a file writer for use by the caller. The type of file writer
     * internally instantiated depends on the value of 'ofMode'.
     */
    static std::unique_ptr<NvOFFileWriter> Create(uint32_t width,
        uint32_t height,
        NV_OF_MODE ofMode,
        float precision = 32.0f);

    void SaveOutput(const void* flowVectors,
        std::string baseOutfileName,
        uint32_t frameNum,
        bool saveFlowAsImage = false);

private:
    void WriteFlowImage(std::string fileName);
    void WriteFlowFile(std::string fileName);
    virtual void ComputeColor(float fx, float fy, uint8_t* pix) = 0;
    virtual void ConvertToFloat(const void* flowVectors) = 0;
    virtual void WriteFlowVectors(const std::string filename) = 0;
#if defined(ENABLE_RAW_NVOF_OUTPUT)
    virtual void WriteRawNvFlowVectors(const void* flowVectors, const std::string filename) = 0;
#endif
protected:
    uint32_t m_width;
    uint32_t m_height;
    float m_precision;
    std::unique_ptr<float[]> m_flowVectors;
};

class NvOFFileWriterFlow : public NvOFFileWriter
{
public:
    NvOFFileWriterFlow(uint32_t width,
        uint32_t height,
        float precision = 32.0f, 
        bool bEnableOutputInKittiFLowFormat = false);
private:
    void WriteFlowVectors(const std::string file_name) override;
    void ConvertToFloat(const void* flowVectors) override;
    void ComputeColor(float fx, float fy, uint8_t* pix) override;
#if defined(ENABLE_RAW_NVOF_OUTPUT)
    void WriteRawNvFlowVectors(const void* flowVectors, const std::string filename) override;
#endif
    void MakeColorWheel();
    void SetColors(int r, int g, int b, int k);
    static const int MAXCOLS = 60;
    int m_ncols;
    int m_colorwheel[MAXCOLS][3];
    bool m_bEnableOutputInKittiFLowFormat;
};

class NvOFFileWriterStereo : public NvOFFileWriter
{
public:
    NvOFFileWriterStereo(uint32_t width,
        uint32_t height,
        float precision = 32.0f);

private:
    void ConvertToFloat(const void* flowVectors) override;
    void ComputeColor(float fx, float fy, uint8_t* pix) override;
    void WriteFlowVectors(const std::string file_name) override;
#if defined(ENABLE_RAW_NVOF_OUTPUT)
    void WriteRawNvFlowVectors(const void* flowVectors, const std::string filename) override;
#endif
    static const float m_disparityMap[8][4];

    std::array<float, 8> m_weights;
    std::array<float, 8> m_cumsum;
};

class NvOFBuffer;
class NvOFUtils
{
public:
    NvOFUtils(NV_OF_MODE eMode);
    virtual ~NvOFUtils() {}
    virtual void Upsample(NvOFBuffer *srcBuffer, NvOFBuffer *dstBuffer, uint32_t nScaleFactor) = 0;
protected:
    NV_OF_MODE m_eMode;
};

/*
 * NvOFStopWatch class provide methods for starting and stopping timer.
 */
class NvOFStopWatch
{
public:
    NvOFStopWatch(bool start = false)
    {
        if (start)
            Start();
    }

    void Start()
    {
        t0 = std::chrono::high_resolution_clock::now();

    }

    double ElapsedTime()
    {
        double d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch() - t0.time_since_epoch()).count() / 1.0e9;
        return d;
    }

    double Stop()
    {
        double d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch() - t0.time_since_epoch()).count() / 1.0e9;
        return d;
    }
private:
    std::chrono::high_resolution_clock::time_point t0;
};

