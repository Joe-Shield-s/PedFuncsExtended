#include "plugin.h"
#include "VariationManager.h"
#include "MultiClumpLoader.h"
#include "PedExtended.h"
#include "CCutsceneMgr.h"
#include "CTimer.h"
#include "CStreaming.h"
#include "CKeyGen.h"
#include "CModelInfo.h"
#include "CPedModelInfo.h"
#include "CTxdStore.h"
#include <fstream>
#include <string>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <windows.h>

using namespace plugin;

static unsigned int g_cutsceneRunLastTime = 0;
static bool g_alreadyLoaded = false;
static bool g_txdsNotLoadedYet = true;
static bool g_anyAdditionalPedsTxd = false;
static int g_pedstxdIndexArray[4] = {0, 0, 0, 0};
static RwTexDictionary* g_pedstxdArray[4] = {nullptr, nullptr, nullptr, nullptr};
static int g_gangHandsTxdIndex = 0;
static RwTexDictionary* g_handsDict = nullptr;
static RwTexture* g_handsBlack = nullptr;
static RwTexture* g_handsWhite = nullptr;

static uintptr_t ORIGINAL_AssignRemapTxd = 0;
static uintptr_t ORIGINAL_RwTexDictionaryFindNamedTexture = 0;

static void CustomAssignRemapTxd(const char* txdName, uint16_t txdId) {
    if (txdName) {
        size_t len = strlen(txdName);
        if (len > 1 && isdigit(txdName[len - 1])) {
            if (strncmp(txdName, "peds", 4) == 0) {
                int arrayIndex = txdName[len - 1] - '0' - 1;
                if (arrayIndex >= 0 && arrayIndex < 4) {
                    g_pedstxdIndexArray[arrayIndex] = txdId;
                    CTxdStore::AddRef(g_pedstxdIndexArray[arrayIndex]);
                    g_anyAdditionalPedsTxd = true;
                    VariationManager::Get().Log(std::string("Found additional peds") + std::to_string(arrayIndex + 1) + ".txd");
                }
            }
            if (g_gangHandsTxdIndex == 0 && strncmp(txdName, "ganghands", 9) == 0) {
                g_gangHandsTxdIndex = txdId;
                CTxdStore::AddRef(txdId);
                VariationManager::Get().Log("Found ganghands.txd");
            }
        }
    }
    plugin::CallDynGlobal<const char*, uint16_t>(ORIGINAL_AssignRemapTxd, txdName, txdId);
}

static void LoadAdditionalTxds() {
    g_txdsNotLoadedYet = false;
    if (!VariationManager::Get().Settings().loadAdditionalPedsTxds) return;
    bool anyRequest = false;

    if (g_gangHandsTxdIndex) {
        CStreaming::RequestTxdModel(g_gangHandsTxdIndex, (eStreamingFlags::GAME_REQUIRED | eStreamingFlags::KEEP_IN_MEMORY));
        g_handsDict = reinterpret_cast<RwTexDictionary*(__cdecl*)(int)>(0x408340)(g_gangHandsTxdIndex);
        anyRequest = true;
    }

    if (g_anyAdditionalPedsTxd) {
        for (int i = 0; i < 4; ++i) {
            if (g_pedstxdIndexArray[i]) {
                CStreaming::RequestTxdModel(g_pedstxdIndexArray[i], (eStreamingFlags::GAME_REQUIRED | eStreamingFlags::KEEP_IN_MEMORY));
                VariationManager::Get().Log("Requesting additional peds txd index " + std::to_string(g_pedstxdIndexArray[i]));
                anyRequest = true;
            }
        }
    }

    if (anyRequest) {
        CStreaming::LoadAllRequestedModels(false);
        for (int i = 0; i < 4; ++i) {
            if (g_pedstxdIndexArray[i]) {
                g_pedstxdArray[i] = reinterpret_cast<RwTexDictionary*(__cdecl*)(int)>(0x408340)(g_pedstxdIndexArray[i]);
            }
        }
    }
}

