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

#include "tensorrt_llm/kernels/decodingCommon.h"
#include "tensorrt_llm/kernels/speculativeDecoding/common.h"
#include "tensorrt_llm/runtime/common.h"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <curand_kernel.h>

namespace tensorrt_llm::kernels::speculative_decoding
{

//! \brief Sets pointers to logits in logitsPtrs according to the draftDecodingTokens.
//! \param logitsPtrs [batchSize][vocabSizePadded]
//! \param decodingTokens [batchSize], on GPU. draftDecodingTokens + 1.
//! \param logits [numTokens, vocabSizePadded], on GPU. Continuous logits in memory.
//! \param draftDecodingTokens [batchSize], on GPU. 0 for context requests, and actual draft len for gen requests
//! \param batchSize batch size. Only batch size <= 512 is supported at the moment
//! \param maxDecodingTokens maximum number of decoding tokens per step per request
//! \param vocabSizePadded vocab size of the logits
//! \param stream cuda stream
template <typename T>
void invokeAssembleTargetLogitsOffsets(T const** logitsPtrs, runtime::SizeType32* decodingTokens, T const* logits,
    runtime::SizeType32 const* draftDecodingTokens, runtime::SizeType32 batchSize,
    runtime::SizeType32 maxDecodingTokens, runtime::SizeType32 vocabSizePadded, cudaStream_t stream);

//! FIXME: We may get rid of this kernel in future optimization
//! \brief Set the logitsPtrs[numInputLogits][1, vocabSizePadded] from logits [numInputLogits * vocabSizePadded]
//! and outputIdsPtrs[numInputLogits][maxDecodingDraftTokens] from outputIds[numInputLogits * maxDecodingDraftTokens]
//! Can be merged into other kernels
//! \param logitsPtrs [numInputLogits][1, vocabSizePadded], on GPU. The logits pointer array that will be used in topK
//! sampling.
//! \param logits [numInputLogits * vocabSizePadded], on GPU. Flatten logits, generated by the EagleNet.
//! \param outputIdsPtrs [numInputLogits][maxDecodingDraftTokens], on GPU. The output buffer of the topK sampling.
//! \param outputIds [numInputLogits * maxDecodingDraftTokens], on GPU. The flatten output buffer.
//! \param numInputLogits SizeType32. Number of logits from all the requests.
//! \param maxDecodingDraftTokens maximum number of decoding draft tokens per step per request
//! \param vocabSizePadded vocab size of the logits
//! \param stream cuda stream
template <typename T>
void invokeAssembleDraftLogitsOffsets(T const** logitsPtrs, T const* logits, runtime::TokenIdType** outputIdsPtrs,
    runtime::TokenIdType* outputIds, runtime::SizeType32 numInputLogits, runtime::SizeType32 maxDecodingDraftTokens,
    runtime::SizeType32 vocabSizePadded, cudaStream_t stream);

//! \brief Extract the TopKs from tree of specific level (layerId).
//! \param paths [batchSize, maxDecodingTokens, maxPathLen], on GPU. Indices of the draft sequences.
//! \param topKs [numInputLogits], on GPU. The topK value for each input logits.
//! \param topKOffset [batchSize], on GPU. The topK start offset for each request. Will be used to slice the output
//! draft tokens.
//! \param numSuccessorsForEachNode [batchSize][maxDecodingTokens], on GPU. Record the number of
//! successors of each node from the corresponding tree for each requests.
//! \param layerId SizeType32. The layerId of the eagle net. Will be used to traverse a specific level of
//! the tree.
//! \param batchSize SizeType32. Batch size.
//! \param numInputLogits SizeType32. Number of logits from all the requests.
//! \param maxDecodingTokens maximum number of decoding tokens per step per request.
//! \param maxPathLen maximum path len of the draft sequence.
//! \param stream cuda stream.
void invokeExtractTopKsFromPath(runtime::SizeType32 const* paths, runtime::SizeType32* topKs,
    runtime::SizeType32* topKOffset, runtime::SizeType32* numSuccessorsForEachNode, runtime::SizeType32 layerId,
    runtime::SizeType32 batchSize, runtime::SizeType32 numInputLogits, runtime::SizeType32 maxDecodingTokens,
    runtime::SizeType32 maxPathLen, cudaStream_t stream);

//! \brief Copy the output draft token from input buffer (generated from previous EagleNets)
//! and new draft tokens generated by this layers to the output buffer of this plugin
//! also update the draft length.
//! \param tmpOutputIdsPtrs [numInputLogits][maxDecodingDraftTokens], on GPU. The temporary output buffer of the topK
//! sampling.
//! \param topKs [numInputLogits], on GPU. The topK value for each input logits.
//! \param topKOffset [batchSize], on GPU. The topK start offset for each request. Will be used to slice the output
//! draft tokens.
//! \param pluginInputDraftIdsPtrs [batchSize * maxDecodingDraftTokens], on GPU. The plugin's input buffer,
//! which contains draft tokens generated by previous EagleNets.
//! \param pluginInputDraftLens [batchSize], on GPU. The
//! plugin's input buffer, which contains the draft length from previous EagleNets.
//! \param pluginOutputDraftIdsPtrs [batchSize * maxDecodingDraftTokens], on GPU. The plugin's output buffer,
//! which will contains all the draft tokens generated by this and previous EagleNets.
//! \param pluginOutputDraftLens [batchSize], on GPU. The plugin's input buffer,
//! which contains the draft length for the draft tokens.
//! \param layerId SizeType32. The layerId of the EagleNet. Will
//! be used to traverse a specific level of the tree.
//! \param batchSize SizeType32. Batch size.
//! \param numInputLogits SizeType32. Number of logits from all the requests.
//! \param maxDecodingDraftTokens maximum number of decoding draft tokens per step per request.
//! \param stream cuda stream.
void invokeCopyOutputTokensIds(runtime::TokenIdType** tmpOutputIdsPtrs, runtime::SizeType32 const* topKs,
    runtime::SizeType32 const* topKOffset, runtime::TokenIdType const* pluginInputDraftIdsPtrs,
    runtime::SizeType32 const* pluginInputDraftLens, runtime::TokenIdType* pluginOutputDraftIdsPtrs,
    runtime::SizeType32* pluginOutputDraftLens, runtime::SizeType32 layerId, runtime::SizeType32 batchSize,
    runtime::SizeType32 numInputLogits, runtime::SizeType32 maxDecodingDraftTokens, cudaStream_t stream);

//! \brief Prepares data for ctx stage EagleNet (EagleNet0).
//! EagleNet0 is always chunked context attn,
//! where we process either context tokens of the ctx requests or
//! newly accepted tokens from base model and append them to EagleNet KV cache.
//! For input/output examples visit test/model/eagle/test_prepare_drafter_inputs_plugin.py (ctx Eagle Net examples)
//! \param eagleNetSequenceLengths output buffer [batchSize]
//! Sequence length for the EagleNet0.
//! \param eagleNetContextLengths output buffer [batchSize]
//! Context lengths for the EagleNet0.
//! \param outputIds output buffer [numOutputTokens], flattened selected tokens ids without padding.
//! \param positionIds output buffer [numOutputTokens], flattened selected pos ids without padding
//! \param hiddenStatesIndices output buffer [numOutputTokens],
//! indices of the hidden states for selected tokens for the next EagleNet iteration.
//! E.g. With 3 requests where the first two are context requests with lengths 5 and 3 respectively and the 3rd
//! is gen request with draftDecodingTokens=8 and acceptedLength=3 and the best path is [0, 2, 5].
//! hiddenStatesIndices equals to [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13].
//! \param lastTokenIndices output buffer [numLastTokenIndices],
//! \param numOutputTokens output buffer [1], single number equals to the total
//! number of select tokens summed over all batches.
//! \param numLastTokenIndices output buffer [1], single number equals to the total
//! number of logits predicted by the next EagleNet iteration tokens summed over all batches.
//! For EagleNet0 it is equal to the batchSize.
//! \param hiddenSizeBatchLevelStarts output buffer [batchSize * maxDraftPathLen + 1]
//! Exclusive sum of the hidden states produced per batch per level.
//! For EagleNet0 it is just cum sum of 1s for batchSize.
//! \param inputIds input buffer [numTokens], input ids (inputs of the Base model)
//! \param baseNetSequenceLengths input buffer [batchSize] sequence lengths (inputs of the Base model).
//! \param baseNetContextLengths input buffer [batchSize], context lengths (inputs of the Base model).
//! \param acceptedTokens input buffer [batchSize, maxPathLen], ids of the accepted tokens.
//! \param acceptedLens input buffer [batchSize], on GPU. Number of accepted tokens.
//! \param prevDraftLens input buffer [batchSize], on GPU. Number of draft tokens (inputs of the Base model).
//! 0 for ctx requests and actual draft len for gen requests.
//! \param prevPaths input buffer [batchSize, maxDecodingTokens, maxPathLen], on GPU.
//! Previous paths (inputs of the Base model).
//! \param bestPathIds input buffer [batchSize], on GPU. Indices of the accepted path in prevPaths
//! \param batchSize batch size
//! \param maxPathLen Max number of accepted tokens per step
//! \param maxDecodingTokens Max number of draft tokens + 1
//! \param stream cuda stream.
void invokePrepareCtxEagleNetInputs(runtime::SizeType32* eagleNetSequenceLengths,
    runtime::SizeType32* eagleNetContextLengths, runtime::TokenIdType* outputIds, runtime::SizeType32* positionIds,
    runtime::SizeType32* hiddenStatesIndices, runtime::SizeType32* lastTokenIndices,
    runtime::SizeType32* numOutputTokens, runtime::SizeType32* numLastTokenIndices,
    runtime::SizeType32* hiddenSizeBatchLevelStarts, runtime::TokenIdType const* inputIds,
    runtime::SizeType32 const* baseNetSequenceLengths, runtime::SizeType32 const* baseNetContextLengths,
    runtime::TokenIdType const* acceptedTokens, runtime::SizeType32 const* acceptedLens,
    runtime::SizeType32 const* prevDraftLens, runtime::SizeType32 const* prevPaths,
    runtime::SizeType32 const* bestPathIds, runtime::SizeType32 batchSize, runtime::SizeType32 maxPathLen,
    runtime::SizeType32 maxDecodingTokens, cudaStream_t stream);

struct PrepareGenEagleNetInputsParams
{
    //! output buffer [batchSize]
    //! Sequence length for the next EagleNet iteration.
    //! Equals to EagleNet0 seqLen + specDecodingGenLengths
    runtime::SizeType32* nextSequenceLengths{nullptr};
    //! output buffer [batchSize]
    //! Context length for the next EagleNet iteration.
    //! Equals to prevContextLengths
    runtime::SizeType32* nextContextLengths{nullptr};
    //! output buffer [numOutputTokens]
    //! Selected tokens ids.
    runtime::TokenIdType* outputIds{nullptr};
    //! output buffer [numOutputTokens]
    //! Position ids of the selected tokens.
    runtime::SizeType32* positionIds{nullptr};
    //! output buffer [batchSize]
    //! Number of the draft tokens per requert.
    runtime::SizeType32* specDecodingGenLengths{nullptr};
    //! output buffer [batchSize, maxDecodingTokens]
    //! Positions offsets (relative depth in the tree) of the selected tokens.
    runtime::SizeType32* specDecodingPositionOffsets{nullptr};
    //! output buffer [batchSize, maxDecodingTokens, ceil(maxDecodingTokens / 32)]
    //! uint32 packed mask of the draft tokens per request.
    runtime::SizeType32* specDecodingPackedMasks{nullptr};
    //! output buffer [numOutputTokens]
    //! Indices of the hidden states for selected tokens for the next EagleNet iteration.
    runtime::SizeType32* hiddenStatesIndices{nullptr};
    //! output buffer [numLastTokenIndices]
    //! Indices of the hidden states where to sample logits from after the next EagleNet iteration.
    runtime::SizeType32* lastTokenIndices{nullptr};
    //! output buffer [1]
    //! Single number equals to the total number of select tokens summed over all batches.
    runtime::SizeType32* numOutputTokens{nullptr};
    //! output buffer [1]
    //! Single number equals to the total number of logits to be predicted by the next EagleNet summed over all batches.
    runtime::SizeType32* numLastTokenIndices{nullptr};
    //! input buffer [(maxPathLen - 1) * batchSize + 1]
    //! Exclusive sum of the hidden states produced per batch per level.
    //! Same as inputHiddenSizeBatchStartsPerLevel, but also with data appended for cur level.
    runtime::SizeType32* outputHiddenSizeBatchStartsPerLevel{nullptr};

