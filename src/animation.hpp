#pragma once

#include "thumbnailer.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Decodes an animated image (gif, webp...) into cached, pre-scaled PNG frames in a background
// thread, so ueberzug can be fed one frame after another.
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
    // Once true, frames() is complete and no longer changes.
    bool ready() const { return ready_; }
    bool animated() const { return ready_ && frames_.size() > 1; }
    const std::vector<Frame> &frames() const { return frames_; }

    static bool canAnimate(const std::string &path);

  private:
    void run();
    bool loadManifest(const std::string &manifest);

    std::string path_;
    int maxWidth_, maxHeight_;
    std::vector<Frame> frames_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
