#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed-size pool of worker threads that serve client sockets.
//
// The main thread calls enqueue(client_socket). Workers wait on a condition
// variable, pop a socket, call the handler and then close the socket.
//
// Challenge 3 metrics: active worker count (atomic), queue length, and
// average service time and queue wait time.
class ThreadPool {
public:
    // Called on a worker thread for every client. The pool closes the socket afterwards.
    using Handler = std::function<void(int client_socket, int worker_id)>;

    ThreadPool(size_t threads, Handler handler);
    ~ThreadPool();  // calls shutdown()

    void enqueue(int client_socket);

    // Stops accepting work, lets the workers finish every queued client, then
    // joins them. Safe to call more than once.
    void shutdown();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // --- metrics ---
    size_t size() const { return workers.size(); }
    int active_workers() const { return active_count.load(); }
    size_t queue_length();
    size_t peak_queue_length() const { return peak_queue.load(); }
    uint64_t completed() const { return completed_count.load(); }
    double average_service_ms() const;
    double average_wait_ms() const;

private:
    struct Task {
        int client_socket;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    void worker(int id);

    std::vector<std::thread> workers;
    std::queue<Task> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

    Handler handler;

    std::atomic<int> active_count{0};
    std::atomic<size_t> peak_queue{0};
    std::atomic<uint64_t> completed_count{0};
    std::atomic<uint64_t> total_service_us{0};
    std::atomic<uint64_t> total_wait_us{0};
};

#endif