    // Workspace buffers
    //! [batchSize, maxDecodingTokens]
    //! Boolean mask to mark node as leaf or not.
    int8_t* isLeafMask{nullptr};
    //! [batchSize, maxDecodingDraftTokens]
    //! Indices of the draft tokens in the nextDraftIds selected at current level.
    runtime::SizeType32* selectedDraftIndices{nullptr};
    //! [batchSize, maxDecodingDraftTokens]
    //! Position offsets of the selected draft tokens.
    runtime::SizeType32* selectedDraftPosOffsets{nullptr};
    //! [batchSize]
    //! Number of selected tokens.
    runtime::SizeType32* numSelectedDraftIndices{nullptr};
    //! [batchSize, maxDecodingTokens, maxDecodingTokens]
    //! Boolean (not packed) mask of the selected draft tokens.
    bool* selectedMasks{nullptr};
    //! [batchSize + 1]
    runtime::SizeType32* cumSumGenerationLengths{nullptr};
    //! [1]
    runtime::SizeType32* maxGenerationLength{nullptr};
    //! [batchSize, maxDecodingTokens]
    runtime::SizeType32* nonLeavesInLevelOffsets{nullptr};
    //! [batchSize, maxDecodingTokens]
    runtime::SizeType32* parentNonLeafInLevelOffset{nullptr};

