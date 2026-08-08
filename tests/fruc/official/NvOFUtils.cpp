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


#include <iostream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include "FreeImage.h"
#include "NvOFUtils.h"

#define UNKNOWN_FLOW_THRESH 1e9

#if !defined(M_PI)
#define M_PI 3.14159265358979f
#endif

const float NvOFFileWriterStereo::m_disparityMap[8][4] = {
    { 0, 0, 0, 114 }, { 0, 0, 1, 185 }, { 1, 0, 0, 114 }, { 1, 0, 1, 174 },
    { 0, 1, 0, 114 }, { 0, 1, 1, 185 }, { 1, 1, 0, 114 }, { 1, 1, 1, 0 }
};

static inline bool unknown_flow(float u, float v)
{
    return (fabs(u) >  UNKNOWN_FLOW_THRESH)
        || (fabs(v) >  UNKNOWN_FLOW_THRESH)
        || std::isnan(u) || std::isnan(v);
}

std::unique_ptr<NvOFFileWriter>  NvOFFileWriter::Create(uint32_t width,
    uint32_t height,
    NV_OF_MODE ofMode,
    float precision)
{
    std::unique_ptr<NvOFFileWriter> ofFileWriter;
    if (ofMode == NV_OF_MODE_OPTICALFLOW)
    {
        ofFileWriter.reset(new NvOFFileWriterFlow(width, height, precision));
    }
    else if (ofMode == NV_OF_MODE_STEREODISPARITY)
    {
        ofFileWriter.reset(new NvOFFileWriterStereo(width, height, precision));
    }
    else
    {
        throw std::runtime_error("Invalid OF Mode");
    }
    return ofFileWriter;
}

NvOFFileWriter::NvOFFileWriter(uint32_t width,
    uint32_t height,
    float precision)
    : m_width(width),
    m_height(height),
    m_precision(precision)
{
    m_flowVectors.reset(new float[2 * m_width * m_height]);
}

void NvOFFileWriter::SaveOutput(const void* flowVectors,
    std::string baseOutfileName,
    uint32_t frameNum,
    bool saveFlowAsImage)
{
    ConvertToFloat(flowVectors);

    std::ostringstream fileName;
    fileName << baseOutfileName << "_";
    fileName << std::setw(5) << std::setfill('0') << frameNum;


#if defined(ENABLE_RAW_NVOF_OUTPUT)
    WriteRawNvFlowVectors(flowVectors, fileName.str());
#endif

    WriteFlowFile(fileName.str());

    if (saveFlowAsImage)
    {
        WriteFlowImage(fileName.str() + std::string("_viz.png"));
    }
}

void NvOFFileWriter::WriteFlowImage(std::string fileName)
{
    float maxx = -999.0f;
    float maxy = -999.0f;
    float minx = 999.0f, miny = 999.0f;
    float maxrad = -1.0f;
    for (uint32_t n = 0; n < (m_height * m_width); ++n)
    {
        float fx = m_flowVectors[2 * n];
        float fy = m_flowVectors[(2 * n) + 1];

        if (unknown_flow(fx, fy))
            return;
        maxx = std::max(maxx, fx);
        maxy = std::max(maxy, fy);
        minx = std::min(minx, fx);
        miny = std::min(miny, fy);
        float rad = sqrt(fx * fx + fy * fy);
        maxrad = std::max(maxrad, rad);
    }
    maxrad = std::max(maxrad, 1.0f);

    const int BPP = 24;
    FreeImage_Initialise();
    FIBITMAP* bitmap = FreeImage_Allocate(m_width, m_height, BPP);

    for (uint32_t y = 0; y < m_height; ++y)
    {
        for (uint32_t x = 0; x < m_width; ++x)
        {
            float fx = m_flowVectors[(y * m_width * 2) + (2 * x)];
            float fy = m_flowVectors[(y * m_width * 2) + (2 * x) + 1];
            uint8_t pix[3];
            if (unknown_flow(fx, fy))
            {
                pix[0] = pix[1] = pix[2] = 0;
            }
            else
            {
                ComputeColor(fx / maxrad, fy / maxrad, pix);
            }

            RGBQUAD rgb;
            rgb.rgbBlue = pix[0]; rgb.rgbGreen = pix[1]; rgb.rgbRed = pix[2];
            FreeImage_SetPixelColor(bitmap, x, m_height - 1 - y, &rgb);
        }
    }

    FreeImage_Save(FIF_PNG, bitmap, fileName.c_str(), PNG_DEFAULT);
    FreeImage_Unload(bitmap);
    FreeImage_DeInitialise();
}

