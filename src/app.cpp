#include "app.hpp"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <ncurses.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{

constexpr int kCellWidths[] = {12, 16, 22, 28, 36, 46};
constexpr int kZoomLevels = sizeof(kCellWidths) / sizeof(kCellWidths[0]);
constexpr int kThumbPixels = 384;

enum Colors
{
    kPairSelected = 1,
    kPairBar,
    kPairDim,
};

volatile sig_atomic_t gQuit = 0;

void onSignal(int)
{
    gQuit = 1;
}

// Truncates a UTF-8 string to at most `width` code points (adding an ellipsis when cut).
std::string truncate(const std::string &s, int width)
{
    if (width <= 0)
        return {};
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    if (static_cast<int>(starts.size()) <= width)
        return s;
    return s.substr(0, starts[width - 1]) + "…";
}

int utf8Length(const std::string &s)
{
    return static_cast<int>(std::count_if(s.begin(), s.end(),
                                          [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string humanSize(uintmax_t bytes)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 3)
    {
        v /= 1024;
        ++u;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

} // namespace

App::App(std::vector<std::string> files, size_t initial, bool startInViewer)
    : files_(std::move(files)), sizes_(files_.size()), probed_(files_.size(), false),
      mode_(startInViewer ? Mode::Viewer : Mode::Grid), selected_(std::min(initial, files_.size() - 1))
{
    thumbs_ = std::make_unique<Thumbnailer>(files_, kThumbPixels);
}

int App::run()
{
    if (!ueberzug_.start())
    {
        fprintf(stderr, "termpicv: could not start ueberzug (install ueberzugpp or ueberzug, "
                        "or set TERMPICV_UEBERZUG)\n");
        return 1;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);

    setlocale(LC_ALL, "");
    set_escdelay(25);
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    if (has_colors())
    {
        start_color();
        use_default_colors();
        init_pair(kPairSelected, COLOR_CYAN, -1);
        init_pair(kPairBar, COLOR_BLACK, COLOR_CYAN);
        init_pair(kPairDim, COLOR_BLUE, -1);
    }

    render();
    while (running_ && !gQuit)
    {
        timeout(inputTimeoutMs());
        int ch = getch();
        bool changed = false;
        if (ch == ERR)
        {
            changed = thumbs_->takeDirty() | tickAnimation();
        }
        else if (ch == KEY_RESIZE || ch == 12 /* ^L */)
        {
            clear();
            forceSync_ = true;
            changed = true;
        }
        else
        {
            changed = mode_ == Mode::Grid ? handleGridKey(ch) : handleViewerKey(ch);
        }
        if (changed && running_)
            render();
    }

    ueberzug_.stop();
    endwin();
    return 0;
}

int App::inputTimeoutMs() const
{
    if (!anim_)
        return 100;
    if (!animStarted_ || animPaused_)
        return 30; // waiting for decoding to finish
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame_ - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<long>(ms, 1, 100));
}

void App::updateAnimation()
{
    const std::string &path = files_[selected_];
    if (mode_ != Mode::Viewer || !Animation::canAnimate(path))
    {
        anim_.reset();
        return;
    }
    if (anim_ && anim_->path() == path)
        return;

    // Decode frames at the size they will be shown, falling back to a reasonable bound.
    winsize ws{};
    int maxW = 1280, maxH = 960;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0 && ws.ws_row > 0)
    {
        maxW = ws.ws_xpixel;
        maxH = ws.ws_ypixel * std::max(1, LINES - 2) / ws.ws_row;
    }
    anim_ = std::make_unique<Animation>(path, maxW, maxH);
    frame_ = 0;
    animStarted_ = false;
}

bool App::tickAnimation()
{
    if (!anim_ || !anim_->ready())
        return false;
    if (!anim_->animated())
        return false;
    const auto now = std::chrono::steady_clock::now();
    if (!animStarted_)
    {
        // Decoding just finished: show the first frame.
        animStarted_ = true;
        frame_ = 0;
        nextFrame_ = now + std::chrono::milliseconds(anim_->frames()[0].delayMs);
        return true;
    }
    if (animPaused_ || now < nextFrame_)
        return false;
    const auto &frames = anim_->frames();
    frame_ = (frame_ + 1) % frames.size();
    nextFrame_ += std::chrono::milliseconds(frames[frame_].delayMs);
    if (nextFrame_ < now) // fell behind (slow draw): don't try to catch up
        nextFrame_ = now + std::chrono::milliseconds(frames[frame_].delayMs);
    return true;
}

App::GridLayout App::gridLayout() const
{
    GridLayout l{};
    l.cellW = std::min(kCellWidths[zoom_], std::max(COLS, 4));
    // Terminal cells are roughly twice as tall as wide: a square image area is (w - 2) x (w - 2) / 2.
    // Plus top border, bottom border and a label row.
    l.cellH = (l.cellW - 2) / 2 + 3;
    l.top = 1;
    l.cols = std::max(1, COLS / l.cellW);
    l.rows = std::max(1, (LINES - 2) / l.cellH);
    l.offsetX = (COLS - l.cols * l.cellW) / 2;
    return l;
}

void App::ensureVisible()
{
    const GridLayout l = gridLayout();
    const int row = static_cast<int>(selected_) / l.cols;
    const int totalRows = (static_cast<int>(files_.size()) + l.cols - 1) / l.cols;
    if (row < scrollRow_)
        scrollRow_ = row;
    else if (row >= scrollRow_ + l.rows)
        scrollRow_ = row - l.rows + 1;
    scrollRow_ = std::clamp(scrollRow_, 0, std::max(0, totalRows - l.rows));
}

Placement App::fit(const std::string &path, ImageSize size, int x, int y, int w, int h) const
{
    Placement p{path, x, y, std::max(1, w), std::max(1, h)};

    winsize ws{};
    if (!size.valid() || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_xpixel == 0 || ws.ws_ypixel == 0 ||
        ws.ws_col == 0 || ws.ws_row == 0)
        return p;

    const double cellPxW = static_cast<double>(ws.ws_xpixel) / ws.ws_col;
    const double cellPxH = static_cast<double>(ws.ws_ypixel) / ws.ws_row;
    const double scale = std::min(w * cellPxW / size.width, h * cellPxH / size.height);
    const int fw = std::clamp(static_cast<int>(std::ceil(size.width * scale / cellPxW)), 1, w);
    const int fh = std::clamp(static_cast<int>(std::ceil(size.height * scale / cellPxH)), 1, h);
    p.x = x + (w - fw) / 2;
    p.y = y + (h - fh) / 2;
    p.width = fw;
    p.height = fh;
    return p;
}

void App::drawHeader(const std::string &left, const std::string &right)
{
    attron(COLOR_PAIR(kPairBar));
    mvhline(0, 0, ' ', COLS);
    const int rightLen = utf8Length(right);
    mvaddstr(0, 1, truncate(left, COLS - rightLen - 3).c_str());
    if (rightLen + 1 < COLS)
        mvaddstr(0, COLS - rightLen - 1, right.c_str());
    attroff(COLOR_PAIR(kPairBar));
}

void App::drawStatus(const std::string &text)
{
    attron(COLOR_PAIR(kPairDim));
    mvhline(LINES - 1, 0, ' ', COLS);
    mvaddstr(LINES - 1, 1, truncate(text, COLS - 2).c_str());
    attroff(COLOR_PAIR(kPairDim));
}

void App::render()
{
    updateAnimation();
    erase();
    Placements placements;
    if (mode_ == Mode::Grid)
        renderGrid(placements);
    else
        renderViewer(placements);
    refresh();
    // Images go on top once the text layer is on screen.
    ueberzug_.sync(placements, forceSync_);
    forceSync_ = false;
}

void App::renderGrid(Placements &placements)
{
    ensureVisible();
    const GridLayout l = gridLayout();
    const size_t first = static_cast<size_t>(scrollRow_) * l.cols;
    const size_t last = std::min(files_.size(), first + static_cast<size_t>(l.cols * l.rows));
    thumbs_->prioritize(first);

    const std::string dir = fs::path(files_[selected_]).parent_path().string();
    drawHeader("termpicv  " + (dir.empty() ? std::string(".") : dir),
               std::to_string(selected_ + 1) + "/" + std::to_string(files_.size()));

    for (size_t i = first; i < last; ++i)
    {
        const int slot = static_cast<int>(i - first);
        const int x = l.offsetX + (slot % l.cols) * l.cellW;
        const int y = l.top + (slot / l.cols) * l.cellH;
        const bool sel = i == selected_;
        const int innerW = l.cellW - 2;
        const int imageH = l.cellH - 3;

        const attr_t boxAttr = sel ? (COLOR_PAIR(kPairSelected) | A_BOLD) : COLOR_PAIR(kPairDim);
        attron(boxAttr);
        mvaddch(y, x, ACS_ULCORNER);
        mvhline(y, x + 1, ACS_HLINE, innerW);
        mvaddch(y, x + l.cellW - 1, ACS_URCORNER);
        mvvline(y + 1, x, ACS_VLINE, l.cellH - 2);
        mvvline(y + 1, x + l.cellW - 1, ACS_VLINE, l.cellH - 2);
        mvaddch(y + l.cellH - 1, x, ACS_LLCORNER);
        mvhline(y + l.cellH - 1, x + 1, ACS_HLINE, innerW);
        mvaddch(y + l.cellH - 1, x + l.cellW - 1, ACS_LRCORNER);
        attroff(boxAttr);

        const std::string name = truncate(fs::path(files_[i]).filename().string(), innerW);
        const int nameX = x + 1 + (innerW - utf8Length(name)) / 2;
        if (sel)
            attron(A_REVERSE | A_BOLD);
        mvaddstr(y + l.cellH - 2, nameX, name.c_str());
        if (sel)
            attroff(A_REVERSE | A_BOLD);

        const Thumbnailer::Entry thumb = thumbs_->get(i);
        if (thumb.state == Thumbnailer::State::Ready)
        {
            placements["thumb" + std::to_string(slot)] =
                fit(thumb.thumbPath, thumb.thumbSize, x + 1, y + 1, innerW, imageH);
        }
        else
        {
            const char *msg = thumb.state == Thumbnailer::State::Failed ? "unreadable" : "…";
            attron(COLOR_PAIR(kPairDim));
            mvaddstr(y + 1 + imageH / 2, x + 1 + (innerW - utf8Length(msg)) / 2, msg);
            attroff(COLOR_PAIR(kPairDim));
        }
    }

    drawStatus("hjkl/arrows move  enter view  +/- size  g/G first/last  q quit");
}

void App::renderViewer(Placements &placements)
{
    const std::string &path = files_[selected_];
    if (!probed_[selected_])
    {
        sizes_[selected_] = Thumbnailer::probe(path);
        probed_[selected_] = true;
    }
    const ImageSize size = sizes_[selected_];

    std::string info;
    if (size.valid())
        info = std::to_string(size.width) + "x" + std::to_string(size.height) + "  ";
    std::error_code ec;
    if (auto bytes = fs::file_size(path, ec); !ec)
        info += humanSize(bytes) + "  ";
    std::string shown = path;
    ImageSize shownSize = size;
    if (anim_)
    {
        if (!anim_->ready())
            info = "decoding…  " + info;
        else if (anim_->animated() && animStarted_)
        {
            const auto &f = anim_->frames()[frame_];
            shown = f.path;
            shownSize = f.size;
            info = (animPaused_ ? "paused " : "") + std::to_string(frame_ + 1) + "/" +
                   std::to_string(anim_->frames().size()) + " frames  " + info;
        }
    }
    info += std::to_string(selected_ + 1) + "/" + std::to_string(files_.size());

    drawHeader(path, info);
    placements["viewer"] = fit(shown, shownSize, 0, 1, COLS, LINES - 2);
    drawStatus(anim_ ? "h/l prev/next  a pause/play  g/G first/last  q/esc back  Q quit"
                     : "h/l prev/next  g/G first/last  q/esc back  Q quit");
}

bool App::handleGridKey(int ch)
{
    const GridLayout l = gridLayout();
    const long n = static_cast<long>(files_.size());
    long sel = static_cast<long>(selected_);
    const long page = static_cast<long>(l.cols) * l.rows;

    switch (ch)
    {
    case 'h': case KEY_LEFT: sel -= 1; break;
    case 'l': case KEY_RIGHT: sel += 1; break;
    case 'k': case KEY_UP: sel -= l.cols; break;
    case 'j': case KEY_DOWN: sel += l.cols; break;
    case KEY_PPAGE: case 2 /* ^B */: case 21 /* ^U */: sel -= page; break;
    case KEY_NPAGE: case 6 /* ^F */: case 4 /* ^D */: sel += page; break;
    case 'g': case KEY_HOME: sel = 0; break;
    case 'G': case KEY_END: sel = n - 1; break;
    case '+': case '=':
        if (zoom_ + 1 >= kZoomLevels)
            return false;
        ++zoom_;
        return true;
    case '-': case '_':
        if (zoom_ == 0)
            return false;
        --zoom_;
        return true;
    case '\n': case KEY_ENTER: case ' ': case 'o':
        mode_ = Mode::Viewer;
        return true;
    case 'q': case 'Q':
        running_ = false;
        return false;
    default:
        return false;
    }

    sel = std::clamp(sel, 0L, n - 1);
    if (sel == static_cast<long>(selected_))
        return false;
    selected_ = static_cast<size_t>(sel);
    return true;
}

bool App::handleViewerKey(int ch)
{
    const size_t n = files_.size();
    switch (ch)
    {
    case 'l': case 'j': case 'n': case ' ': case KEY_RIGHT: case KEY_DOWN: case KEY_NPAGE:
        if (selected_ + 1 >= n)
            return false;
        ++selected_;
        return true;
    case 'h': case 'k': case 'p': case KEY_BACKSPACE: case KEY_LEFT: case KEY_UP: case KEY_PPAGE:
        if (selected_ == 0)
            return false;
        --selected_;
        return true;
    case 'g': case KEY_HOME:
        selected_ = 0;
        return true;
    case 'G': case KEY_END:
        selected_ = n - 1;
        return true;
    case 'q': case 27 /* esc */: case '\n': case KEY_ENTER:
        mode_ = Mode::Grid;
        return true;
    case 'a':
        if (!anim_ || !anim_->animated())
            return false;
        animPaused_ = !animPaused_;
        if (!animPaused_)
            nextFrame_ = std::chrono::steady_clock::now();
        return true;
    case 'Q':
        running_ = false;
        return false;
    default:
        return false;
    }
}