    //! input buffer [batchSize, maxDecodingDraftTokens]
    //! Drafted draft tokens. All tokens for the next Base model itertion are in the same buffer.
    runtime::TokenIdType const* nextDraftIds{nullptr};
    //! input buffer [batchSize]
    //! Sequence lengths after the ctx EagleNet0.
    runtime::SizeType32 const* eagleNet0SequenceLengths{nullptr};
    //! input buffer [batchSize]
    //! Context lengths after the ctx EagleNet0.
    runtime::SizeType32 const* prevContextLengths{nullptr};
    //! input buffer [batchSize, maxDecodingTokens, maxPathLen]
    //! Draft paths for the next iteration of the Base model. We use these paths to assemble output ids.
    runtime::SizeType32 const* nextPaths{nullptr};
    //! input buffer [(maxPathLen - 1) * batchSize + 1]
    //! Exclusive sum of the hidden states sizes per batch per layer.
    //! E.g. with BS=2, r0 and r1 have 1 hidden state at level 0 (golden token).
    //! r0 has 2 hidden states and r1 has 3 hidden states at level 1.
    //! Thus, hidden states are placed in memory as
    //! [h_0_0_0, h_0_0_1, h_0_1_0, h_1_1_0, h_0_1_1, h_1_1_1, h_2_1_1], where
    //! h_i_j_k means ith hidden state of request k at level j.
    // hiddenSizeBatchStartsPerLevel equals to [0, 1, 2, 4]
    runtime::SizeType32 const* inputHiddenSizeBatchStartsPerLevel{nullptr};

