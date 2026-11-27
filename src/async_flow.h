#pragma once

// The render thread selects the latest pair and owns its capture slots until
// the optical-flow output event completes. Only the worker calls the NvOF API.
class AsyncFlow {
    struct Pair {
        int current, previous;
        uint64_t sequence;
    };

    CudaPipeline& gpu;
    const CudaDriver& driver;
    CUcontext context;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Pair> request; // protected by mutex
    bool stopping=false;

    // Written by the worker after nvOFExecute, the scene gate and event record.
    std::atomic<uint64_t> submitted{0};
    std::exception_ptr failure;
    std::atomic<bool> failed{false};

    // Render-thread only. A recycled slot always has a new capture sequence.
    std::optional<Pair> active;
    std::array<uint64_t,kNumCapture> completed{};
    std::thread worker;

    void run() noexcept {
        try {
            CudaThreadContext binding(driver,context);
            for (;;) {
                Pair job{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock,[&]{return stopping || request.has_value();});
                    if (stopping) return;
                    job=*request;
                    request.reset();
                }
                gpu.Calculate(job.current,job.previous);
                submitted.store(job.sequence,std::memory_order_release);
            }
        } catch (...) {
            failure=std::current_exception();
            failed.store(true,std::memory_order_release);
        }
    }

public:
    AsyncFlow(CudaPipeline& pipeline,const CudaDriver& cuda,CUcontext ctx)
        : gpu(pipeline),driver(cuda),context(ctx),worker([this]{run();}) {}

    void Poll() {
        if (failed.load(std::memory_order_acquire)) std::rethrow_exception(failure);
        if (!active || submitted.load(std::memory_order_acquire)!=active->sequence) return;
        if (gpu.PairReady(active->current)) {
            completed[active->current]=active->sequence;
            active.reset();
        }
    }
    bool UsesSlot(int slot) const {
        return active && (slot==active->current || slot==active->previous);
    }
    bool Ready(int slot,uint64_t sequence) const { return completed[slot]==sequence; }

    void Request(int current,int previous,uint64_t sequence) {
        if (active || Ready(current,sequence)) return;
        active=Pair{current,previous,sequence};
        {
            std::lock_guard<std::mutex> lock(mutex);
            request=active;
        }
        changed.notify_one();
    }
    ~AsyncFlow() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping=true;
        }
        changed.notify_one();
        worker.join();
        // The enclosing GPU session drains outstanding work before freeing slots.
    }
};
