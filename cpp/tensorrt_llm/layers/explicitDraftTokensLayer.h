/*
 * Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
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

#pragma once

#include "tensorrt_llm/layers/baseLayer.h"
#include "tensorrt_llm/layers/decodingParams.h"
#include "tensorrt_llm/runtime/common.h"

#include <curand_kernel.h>

namespace tensorrt_llm::layers
{

//! \brief Decoding layer for speculative decoding technique, when all tokens are generated, decoded and accepted in the
//! engine.
template <typename T>
class ExplicitDraftTokensLayer : public BaseLayer
{
public:
    using Base = BaseLayer;
    using PathsVec = std::vector<std::vector<std::vector<runtime::SizeType32>>>;

    ExplicitDraftTokensLayer(DecoderDomain const& decoderDomain, std::shared_ptr<runtime::BufferManager> bufferManager);

    void setup(runtime::SizeType32 batchSize, runtime::SizeType32 beamWidth, BufferConstPtr batchSlots,
        std::shared_ptr<BaseSetupParams> const& setupParams) override;

    void forwardAsync(std::shared_ptr<BaseDecodingOutputs> const& outputs,
        std::shared_ptr<BaseDecodingInputs> const& inputs) override;

    //! @returns workspace needed for this layer in bytes
    [[nodiscard]] size_t getWorkspaceSize() const noexcept override;

private:
    void allocateBuffer();

    void convertPackedMask(ExplicitDraftTokensOutputs const& outputs, ExplicitDraftTokensInputs const& inputs);

    void packAcceptedPaths(ExplicitDraftTokensOutputs const& outputs, ExplicitDraftTokensInputs const& inputs);

    template <typename Dtype>
    void fillContextBuffers(
        SizeType32 batchSize, BufferConstPtr batchSlots, ExplicitDraftTokensSetupParams const& setupParams);

    template <typename Dtype>
    void splitInputDataToBatchSlots(ExplicitDraftTokensOutputs const& outputs, ExplicitDraftTokensInputs const& inputs);

private:
    using Base::mDecoderDomain;

    SizeType32 mNumPaths;
    SizeType32 mMaxPathLength;

    size_t mScanWorkspaceSizeInBytes{0};
    size_t mReduceWorkspaceSizeInBytes{0};

    TensorPtr mRandomSeedsDevice;
    TensorPtr mCurandStatesDevice;
    BufferPtr mWorkspaceDevice;
    TensorPtr mGenerationLengthInclusiveSum;
    TensorPtr mMaxGenerationLength;
    TensorPtr mTemperatureDevice;
    TensorPtr mBestPathIndicesSlots;
    TensorPtr mLastDraftIndicesSlots;

    TensorPtr mTemperature;

    std::optional<nvinfer1::DataType> mDecoderDtype{std::nullopt};
};

} // namespace tensorrt_llm::layers
