#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace windbg_agent
{

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string GetSettingsDir()
{
    // Get user home directory
    const char* home = std::getenv("USERPROFILE");
    if (!home)
        home = std::getenv("HOME");
    if (!home)
        return ".windbg_agent";
    return std::string(home) + "\\.windbg_agent";
}

std::string GetSettingsPath()
{
    return GetSettingsDir() + "\\settings.json";
}

libagents::ProviderType ParseProviderType(const std::string& name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "claude" || lower == "claude-code")
        return libagents::ProviderType::Claude;
    if (lower == "copilot" || lower == "github-copilot")
        return libagents::ProviderType::Copilot;

    throw std::runtime_error("Unknown provider: " + name);
}

Settings LoadSettings()
{
    Settings settings;
    std::string path = GetSettingsPath();

    // Create directory if it doesn't exist
    std::string dir = GetSettingsDir();
    if (!fs::exists(dir))
        fs::create_directories(dir);

    // Load if file exists
    if (fs::exists(path))
    {
        try
        {
            std::ifstream file(path);
            if (file.is_open())
            {
                json j;
                file >> j;

                if (j.contains("default_provider"))
                {
                    std::string provider = j["default_provider"].get<std::string>();
                    try
                    {
                        settings.default_provider = ParseProviderType(provider);
                    }
                    catch (...)
                    {
                        // Keep default if invalid
                    }
                }

                if (j.contains("custom_prompt"))
                    settings.custom_prompt = j["custom_prompt"].get<std::string>();

                if (j.contains("response_timeout_ms"))
                    settings.response_timeout_ms = j["response_timeout_ms"].get<int>();

                if (j.contains("sessions"))
                    for (auto& [key, value] : j["sessions"].items())
                        settings.sessions[key] = value.get<std::string>();

                if (j.contains("byok"))
                {
                    // BYOK is a map of provider_name -> BYOKSettings
                    for (auto& [provider_name, byok_json] : j["byok"].items())
                    {
                        BYOKSettings byok_settings;
                        if (byok_json.contains("enabled"))
                            byok_settings.enabled = byok_json["enabled"].get<bool>();
                        if (byok_json.contains("api_key"))
                            byok_settings.api_key = byok_json["api_key"].get<std::string>();
                        if (byok_json.contains("base_url"))
                            byok_settings.base_url = byok_json["base_url"].get<std::string>();
                        if (byok_json.contains("model"))
                            byok_settings.model = byok_json["model"].get<std::string>();
                        if (byok_json.contains("provider_type"))
                            byok_settings.provider_type =
                                byok_json["provider_type"].get<std::string>();
                        if (byok_json.contains("timeout_ms"))
                            byok_settings.timeout_ms = byok_json["timeout_ms"].get<int>();
                        settings.byok[provider_name] = byok_settings;
                    }
                }
            }
        }
        catch (...)
        {
            // Keep defaults on parse error
        }
    }
    else
    {
        // Create default settings file
        SaveSettings(settings);
    }

    return settings;
}

void SaveSettings(const Settings& settings)
{
    std::string dir = GetSettingsDir();
    if (!fs::exists(dir))
        fs::create_directories(dir);

    json j;
    j["default_provider"] = libagents::provider_type_name(settings.default_provider);
    if (!settings.custom_prompt.empty())
        j["custom_prompt"] = settings.custom_prompt;
    if (settings.response_timeout_ms > 0)
        j["response_timeout_ms"] = settings.response_timeout_ms;
    if (!settings.sessions.empty())
    {
        json sessions_json;
        for (const auto& [key, value] : settings.sessions)
            sessions_json[key] = value;
        j["sessions"] = sessions_json;
    }

    // Save BYOK settings per provider
    if (!settings.byok.empty())
    {
        json byok_map;
        for (const auto& [provider_name, byok_settings] : settings.byok)
        {
            json byok_json;
            byok_json["enabled"] = byok_settings.enabled;
            if (!byok_settings.api_key.empty())
                byok_json["api_key"] = byok_settings.api_key;
            if (!byok_settings.base_url.empty())
                byok_json["base_url"] = byok_settings.base_url;
            if (!byok_settings.model.empty())
                byok_json["model"] = byok_settings.model;
            if (!byok_settings.provider_type.empty())
                byok_json["provider_type"] = byok_settings.provider_type;
            if (byok_settings.timeout_ms > 0)
                byok_json["timeout_ms"] = byok_settings.timeout_ms;
            byok_map[provider_name] = byok_json;
        }
        j["byok"] = byok_map;
    }

    std::ofstream file(GetSettingsPath());
    if (file.is_open())
        file << j.dump(2);
}

} // namespace windbg_agent
