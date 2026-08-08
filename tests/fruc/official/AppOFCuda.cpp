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


#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <cuda.h>
#include <sstream>
#include <iterator>
#include "NvOFCuda.h"
#include "NvOFDataLoader.h"
#include "NvOFUtils.h"
#include "NvOFCmdParser.h"

void NvOFBatchExecute(NvOFObj &nvOpticalFlow,
    std::vector<NvOFBufferObj> &inputBuffers,
    std::vector<NvOFBufferObj> &outputBuffers,
    std::vector<NvOFBufferObj> &hintBuffers,
    uint32_t batchSize,
    double &executionTime,
    bool measureFPS,
    CUstream  inputStream,
    CUstream  outputStream,
    uint32_t numROIs,
    NV_OF_ROI_RECT *ROIData)
{
    //measureFPS makes sure that data upload is finished before kicking off
    //optical flow execute for a batch of frames. It also makes sure that 
    //optical flow batch execute is finished before output download.
    if (measureFPS)
    {
        NvOFStopWatch nvStopWatch;

        CUDA_DRVAPI_CALL(cuStreamSynchronize(inputStream));
        nvStopWatch.Start();
        for (uint32_t i = 0; i < batchSize; i++)
        {
            nvOpticalFlow->Execute(inputBuffers[i].get(),
                inputBuffers[i + 1].get(),
                outputBuffers[i].get(),
                !hintBuffers.empty() ? hintBuffers[i].get() : nullptr);
        }
        CUDA_DRVAPI_CALL(cuStreamSynchronize(outputStream));
        executionTime += nvStopWatch.Stop();
    }
    else
    {
        for (uint32_t i = 0; i < batchSize; i++)
        {
            nvOpticalFlow->Execute(inputBuffers[i].get(),
                inputBuffers[i + 1].get(),
                outputBuffers[i].get(), 
                !hintBuffers.empty() ? hintBuffers[i].get() : nullptr,
                nullptr,
                numROIs,
                ROIData);
        }
    }
}

/*
ROI config file format.
numrois 3
roi0 640 96 1152 192
roi1 640 64 896 864
roi2 640 960 256 32
*/
int parseROI(std::string ROIFileName, uint32_t &numROIs, NV_OF_ROI_RECT* roiData)
{
    std::string str;
    uint32_t nRois = 0;
    numROIs = 0;
    bool bRoiStarted = false;
    std::ifstream hRoiFile;
    hRoiFile.open(ROIFileName, std::ios::in);

    if (hRoiFile.is_open())
    {
        while (std::getline(hRoiFile, str))
        {
            std::istringstream iss(str);
            std::vector<std::string> tokens{ std::istream_iterator<std::string>{iss},
                std::istream_iterator<std::string>{} };

            if (tokens.size() == 0) continue; // if empty line, coninue

            transform(tokens[0].begin(), tokens[0].end(), tokens[0].begin(), ::tolower);
            if (tokens[0] == "numrois")
            {
                nRois = atoi(tokens[1].data());
            }
            else if (tokens[0].rfind("roi", 0) == 0)
            {
                NV_OF_ROI_RECT roi;
                roi.start_x = atoi(tokens[1].data());
                roi.start_y = atoi(tokens[2].data());
                roi.width = atoi(tokens[3].data());
                roi.height = atoi(tokens[4].data());
                roiData[numROIs].start_x = roi.start_x;
                roiData[numROIs].start_y = roi.start_y;
                roiData[numROIs].width = roi.width;
                roiData[numROIs].height = roi.height;
                (numROIs)++;
            }
            else if (tokens[0].rfind("#", 0) == 0)
            {
                continue;
            }
            else
            {
                std::cout << "Unidentified keyword in roi config file " << tokens[0] << std::endl;
                hRoiFile.close();
                return NV_OF_ERR_INVALID_PARAM;
            }
        }
    }
    else
    {
        std::cout << "Unable to open ROI file " << std::endl;
        return NV_OF_ERR_GENERIC;
    }
    if (nRois != numROIs)
    {
        std::cout << "NumRois(" << nRois << ")and specified roi rects (" << numROIs << ")are not matching " << std::endl;
        hRoiFile.close();
        return NV_OF_ERR_INVALID_PARAM;
    }
    hRoiFile.close();
    return NV_OF_SUCCESS;
}

