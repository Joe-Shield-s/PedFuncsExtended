#pragma once

#include "plugin.h"
#include "PedVariationDef.h"
#include "PedExtended.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <fstream>

enum class LogLevel : int { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4 };

// Values from PedFuncsExtended.ini (defaults are used when the file or a key is missing)
struct PluginSettings {
    bool useLog = true;
    LogLevel logLevel = LogLevel::Info;
    bool enableGtaVVariations = true;
    bool randomizeOnSpawn = true;
    bool loadAdditionalPedsTxds = true;
};

class VariationManager {
public:
    static VariationManager& Get() {
        static VariationManager instance;
        return instance;
    }

    void Init();
    void Shutdown();

    // Model names
    void RegisterModelName(int modelIndex, const std::string& name);
    std::string GetModelName(int modelIndex);

    // Resolves and initializes model variation config for a model index
    ModelVariationConfig* GetOrLoadModelConfig(int modelIndex);

    // Apply variations to a newly spawned / model-switched ped
    void ApplyPedVariations(CPed* ped, bool forceRandom = false);

    // Randomize variations obeying GTA V rules
    void RandomizePedVariations(CPed* ped);

    // Set specific component drawable and texture
    bool SetPedComponentVariation(CPed* ped, int compId, int drawableId, int textureId);

    // Set specific prop and texture
    bool SetPedPropVariation(CPed* ped, int anchorId, int propId, int textureId);

    // Skin tone management
    const char* GetPedSkinTone(CPed* ped);
    bool SetPedSkinTone(CPed* ped, const std::string& tone);
    void AlignPedComponentsToSkinTone(CPed* ped, const std::string& tone);

    // Render hooks
    void BeforePedRender(CPed* ped);
    void AfterPedRender(CPed* ped);

    // Legacy PedFuncs texture remap support
    void FindLegacyRemaps(CPed* ped);

    // Settings / paths
    PluginSettings& Settings() { return m_settings; }
    // Directory PedFuncsExtended.asi was loaded from (with trailing slash), e.g. "modloader/PedFuncsExtended/"
    const std::string& PluginDir() const { return m_pluginDir; }
    void SetPluginDir(const std::string& dir) { m_pluginDir = dir; }

    // Logging (PedFuncsExtended.log next to the ASI)
    void Log(const std::string& msg) { Write(LogLevel::Info, msg); }
    void LogError(const std::string& msg) { Write(LogLevel::Error, msg); }
    void LogWarn(const std::string& msg) { Write(LogLevel::Warn, msg); }
    void LogDebug(const std::string& msg) { Write(LogLevel::Debug, msg); }
    bool IsLogging(LogLevel level) const { return m_logFile.is_open() && level <= m_settings.logLevel; }
    void Write(LogLevel level, const std::string& msg);

private:
    VariationManager() = default;

    bool LoadConfigFromYmtXml(const std::string& xmlPath, ModelVariationConfig& config);
    bool AutoDiscoverFromClumpAndTxd(int modelIndex, RpClump* clump, RwTexDictionary* txd, ModelVariationConfig& config);

    std::unordered_map<int, std::string> m_modelNames;
    std::unordered_map<int, ModelVariationConfig> m_modelConfigs;
    PluginSettings m_settings;
    std::string m_pluginDir;
    std::fstream m_logFile;

    void LogModelSummary(const ModelVariationConfig& config);
    void ResetPedToDefaultVariation(CPed* ped, const ModelVariationConfig& config);
};
