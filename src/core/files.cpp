#include "core/files.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "core/logger.h"
#include "core/util.h"

namespace files {
namespace {

// A function-local static, so pending writes land and the thread is joined on exit even
// when nobody calls flush().
class Writer {
public:
    ~Writer() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    void queue(std::string path, std::string data) {
        {
            std::lock_guard lock(mutex_);
            pending_[std::move(path)] = std::move(data);
            if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        }
        wake_.notify_one();
    }

    void flush() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return pending_.empty() && !writing_; });
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;
            auto next = pending_.extract(pending_.begin());
            writing_ = true;
            lock.unlock();
            if (!util::writeFile(next.key(), next.mapped())) {
                logging::warn("files", "could not write {}", next.key());
            }
            lock.lock();
            writing_ = false;
            idle_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::map<std::string, std::string> pending_;
    std::thread thread_;
    bool writing_ = false;
    bool stopping_ = false;
};

Writer& writer() {
    static Writer instance;
    return instance;
}

}  // namespace

void writeLater(std::string path, std::string data) { writer().queue(std::move(path), std::move(data)); }

void flush() { writer().flush(); }

}  // namespace files