void EstimateFlow(CUcontext cuContext,
    CUstream  inputStream,
    CUstream   outputStream,
    std::string inputFileName,
    std::string outputFileBaseName,
    std::string hintFileName,
    std::string RoiFileName,
    NV_OF_CUDA_BUFFER_TYPE inputBufferType, 
    NV_OF_CUDA_BUFFER_TYPE outputBufferType,
    NV_OF_PERF_LEVEL perfPreset,
    uint32_t gridSize,
    uint32_t hintGridSize,
    bool saveFlowAsImage,
    bool measureFPS)
{
    std::unique_ptr<NvOFDataLoader> dataLoader = CreateDataloader(inputFileName);
    uint32_t width = dataLoader->GetWidth();
    uint32_t height = dataLoader->GetHeight();
    uint32_t nFrameSize = width * height;
    uint32_t nScaleFactor = 1;
    uint32_t numROIs = 0;
    NV_OF_ROI_RECT ROIData[8];
    memset(&ROIData, 0, sizeof(ROIData));
    bool bEnableRoi = false;
    bool dumpOfOutput = (!outputFileBaseName.empty());
    bool enableExternalHints = (!hintFileName.empty());
    std::unique_ptr<NvOFDataLoader> dataLoaderFlo;
    if (enableExternalHints)
    {
        dataLoaderFlo = CreateDataloader(hintFileName);
        uint32_t hintWidth = dataLoaderFlo->GetWidth();
        uint32_t hintHeight = dataLoaderFlo->GetHeight();
        if ((hintWidth != (width + hintGridSize - 1) / hintGridSize) ||
            (hintHeight != (height + hintGridSize - 1) / hintGridSize))
        {
            throw std::runtime_error("Invalid hint file");
        }
    }

    NvOFObj nvOpticalFlow = NvOFCuda::Create(cuContext, width, height, dataLoader->GetBufferFormat(),
        inputBufferType, outputBufferType, NV_OF_MODE_OPTICALFLOW, perfPreset, inputStream, outputStream);

    uint32_t hwGridSize;
    if (!nvOpticalFlow->CheckGridSize(gridSize))
    {
        if (!nvOpticalFlow->GetNextMinGridSize(gridSize, hwGridSize))
        {
            throw std::runtime_error("Invalid parameter");
        }
        else
        {
            nScaleFactor = hwGridSize / gridSize;
        }
    }
    else
    {
        hwGridSize = gridSize;
    }
    if (enableExternalHints && (hintGridSize < hwGridSize))
    {
        throw std::runtime_error("Hint grid size must be same or bigger than the output grid size");
    }
    if (!RoiFileName.empty())
    {
        if (!nvOpticalFlow->IsROISupported())
        {
            throw std::runtime_error("Invalid parameter: ROI not supported on this GPU");
        }
        else
        {
            if (parseROI(RoiFileName, numROIs, ROIData))
            {
                std::cout << "Wrong Region of Interest config file proceeding without ROI" << std::endl;
                numROIs = 0;
                memset(&ROIData, 0, sizeof(ROIData));
            }
            if (numROIs)
            {
                bEnableRoi = true;
            }
        }
    }

    nvOpticalFlow->Init(hwGridSize, hintGridSize, enableExternalHints, bEnableRoi);

    const uint32_t NUM_INPUT_BUFFERS = 16;
    const uint32_t NUM_OUTPUT_BUFFERS = NUM_INPUT_BUFFERS - 1;
    std::vector<NvOFBufferObj> inputBuffers;
    std::vector<NvOFBufferObj> outputBuffers;
    inputBuffers = nvOpticalFlow->CreateBuffers(NV_OF_BUFFER_USAGE_INPUT, NUM_INPUT_BUFFERS);
    outputBuffers = nvOpticalFlow->CreateBuffers(NV_OF_BUFFER_USAGE_OUTPUT, NUM_OUTPUT_BUFFERS);
    std::vector<NvOFBufferObj> exthintBuffers;
    if (enableExternalHints)
    {
        exthintBuffers = nvOpticalFlow->CreateBuffers(NV_OF_BUFFER_USAGE_HINT, NUM_OUTPUT_BUFFERS);
    }
    std::unique_ptr<NvOFUtils> nvOFUtils(new NvOFUtilsCuda(NV_OF_MODE_OPTICALFLOW));
    std::vector<NvOFBufferObj> upsampleBuffers;
    std::unique_ptr<NV_OF_FLOW_VECTOR[]> pOut;
    std::unique_ptr<NvOFFileWriter> flowFileWriter;
    if (nScaleFactor > 1)
    {
        auto nOutWidth = (width + gridSize - 1) / gridSize;
        auto nOutHeight = (height + gridSize - 1) / gridSize;

        upsampleBuffers = nvOpticalFlow->CreateBuffers(nOutWidth, nOutHeight, NV_OF_BUFFER_USAGE_OUTPUT, NUM_OUTPUT_BUFFERS);

        uint32_t nOutSize = nOutWidth * nOutHeight;
        pOut.reset(new NV_OF_FLOW_VECTOR[nOutSize]);
        if (pOut == nullptr)
        {
            std::ostringstream err;
            err << "Failed to allocate output host memory of size " << nOutSize * sizeof(NV_OF_FLOW_VECTOR) << " bytes" << std::endl;
            throw std::bad_alloc();
        }

        flowFileWriter = NvOFFileWriter::Create(nOutWidth,
            nOutHeight,
            NV_OF_MODE_OPTICALFLOW,
            32.0f);
    }
    else
    {
        uint32_t nOutSize = outputBuffers[0]->getWidth() * outputBuffers[0]->getHeight();
        pOut.reset(new NV_OF_FLOW_VECTOR[nOutSize]);
        if (pOut == nullptr)
        {
            std::ostringstream err;
            err << "Failed to allocate output host memory of size " << nOutSize * sizeof(NV_OF_FLOW_VECTOR) << " bytes" << std::endl;
            throw std::bad_alloc();
        }

        flowFileWriter = NvOFFileWriter::Create(outputBuffers[0]->getWidth(),
            outputBuffers[0]->getHeight(),
            NV_OF_MODE_OPTICALFLOW,
            32.0f);
    }

    uint32_t                        curFrameIdx = 0;
    uint32_t                        frameCount = 0;
    bool                            lastSet = false;
    double                          executionTime = 0;
    for (; (!dataLoader->IsDone() || curFrameIdx > 1); dataLoader->Next())
    {
        if (!dataLoader->IsDone())
        {
            inputBuffers[curFrameIdx]->UploadData(dataLoader->CurrentItem());
            if (enableExternalHints && (curFrameIdx > 0))
            {
                if (!dataLoaderFlo->IsDone())
                {
                    exthintBuffers[curFrameIdx - 1]->UploadData(dataLoaderFlo->CurrentItem());
                    dataLoaderFlo->Next();
                }
                else
                {
                    throw std::runtime_error("no hint file!");
                }
            }
        }
        else
        {
            // If number of frames is non multiple of NUM_INPUT_BUFFERS then execute will be
            // called for TotalFrames % NUM_INPUT_BUFFERS frames in last set.
            // No uploadData() called for last frame so curFrameIdx is decremented by 1.
            curFrameIdx--;
            lastSet = true;
        }

        if (curFrameIdx == NUM_INPUT_BUFFERS-1 || lastSet)
        {
            NvOFBatchExecute(nvOpticalFlow, inputBuffers, outputBuffers, exthintBuffers, curFrameIdx, executionTime, measureFPS,
                             inputStream,outputStream, numROIs, ROIData);
            if (dumpOfOutput)
            {
                for (uint32_t i = 0; i < curFrameIdx; i++)
                {
                    if (nScaleFactor > 1)
                    {
                        nvOFUtils->Upsample(outputBuffers[i].get(), upsampleBuffers[i].get(), nScaleFactor);
                        upsampleBuffers[i]->DownloadData(pOut.get());
                    }
                    else
                    {
                        outputBuffers[i]->DownloadData(pOut.get());
                    }
                    flowFileWriter->SaveOutput((void*)pOut.get(),
                        outputFileBaseName, frameCount, saveFlowAsImage);
                    frameCount++;
                }
            }
            else
            {
                frameCount += curFrameIdx;
            }

            if (lastSet)
            {
                break;
            }
            // Last frame of previous set of input buffers is reused as first element for next iteration.
            swap(inputBuffers[curFrameIdx], inputBuffers[0]);
            curFrameIdx = 0;
        }
        curFrameIdx++;
    }

    if (measureFPS)
    {
        double fps = (executionTime > 0.0) ? (frameCount / executionTime) : 1.0;
        std::cout << "Total Frames = " << frameCount << "\n";
        std::cout << "Time = " << executionTime << " s, NvOF FPS = " << fps << "\n";
    }
    else
    {
        CUDA_DRVAPI_CALL(cuCtxSynchronize());
    }
}

