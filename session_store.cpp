#include "session_store.hpp"
#include "settings.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace windbg_copilot
{

static std::unique_ptr<SessionStore> g_session_store;

SessionStore& GetSessionStore()
{
    if (!g_session_store)
    {
        g_session_store = std::make_unique<SessionStore>();
        g_session_store->Load();
    }
    return *g_session_store;
}

std::string SessionStore::MakeKey(const std::string& target_name, const std::string& provider)
{
    // Use actual path with provider for human-readable keys
    // Format: "path|provider" (pipe separator unlikely in paths)
    return target_name + "|" + provider;
}

std::string SessionStore::GenerateSessionId()
{
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();

    std::stringstream ss;
    ss << "session_" << std::hex << millis;
    return ss.str();
}

std::string SessionStore::GetSessionId(const std::string& target_name,
                                       const std::string& provider) const
{
    if (target_name.empty() || provider.empty())
        return "";

    std::string key = MakeKey(target_name, provider);
    auto it = sessions_.find(key);
    if (it != sessions_.end())
        return it->second;
    return "";
}

void SessionStore::SetSessionId(const std::string& target_name, const std::string& provider,
                                const std::string& session_id)
{
    if (target_name.empty() || provider.empty())
        return;

    std::string key = MakeKey(target_name, provider);
    sessions_[key] = session_id;
    Save();
}

void SessionStore::ClearSession(const std::string& target_name, const std::string& provider)
{
    if (target_name.empty() || provider.empty())
        return;

    std::string key = MakeKey(target_name, provider);
    sessions_.erase(key);
    Save();
}

void SessionStore::Load()
{
    // Load from settings
    Settings settings = LoadSettings();
    sessions_ = settings.sessions;
}

void SessionStore::Save() const
{
    // Load current settings, update sessions, save back
    Settings settings = LoadSettings();
    settings.sessions = sessions_;
    SaveSettings(settings);
}

} // namespace windbg_copilot
