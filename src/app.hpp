#pragma once

#include "animation.hpp"
#include "thumbnailer.hpp"
#include "ueberzug.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class App
{
  public:
    App(std::vector<std::string> files, size_t initial, bool startInViewer);
    int run();

  private:
    enum class Mode
    {
        Grid,
        Viewer
    };

    struct GridLayout
    {
        int cols, rows, cellW, cellH, offsetX, top;
    };

    GridLayout gridLayout() const;
    void ensureVisible();

    void render();
    void renderGrid(Placements &placements);
    void renderViewer(Placements &placements);
    void drawHeader(const std::string &left, const std::string &right);
    void drawStatus(const std::string &text);

    // Fits an image of `size` into the given cell rectangle, centering it when pixel metrics are known.
    Placement fit(const std::string &path, ImageSize size, int x, int y, int w, int h) const;

    // Starts/stops the animation for the image shown in the viewer, and advances frames.
    void updateAnimation();
    bool tickAnimation();
    int inputTimeoutMs() const;

    bool handleGridKey(int ch);
    bool handleViewerKey(int ch);

    std::vector<std::string> files_;
    std::vector<ImageSize> sizes_; // original sizes, probed lazily
    std::vector<bool> probed_;
    std::unique_ptr<Thumbnailer> thumbs_;
    Ueberzug ueberzug_;

    Mode mode_ = Mode::Grid;
    size_t selected_ = 0;
    int scrollRow_ = 0;
    int zoom_ = 2; // index into kCellWidths
    std::unique_ptr<Animation> anim_;
    size_t frame_ = 0;
    bool animStarted_ = false;
    bool animPaused_ = false;
    std::chrono::steady_clock::time_point nextFrame_;
    bool forceSync_ = true;
    bool running_ = true;
};
