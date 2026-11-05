#include "animation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef TERMPICV_HAVE_MAGICK
#include <Magick++.h>
#include <png.h>
#endif

namespace fs = std::filesystem;

namespace
{

constexpr size_t kMaxFrames = 1000;
// Browsers treat tiny GIF delays (0 or 1 tick) as 100ms; do the same.
constexpr int kDefaultDelayMs = 100;

std::string makeTempDir()
{
    fs::path base;
    if (const char *runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        base = runtime;
    else
        base = fs::temp_directory_path();
    std::string tmpl = (base / "termpicv-XXXXXX").string();
    if (!mkdtemp(tmpl.data()))
        return {};
    return tmpl;
}

#ifdef TERMPICV_HAVE_MAGICK
// ImageMagick's PNG encoder analyses every frame's colors (~100ms for a typical GIF frame);
// exporting the pixels and writing them with libpng, unfiltered at zlib level 1, takes a few ms.
bool writeFastPng(const std::string &file, Magick::Image &img)
{
    const bool alpha = img.alpha();
    const size_t w = img.columns(), h = img.rows(), channels = alpha ? 4 : 3;
    std::vector<unsigned char> pixels(w * h * channels);
    img.write(0, 0, w, h, alpha ? "RGBA" : "RGB", Magick::CharPixel, pixels.data());

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp)
        return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info || setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_set_compression_level(png, 1);
    png_set_filter(png, 0, PNG_FILTER_NONE);
    png_set_IHDR(png, info, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h), 8,
                 alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (size_t y = 0; y < h; ++y)
        png_write_row(png, pixels.data() + y * w * channels);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return fclose(fp) == 0;
}
#endif

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
    if (!dir_.empty())
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
}

size_t Animation::frameCount()
{
    std::lock_guard lock(mutex_);
    return frames_.size();
}

Animation::Frame Animation::frame(size_t index)
{
    std::lock_guard lock(mutex_);
    return index < frames_.size() ? frames_[index] : Frame{};
}

void Animation::run()
{
#ifdef TERMPICV_HAVE_MAGICK
    try
    {
        std::vector<Magick::Image> raw, frames;
        Magick::readImages(&raw, path_);
        if (raw.size() > kMaxFrames)
            raw.resize(kMaxFrames);
        if (raw.size() <= 1 || stop_)
        {
            done_ = true; // still image: the viewer shows the original
            return;
        }
        // Frames of a GIF are often partial updates; coalescing turns them into full images.
        Magick::coalesceImages(&frames, raw.begin(), raw.end());
        raw.clear();

        dir_ = makeTempDir();
        if (dir_.empty())
        {
            done_ = true;
            return;
        }

        Magick::Geometry bounds(static_cast<size_t>(maxWidth_), static_cast<size_t>(maxHeight_));
        bounds.greater(true); // only shrink
        for (size_t i = 0; i < frames.size() && !stop_; ++i)
        {
            Magick::Image &img = frames[i];
            img.resize(bounds);

            const std::string file = dir_ + "/" + std::to_string(i) + ".png";
            if (!writeFastPng(file, img))
                break;

            // animationDelay() is in 1/100 s ticks (Magick++ doesn't expose ticks_per_second).
            int delay = static_cast<int>(img.animationDelay() * 10);
            if (delay < 20)
                delay = kDefaultDelayMs;

            std::lock_guard lock(mutex_);
            frames_.push_back({file, {static_cast<int>(img.columns()), static_cast<int>(img.rows())}, delay});
        }
    }
    catch (const std::exception &)
    {
        // Keep whatever frames were decoded.
    }
#endif
    done_ = true;
}
