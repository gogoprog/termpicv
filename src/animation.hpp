#pragma once

#include "thumbnailer.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Decodes an animated image (gif, webp...) into pre-scaled PNG frames in a background thread,
// so ueberzug can be fed one frame after another. Frames become available as they are decoded,
// and live in a temporary directory removed with the object.
class Animation
{
  public:
    struct Frame
    {
        std::string path;
        ImageSize size;
        int delayMs;
    };

    Animation(std::string path, int maxWidth, int maxHeight);
    ~Animation();

    const std::string &path() const { return path_; }
    // True once every frame has been decoded (or decoding failed).
    bool done() const { return done_; }
    size_t frameCount();
    Frame frame(size_t index);

    static bool canAnimate(const std::string &path);

  private:
    void run();

    std::string path_;
    int maxWidth_, maxHeight_;
    std::string dir_;
    std::mutex mutex_;
    std::vector<Frame> frames_;
    std::atomic<bool> done_{false};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