void NvOFFileWriter::WriteFlowFile(std::string fileName)
{
    WriteFlowVectors(fileName);
}


NvOFFileWriterFlow::NvOFFileWriterFlow(uint32_t width,
    uint32_t height,
    float precision, bool bEnableOutputInKittiFLowFormat)
    : NvOFFileWriter(width, height), m_ncols(0), m_bEnableOutputInKittiFLowFormat(bEnableOutputInKittiFLowFormat)
{
    MakeColorWheel();
}


void NvOFFileWriterFlow::MakeColorWheel()
{
    // relative lengths of color transitions:
    // these are chosen based on perceptual similarity
    // (e.g. one can distinguish more shades between red and yellow 
    //  than between yellow and green)
    int RY = 15;
    int YG = 6;
    int GC = 4;
    int CB = 11;
    int BM = 13;
    int MR = 6;
    m_ncols = RY + YG + GC + CB + BM + MR;

    int i;
    int k = 0;

    for (i = 0; i < RY; i++) SetColors(255, 255 * i / RY, 0, k++);
    for (i = 0; i < YG; i++) SetColors(255 - 255 * i / YG, 255, 0, k++);
    for (i = 0; i < GC; i++) SetColors(0, 255, 255 * i / GC, k++);
    for (i = 0; i < CB; i++) SetColors(0, 255 - 255 * i / CB, 255, k++);
    for (i = 0; i < BM; i++) SetColors(255 * i / BM, 0, 255, k++);
    for (i = 0; i < MR; i++) SetColors(255, 0, 255 - 255 * i / MR, k++);
}

void NvOFFileWriterFlow::SetColors(int r, int g, int b, int k)
{
    m_colorwheel[k][0] = r;
    m_colorwheel[k][1] = g;
    m_colorwheel[k][2] = b;
}

void NvOFFileWriterFlow::ComputeColor(float fx, float fy, uint8_t* pix)
{
    float rad = sqrtf(fx * fx + fy * fy);
    float a = atan2f(-fy, -fx) / M_PI;
    float fk = (a + 1.0f) / 2.0f * (m_ncols - 1);
    int k0 = (int)fk;
    int k1 = (k0 + 1) % m_ncols;
    float f = fk - k0;
    //f = 0; // uncomment to see original color wheel
    for (int b = 0; b < 3; b++)
    {
        float col0 = m_colorwheel[k0][b] / 255.0f;
        float col1 = m_colorwheel[k1][b] / 255.0f;
        float col = (1 - f) * col0 + f * col1;
        if (rad <= 1)
            col = 1 - rad * (1 - col); // increase saturation with radius
        else
            col *= .75f; // out of range
        pix[2 - b] = (int)(255.0f * col);
    }
}

void NvOFFileWriterFlow::ConvertToFloat(const void* _flowVectors)
{
    const NV_OF_FLOW_VECTOR* flowVectors = static_cast<const NV_OF_FLOW_VECTOR*>(_flowVectors);
    for (uint32_t y = 0; y < m_height; ++y)
    {
        for (uint32_t x = 0; x < m_width; ++x)
        {
            m_flowVectors[(y * 2 * m_width) + 2 * x] = (float)(flowVectors[y * m_width + x].flowx / m_precision);
            m_flowVectors[(y * 2 * m_width) + 2 * x + 1] = (float)(flowVectors[y * m_width + x].flowy / m_precision);
        }
    }
}


void NvOFFileWriterFlow::WriteFlowVectors(const std::string fileName)
{
    if (!m_bEnableOutputInKittiFLowFormat)
    {
        std::ofstream fpOut(fileName + std::string("_middlebury.flo"), std::ios::out | std::ios::binary);

        fpOut << TAG_STRING;

        fpOut.write((char*)(&m_width), sizeof(uint32_t));
        fpOut.write((char*)(&m_height), sizeof(uint32_t));
        fpOut.write((char*)m_flowVectors.get(), sizeof(float) * m_width * m_height * 2);
        fpOut.close();
    }
    else
    {
        // KITTI flow format , flow vectors are stored in 16 bit RGB PNG files
        FIBITMAP* bitmap = FreeImage_AllocateT(FIT_RGB16, m_width, m_height);

        for (uint32_t y = 0; y < m_height; y++)
        {
            FIRGB16 *dstBits = (FIRGB16*)FreeImage_GetScanLine(bitmap, m_height - y - 1);
            for (unsigned x = 0; x < m_width; x++)
            {
                float fx = m_flowVectors[(y * m_width * 2) + (2 * x)];
                float fy = m_flowVectors[(y * m_width * 2) + (2 * x) + 1];
                dstBits[x].red = (uint16_t)std::max(std::min(fx * 64.0f + 32768.0f, 65535.0f), 0.0f);
                dstBits[x].green = (uint16_t)std::max(std::min(fy * 64.0f + 32768.0f, 65535.0f), 0.0f);
                dstBits[x].blue = 1;
            }
        }
        auto imageFileName = fileName + std::string("_kitti.png");
        FreeImage_Save(FIF_PNG, bitmap, imageFileName.c_str(), PNG_DEFAULT);
        FreeImage_Unload(bitmap);
    }
}

