#pragma once

#include <string>
#include <unordered_map>

namespace windbg_agent
{

// Stores session IDs mapped to target files and providers
// Persisted in ~/.windbg_agent/settings.json
// Key format: target_path|provider -> session_id (human-readable)
class SessionStore
{
  public:
    // Get session ID for a target+provider (returns empty if not found)
    std::string GetSessionId(const std::string& target_name, const std::string& provider) const;

    // Set session ID for a target+provider (saves to disk)
    void SetSessionId(const std::string& target_name, const std::string& provider,
                      const std::string& session_id);

    // Clear session for a target+provider (removes mapping, saves to disk)
    void ClearSession(const std::string& target_name, const std::string& provider);

    // Load from disk
    void Load();

    // Save to disk
    void Save() const;

    // Generate a new unique session ID
    static std::string GenerateSessionId();

  private:
    // Create composite key from target name and provider
    static std::string MakeKey(const std::string& target_name, const std::string& provider);

    // target_path|provider -> session_id
    std::unordered_map<std::string, std::string> sessions_;
};

// Global session store
SessionStore& GetSessionStore();

} // namespace windbg_agent
