#include "ueberzug.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

bool inPath(const std::string &name)
{
    const char *path = getenv("PATH");
    if (!path)
        return false;
    std::string p = path;
    size_t start = 0;
    while (start <= p.size())
    {
        size_t end = p.find(':', start);
        if (end == std::string::npos)
            end = p.size();
        std::string candidate = p.substr(start, end - start) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

} // namespace

Ueberzug::~Ueberzug()
{
    stop();
}

bool Ueberzug::start()
{
    if (const char *env = getenv("TERMPICV_UEBERZUG"); env && *env)
        binary_ = env;
    else if (inPath("ueberzugpp"))
        binary_ = "ueberzugpp";
    else if (inPath("ueberzug"))
        binary_ = "ueberzug";
    else
        return false;

    int fds[2];
    if (pipe(fds) != 0)
        return false;

    signal(SIGPIPE, SIG_IGN);

    pid_ = fork();
    if (pid_ < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    if (pid_ == 0)
    {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        // Keep stdout on the tty (sixel/kitty backends write there), silence stderr.
        const char *log = getenv("TERMPICV_UEBERZUG_LOG");
        int devnull = open(log && *log ? log : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (devnull >= 0)
        {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (log && *log)
            execlp(binary_.c_str(), binary_.c_str(), "layer", "--parser", "json", nullptr);
        else
            execlp(binary_.c_str(), binary_.c_str(), "layer", "--silent", "--parser", "json", nullptr);
        _exit(127);
    }

    close(fds[0]);
    fd_ = fds[1];
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
    return true;
}

void Ueberzug::stop()
{
    if (fd_ < 0)
        return;
    clear();
    close(fd_);
    fd_ = -1;
    if (pid_ > 0)
    {
        // Give it a moment to exit on EOF, then make sure it is gone.
        for (int i = 0; i < 20; ++i)
        {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_)
            {
                pid_ = -1;
                return;
            }
            usleep(10000);
        }
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
}

bool Ueberzug::alive()
{
    if (fd_ < 0)
        return false;
    if (pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) == pid_)
    {
        pid_ = -1;
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void Ueberzug::sync(const Placements &desired, bool force)
{
    if (!alive())
    {
        // ueberzug crashed: bring up a fresh one and re-send everything.
        if (restarts_ >= kMaxRestarts || !start())
            return;
        ++restarts_;
        placed_.clear();
    }

    std::vector<std::string> commands;
    // Changed placements are simply re-added: an add with a known identifier replaces it.
    for (auto it = placed_.begin(); it != placed_.end();)
    {
        if (!desired.contains(it->first))
        {
            commands.push_back(removeCommand(it->first));
            it = placed_.erase(it);
        }
        else
            ++it;
    }
    for (const auto &[id, p] : desired)
    {
        auto it = placed_.find(id);
        if (force || it == placed_.end() || !(it->second == p))
        {
            commands.push_back(addCommand(id, p));
            placed_[id] = p;
        }
    }
    flush(commands);
}

void Ueberzug::clear()
{
    std::vector<std::string> commands;
    for (const auto &[id, _] : placed_)
        commands.push_back(removeCommand(id));
    placed_.clear();
    flush(commands);
}

std::string Ueberzug::addCommand(const std::string &id, const Placement &p)
{
    return "{\"action\":\"add\",\"identifier\":\"" + jsonEscape(id) + "\",\"path\":\"" + jsonEscape(p.path) +
           "\",\"x\":" + std::to_string(p.x) + ",\"y\":" + std::to_string(p.y) +
           ",\"max_width\":" + std::to_string(p.width) + ",\"max_height\":" + std::to_string(p.height) +
           ",\"scaler\":\"fit_contain\"";
}

std::string Ueberzug::removeCommand(const std::string &id)
{
    return "{\"action\":\"remove\",\"identifier\":\"" + jsonEscape(id) + "\"";
}

void Ueberzug::flush(const std::vector<std::string> &commands)
{
    // Only the last command of a batch draws, and it draws synchronously. Legacy ueberzug's
    // asynchronous redraw sets a "scheduled" flag that is never reset if a draw raises, after
    // which it silently stops drawing for good.
    std::string out;
    for (size_t i = 0; i < commands.size(); ++i)
        out += commands[i] + (i + 1 < commands.size() ? ",\"draw\":false}\n" : ",\"synchronously_draw\":true}\n");
    if (!out.empty())
        send(out);
}

void Ueberzug::send(const std::string &line)
{
    if (fd_ < 0)
        return;
    const char *data = line.data();
    size_t left = line.size();
    while (left > 0)
    {
        ssize_t n = write(fd_, data, left);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            // The child died; stop talking to it.
            close(fd_);
            fd_ = -1;
            return;
        }
        data += n;
        left -= static_cast<size_t>(n);
    }
}