#if defined(ENABLE_RAW_NVOF_OUTPUT)
void NvOFFileWriterFlow::WriteRawNvFlowVectors(const void* flowVectors, const std::string filename)
{
    std::ofstream fpOut(filename + std::string("_nvof.bin"), std::ios::out | std::ios::binary);
    fpOut.write((const char*)flowVectors, sizeof(NV_OF_FLOW_VECTOR) * m_width * m_height);
    fpOut.close();
}
#endif

NvOFFileWriterStereo::NvOFFileWriterStereo(uint32_t width,
    uint32_t height,
    float precision)
    : NvOFFileWriter(width, height)
{
    float sum = 0;
    for (int32_t i = 0; i<8; i++)
        sum += m_disparityMap[i][3];

    m_cumsum[0] = 0;
    for (int32_t i = 0; i<7; i++)
    {
        m_weights[i] = sum / m_disparityMap[i][3];
        m_cumsum[i + 1] = m_cumsum[i] + m_disparityMap[i][3] / sum;
    }
}

void NvOFFileWriterStereo::ComputeColor(float fx, float fy, uint8_t* pix)
{
    float val = std::min(std::max(fx, 0.0f), 1.0f);

    // find bin
    int32_t i;
    for (i = 0; i<7; i++)
        if (val <= m_cumsum[i + 1])
            break;

    // compute red/green/blue values
    float   w = 1.0f - (val - m_cumsum[i]) * m_weights[i];
    pix[2] = (uint8_t)((w * m_disparityMap[i][0] + (1.0 - w) * m_disparityMap[i + 1][0]) * 255.0);
    pix[1] = (uint8_t)((w * m_disparityMap[i][1] + (1.0 - w) * m_disparityMap[i + 1][1]) * 255.0);
    pix[0] = (uint8_t)((w * m_disparityMap[i][2] + (1.0 - w) * m_disparityMap[i + 1][2]) * 255.0);
}

void NvOFFileWriterStereo::ConvertToFloat(const void* _flowVectors)
{
    const NV_OF_STEREO_DISPARITY* flowVectors = static_cast<const NV_OF_STEREO_DISPARITY*>(_flowVectors);
    for (uint32_t y = 0; y < m_height; ++y)
    {
        for (uint32_t x = 0; x < m_width; ++x)
        {
            m_flowVectors[(y * 2 * m_width) + 2 * x] = (float)(flowVectors[y * m_width + x].disparity / m_precision);
            m_flowVectors[(y * 2 * m_width) + 2 * x + 1] = 0;
        }
    }
}

void NvOFFileWriterStereo::WriteFlowVectors(const std::string fileName)
{
    // KITTI format
    FIBITMAP* bitmap = FreeImage_AllocateT(FIT_UINT16, m_width, m_height);

    for (uint32_t y = 0; y < m_height; y++)
    {
        uint16_t *dstBits = (uint16_t*)FreeImage_GetScanLine(bitmap, m_height - y - 1);
        for (unsigned x = 0; x < m_width; x++)
        {
            float fx = m_flowVectors[(y * m_width * 2) + (2 * x)];
            dstBits[x] = (uint16_t)(std::max(fx*256.0, 1.0));
        }
    }
    std::string imageFileName(fileName);
    imageFileName.append("_kitti.png");
    FreeImage_Save(FIF_PNG, bitmap, imageFileName.c_str(), PNG_DEFAULT);
    FreeImage_Unload(bitmap);
}

#if defined(ENABLE_RAW_NVOF_OUTPUT)
void NvOFFileWriterStereo::WriteRawNvFlowVectors(const void* flowVectors, const std::string filename)
{
    std::ofstream fpOut(filename + std::string("_nvdisp.bin"), std::ios::out | std::ios::binary);
    fpOut.write((const char*)flowVectors, sizeof(NV_OF_STEREO_DISPARITY) * m_width * m_height);
    fpOut.close();
}
#endif

NvOFUtils::NvOFUtils(NV_OF_MODE eMode)
    : m_eMode(eMode)
{
}