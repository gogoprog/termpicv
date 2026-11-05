#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ~/.cache/termpicv (created on demand).
std::string cacheDirectory();
// Stable cache file stem for `path` (changes when the file does).
std::string cacheKey(const std::string &path, int size);

struct ImageSize
{
    int width = 0, height = 0;
    bool valid() const { return width > 0 && height > 0; }
};

// Produces small cached thumbnails in a background thread (when built with Magick++).
// Without Magick++, thumbnails are the original files.
class Thumbnailer
{
  public:
    enum class State
    {
        Pending,
        Working,
        Ready,
        Failed
    };

    struct Entry
    {
        State state = State::Pending;
        std::string thumbPath;
        ImageSize thumbSize;
    };

    Thumbnailer(const std::vector<std::string> &files, int size);
    ~Thumbnailer();

    // Work on images starting from `index` first (usually the first visible one).
    void prioritize(size_t index);
    Entry get(size_t index);
    // True if any thumbnail finished since the last call.
    bool takeDirty();

    // Size of the original image, if it can be determined.
    static ImageSize probe(const std::string &path);
    static bool enabled();

  private:
    void run();
    bool generate(const std::string &src, Entry &out);

    std::vector<std::string> files_;
    std::vector<Entry> entries_;
    std::string cacheDir_;
    int size_;
    size_t priority_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dirty_{false};
    std::thread worker_;
};
