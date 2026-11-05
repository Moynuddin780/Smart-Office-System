#pragma once

#include <dxrt/dxrt_cxx_api.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace so {

/**
 * A non-owning look at one float32 output tensor.
 *
 * The decoders take these rather than `dxrt::TensorPtrs` directly so that the
 * decode logic can be tested against captured fixtures on a machine with no
 * NPU attached — which is what makes the "match the ORT reference exactly"
 * tests possible at all.
 */
struct TensorView {
    std::vector<std::int64_t> shape;
    const float* data{nullptr};

    std::int64_t dim(std::size_t i) const { return shape[i]; }
    std::size_t ndim() const { return shape.size(); }
    std::size_t elems() const {
        std::size_t n = 1;
        for (auto d : shape) n *= static_cast<std::size_t>(d);
        return n;
    }
};

/// Adapt DX-RT's output tensors. The tensors must outlive the views.
std::vector<TensorView> views_of(const dxrt::TensorPtrs& tensors);

}  // namespace so
