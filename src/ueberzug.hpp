#pragma once

#include <map>
#include <string>
#include <vector>
#include <sys/types.h>

struct Placement
{
    std::string path;
    int x = 0, y = 0, width = 0, height = 0;

    bool operator==(const Placement &) const = default;
};

using Placements = std::map<std::string, Placement>;

// Drives an `ueberzug layer` (or ueberzugpp) child process via its JSON protocol.
class Ueberzug
{
  public:
    ~Ueberzug();

    bool start();
    void stop();
    bool running() const { return fd_ >= 0; }
    const std::string &binary() const { return binary_; }

    // Brings the on-screen placements in line with `desired`, sending only the differences.
    // `force` re-sends every placement (needed after the terminal was fully repainted).
    void sync(const Placements &desired, bool force = false);
    void clear();
    // True while ueberzug still has unread commands waiting in its pipe.
    bool busy() const;

  private:
    static constexpr int kMaxRestarts = 5;

    bool alive();
    static std::string addCommand(const std::string &id, const Placement &p);
    static std::string removeCommand(const std::string &id);
    void flush(const std::vector<std::string> &commands);
    void send(const std::string &data);

    pid_t pid_ = -1;
    int fd_ = -1;
    std::string binary_;
    Placements placed_;
    int restarts_ = 0;
};
