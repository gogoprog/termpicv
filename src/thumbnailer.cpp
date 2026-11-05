#include "thumbnailer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sys/stat.h>

#ifdef TERMPICV_HAVE_MAGICK
#include <Magick++.h>
#endif

namespace fs = std::filesystem;

std::string cacheDirectory()
{
    fs::path base;
    if (const char *xdg = getenv("XDG_CACHE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = getenv("HOME"); home && *home)
        base = fs::path(home) / ".cache";
    else
        base = fs::temp_directory_path();
    fs::path dir = base / "termpicv";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string cacheKey(const std::string &path, int size)
{
    struct stat st{};
    std::string key = fs::absolute(path).string();
    if (stat(path.c_str(), &st) == 0)
        key += ":" + std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec) + ":" +
               std::to_string(st.st_size);
    key += ":" + std::to_string(size);
    char buf[32];
    snprintf(buf, sizeof(buf), "%016zx", std::hash<std::string>{}(key));
    return buf;
}

bool Thumbnailer::enabled()
{
#ifdef TERMPICV_HAVE_MAGICK
    return true;
#else
    return false;
#endif
}

Thumbnailer::Thumbnailer(const std::vector<std::string> &files, int size)
    : files_(files), entries_(files.size()), size_(size)
{
#ifdef TERMPICV_HAVE_MAGICK
    Magick::InitializeMagick(nullptr);
    cacheDir_ = cacheDirectory();
    worker_ = std::thread([this] { run(); });
#else
    for (size_t i = 0; i < files_.size(); ++i)
    {
        entries_[i].state = State::Ready;
        entries_[i].thumbPath = files_[i];
    }
#endif
}

Thumbnailer::~Thumbnailer()
{
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void Thumbnailer::prioritize(size_t index)
{
    std::lock_guard lock(mutex_);
    priority_ = index;
}

Thumbnailer::Entry Thumbnailer::get(size_t index)
{
    std::lock_guard lock(mutex_);
    return index < entries_.size() ? entries_[index] : Entry{State::Failed, {}, {}};
}

bool Thumbnailer::takeDirty()
{
    return dirty_.exchange(false);
}

void Thumbnailer::run()
{
    const size_t n = files_.size();
    while (!stop_)
    {
        size_t index = n;
        {
            std::lock_guard lock(mutex_);
            for (size_t k = 0; k < n; ++k)
            {
                size_t i = (priority_ + k) % n;
                if (entries_[i].state == State::Pending)
                {
                    index = i;
                    entries_[i].state = State::Working;
                    break;
                }
            }
        }
        if (index == n)
            return; // all done

        Entry result;
        bool ok = generate(files_[index], result);
        {
            std::lock_guard lock(mutex_);
            entries_[index] = result;
            entries_[index].state = ok ? State::Ready : State::Failed;
        }
        dirty_ = true;
    }
}

bool Thumbnailer::generate([[maybe_unused]] const std::string &src, [[maybe_unused]] Entry &out)
{
#ifdef TERMPICV_HAVE_MAGICK
    const fs::path dst = fs::path(cacheDir_) / (cacheKey(src, size_) + ".png");
    try
    {
        if (fs::exists(dst))
        {
            Magick::Image img;
            img.ping(dst.string());
            out.thumbPath = dst.string();
            out.thumbSize = {static_cast<int>(img.columns()), static_cast<int>(img.rows())};
            return true;
        }

        const std::string geometry = std::to_string(size_) + "x" + std::to_string(size_);
        Magick::Image img;
        img.subImage(0); // first frame only (gif, multi-page tiff...)
        img.subRange(1);
        img.defineValue("jpeg", "size", std::to_string(size_ * 2) + "x" + std::to_string(size_ * 2));
        img.read(src);
        img.autoOrient();
        img.thumbnail(Magick::Geometry(geometry));
        img.magick("PNG");

        // Write then rename so a half-written thumbnail is never picked up.
        const fs::path tmp = dst.string() + ".tmp";
        img.write(tmp.string());
        fs::rename(tmp, dst);

        out.thumbPath = dst.string();
        out.thumbSize = {static_cast<int>(img.columns()), static_cast<int>(img.rows())};
        return true;
    }
    catch (const Magick::Warning &)
    {
        // Image may still have been written; fall back to the original if not.
        out.thumbPath = fs::exists(dst) ? dst.string() : src;
        return true;
    }
    catch (const std::exception &)
    {
        // ImageMagick can't read it; let ueberzug try the original.
        out.thumbPath = src;
        return true;
    }
#else
    return false;
#endif
}

ImageSize Thumbnailer::probe([[maybe_unused]] const std::string &path)
{
#ifdef TERMPICV_HAVE_MAGICK
    try
    {
        Magick::Image img;
        img.subImage(0);
        img.subRange(1);
        img.ping(path);
        int w = static_cast<int>(img.columns()), h = static_cast<int>(img.rows());
        // Orientations 5-8 swap width and height once auto-oriented.
        if (img.orientation() >= Magick::LeftTopOrientation)
            std::swap(w, h);
        return {w, h};
    }
    catch (const std::exception &)
    {
    }
#endif
    return {};
}
