#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

const std::set<std::string> kExtensions = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif",
                                           ".tiff", ".avif", ".heic", ".jxl", ".ico", ".ppm", ".tga"};

bool isImage(const fs::path &p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return kExtensions.contains(ext);
}

void scan(const fs::path &dir, bool recursive, std::vector<std::string> &out)
{
    std::vector<std::string> found;
    std::error_code ec;
    auto collect = [&](const fs::directory_entry &e) {
        std::error_code fec;
        if (e.is_regular_file(fec) && isImage(e.path()))
            found.push_back(e.path().lexically_normal().string());
    };
    if (recursive)
    {
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
            collect(*it);
    }
    else
    {
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::directory_iterator(); it.increment(ec))
            collect(*it);
    }
    std::sort(found.begin(), found.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    out.insert(out.end(), found.begin(), found.end());
}

void usage()
{
    printf("usage: termpicv [-r] [path...]\n"
           "\n"
           "Browse image thumbnails in the terminal using ueberzug.\n"
           "Paths can be directories (scanned for images) or image files.\n"
           "A single file argument opens its directory in the viewer, on that file.\n"
           "\n"
           "  -r, --recursive   scan directories recursively\n"
           "  -h, --help        show this help\n"
           "\n"
           "Env: TERMPICV_UEBERZUG overrides the ueberzug binary (default: ueberzugpp, then ueberzug).\n");
}

} // namespace

int main(int argc, char **argv)
{
    bool recursive = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-r" || a == "--recursive")
            recursive = true;
        else if (a == "-h" || a == "--help")
        {
            usage();
            return 0;
        }
        else
            args.push_back(a);
    }
    if (args.empty())
        args.push_back(".");

    std::vector<std::string> files;
    size_t initial = 0;
    bool startInViewer = false;

    if (args.size() == 1 && fs::is_regular_file(args[0]))
    {
        // Open the containing directory, positioned on the given file.
        const fs::path file = fs::path(args[0]).lexically_normal();
        fs::path dir = file.parent_path();
        scan(dir.empty() ? "." : dir, false, files);
        const fs::path target = dir.empty() ? fs::path(".") / file : file;
        auto it = std::find_if(files.begin(), files.end(), [&](const std::string &f) {
            std::error_code ec;
            return fs::equivalent(f, target, ec);
        });
        if (it == files.end())
        {
            files.insert(files.begin(), file.string());
            it = files.begin();
        }
        initial = static_cast<size_t>(it - files.begin());
        startInViewer = true;
    }
    else
    {
        for (const auto &a : args)
        {
            std::error_code ec;
            if (fs::is_directory(a, ec))
                scan(a, recursive, files);
            else if (fs::is_regular_file(a, ec))
                files.push_back(a);
            else
                fprintf(stderr, "termpicv: %s: no such file or directory\n", a.c_str());
        }
    }

    if (files.empty())
    {
        fprintf(stderr, "termpicv: no images found\n");
        return 1;
    }

    App app(std::move(files), initial, startInViewer);
    return app.run();
}
