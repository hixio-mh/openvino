// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_reorder_node.h"
#include <memory>
#include <string>
#include <algorithm>
#include <mkldnn_types.h>
#include <mkldnn_extension_utils.h>
#include "ie_parallel.hpp"
#include "utils/general_utils.h"
#include <cpu/x64/cpu_isa_traits.hpp>

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;

MKLDNNReorderNode::MKLDNNReorderNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &w_cache) :
        MKLDNNNode(op, eng, w_cache) {
    IE_THROW() << "Can't create reorder node from ngraph node";
}

MKLDNNReorderNode::MKLDNNReorderNode(const std::string& name, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &w_cache) :
        MKLDNNNode("Reorder", name, eng, w_cache) {
}
void MKLDNNReorderNode::getSupportedDescriptors() {
    if (getParentEdges().size() != 1)
        IE_THROW() << "Incorrect number of input edges for layer " << getName();
    if (getChildEdges().empty())
        IE_THROW() << "Incorrect number of output edges for layer " << getName();
}

void MKLDNNReorderNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    auto parent = getParentEdgeAt(0)->getParent();
    auto child = getChildEdgeAt(0)->getChild();

    NodeConfig config;
    config.dynBatchSupport = true;
    config.inConfs.resize(1);
    config.outConfs.resize(1);
    config.inConfs[0].inPlace = -1;
    config.inConfs[0].constant = false;
    config.outConfs[0].inPlace = -1;
    config.outConfs[0].constant = false;
    if (isOptimized) {
        config.inConfs[0].inPlace = 0;
        config.outConfs[0].inPlace = 0;
    }
    if (input && output) {
        config.inConfs[0].desc = input->clone();
        config.outConfs[0].desc = output->clone();
    } else if (parent->getSelectedPrimitiveDescriptor() != nullptr &&
               child->getSelectedPrimitiveDescriptor() != nullptr) {
        config.inConfs[0].desc = parent->getSelectedPrimitiveDescriptor()->getConfig().outConfs[0].desc->clone();
        config.outConfs[0].desc = child->getSelectedPrimitiveDescriptor()->getConfig().inConfs[0].desc->clone();
    } else {
        IE_THROW() << "Cannot initialize supported PDs for Reorder node with name `" << getName() << "`";
    }

    supportedPrimitiveDescriptors.emplace_back(config, impl_desc_type::reorder);
}

void MKLDNNReorderNode::createPrimitive() {
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
        IE_THROW() << "Destination memory didn't allocate.";
    if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
        IE_THROW() << "Input memory didn't allocate.";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        IE_THROW() << "Preferable primitive descriptor is not set.";

    auto inDims = getParentEdgeAt(0)->getShape().getStaticDims();

    if (!isOptimized) {
        if (MKLDNNPlugin::one_of(inDims.size(), 4, 5) &&
                inDims[1] <= 64 &&
                inDims[1] >= 16 &&
                (getParentEdgeAt(0)->getMemory().GetElementsCount() / inDims[1]) >= 128 &&
                getParentEdgeAt(0)->getMemory().GetDesc().hasLayoutType(LayoutType::nspc) &&
                getChildEdgeAt(0)->getMemory().GetDesc().hasLayoutType(LayoutType::ncsp) &&
                getParentEdgeAt(0)->getMemory().GetDesc().getPrecision() == Precision::FP32 &&
                getChildEdgeAt(0)->getMemory().GetDesc().getPrecision() == Precision::FP32) {
            // oneDNN JIT reorder shows bad perf for nspc to ncsp reorder case so we fallback on simple c++ implementation
            canUseOptimizedNspc2Ncsp = true;
        } else if (!impl::cpu::x64::mayiuse(impl::cpu::x64::avx2) &&
                   MKLDNNPlugin::one_of(inDims.size(), 4, 5) &&
                   getParentEdgeAt(0)->getMemory().GetDesc().hasLayoutType(LayoutType::ncsp) &&
                   getChildEdgeAt(0)->getMemory().GetDesc().hasLayoutType(LayoutType::nspc) &&
                   getParentEdgeAt(0)->getMemory().GetDataType() == getChildEdgeAt(0)->getMemory().GetDataType() &&
                   MKLDNNExtensionUtils::sizeOfDataType(getParentEdgeAt(0)->getMemory().GetDataType()) == 1) {
            // oneDNN doesn't provide JIT reorder impl for non-avx2 targets so we fallback on simple c++ implementation which shows better perf
            canUseOptimizedNcsp2Nspc = true;
        } else {
            createReorderPrimitive(srcMemPtr->GetDescriptor(), srcMemPtr->GetPrimitive().get_data_handle(),
                                   dstMemPtr->GetDescriptor(), dstMemPtr->GetPrimitive().get_data_handle());
        }
    }
}