static RwTexture* __cdecl Custom_RwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const char* name) {
    RwTexture* texture = plugin::CallAndReturnDynGlobal<RwTexture*, RwTexDictionary*, const char*>(ORIGINAL_RwTexDictionaryFindNamedTexture, dict, name);
    if (texture) return texture;

    if (g_anyAdditionalPedsTxd) {
        for (int i = 0; i < 4; ++i) {
            if (g_pedstxdArray[i]) {
                texture = RwTexDictionaryFindNamedTexture(g_pedstxdArray[i], name);
                if (texture) return texture;
            }
        }
    }

    if (g_txdsNotLoadedYet && g_anyAdditionalPedsTxd) {
        LoadAdditionalTxds();
    }
    return nullptr;
}

// Directory of this ASI (with trailing slash, relative to the game folder when possible), so the INI
// and log live next to PedFuncsExtended.asi whether it sits in the game root or in modloader/<folder>/.
static std::string GetPluginDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&GetPluginDirectory), &self);
    char path[MAX_PATH] = {};
    if (!self || !GetModuleFileNameA(self, path, MAX_PATH)) return "";
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);

    char cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryA(MAX_PATH, cwd)) {
        std::string root(cwd);
        if (!root.empty() && root.back() != '\\' && root.back() != '/') root += '\\';
        if (_strnicmp(dir.c_str(), root.c_str(), root.size()) == 0) dir = dir.substr(root.size());
    }
    return dir;
}

static bool ParseBool(const std::string& v) {
    return v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes";
}

// Simple INI parser. Returns the path that was read, or empty if none was found.
static std::string LoadConfiguration() {
    PluginSettings& cfg = VariationManager::Get().Settings();
    std::string iniPath = VariationManager::Get().PluginDir() + "PedFuncsExtended.ini";
    std::ifstream iniFile(iniPath);
    if (!iniFile.is_open()) {
        iniPath = "PedFuncsExtended.ini";
        iniFile.open(iniPath);
    }
    if (!iniFile.is_open()) return "";

    std::string line;
    std::string currentSection;
    while (std::getline(iniFile, line)) {
        size_t commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);

        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);

        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = line.substr(0, eqPos);
            std::string val = line.substr(eqPos + 1);

            auto trim = [](std::string& s) {
                size_t p1 = s.find_first_not_of(" \t\r\n");
                size_t p2 = s.find_last_not_of(" \t\r\n");
                s = (p1 == std::string::npos) ? "" : s.substr(p1, p2 - p1 + 1);
            };
            trim(key);
            trim(val);

            if (currentSection == "Settings") {
                if (key == "UseLog") cfg.useLog = ParseBool(val);
                else if (key == "LogLevel") cfg.logLevel = static_cast<LogLevel>(std::clamp(std::atoi(val.c_str()), 0, 4));
                else if (key == "EnableGtaVVariations") cfg.enableGtaVVariations = ParseBool(val);
                else if (key == "RandomizeOnSpawn") cfg.randomizeOnSpawn = ParseBool(val);
                else if (key == "LoadAdditionalPedsTxds") cfg.loadAdditionalPedsTxds = ParseBool(val);
            }
        }
    }
    return iniPath;
}

