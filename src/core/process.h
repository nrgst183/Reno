// Running a copy of this program as a child process (used to try plug-ins out where a crash cannot hurt).
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace reno {

std::string selfExePath();

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    long long exitCode = -1;        // on Windows a crash shows up as its exception code (0xC0000005 ...)
    bool stopped = false;           // killed because 'stop' was set (see runProcess)
    bool crashed() const;
    std::string describe() const;   // "exit code 3", "crashed (access violation)", "took too long and was stopped"
};

// Runs the program at 'exe' with the given arguments and waits for it (at most timeoutSeconds, then it is killed). If
// 'stop' is given and becomes true while the child runs, the child is killed at once (a plug-in scan being cancelled,
// or Reno closing during one).
ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutSeconds,
                         const std::atomic<bool>* stop = nullptr);

// For the child: no "program has stopped working" boxes when it crashes.
void quietCrashes();

}  // namespace reno