void MKLDNNReorderNode::createReorderPrimitive(const mkldnn::memory::desc &srcDesc, void* srcPtr, const mkldnn::memory::desc &dstDesc, void* dstPtr) {
    src_blocked = std::make_shared<MKLDNNMemory>(getEngine());
    src_blocked->Create(MKLDNNMemoryDesc(srcDesc), srcPtr, false);

    dst_blocked = std::make_shared<MKLDNNMemory>(getEngine());
    dst_blocked->Create(MKLDNNMemoryDesc(dstDesc), dstPtr, false);

    mkldnn::primitive_attr attr;
    auto createReorder = [&]() -> bool {
        // No autoblocking. Reorder can be applied as is
        reorder::primitive_desc pd = mkldnn::reorder::primitive_desc(src_blocked->GetPrimitive(), dst_blocked->GetPrimitive(), attr, true);

        if (!pd)
            return false;

        auto info = pd.impl_info_str();
        supportedPrimitiveDescriptors[0].setImplementationType(parse_impl_name(info));

        prim.reset(new mkldnn::reorder(pd));
        return true;
    };

    auto success = createReorder();
    if (!success) {
        // TODO: We should keep shape consistency for const and expected shape for node.
        //       If it requires reshape operation it should explicitly injected into graph.
        //
        // There is a limitation for IE representing of weights for grouped convolutions. IE doesn't
        // split group dimension in separate shape dimension. IE use OIHW, but mkldnn expect GOIHW.
        // So we will perform implicit reshape to dst shape.
        //
        // MKLDNN doesn't support direct reorders for tensors of different rank. The code below tries to
        // perform such conversion if the source tensor can be reshaped to the destination rank. This is
        // useful in situations when rank in IR does not much rank that is required by the oneDNN primitive,
        // but the input tensor can be reshaped (e.g. weights for grouped convolutions, biases etc.)
        if (src_blocked->GetDesc().hasLayoutType(LayoutType::ncsp) &&
            src_blocked->GetDims().size() != dst_blocked->GetDims().size()) {
            const auto newDims = dst_blocked->GetDims();
            const auto newFormat = MKLDNNMemory::GetPlainFormatByRank(newDims.size());

            auto newDesc = mkldnn::memory::desc(newDims, src_blocked->GetDataType(), newFormat);
            src_blocked->Create(MKLDNNMemoryDesc(newDesc), srcPtr, false);

            success = createReorder();
        }
    }

    if (!success) {
        IE_THROW() << "Cannot create reorder primitive: unsupported reorder case";
    }

    auto src = getParentEdgesAtPort(0)[0]->getMemoryPtr()->GetPrimitive();
    auto dst = getChildEdgesAtPort(0)[0]->getMemoryPtr()->GetPrimitive();
    primArgs = {{DNNL_ARG_SRC, src}, {DNNL_ARG_DST, dst}};
}

const std::vector<impl_desc_type>& MKLDNNReorderNode::getPrimitivesPriority() {
    implPriorities = {impl_desc_type::reorder};
    return implPriorities;
}

bool MKLDNNReorderNode::created() const {
    return getType() == Reorder;
}

void MKLDNNReorderNode::optimizedNcsp2Nspc() {
    auto parentEdge = getParentEdgeAt(0);
    auto childEdge = getChildEdgeAt(0);

    auto inDims = parentEdge->getShape().getStaticDims();
    const size_t ndims = inDims.size();
    const size_t DIM0 = inDims[0];
    const size_t DIM1 = inDims[1];
    const size_t DIM2 = ndims == 5 ? inDims[ndims - 3] : 1;
    const size_t DIM3 = inDims[ndims - 2];
    const size_t DIM4 = inDims[ndims - 1];

    auto src_data = reinterpret_cast<const uint8_t *>(parentEdge->getMemoryPtr()->GetPtr());
    auto dst_data = reinterpret_cast<uint8_t *>(childEdge->getMemoryPtr()->GetPtr());

    const size_t stride0 = DIM1 * DIM2 * DIM3 * DIM4;
    const size_t stride1 = DIM2 * DIM3 * DIM4;
    const size_t stride2 = DIM2 * DIM3;

    parallel_for3d(DIM0, DIM1, stride2, [&](size_t dim0, size_t dim1, size_t j) {
        size_t src_off = dim0 * stride0 + j * DIM4 + dim1 * stride1;
        size_t dst_off = dim0 * stride0 + j * DIM4 * DIM1 + dim1;

        for (size_t dim4 = 0; dim4 < DIM4; ++dim4) {
            dst_data[dst_off] = src_data[src_off];
            src_off++;
            dst_off += DIM1;
        }
    });
}