int main(int argc, char **argv)
{
    std::string inputFileName;
    std::string outputFileBaseName;
    std::string hintFileName;
    std::string ROIFileName;
    std::string preset = "medium";
    std::string inputBufferType = "cudaArray";
    std::string outputBufferType = "cudaArray";
    int gridSize = 1;
    int hintGridSize = 1;
    bool visualFlow = 0;
    int gpuId = 0;
    bool showHelp = 0;
    bool useCudaStream = 0;
    bool measureFPS = false;
    CUcontext cuContext = nullptr;
    CUstream   inputStream = nullptr;
    CUstream   outputStream = nullptr;

    std::unordered_map<std::string, NV_OF_PERF_LEVEL> presetMap = {
        { "slow", NV_OF_PERF_LEVEL_SLOW },
        { "medium", NV_OF_PERF_LEVEL_MEDIUM },
        { "fast", NV_OF_PERF_LEVEL_FAST } };

    std::unordered_map<std::string, NV_OF_CUDA_BUFFER_TYPE> bufferTypeMap = {
        { "cudaArray", NV_OF_CUDA_BUFFER_TYPE_CUARRAY },
        { "cudaDevicePtr", NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR } };

    NV_OF_PERF_LEVEL perfPreset = NV_OF_PERF_LEVEL_MEDIUM;
    NV_OF_CUDA_BUFFER_TYPE inputBufferTypeEnum = NV_OF_CUDA_BUFFER_TYPE_CUARRAY;
    NV_OF_CUDA_BUFFER_TYPE outputBufferTypeEnum = NV_OF_CUDA_BUFFER_TYPE_CUARRAY;

    try
    {
        NvOFCmdParser cmdParser;
        cmdParser.AddOptions("input", inputFileName, "Input filename "
                                                     "[ e.g. inputDir" DIR_SEP "input*.png, "
                                                     "inputDir" DIR_SEP "input%d.png, "
                                                     "inputDir" DIR_SEP "input_wxh.yuv ]");
        cmdParser.AddOptions("output", outputFileBaseName, "Output file base name "
                                                           "[ e.g. outputDir" DIR_SEP "outFilename ]");
        cmdParser.AddOptions("hint", hintFileName, "Hint filename "
                                                   "[ e.g hintDir" DIR_SEP "hint*.flo ]");
        cmdParser.AddOptions("RoiConfig", ROIFileName, "Region of Interest filename ");
        cmdParser.AddOptions("gpuIndex", gpuId, "cuda device index");
        cmdParser.AddOptions("preset", preset, "perf preset for OF algo [ options : slow, medium, fast ]");
        cmdParser.AddOptions("visualFlow", visualFlow, "save flow vectors as RGB image");
        cmdParser.AddOptions("inputBufferType", inputBufferType, "input cuda buffer type [options : cudaArray cudaDevicePtr]");
        cmdParser.AddOptions("outputBufferType", outputBufferType, "output cuda buffer type [options : cudaArray cudaDevicePtr]");
        cmdParser.AddOptions("useCudaStream", useCudaStream, "Use cuda stream for input and output processing");
        cmdParser.AddOptions("gridSize", gridSize, "Block size per motion vector");
        cmdParser.AddOptions("hintGridSize", hintGridSize, "Block size per hint motion vector");
        cmdParser.AddOptions("measureFPS", measureFPS, "Measure performance(frames per second). When this option is set it is not mandatory to specify --output option,"
                             " output is generated only if --output option is specified");
        NVOF_ARGS_PARSE(cmdParser, argc, (const char**)argv);

        if (inputFileName.empty())
        {
            std::cout << "Input file not specified" << std::endl;
            std::cout << cmdParser.help(argv[0]) << std::endl;
            return 1;
        }

        if (!measureFPS && outputFileBaseName.empty())
        {
            std::cout << "Output file not specified" << std::endl;
            std::cout << cmdParser.help(argv[0]) << std::endl;
            return 1;
        }

        auto search = presetMap.find(preset);
        if (search == presetMap.end())
        {
            std::cout << "Invalid preset level : " << preset << std::endl;
            std::cout << cmdParser.help(argv[0]) << std::endl;
            return 1;
        }
        perfPreset = search->second;

        auto inputbufferTypeIt = bufferTypeMap.find(inputBufferType);
        if (inputbufferTypeIt == bufferTypeMap.end())
        {
            std::cout << "Invalid input buffer type : " << inputBufferType << std::endl;
            std::cout << cmdParser.help(argv[0]) << std::endl;
            return 1;
        }
        inputBufferTypeEnum = inputbufferTypeIt->second;

        auto outputBufferTypeIt = bufferTypeMap.find(outputBufferType);
        if (outputBufferTypeIt == bufferTypeMap.end())
        {
            std::cout << "Invalid output buffer type : " << outputBufferType << std::endl;
            std::cout << cmdParser.help(argv[0]) << std::endl;
            return 1;
        }
        outputBufferTypeEnum = outputBufferTypeIt->second;

        int nGpu = 0;
        CUDA_DRVAPI_CALL(cuInit(0));
        CUDA_DRVAPI_CALL(cuDeviceGetCount(&nGpu));
        if (gpuId < 0 || gpuId >= nGpu)
        {
            std::cout << "GPU ordinal out of range. Should be with in [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
            return 1;
        }
        CUstream   inputStream = nullptr;
        CUstream   outputStream = nullptr;
        CUdevice cuDevice = 0;
        CUDA_DRVAPI_CALL(cuDeviceGet(&cuDevice, gpuId));
        char szDeviceName[80];
        CUDA_DRVAPI_CALL(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        std::cout << "GPU in use: " << szDeviceName << std::endl;
        CUcontext cuContext = nullptr;
        CUDA_DRVAPI_CALL(cuCtxCreate(&cuContext, 0, cuDevice));


        if (useCudaStream)
        {
            CUDA_DRVAPI_CALL(cuStreamCreate(&inputStream, CU_STREAM_DEFAULT));
            CUDA_DRVAPI_CALL(cuStreamCreate(&outputStream, CU_STREAM_DEFAULT));
        }

        EstimateFlow(cuContext, 
            inputStream,
            outputStream,
            inputFileName,
            outputFileBaseName,
            hintFileName,
            ROIFileName,
            inputBufferTypeEnum,
            outputBufferTypeEnum,
            perfPreset,
            gridSize,
            hintGridSize,
            !!visualFlow,
            !!measureFPS);

        if (useCudaStream)
        {
            CUDA_DRVAPI_CALL(cuStreamDestroy(outputStream));
            outputStream = nullptr;
            CUDA_DRVAPI_CALL(cuStreamDestroy(inputStream));
            inputStream = nullptr;
        }
        CUDA_DRVAPI_CALL(cuCtxDestroy(cuContext));
        cuContext = nullptr;

    }
    catch (const std::exception &ex)
    {
        if (outputStream)
        {
            CUDA_DRVAPI_CALL(cuStreamDestroy(outputStream));
        }

        if (inputStream)
        {
            CUDA_DRVAPI_CALL(cuStreamDestroy(inputStream));
        }

        if (cuContext)
        {
            CUDA_DRVAPI_CALL(cuCtxDestroy(cuContext));
        }

        std::cout << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
