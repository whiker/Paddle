/* Copyright (c) 2016 Baidu, Inc. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */


#pragma once

#include "hl_gpu.h"

#include "paddle/math/Matrix.h"
#include "paddle/math/Vector.h"
#include "paddle/utils/Locks.h"
#include "paddle/utils/Util.h"
#include "paddle/parameter/Parameter.h"

namespace paddle {

// vector of user defined pointers
typedef std::shared_ptr<std::vector<void*>> UserDefinedVectorPtr;
typedef std::shared_ptr<std::vector<std::string>> SVectorPtr;

struct Argument {
  Argument()
      : in(nullptr),
        value(nullptr),
        ids(nullptr),
        grad(nullptr),
        strs(nullptr),
        frameHeight(0),
        frameWidth(0),
        sequenceStartPositions(nullptr),
        subSequenceStartPositions(nullptr),
        cpuSequenceDims(nullptr),
        udp(nullptr),
        deviceId(-1),
        allCount(0),
        valueCount(0),
        gradCount(0),
        dataId(0) {}
  Argument(const Argument& argument) {
    *this = argument;
    valueCount = 0;
    gradCount = 0;
    dataId = argument.dataId;
  }
  ~Argument() {}

  void operator=(const Argument& argument) {
    in = argument.in;
    value = argument.value;
    ids = argument.ids;
    grad = argument.grad;
    strs = argument.strs;
    sequenceStartPositions = argument.sequenceStartPositions;
    subSequenceStartPositions = argument.subSequenceStartPositions;
    cpuSequenceDims = argument.cpuSequenceDims;
    udp = argument.udp;
    deviceId = argument.deviceId;
    allCount = argument.allCount;
    frameHeight = argument.frameHeight;
    frameWidth = argument.frameWidth;
    dataId = argument.dataId;
  }

  MatrixPtr in;  // used if needed
  MatrixPtr value;
  IVectorPtr ids;  // a sequence of ids. Can be use for class id for costLayer
  MatrixPtr grad;  // If empty, gradient is not needed.
  SVectorPtr strs;

  // A dataBatch includes batchSize frames, one frame maybe not only vector
  size_t frameHeight;
  size_t frameWidth;

  // If NULL, each position is treated independently.
  // Otherwise, its size should be #NumberOfSequences + 1.
  // The first position is always 0 and
  // the last position should be equal to batchSize.
  ICpuGpuVectorPtr sequenceStartPositions;

  // If NULL, each sequence has no subsequence.
  // Otherwise, its size should be #NumberOfSubSequences + 1.
  // The first position is always 0 and
  // the last position should be equal to batchSize.
  ICpuGpuVectorPtr subSequenceStartPositions;

  // dimension of sequence, stored only in CPU
  IVectorPtr cpuSequenceDims;

  UserDefinedVectorPtr udp;  // user defined pointer

  int deviceId;            // the GPU device id which the argument in
  int allCount;            // the number of output layers using this argument
  mutable int valueCount;  // waiting this member when layer do forward
  mutable int gradCount;   // waiting this member when layer do backward
  mutable LockedCondition valueReadyCond;
  mutable LockedCondition gradReadyCond;

  int dataId;  // dataProvider id

  /* Increase the reference count of the argument. */
  void countIncrement() { allCount++; }

  int getAllCount() const { return allCount; }

  void waitValueReady() const {
    valueReadyCond.wait([this] { return (valueCount != 0); });

    std::lock_guard<std::mutex> guard(*valueReadyCond.mutex());
    valueCount--;
  }

  void notifyValueReady() const {
    valueReadyCond.notify_all([this] { valueCount = allCount; });
  }

  void waitGradReady() const {
    gradReadyCond.wait([this] { return (gradCount == allCount); });
    gradCount = 0;
  }

  void notifyGradReady() const {
    gradReadyCond.notify_all([this] { gradCount++; });
  }

  int64_t getBatchSize() const {
    if (value) return value->getHeight();
    if (ids) return ids->getSize();
    if (grad) return grad->getHeight();
    if (in) return in->getHeight();
    if (udp) return udp->size();
    if (strs) return strs->size();
    return 0;
  }
  size_t getFrameHeight() const { return frameHeight; }
  size_t getFrameWidth() const { return frameWidth; }
  void setFrameHeight(size_t h) { frameHeight = h; }
  void setFrameWidth(size_t w) { frameWidth = w; }

