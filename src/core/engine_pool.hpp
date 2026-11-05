#pragma once

#include <dxrt/dxrt_cxx_api.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace so {

/**
 * One .dxnn model loaded once per NPU core, with jobs dispatched round-robin.
 *
 * WHY THIS CLASS EXISTS
 * ---------------------
 * `InferenceOption::NPU_ALL` does NOT spread one engine's async job stream
 * across the DX-M1's three cores. Measured on this board: six threads
 * saturating a single NPU_ALL engine (RunAsync immediately followed by Wait,
 * no other work) held core 0 at ~20% while cores 1 and 2 read 0.0%. NPU_ALL
 * grants a job access to all three cores; it does not cause DX-RT to
 * distribute a single engine's jobs among them.
 *
 * The real per-core mechanism is BOUND_OPTION NPU_0 / NPU_1 / NPU_2: three
 * engines, each pinned to one core, with jobs handed out round-robin. That is
 * what this class is.
 *
 * HARD CONSTRAINT
 * ---------------
 * DX-RT refuses to create a core-bound engine for a model that already has an
 * NPU_ALL engine open — it fails with "Dynamic IPC task init failed". A
 * process must therefore pick one binding style per model and keep it. This
 * codebase always builds pools and NEVER constructs an NPU_ALL engine, for any
 * model, anywhere, which is why there is no option to do so here.
 *
 * COST
 * ----
 * The model is resident once per core. For the three models this deployment
 * uses that is ~196 MB of the DX-M1's 3.92 GiB.
 */
class EnginePool {
public:
    /// Fires on a DX-RT worker thread once a job completes. Keep the body
    /// trivial — see `register_callback`.
    using Callback = std::function<void(dxrt::TensorPtrs&, void*)>;

    /**
     * @param model_path   path to the .dxnn
     * @param num_cores    1..3; 3 uses the whole board
     * @param use_ort      false runs the model natively on the NPU. True moves
     *                     the fused tail onto the CPU through ONNX Runtime,
     *                     which is exactly the work this deployment is trying
     *                     to keep off the CPU, so it defaults to false.
     * @param buffer_count DX-RT job buffers per engine
     */
    EnginePool(const std::string& model_path, int num_cores = 3,
               bool use_ort = false, int buffer_count = 4);

    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;

    /**
     * Submit one preprocessed input to the next core in rotation.
     *
     * @param input     model-shaped input buffer (uint8 NHWC for every model here)
     * @param user_arg  handed back to the callback untouched
     * @param output    caller-owned output buffer of at least output_size() bytes
     * @return DX-RT job id
     */
    int submit(void* input, void* user_arg, void* output);

    /**
     * Synchronous inference on the next core in rotation.
     *
     * For the high-rate detector path this would be wrong — it blocks the
     * caller for the whole round trip. It is the right shape for the secondary
     * face stage, where a frame yields a handful of crops and the caller is
     * already a postprocess worker rather than the frame thread.
     */
    dxrt::TensorPtrs run_sync(void* input, void* output);

    /**
     * Install the completion callback on every engine in the pool.
     *
     * The callback runs on a DX-RT worker thread. It must do one thing: hand
     * the buffer to a postprocess queue. Decoding tensors or running a tracker
     * here stalls the NPU job queue and serialises exactly the parallelism this
     * class exists to create.
     */
    void register_callback(Callback cb);

    std::size_t size() const { return engines_.size(); }
    const std::vector<std::int64_t>& input_shape() const { return input_shape_; }
    std::uint64_t input_size() const { return input_size_; }
    std::uint64_t output_size() const { return output_size_; }
    const std::vector<std::vector<std::int64_t>>& output_shapes() const { return output_shapes_; }
    const std::vector<std::string>& output_names() const { return output_names_; }
    const std::string& model_path() const { return model_path_; }

    /// Index of the engine the next submit() will use, advancing the rotation.
    int next_index();

    /// BOUND_OPTION for a core index. Throws for anything outside 0..2 rather
    /// than silently falling back to NPU_ALL, which would break the pool
    /// invariant documented above.
    static std::uint32_t bound_option_for(int core);

    /// Live per-core utilisation (0.0-100.0) straight from the DX-RT C API;
    /// the C++ DeviceStatus wrapper does not expose it. Returns -1.0 when the
    /// device or core is unavailable.
    static double core_utilization(int core, int device_id = 0);

private:
    std::string model_path_;
    // InferenceEngine holds `this` as the callback user_data and is therefore
    // neither copyable nor movable; it must live behind a pointer.
    std::vector<std::unique_ptr<dxrt::InferenceEngine>> engines_;
    std::vector<std::unique_ptr<dxrt::InferenceOption>> options_;
    std::atomic<unsigned> next_{0};

    std::vector<std::int64_t> input_shape_;
    std::uint64_t input_size_{0};
    std::uint64_t output_size_{0};
    std::vector<std::vector<std::int64_t>> output_shapes_;
    std::vector<std::string> output_names_;
    Callback cb_;
};

}  // namespace so