class PedFuncsExtendedPlugin {
public:
    PedFuncsExtendedPlugin() {
        VariationManager& vm = VariationManager::Get();
        vm.SetPluginDir(GetPluginDirectory());
        std::string iniPath = LoadConfiguration();
        vm.Init();

        const PluginSettings& cfg = vm.Settings();
        if (iniPath.empty()) vm.LogWarn("PedFuncsExtended.ini not found next to the ASI or in the game folder, using defaults");
        else vm.Log("Config: " + iniPath);
        vm.Log(std::string("Settings: LogLevel=") + std::to_string(static_cast<int>(cfg.logLevel)) +
               " EnableGtaVVariations=" + std::to_string(cfg.enableGtaVVariations) +
               " RandomizeOnSpawn=" + std::to_string(cfg.randomizeOnSpawn) +
               " LoadAdditionalPedsTxds=" + std::to_string(cfg.loadAdditionalPedsTxds));
        // Original PedFuncs (or an old build of this plugin named PedFuncs.asi) hooks the same addresses.
        if (GetModuleHandleA("PedFuncs.asi")) {
            vm.LogError("PedFuncs.asi is also loaded! It patches the same code as PedFuncs Extended; remove it "
                        "(PedFuncsExtended already includes all of its features).");
        }

        Events::initRwEvent += [] {
            memset(g_pedstxdArray, 0, sizeof(g_pedstxdArray));
            memset(g_pedstxdIndexArray, 0, sizeof(g_pedstxdIndexArray));

            ORIGINAL_AssignRemapTxd = patch::GetUInt(0x5B62C2 + 1, true);
            ORIGINAL_AssignRemapTxd += (plugin::GetGlobalAddress(0x5B62C2) + 5);
            patch::RedirectCall(0x5B62C2, CustomAssignRemapTxd, true);

            ORIGINAL_RwTexDictionaryFindNamedTexture = patch::GetUInt(0x4C7533 + 1, true);
            ORIGINAL_RwTexDictionaryFindNamedTexture += (plugin::GetGlobalAddress(0x4C7533) + 5);
            patch::RedirectCall(0x4C7533, Custom_RwTexDictionaryFindNamedTexture, true);
            patch::RedirectCall(0x731733, Custom_RwTexDictionaryFindNamedTexture, true);

            MultiClumpLoader::Get().Init();
        };

        Events::initGameEvent += [] {
            if (!g_alreadyLoaded) {
                g_alreadyLoaded = true;
                if (g_txdsNotLoadedYet && VariationManager::Get().Settings().loadAdditionalPedsTxds) LoadAdditionalTxds();

                if (g_handsDict) {
                    g_handsBlack = RwTexDictionaryFindHashNamedTexture(g_handsDict, CKeyGen::GetUppercaseKey("hands_black"));
                    g_handsWhite = RwTexDictionaryFindHashNamedTexture(g_handsDict, CKeyGen::GetUppercaseKey("hands_white"));

                    if (g_handsBlack && g_handsWhite) {
                        injector::MakeInline<0x59EF79, 0x59EF82>([](injector::reg_pack& regs) {
                            CPed* ped = *reinterpret_cast<CPed**>(regs.esp + 0x1C + 0x8);
                            CPedModelInfo* pedModelInfo = reinterpret_cast<CPedModelInfo*>(CModelInfo::GetModelInfo(ped->m_nModelIndex));

                            if (pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG1 || pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG2) {
                                regs.eax = reinterpret_cast<uint32_t>(g_handsBlack);
                            } else {
                                regs.eax = reinterpret_cast<uint32_t>(g_handsWhite);
                            }
                        });
                    }
                }
            }
        };

        Events::processScriptsEvent += [] {
            if (CCutsceneMgr::ms_running) {
                g_cutsceneRunLastTime = CTimer::m_snTimeInMilliseconds;
            }
        };

        Events::pedSetModelEvent += [](CPed* ped, int /*model*/) {
            if ((CTimer::m_snTimeInMilliseconds - g_cutsceneRunLastTime) > 3000) {
                VariationManager::Get().ApplyPedVariations(ped, true);
            }
        };

        Events::pedRenderEvent.before += [](CPed* ped) {
            VariationManager::Get().BeforePedRender(ped);
        };

        Events::pedRenderEvent.after += [](CPed* ped) {
            VariationManager::Get().AfterPedRender(ped);
        };
    }

