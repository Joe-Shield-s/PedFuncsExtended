#pragma once

#include "plugin.h"
#include <string>

class MultiClumpLoader {
public:
    static MultiClumpLoader& Get() {
        static MultiClumpLoader instance;
        return instance;
    }

    void Init();

    // Merges all atomics from childClump into parentClump, reparenting frames
    void MergeChildClumpIntoParent(RpClump* parentClump, RpClump* childClump);

    // Processes a freshly loaded ped model clump, reading any secondary clumps
    // in the DFF stream and scanning companion modular DFF pieces
    void ProcessLoadedPedClump(RwStream* stream, unsigned int modelIndex);

private:
    MultiClumpLoader() = default;

    void LoadCompanionModularPieces(unsigned int modelIndex, const std::string& modelName, RpClump* parentClump);
};
