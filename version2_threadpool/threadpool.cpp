#include "threadpool.h"

#include <unistd.h>

#include <exception>
#include <iostream>

#include "../common/net_utils.h"

ThreadPool::ThreadPool(size_t threads, Handler handler) : stop(false), handler(std::move(handler)) {
    for (size_t i = 0; i < threads; ++i)
        workers.emplace_back(&ThreadPool::worker, this, static_cast<int>(i + 1));
}

void ThreadPool::enqueue(int client_socket) {
    size_t length;
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        tasks.push({client_socket, Clock::now()});
        length = tasks.size();
        if (length > peak_queue.load()) peak_queue.store(length);
    }
    condition.notify_one();
    log_msg("[pool] queued client fd ", client_socket, " | queue length: ", length,
            " | active workers: ", active_count.load(), "/", workers.size());
}

void ThreadPool::worker(int id) {
    while (true) {
        Task task;
        size_t remaining;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, [this] { return stop || !tasks.empty(); });

            if (stop && tasks.empty())
                return;

            task = tasks.front();
            tasks.pop();
            remaining = tasks.size();
        }

        auto start = Clock::now();
        auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(start - task.enqueued_at).count();
        int active = ++active_count;
        log_msg("[worker ", id, "] picked up fd ", task.client_socket, " after waiting ",
                format_ms(wait_us / 1000.0), " in queue | active workers: ", active, "/", workers.size(),
                " | queue length: ", remaining);

        try {
            handler(task.client_socket, id);
        } catch (const std::exception& e) {
            log_msg("[worker ", id, "] handler error: ", e.what());
        }
        close(task.client_socket);

        auto service_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        --active_count;
        ++completed_count;
        total_service_us += static_cast<uint64_t>(service_us);
        total_wait_us += static_cast<uint64_t>(wait_us);

        log_msg("[worker ", id, "] finished fd ", task.client_socket, " | service time ",
                format_ms(service_us / 1000.0), " | average service time ", format_ms(average_service_ms()),
                " over ", completed_count.load(), " requests");
    }
}

size_t ThreadPool::queue_length() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.size();
}

double ThreadPool::average_service_ms() const {
    uint64_t n = completed_count.load();
    return n ? total_service_us.load() / 1000.0 / n : 0.0;
}

double ThreadPool::average_wait_ms() const {
    uint64_t n = completed_count.load();
    return n ? total_wait_us.load() / 1000.0 / n : 0.0;
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();

    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
}
