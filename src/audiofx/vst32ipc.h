// The 32-bit VST2 bridge's plumbing: shared memory, events, pipes and the child process, for Windows and for Linux
// (so the whole bridge can be tested there against a 64-bit server). See vst32proto.h for the messages.
#pragma once
#include "vst32proto.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace reno {
namespace vst32 {

// One end of the link between Reno and a bridge process: shared memory, two events and two pipes. Reno's end is made
// with start() (which also starts the process); the process's end with attach().
class Endpoint {
public:
    Endpoint();
    ~Endpoint();
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    // Reno's end: makes the memory, events and pipes and starts 'exe' with them. False (with *error) on failure.
    bool start(const std::string& exe, std::string* error);
    // The process's end, from its command line (what start() put there): argv[1...].
    bool attach(const std::vector<std::string>& args, std::string* error);

    // Reno: ends the process at once and lets the reader see the end of the pipe. Safe to call twice.
    void kill();
    // Reno: the process is still running. (The server asks whether Reno is: Reno's end of the pipe is open.)
    bool alive();
    // Closes the pipes and handles (after the reader thread has returned).
    void close();

    // Control messages. send() is safe from several threads; read() from one at a time. read() blocks until a whole
    // message has arrived, and returns false when the other end is gone (or a message is malformed).
    bool send(uint32_t op, uint32_t seq, const std::string& payload);
    bool read(FrameHead& head, std::string& payload);

    // Audio hand-over. The server waits for a request, Reno for the response. waitResponse: 1 it came, 0 not within
    // 'ms', -1 the process is gone. waitRequest: true if one came within 'ms' (negative: no limit).
    void postRequest();
    bool waitRequest(int ms);
    void postResponse();
    int waitResponse(int ms);

    void* memory() const { return mem_; }
    ShmHeader* header() const { return (ShmHeader*)mem_; }
    long long processId() const;

private:
    struct Impl;
    Impl* d_;
    void* mem_ = nullptr;
    std::mutex writeMutex_;
};

// Path of the bridge program for this installation (beside the running program); "" if none.
std::string serverPath();

}  // namespace vst32
}  // namespace reno