void MKLDNNReorderNode::optimizedNspc2Ncsp() {
    auto parentEdge = getParentEdgeAt(0);
    auto childEdge = getChildEdgeAt(0);

    auto inDims = parentEdge->getShape().getStaticDims();
    const size_t ndims = inDims.size();
    const size_t DIM0 = inDims[0];
    const size_t DIM1 = inDims[1];
    const size_t DIM2 = ndims == 5 ? inDims[ndims - 3] : 1;
    const size_t DIM3 = inDims[ndims - 2];
    const size_t DIM4 = inDims[ndims - 1];

    auto src_data = reinterpret_cast<const float *>(parentEdge->getMemoryPtr()->GetPtr());
    auto dst_data = reinterpret_cast<float *>(childEdge->getMemoryPtr()->GetPtr());

    const size_t stride1 = DIM2 * DIM3 * DIM4;
    const size_t stride0 = stride1 * DIM1;
    parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
        auto src_off = b*stride0 + j*DIM1;
        auto dst_off = b*stride0 + j;
        for (size_t dim1 = 0; dim1 < DIM1; ++dim1) {
            dst_data[dst_off] = src_data[src_off];
            src_off++;
            dst_off += stride1;
        }
    });
}

void MKLDNNReorderNode::execute(mkldnn::stream strm) {
    if (isOptimized)
        return;

    if (canUseOptimizedNspc2Ncsp) {
        optimizedNspc2Ncsp();
    } else if (canUseOptimizedNcsp2Nspc) {
        optimizedNcsp2Nspc();
    } else {
        src_blocked->GetPrimitivePtr()->set_data_handle(getParentEdgeAt(0)->getMemory().GetPrimitive().get_data_handle());
        dst_blocked->GetPrimitivePtr()->set_data_handle(getChildEdgeAt(0)->getMemory().GetPrimitive().get_data_handle());

        MKLDNNNode::execute(strm);
    }
}

void MKLDNNReorderNode::setDynamicBatchLim(int lim) {
    dynBatchLim = lim;
    if (prim) {
        auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
        auto &srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
        memory::desc src_d = srcMemPtr->GetDescriptor();
        memory::desc dst_d = dstMemPtr->GetDescriptor();
        void *src_data_hdl = srcMemPtr->GetPrimitive().get_data_handle();
        void *dst_data_hdl = dstMemPtr->GetPrimitive().get_data_handle();

        src_d.data.dims[0] = batchToProcess();
        src_d.data.padded_dims[0] = batchToProcess();

        dst_d.data.dims[0] = batchToProcess();
        dst_d.data.padded_dims[0] = batchToProcess();

        createReorderPrimitive(src_d, src_data_hdl, dst_d, dst_data_hdl);
    }
}

std::string MKLDNNReorderNode::getReorderArgs(const MemoryDesc &parentDesc, const MemoryDesc &childDesc) {
    std::string inArgs, outArgs;
    if (parentDesc.getPrecision() != childDesc.getPrecision()) {
        inArgs += (inArgs.empty() ? "" : "_") + std::string(parentDesc.getPrecision().name());
        outArgs += (outArgs.empty() ? "" : "_") + std::string(childDesc.getPrecision().name());
    }
    auto formatSrc = parentDesc.serializeFormat();
    auto formatDst = childDesc.serializeFormat();
    if (formatSrc != formatDst || one_of(std::string("undef"), formatSrc, formatDst)) {
        inArgs += (inArgs.empty() ? "" : "_") + formatSrc;
        outArgs += (outArgs.empty() ? "" : "_") + formatDst;
    }
    return inArgs + "_" + outArgs;
}

REG_MKLDNN_PRIM_FOR(MKLDNNReorderNode, Reorder);