  int64_t getNumSequences() const {
    return sequenceStartPositions ? sequenceStartPositions->getSize() - 1
                                  : getBatchSize();
  }

  int64_t getNumSubSequences() const {
    return subSequenceStartPositions
               ? subSequenceStartPositions->getSize() - 1
               : getBatchSize();
  }

  bool hasSubseq() const { return subSequenceStartPositions != nullptr; }

  const int* getCpuStartPositions() const {
    return hasSubseq() ? subSequenceStartPositions->getData(false)
                       : sequenceStartPositions->getData(false);
  }

  static inline real sumCosts(const std::vector<Argument>& arguments) {
    real cost = 0;
    for (auto& arg : arguments) {
      if (arg.value) {
        SetDevice device(arg.deviceId);
        cost += arg.value->getSum();
      }
    }
    return cost;
  }

  /**
   * @brief (value, grad, sequenceStartPositions) of output are subset of
   *        input. Note that, output share the same memory of input.
   *
   * @param input[in]       input
   * @param offset[in]      offset of input.value
   * @param height[in]      height of output.value
   * @param width[in]       width of output.value
   * @param useGpu[in]
   * @param trans[in]       whether input.value is transform
   * @param seqFlag[in]     whether input has sequenceStartPositions
   * @param seqStart[in]    offset of input.sequenceStartPositions
   * @param seqSize[in]     lenght of output.sequenceStartPositions
   */
  void subArgFrom(const Argument& input, size_t offset, size_t height,
                  size_t width, bool useGpu, bool trans = false,
                  bool seqFlag = false, size_t seqStart = 0,
                  size_t seqSize = 0);
  /*
   * for sequence input:
   *   startSeq: the sequence id of start
   *   copySize: how many sequences need to copy
   *   return value: how many samples are copied
   * for non-sequence input:
   *   startSeq: the sample id of start
   *   copySize: how many samples need to copy
   *   return value: how many samples are copied
   */
  int32_t resizeAndCopyFrom(const Argument& src, int32_t startSeq,
                            int32_t copySize, bool useGpu = FLAGS_use_gpu,
                            hl_stream_t stream = HPPL_STREAM_DEFAULT);

  void resizeAndCopyFrom(const Argument& src, bool useGpu = FLAGS_use_gpu,
                         hl_stream_t stream = HPPL_STREAM_DEFAULT);

  /*
    @brief Concatenate several arguments into one and put the result into it.
    @param args : a vector of argument, each element of which is a frame in a
    batch of sequences.
    @param selectRows : select several row of args to concatenate
    @param seqStartPos : sequence start positions in the final Argument
    @param hl_stream_t : cuda stream
    @param passTyoe : type of task, training or testing
   */
  void concat(const std::vector<Argument>& args,
              const std::vector<int>& selectRows,
              const std::vector<int>& seqStartPos, bool useGpu,
              hl_stream_t stream, PassType passType);

  /*
    Concatenate several args into one and put the result into this.
   */
  void concat(const std::vector<Argument>& src, bool useGpu = FLAGS_use_gpu,
              hl_stream_t stream = HPPL_STREAM_DEFAULT,
              PassType passType = PASS_TEST);

  /*
   * split vector<Argument> to several vectors according to dataId
   */
  static void splitByDataId(const std::vector<Argument>& argus,
                            std::vector<std::vector<Argument>>* arguGroups);

  /*
   Get Sequence Length, startPositions and max Length according to input
   */
  void getSeqLengthAndStart(
      std::vector<std::tuple<int, int, int, int>>* seqLengthAndStart,
      int* maxSequenceLength) const;
  /*
   Check Whether sequenceStartPositions is subset of
   subSequenceStartPositions.
   */
  void checkSubset() const;

  /*
   sequence has sub-sequence degrades to a sequence.
   */
  void degradeSequence(const Argument& input, bool useGpu);
};

}  // namespace paddle