    //! Tree level index. Same as gen iter of the EagleNet. For gen EagleNet it is >= 1 and < maxPathLen - 1
    runtime::SizeType32 levelIdx{0};
    //! Batch size
    runtime::SizeType32 batchSize{0};
    //! Max number of accepted tokens per step
    runtime::SizeType32 maxPathLen{0};
    //! Max number of draft tokens + 1
    runtime::SizeType32 maxDecodingTokens{0};
    cudaStream_t stream;

    void checkParams()
    {
        TLLM_CHECK(nextSequenceLengths);
        TLLM_CHECK(nextContextLengths);
        TLLM_CHECK(outputIds);
        TLLM_CHECK(positionIds);
        TLLM_CHECK(specDecodingGenLengths);
        TLLM_CHECK(specDecodingPositionOffsets);
        TLLM_CHECK(specDecodingPackedMasks);
        TLLM_CHECK(hiddenStatesIndices);
        TLLM_CHECK(lastTokenIndices);
        TLLM_CHECK(numOutputTokens);
        TLLM_CHECK(numLastTokenIndices);
        TLLM_CHECK(outputHiddenSizeBatchStartsPerLevel);

        TLLM_CHECK(isLeafMask);
        TLLM_CHECK(selectedDraftIndices);
        TLLM_CHECK(selectedDraftPosOffsets);
        TLLM_CHECK(numSelectedDraftIndices);
        TLLM_CHECK(selectedMasks);
        TLLM_CHECK(cumSumGenerationLengths);
        TLLM_CHECK(maxGenerationLength);
        TLLM_CHECK(nonLeavesInLevelOffsets);
        TLLM_CHECK(parentNonLeafInLevelOffset);

        TLLM_CHECK(nextDraftIds);
        TLLM_CHECK(eagleNet0SequenceLengths);
        TLLM_CHECK(prevContextLengths);
        TLLM_CHECK(nextPaths);
        TLLM_CHECK(inputHiddenSizeBatchStartsPerLevel);

        TLLM_CHECK(batchSize > 0);
        TLLM_CHECK(maxPathLen > 0);
        TLLM_CHECK(maxDecodingTokens > 0);
        TLLM_CHECK(0 < levelIdx && levelIdx < maxPathLen - 1);
    }
};

//! \brief Prepares inputs for the gen stage EagleNet itearion (layerIdx > 0).
//! For input/output examples visit test/model/eagle/test_prepare_drafter_inputs_plugin.py (gen Eagle Net examples)
void invokePrepareGenEagleNetInputs(PrepareGenEagleNetInputsParams const& params);

struct PackEagleParams
{
    runtime::SizeType32 batchSize{0};
    runtime::SizeType32 maxNumPaths{0};
    runtime::SizeType32 maxDecodingTokens{0};
    runtime::SizeType32 maxPathLength{0};
    runtime::SizeType32 numContextRequests{0};
    runtime::SizeType32 numGenerationRequests{0};

