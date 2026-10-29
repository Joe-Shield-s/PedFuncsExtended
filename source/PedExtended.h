#pragma once

#include "plugin.h"
#include "PedVariationDef.h"
#include <vector>
#include <list>
#include <cstdint>

constexpr int LEGACY_TEXTURE_LIMIT = 16;

struct MaterialTextureReset {
    RwTexture** pTexturePtr;
    RwTexture* pOriginalTexture;
};

class PedExtended {
public:
    // GTA V component variation state
    int activeDrawable[PV_COMP_COUNT];
    int activeTexture[PV_COMP_COUNT];
    std::string currentSkinTone = "uni"; // e.g. "whi", "lat", "bla", "uni"

    // GTA V prop variation state
    int activeProp[PV_ANCHOR_COUNT];
    int activePropTexture[PV_ANCHOR_COUNT];

    // Legacy PedFuncs texture remap state
    int curRemapNum[LEGACY_TEXTURE_LIMIT];
    int TotalRemapNum[LEGACY_TEXTURE_LIMIT];
    RwTexture* originalRemap[LEGACY_TEXTURE_LIMIT];
    std::list<RwTexture*> remaps[LEGACY_TEXTURE_LIMIT];

    // Flag indicating whether this ped has GTA V variations applied
    bool hasGtaVVariations = false;
    bool variationsInitialized = false;
    int legacyScannedModel = -1; // model whose _remap textures are in remaps[]

    PedExtended(CPed* /*ped*/ = nullptr) {
        Reset();
    }

    void Reset() {
        currentSkinTone = "uni";
        for (int i = 0; i < PV_COMP_COUNT; ++i) {
            activeDrawable[i] = 0;
            activeTexture[i] = 0;
        }
        for (int i = 0; i < PV_ANCHOR_COUNT; ++i) {
            activeProp[i] = -1;
            activePropTexture[i] = 0;
        }
        for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
            curRemapNum[i] = -1;
            TotalRemapNum[i] = 0;
            originalRemap[i] = nullptr;
            remaps[i].clear();
        }
        hasGtaVVariations = false;
        variationsInitialized = false;
        legacyScannedModel = -1;
    }
};

extern plugin::PedExtendedData<PedExtended> extData;
extern std::vector<MaterialTextureReset> g_renderTextureResets;
