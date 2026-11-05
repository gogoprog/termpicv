#include "animation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#ifdef TERMPICV_HAVE_MAGICK
#include <Magick++.h>
#endif

namespace fs = std::filesystem;

namespace
{

constexpr size_t kMaxFrames = 1000;
// Browsers treat tiny GIF delays (0 or 1 tick) as 100ms; do the same.
constexpr int kDefaultDelayMs = 100;

} // namespace

bool Animation::canAnimate([[maybe_unused]] const std::string &path)
{
#ifdef TERMPICV_HAVE_MAGICK
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".gif" || ext == ".webp";
#else
    return false;
#endif
}

Animation::Animation(std::string path, int maxWidth, int maxHeight)
    : path_(std::move(path)), maxWidth_(std::max(1, maxWidth)), maxHeight_(std::max(1, maxHeight))
{
    worker_ = std::thread([this] { run(); });
}

Animation::~Animation()
{
    stop_ = true;
    if (worker_.joinable())
        worker_.join();
}

bool Animation::loadManifest(const std::string &manifest)
{
    std::ifstream in(manifest);
    if (!in)
        return false;
    std::vector<Frame> frames;
    Frame f;
    while (in >> f.size.width >> f.size.height >> f.delayMs && std::getline(in >> std::ws, f.path))
    {
        if (!fs::exists(f.path))
            return false;
        frames.push_back(f);
    }
    if (frames.empty())
        return false;
    frames_ = std::move(frames);
    return true;
}

void Animation::run()
{
#ifdef TERMPICV_HAVE_MAGICK
    const std::string geometry = std::to_string(maxWidth_) + "x" + std::to_string(maxHeight_);
    const fs::path stem = fs::path(cacheDirectory()) / (cacheKey(path_, maxWidth_ * 100000 + maxHeight_) + "-anim");
    const std::string manifest = stem.string() + ".txt";

    if (loadManifest(manifest))
    {
        ready_ = true;
        return;
    }

    try
    {
        std::vector<Magick::Image> raw, frames;
        Magick::readImages(&raw, path_);
        if (raw.size() > kMaxFrames)
            raw.resize(kMaxFrames);
        if (raw.size() <= 1 || stop_)
        {
            ready_ = true; // still image: the viewer shows the original
            return;
        }
        // Frames of a GIF are often partial updates; coalescing turns them into full images.
        Magick::coalesceImages(&frames, raw.begin(), raw.end());
        raw.clear();

        std::vector<Frame> out;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            if (stop_)
                return;
            Magick::Image &img = frames[i];
            Magick::Geometry g(geometry);
            g.greater(true); // only shrink
            img.resize(g);
            img.magick("PNG");
            img.quality(10); // fast zlib level, these are throwaway cache files

            const std::string file = stem.string() + "-" + std::to_string(i) + ".png";
            img.write(file);

            // animationDelay() is in 1/100 s ticks (Magick++ doesn't expose ticks_per_second).
            int delay = static_cast<int>(img.animationDelay() * 10);
            if (delay < 20)
                delay = kDefaultDelayMs;
            out.push_back({file, {static_cast<int>(img.columns()), static_cast<int>(img.rows())}, delay});
        }

        // Manifest last, via rename, so a partial decode is never reused.
        const std::string tmp = manifest + ".tmp";
        {
            std::ofstream mf(tmp);
            for (const auto &f : out)
                mf << f.size.width << ' ' << f.size.height << ' ' << f.delayMs << ' ' << f.path << '\n';
        }
        fs::rename(tmp, manifest);
        frames_ = std::move(out);
    }
    catch (const std::exception &)
    {
        frames_.clear();
    }
#endif
    ready_ = true;
}