    //! inputs
    //! [batchSize]
    runtime::SizeType32 const* batchSlots{nullptr};

    //! [maxBatchSize]
    float const* inputTemperatures{nullptr};
    //! [maxBatchSize]
    float const* inputRandomDataSample{nullptr};
    //! [maxBatchSize]
    float const* inputRandomDataValidation{nullptr};
    //! [maxBatchSize, maxDecodingDraftTokens]
    runtime::TokenIdType const* inputNextDraftTokens{nullptr};
    //! [maxBatchSize]
    runtime::SizeType32 const* inputNextDraftLens{nullptr};
    //! [maxBatchSize, maxDecodingTokens, maxPathLen]
    runtime::SizeType32 const* inputNextDraftPaths{nullptr};
    //! [maxBatchSize]
    runtime::SizeType32 const* inputSpecDecodingGenerationLengths{nullptr};
    //! [maxBatchSize]
    runtime::SizeType32 const* inputSpecDecodingPositionOffsets{nullptr};
    //! [maxBatchSize, maxDecodingTokens, ceil(maxDecodingTokens / 32)]
    int32_t const* inputSpecDecodingPackedMasks{nullptr};

    //! outputs
    //! [batchSize]
    float* outputTemperatures{nullptr};
    //! [batchSize]
    float* outputRandomDataSample{nullptr};
    //! [batchSize]
    float* outputRandomDataValidation{nullptr};
    //! [batchSize, maxDecodingDraftTokens]
    runtime::TokenIdType* outputNextDraftTokens{nullptr};
    //! [batchSize]
    runtime::SizeType32* outputNextDraftLens{nullptr};
    //! [batchSize, maxDecodingTokens, maxPathLen]
    runtime::SizeType32* outputNextDraftPaths{nullptr};
    //! [batchSize]
    runtime::SizeType32* outputSpecDecodingGenerationLengths{nullptr};
    //! [batchSize]
    runtime::SizeType32* outputSpecDecodingPositionOffsets{nullptr};
    //! [maxBatchSize, maxDecodingTokens, ceil(maxDecodingTokens / 32)]
    int32_t* outputSpecDecodingPackedMasks{nullptr};

    // workspace
    //! [1]
    runtime::SizeType32* maxGenerationLength{nullptr};
    //! [batchSize + 1]
    runtime::SizeType32* cumSumGenerationLengths{nullptr};

    void checkParams()
    {
        TLLM_CHECK(batchSlots);

        TLLM_CHECK(inputTemperatures);
        TLLM_CHECK(inputRandomDataSample);
        TLLM_CHECK(inputRandomDataValidation);
        TLLM_CHECK(inputNextDraftTokens);
        TLLM_CHECK(inputNextDraftLens);
        TLLM_CHECK(inputNextDraftPaths);
        TLLM_CHECK(inputSpecDecodingGenerationLengths);
        TLLM_CHECK(inputSpecDecodingPositionOffsets);
        TLLM_CHECK(inputSpecDecodingPackedMasks);

        TLLM_CHECK(outputTemperatures);
        TLLM_CHECK(outputRandomDataSample);
        TLLM_CHECK(outputRandomDataValidation);
        TLLM_CHECK(outputNextDraftTokens);
        TLLM_CHECK(outputNextDraftLens);
        TLLM_CHECK(outputNextDraftPaths);
        TLLM_CHECK(outputSpecDecodingGenerationLengths);
        TLLM_CHECK(outputSpecDecodingPositionOffsets);
        TLLM_CHECK(outputSpecDecodingPackedMasks);

        TLLM_CHECK(maxGenerationLength);
        TLLM_CHECK(cumSumGenerationLengths);

        TLLM_CHECK(batchSize > 0);
        TLLM_CHECK(batchSize == numContextRequests + numGenerationRequests);
        TLLM_CHECK(maxDecodingTokens > 0);
        TLLM_CHECK(maxPathLength > 0);
        TLLM_CHECK(maxNumPaths > 0);
    }
};

//! \brief packs outputSpecDecodingGenerationLengths from batch slots positions to continuous memory.
void invokePackEagleGenerationLengths(PackEagleParams const& params, cudaStream_t stream);
//! \brief packs the rest of the output tensors from batch slots positions to continuous memory.
void invokePackEagle(PackEagleParams const& params, cudaStream_t stream);

} // namespace tensorrt_llm::kernels::speculative_decoding
