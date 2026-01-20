#pragma once

#include <libagents/config.hpp>
#include <libagents/provider.hpp>
#include <string>
#include <unordered_map>

namespace windbg_copilot
{

// BYOK settings for a single provider
struct BYOKSettings
{
    bool enabled = false;
    std::string api_key;
    std::string base_url;
    std::string model;
    std::string provider_type; // "openai", "anthropic", "azure"
    int timeout_ms = 0;

    // Convert to libagents BYOKConfig
    libagents::BYOKConfig to_config() const
    {
        libagents::BYOKConfig config;
        config.api_key = api_key;
        config.base_url = base_url;
        config.model = model;
        config.provider_type = provider_type;
        config.timeout_ms = timeout_ms;
        return config;
    }

    // Check if BYOK is usable (enabled and has API key)
    bool is_usable() const { return enabled && !api_key.empty(); }
};

// Settings stored in ~/.windbg_copilot/settings.json
struct Settings
{
    // Default provider (claude, copilot)
    libagents::ProviderType default_provider = libagents::ProviderType::Copilot;

    // User's custom prompt (additive to system prompt)
    std::string custom_prompt;

    // Response timeout in milliseconds (0 = use default 60s)
    int response_timeout_ms = 120000; // 2 minutes default

    // Session ID mappings (target_path|provider -> session_id)
    std::unordered_map<std::string, std::string> sessions;

    // BYOK (Bring Your Own Key) configuration per provider
    // Key: provider name ("copilot", "claude")
    std::unordered_map<std::string, BYOKSettings> byok;

    // Get BYOK settings for the current provider
    const BYOKSettings* get_byok() const
    {
        std::string provider_name = libagents::provider_type_name(default_provider);
        auto it = byok.find(provider_name);
        if (it != byok.end())
            return &it->second;
        return nullptr;
    }

    // Get or create BYOK settings for the current provider
    BYOKSettings& get_or_create_byok()
    {
        std::string provider_name = libagents::provider_type_name(default_provider);
        return byok[provider_name];
    }
};

// Get the settings directory path (~/.windbg_copilot)
std::string GetSettingsDir();

// Get the settings file path (~/.windbg_copilot/settings.json)
std::string GetSettingsPath();

// Load settings from disk (creates default if not exists)
Settings LoadSettings();

// Save settings to disk
void SaveSettings(const Settings& settings);

// Parse provider type from string (e.g., "claude", "copilot")
libagents::ProviderType ParseProviderType(const std::string& name);

} // namespace windbg_copilot
