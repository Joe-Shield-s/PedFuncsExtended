#include "MultiClumpLoader.h"
#include "VariationManager.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "CPedModelInfo.h"
#include "NodeName.h"
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static bool __cdecl Hooked_LoadClumpFile(RwStream* stream, unsigned int modelIndex) {
    bool result = plugin::CallAndReturn<bool, 0x5372D0, RwStream*, unsigned int>(stream, modelIndex);
    if (result) {
        MultiClumpLoader::Get().ProcessLoadedPedClump(stream, modelIndex);
    }
    return result;
}

static bool __cdecl Hooked_FinishLoadClumpFile(RwStream* stream, unsigned int modelIndex) {
    bool result = plugin::CallAndReturn<bool, 0x537450, RwStream*, unsigned int>(stream, modelIndex);
    if (result) {
        MultiClumpLoader::Get().ProcessLoadedPedClump(stream, modelIndex);
    }
    return result;
}

void MultiClumpLoader::Init() {
    // 0x40C80F: CStreaming::ConvertBufferToObject -> CFileLoader::LoadClumpFile
    plugin::patch::RedirectCall(0x40C80F, Hooked_LoadClumpFile, true);

    // 0x408D34: CStreaming::ProcessLoadingChannel -> CFileLoader::FinishLoadClumpFile
    plugin::patch::RedirectCall(0x408D34, Hooked_FinishLoadClumpFile, true);

    VariationManager::Get().Log("MultiClumpLoader hooks installed at 0x40C80F and 0x408D34");
}

void MultiClumpLoader::MergeChildClumpIntoParent(RpClump* parentClump, RpClump* childClump) {
    if (!parentClump || !childClump || parentClump == childClump) return;

    RwFrame* parentRoot = RpClumpGetFrame(parentClump);
    if (!parentRoot) return;

    std::vector<RpAtomic*> atomicsToMove;
    RpClumpForAllAtomics(childClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto* vec = reinterpret_cast<std::vector<RpAtomic*>*>(data);
        vec->push_back(atomic);
        return atomic;
    }, &atomicsToMove);

    for (RpAtomic* atomic : atomicsToMove) {
        RwFrame* frame = RpAtomicGetFrame(atomic);

        RpClumpRemoveAtomic(childClump, atomic);
        if (frame) {
            RwFrameRemoveChild(frame);
            RwFrameAddChild(parentRoot, frame);
        }
        RpClumpAddAtomic(parentClump, atomic);

        const char* name = frame ? GetFrameNodeName(frame) : "unnamed";
        VariationManager::Get().Log(std::string("Merged atomic into parent clump: ") + (name ? name : "null"));
    }

    RpClumpDestroy(childClump);
}

void MultiClumpLoader::LoadCompanionModularPieces(unsigned int modelIndex, const std::string& modelName, RpClump* parentClump) {
    if (modelName.empty() || !parentClump) return;

    std::string searchDirs[] = {
        "models/" + modelName,
        "PedFuncs/" + modelName,
        "models/generic/" + modelName
    };

    for (const auto& dirPath : searchDirs) {
        std::error_code ec;
        if (fs::exists(dirPath, ec) && fs::is_directory(dirPath, ec)) {
            for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".dff") {
                    std::string dffPath = entry.path().string();
                    RwStream* fileStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, dffPath.c_str());
                    if (fileStream) {
                        if (RwStreamFindChunk(fileStream, rwID_CLUMP, nullptr, nullptr)) {
                            RpClump* childClump = RpClumpStreamRead(fileStream);
                            if (childClump) {
                                VariationManager::Get().Log("Loaded modular companion piece: " + dffPath);
                                MergeChildClumpIntoParent(parentClump, childClump);
                            }
                        }
                        RwStreamClose(fileStream, nullptr);
                    }
                }
            }
        }
    }
}

void MultiClumpLoader::ProcessLoadedPedClump(RwStream* stream, unsigned int modelIndex) {
    CBaseModelInfo* pBaseInfo = CModelInfo::GetModelInfo(modelIndex);
    if (!pBaseInfo || pBaseInfo->GetModelType() != MODEL_INFO_PED) {
        return;
    }

    RpClump* parentClump = pBaseInfo->m_pRwClump;
    if (!parentClump) return;

    std::string modelName = VariationManager::Get().GetModelName(modelIndex);
    VariationManager::Get().Log("ProcessLoadedPedClump: model " + std::to_string(modelIndex) + " (" + modelName + ")");

    // 1. Check if the DFF stream has additional clumps (multi-clump single DFF)
    if (stream) {
        int secondaryClumpCount = 0;
        while (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
            RpClump* childClump = RpClumpStreamRead(stream);
            if (childClump) {
                secondaryClumpCount++;
                VariationManager::Get().Log("Found secondary clump #" + std::to_string(secondaryClumpCount) + " in DFF stream");
                MergeChildClumpIntoParent(parentClump, childClump);
            } else {
                break;
            }
        }
    }

    // 2. Check for modular DFF companion pieces in models/<modelName>/ or PedFuncs/<modelName>/
    LoadCompanionModularPieces(modelIndex, modelName, parentClump);

    // 3. Inform VariationManager to discover and catalog the components
    VariationManager::Get().GetOrLoadModelConfig(modelIndex);
}