    ~PedFuncsExtendedPlugin() {
        VariationManager::Get().Shutdown();
    }
} g_pedFuncsExtendedPlugin;

// ==============================================================================
// Script & DLL Exports
// ==============================================================================

extern "C" {
    // GTA V Component Variation Exports
    __declspec(dllexport) int32_t Ext_GetPedComponentDrawable(CPed* ped, int compId) {
        if (!ped || compId < 0 || compId >= PV_COMP_COUNT) return 0;
        PedExtended& ext = extData.Get(ped);
        return ext.activeDrawable[compId];
    }

    __declspec(dllexport) int32_t Ext_GetPedComponentTexture(CPed* ped, int compId) {
        if (!ped || compId < 0 || compId >= PV_COMP_COUNT) return 0;
        PedExtended& ext = extData.Get(ped);
        return ext.activeTexture[compId];
    }

    __declspec(dllexport) bool Ext_SetPedComponentVariation(CPed* ped, int compId, int drawableId, int textureId) {
        if (!ped || compId < 0 || compId >= PV_COMP_COUNT) return false;
        return VariationManager::Get().SetPedComponentVariation(ped, compId, drawableId, textureId);
    }

    __declspec(dllexport) int32_t Ext_GetPedPropIndex(CPed* ped, int anchorId) {
        if (!ped || anchorId < 0 || anchorId >= PV_ANCHOR_COUNT) return -1;
        PedExtended& ext = extData.Get(ped);
        return ext.activeProp[anchorId];
    }

    __declspec(dllexport) int32_t Ext_GetPedPropTexture(CPed* ped, int anchorId) {
        if (!ped || anchorId < 0 || anchorId >= PV_ANCHOR_COUNT) return 0;
        PedExtended& ext = extData.Get(ped);
        return ext.activePropTexture[anchorId];
    }

    __declspec(dllexport) bool Ext_SetPedPropVariation(CPed* ped, int anchorId, int propId, int textureId) {
        if (!ped || anchorId < 0 || anchorId >= PV_ANCHOR_COUNT) return false;
        return VariationManager::Get().SetPedPropVariation(ped, anchorId, propId, textureId);
    }

    __declspec(dllexport) void Ext_RandomizePedVariations(CPed* ped) {
        if (!ped) return;
        VariationManager::Get().RandomizePedVariations(ped);
        VariationManager::Get().ApplyPedVariations(ped, false);
    }

    __declspec(dllexport) bool Ext_PedHasVariations(CPed* ped) {
        if (!ped) return false;
        PedExtended& ext = extData.Get(ped);
        return ext.hasGtaVVariations;
    }

    __declspec(dllexport) const char* Ext_GetPedSkinTone(CPed* ped) {
        if (!ped) return "uni";
        return VariationManager::Get().GetPedSkinTone(ped);
    }

    __declspec(dllexport) bool Ext_SetPedSkinTone(CPed* ped, const char* skinTone) {
        if (!ped || !skinTone) return false;
        return VariationManager::Get().SetPedSkinTone(ped, skinTone);
    }

    // Legacy PedFuncs Exports (100% backward compatible)
    __declspec(dllexport) int32_t Ext_GetPedRemap(CPed* ped, int index) {
        if (!ped || index < 0 || index >= LEGACY_TEXTURE_LIMIT) return -1;
        PedExtended& ext = extData.Get(ped);
        return ext.curRemapNum[index];
    }

    __declspec(dllexport) void Ext_SetPedRemap(CPed* ped, int index, int num) {
        if (!ped || index < 0 || index >= LEGACY_TEXTURE_LIMIT) return;
        PedExtended& ext = extData.Get(ped);
        ext.curRemapNum[index] = num;
    }

    __declspec(dllexport) void Ext_SetAllPedRemaps(CPed* ped, int num) {
        if (!ped) return;
        PedExtended& ext = extData.Get(ped);
        for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
            ext.curRemapNum[i] = num;
        }
    }
}
