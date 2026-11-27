// Exercise the actual render/worker handoff without a Windows driver or a GPU.
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
constexpr int kNumCapture=5;
using CUcontext=int;
struct CudaDriver { mutable std::atomic<int> bindings{0}; };
struct CudaThreadContext {
    const CudaDriver& driver;
    CudaThreadContext(const CudaDriver& d,CUcontext) : driver(d) { ++driver.bindings; }
    ~CudaThreadContext() { --driver.bindings; }
};

class CudaPipeline {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<int,int>> jobs;
    size_t released=0;
public:
    std::array<std::atomic<bool>,kNumCapture> signaled{};
    std::atomic<int> queries{0};
    bool fail=false;

    void Calculate(int current,int previous) {
        std::unique_lock lock(mutex);
        jobs.emplace_back(current,previous);
        size_t number=jobs.size();
        changed.notify_all();
        if (!changed.wait_for(lock,5s,[&]{return released>=number;}))
            throw std::runtime_error("test worker timed out");
        if (fail) throw std::runtime_error("submission failed");
    }
    bool PairReady(int current) {
        ++queries;
        return signaled[current].load();
    }
    void expect(size_t number,int current,int previous) {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock,5s,[&]{return jobs.size()>=number;}));
        assert((jobs[number-1]==std::pair{current,previous}));
    }
    void release(size_t number) {
        { std::lock_guard lock(mutex); released=number; }
        changed.notify_all();
    }
};

#include "../src/async_flow.h"

template<class F> void eventually(F predicate) {
    auto deadline=std::chrono::steady_clock::now()+5s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}

void checkHandoff() {
    CudaDriver driver;
    CudaPipeline gpu;
    {
        AsyncFlow flow(gpu,driver,0);
        gpu.signaled[1]=true; // Completed event from this slot's former occupant.
        flow.Request(1,0,2);
        gpu.expect(1,1,0);
        assert(driver.bindings==1);
        flow.Poll();
        assert(gpu.queries==0 && !flow.Ready(1,2));
        assert(flow.UsesSlot(0) && flow.UsesSlot(1) && !flow.UsesSlot(2));
        flow.Request(2,1,3);
        flow.Request(3,2,4);
        gpu.signaled[1]=false; // A new event has been recorded but has not fired.
        gpu.release(1);
        eventually([&]{flow.Poll(); return gpu.queries>0;});
        assert(!flow.Ready(1,2) && flow.UsesSlot(0) && flow.UsesSlot(1));
        gpu.signaled[1]=true;
        eventually([&]{flow.Poll(); return flow.Ready(1,2);});
        assert(!flow.UsesSlot(0) && !flow.UsesSlot(1));

        // Only the most recently offered pair gets submitted after the GPU is free.
        flow.Request(3,2,4);
        gpu.expect(2,3,2);
        assert(flow.UsesSlot(3) && flow.UsesSlot(2));
        gpu.signaled[3]=true;
        gpu.release(2);
        eventually([&]{flow.Poll(); return flow.Ready(3,4);});
        flow.Request(3,2,4); // A repeat request for a completed pair has no effect.
        flow.Request(1,3,5); // Recycle a previously completed slot.
        gpu.expect(3,1,3);
        int priorQueries=gpu.queries;
        flow.Poll();
        assert(gpu.queries==priorQueries && !flow.Ready(1,5));
        gpu.release(3);
        eventually([&]{flow.Poll(); return flow.Ready(1,5);});
        assert(!flow.Ready(1,2));
    }
    assert(driver.bindings==0);
}

void checkStopAndFailure() {
    CudaDriver driver;
    CudaPipeline pending;
    auto flow=std::make_unique<AsyncFlow>(pending,driver,0);
    flow->Request(1,0,2);
    pending.expect(1,1,0);
    auto stop=std::async(std::launch::async,[owner=std::move(flow)]() mutable { owner.reset(); });
    pending.release(1);
    assert(stop.wait_for(5s)==std::future_status::ready);
    stop.get();
    assert(driver.bindings==0);

    CudaPipeline failed;
    failed.fail=true;
    AsyncFlow broken(failed,driver,0);
    broken.Request(1,0,2);
    failed.expect(1,1,0);
    failed.release(1);
    eventually([&]{
        try { broken.Poll(); }
        catch (const std::runtime_error& e) { return std::string(e.what())=="submission failed"; }
        return false;
    });
}

int main() {
    checkHandoff();
    checkStopAndFailure();
    std::puts("PASS: asynchronous submission, latest-pair selection, slot ownership and reuse, shutdown, worker errors.");
}
