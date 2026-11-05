#include "vision/tensor_view.hpp"

namespace so {

std::vector<TensorView> views_of(const dxrt::TensorPtrs& tensors) {
    std::vector<TensorView> out;
    out.reserve(tensors.size());
    for (const auto& t : tensors)
        out.push_back(TensorView{t->shape(), static_cast<const float*>(t->data())});
    return out;
}

}  // namespace so
