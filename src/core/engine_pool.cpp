#include "core/engine_pool.hpp"

#include "core/log.hpp"

#include <stdexcept>
#include <string>

namespace so {

std::uint32_t EnginePool::bound_option_for(int core) {
    switch (core) {
        case 0: return static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_0);
        case 1: return static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_1);
        case 2: return static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_2);
        default:
            throw std::invalid_argument(
                "the DX-M1 has exactly three cores (0..2); got core index " +
                std::to_string(core));
    }
}

double EnginePool::core_utilization(int core, int device_id) {
    double pct = -1.0;
    if (dxrt_device_get_core_utilization(device_id, core, &pct) != DXRT_OK) return -1.0;
    return pct;
}

EnginePool::EnginePool(const std::string& model_path, int num_cores,
                       bool use_ort, int buffer_count)
    : model_path_(model_path) {
    if (num_cores < 1 || num_cores > 3)
        throw std::invalid_argument("num_cores must be 1..3; got " + std::to_string(num_cores));

    engines_.reserve(static_cast<std::size_t>(num_cores));
    options_.reserve(static_cast<std::size_t>(num_cores));

    for (int c = 0; c < num_cores; ++c) {
        auto opt = std::make_unique<dxrt::InferenceOption>();
        opt->devices = {0};
        opt->boundOption = bound_option_for(c);   // never NPU_ALL — see the header
        opt->useORT = use_ort;
        opt->bufferCount = buffer_count;

        // The InferenceEngine constructor takes the option by non-const
        // reference, so the option object has to outlive the call; it is kept
        // alongside the engine for the pool's lifetime.
        engines_.emplace_back(
            std::make_unique<dxrt::InferenceEngine>(model_path, *opt));
        options_.emplace_back(std::move(opt));

        SO_LOG_INFO("[engine] loaded %s (bound=NPU_%d, ort=%s, buffers=%d)",
                 model_path.c_str(), c, use_ort ? "yes" : "no", buffer_count);
    }

    // Geometry is identical on every core; read it from the first engine.
    auto& e0 = *engines_.front();
    {
        const dxrt::Tensors inputs = e0.GetInputs();
        if (inputs.empty())
            throw std::runtime_error(model_path + ": model reports no input tensor");
        input_shape_ = inputs.front().shape();
    }
    input_size_   = e0.GetInputSize();
    output_size_  = e0.GetOutputSize();
    output_names_ = e0.GetOutputTensorNames();
    {
        const dxrt::Tensors outputs = e0.GetOutputs();
        output_shapes_.reserve(outputs.size());
        for (const auto& t : outputs) output_shapes_.push_back(t.shape());
    }
}

void EnginePool::register_callback(Callback cb) {
    cb_ = std::move(cb);
    for (auto& e : engines_) {
        e->RegisterCallback([this](dxrt::TensorPtrs& out, void* arg) {
            if (cb_) cb_(out, arg);
            return 0;
        });
    }
}

int EnginePool::next_index() {
    return static_cast<int>(next_.fetch_add(1, std::memory_order_relaxed) % engines_.size());
}

dxrt::TensorPtrs EnginePool::run_sync(void* input, void* output) {
    return engines_[static_cast<std::size_t>(next_index())]->Run(input, nullptr, output);
}

int EnginePool::submit(void* input, void* user_arg, void* output) {
    return engines_[static_cast<std::size_t>(next_index())]->RunAsync(input, user_arg, output);
}

}  // namespace so
