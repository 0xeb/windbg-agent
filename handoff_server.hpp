#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>

namespace windbg_agent {

// Callbacks for handling requests
using ExecCallback = std::function<std::string(const std::string& command)>;
using AskCallback = std::function<std::string(const std::string& query)>;

// Internal command structure for cross-thread execution
struct PendingCommand {
    enum class Type { Exec, Ask };
    Type type;
    std::string input;
    std::string result;
    bool completed = false;
    std::mutex* done_mutex = nullptr;
    std::condition_variable* done_cv = nullptr;
};

struct QueueResult {
    bool success;
    std::string payload;
};

class HandoffServer {
public:
    HandoffServer();
    ~HandoffServer();

    // Non-copyable
    HandoffServer(const HandoffServer&) = delete;
    HandoffServer& operator=(const HandoffServer&) = delete;

    // Start server on given port with callbacks
    // Returns actual port used (may differ if auto-assigned)
    // Callbacks will be called on the main thread (in wait())
    int start(int port, ExecCallback exec_cb, AskCallback ask_cb);

    // Block until server stops, processing commands on the calling thread
    // This is where exec_cb and ask_cb get called
    void wait();

    // Stop the server
    void stop();

    // Check if running
    bool is_running() const { return running_.load(); }

    // Get the port the server is listening on
    int port() const { return port_; }

    // Queue a command for execution on the main thread (called by HTTP handlers)
    QueueResult queue_and_wait(PendingCommand::Type type, const std::string& input);

    // Set interrupt check function (called during wait loop)
    void set_interrupt_check(std::function<bool()> check);

private:
    std::function<bool()> interrupt_check_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    int port_{0};

    // Command queue for cross-thread execution
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<PendingCommand*> pending_commands_;

    // Callbacks stored for main thread execution
    ExecCallback exec_cb_;
    AskCallback ask_cb_;

    // Forward declaration - impl hides httplib
    class Impl;
    std::unique_ptr<Impl> impl_;

    void complete_pending_commands(const std::string& result);
};

// Find a free port starting from start_port
int find_free_port(int start_port = 9999);

// Copy text to Windows clipboard
bool copy_to_clipboard(const std::string& text);

// Format handoff info for display and clipboard
std::string format_handoff_info(
    const std::string& target_name,
    unsigned long pid,
    const std::string& state,
    const std::string& url
);

} // namespace windbg_agent
