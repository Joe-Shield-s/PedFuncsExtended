#include "VariationManager.h"
#include "XmlParser.h"
#include "NodeName.h"
#include "CModelInfo.h"
#include "CPedModelInfo.h"
#include "CTxdStore.h"
#include "CGeneral.h"
#include "CTimer.h"
#include "CKeyGen.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <set>
#include <map>

std::vector<MaterialTextureReset> g_renderTextureResets;
plugin::PedExtendedData<PedExtended> extData;

void VariationManager::Init() {
    if (m_settings.useLog && m_settings.logLevel != LogLevel::Off) {
        m_logFile.open(m_pluginDir + "PedFuncsExtended.log", std::ios::out | std::ios::trunc);
        Log("PedFuncs Extended v1.0.0 initialized (GTA V ped variation architecture)");
        Log("Plugin directory: '" + (m_pluginDir.empty() ? std::string(".") : m_pluginDir) + "'");
    }
}

void VariationManager::Shutdown() {
    if (m_logFile.is_open()) {
        m_logFile.close();
    }
}

void VariationManager::Write(LogLevel level, const std::string& msg) {
    if (!IsLogging(level)) return;
    static const char* tags[] = {"", "ERROR", "WARN ", "INFO ", "DEBUG"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[48];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d [%7u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             CTimer::m_snTimeInMilliseconds);
    // Flush every line so the log is complete even if the game crashes right after.
    m_logFile << stamp << tags[static_cast<int>(level)] << " " << msg << std::endl;
}

static bool ParseU32(const std::string& text, uint32_t& out) {
    try {
        out = static_cast<uint32_t>(std::stoul(text, nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

void VariationManager::LogModelSummary(const ModelVariationConfig& config) {
    if (!IsLogging(LogLevel::Info)) return;
    std::string tones;
    for (const auto& t : config.availableSkinTones) tones += t + " ";
    Log("Model " + std::to_string(config.modelIndex) + " (" + config.modelName + "): skin tones [" +
        (tones.empty() ? std::string("uni") : tones.substr(0, tones.size() - 1)) + "]");
    for (int c = 0; c < PV_COMP_COUNT; ++c) {
        const ComponentSlot& slot = config.components[c];
        if (!slot.isAvailable && slot.drawables.empty()) continue;
        for (const auto& dv : slot.drawables) {
            std::string texList;
            int missing = 0;
            for (const auto& tv : dv.textures) {
                if (tv.pRwTexture) texList += tv.name + " ";
                else ++missing;
            }
            std::string line = std::string("  ") + GetComponentPrefix(c) + "[" + std::to_string(dv.drawableIndex) + "] '" +
                               dv.drawableName + "' atomics=" + std::to_string(dv.atomicIndices.size()) +
                               " textures: " + (texList.empty() ? std::string("(none) ") : texList);
            if (missing) line += "(+" + std::to_string(missing) + " listed in XML but not in TXD)";
            if (dv.atomicIndices.empty()) {
                LogWarn(line + " <- no atomic named " + GetComponentPrefix(c) + "_" + (dv.drawableIndex < 10 ? "00" : dv.drawableIndex < 100 ? "0" : "") +
                        std::to_string(dv.drawableIndex) + "_* in the DFF, drawable can never show");
            } else {
                Log(line);
            }
        }
    }
}

void VariationManager::RegisterModelName(int modelIndex, const std::string& name) {
    m_modelNames[modelIndex] = name;
}

// Model names are not kept by SA (only the CKeyGen hash in CBaseModelInfo::m_nKey), so read them
// from data/peds.ide ("id, name, txd, type, ...") and verify each against the hash. Peds whose IDE
// lives elsewhere (modloader / limit adjusters) are matched by hashing the names of variation XMLs.
static std::string TrimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string VariationManager::GetModelName(int modelIndex) {
    auto it = m_modelNames.find(modelIndex);
    if (it != m_modelNames.end()) return it->second;

    CBaseModelInfo* info = CModelInfo::GetModelInfo(modelIndex);
    const unsigned int key = info ? info->m_nKey : 0;

    static bool pedsIdeParsed = false;
    if (!pedsIdeParsed) {
        pedsIdeParsed = true;
        std::ifstream ide("data/peds.ide");
        std::string line;
        bool inPeds = false;
        while (ide.is_open() && std::getline(ide, line)) {
            size_t c = line.find_first_of("#;");
            if (c != std::string::npos) line = line.substr(0, c);
            line = TrimCopy(line);
            if (line.empty()) continue;
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "peds") { inPeds = true; continue; }
            if (lower == "end") { inPeds = false; continue; }
            if (!inPeds) continue;
            size_t c1 = line.find(','), c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
            if (c1 == std::string::npos) continue;
            int id = std::atoi(line.substr(0, c1).c_str());
            std::string name = TrimCopy(line.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
            CBaseModelInfo* mi = id > 0 ? CModelInfo::GetModelInfo(id) : nullptr;
            if (!name.empty() && mi && mi->m_nKey == CKeyGen::GetUppercaseKey(name.c_str())) m_modelNames[id] = name;
        }
        LogDebug("peds.ide: " + std::to_string(m_modelNames.size()) + " ped model names verified against model keys");
    }
    it = m_modelNames.find(modelIndex);
    if (it != m_modelNames.end()) return it->second;

    if (key) {
        auto tryName = [&](std::string file) -> bool {
            for (const char* ext : {".ymt.xml", ".xml"}) {   // longest suffix first
                size_t n = strlen(ext);
                if (file.size() > n && _stricmp(file.c_str() + file.size() - n, ext) == 0) {
                    std::string name = file.substr(0, file.size() - n);
                    if (CKeyGen::GetUppercaseKey(name.c_str()) == key) { m_modelNames[modelIndex] = name; return true; }
                    return false;
                }
            }
            return false;
        };
        auto scan = [&](const std::string& pattern) -> bool {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            bool hit = false;
            if (h == INVALID_HANDLE_VALUE) return false;
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && tryName(fd.cFileName)) hit = true; }
            while (!hit && FindNextFileA(h, &fd));
            FindClose(h);
            return hit;
        };
        bool found = scan("*.xml") || scan("models/*.xml") || scan("PedFuncs/*.xml");
        if (!found) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA("modloader/*", &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                        found = scan(std::string("modloader/") + fd.cFileName + "/*.xml");
                } while (!found && FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        it = m_modelNames.find(modelIndex);
        if (it != m_modelNames.end()) {
            LogDebug("Model " + std::to_string(modelIndex) + " named '" + it->second + "' by matching its key to a variation XML");
            return it->second;
        }
    }
    return "ped_" + std::to_string(modelIndex);
}

bool VariationManager::LoadConfigFromYmtXml(const std::string& xmlPath, ModelVariationConfig& config) {
    XmlLite::XmlNode root;
    if (!XmlLite::LoadXmlFromFile(xmlPath, root)) {
        return false;
    }

    Log("Loading GTA V variation XML: " + xmlPath);
    config.hasVariations = true;
    config.hasTexVariations = true;
    config.hasDrawblVariations = true;

    // 1. Parse availComp string (e.g. 00FF010203FFFF0405FF06FF)
    const XmlLite::XmlNode* pAvailComp = root.FindChild("availComp");
    std::string availCompStr = pAvailComp ? pAvailComp->text : "";
    std::vector<int> compDataIndices(PV_COMP_COUNT, -1);

    if (availCompStr.length() >= (size_t)PV_COMP_COUNT * 2) {
        for (int i = 0; i < PV_COMP_COUNT; ++i) {
            std::string byteHex = availCompStr.substr(i * 2, 2);
            uint32_t dataIdx = 0xFF;
            if (!ParseU32("0x" + byteHex, dataIdx)) LogWarn(xmlPath + ": bad availComp byte '" + byteHex + "'");
            if (dataIdx != 0xFF) {
                config.components[i].isAvailable = true;
                compDataIndices[i] = static_cast<int>(dataIdx);
            }
        }
    }

    // 2. Parse aComponentData3
    const XmlLite::XmlNode* pCompData = root.FindChild("aComponentData3");
    if (pCompData) {
        auto compItems = pCompData->FindChildren("Item");
        for (int compId = 0; compId < PV_COMP_COUNT; ++compId) {
            int dataIdx = compDataIndices[compId];
            if (dataIdx >= 0 && dataIdx < (int)compItems.size()) {
                const XmlLite::XmlNode* item = compItems[dataIdx];
                const XmlLite::XmlNode* pDrawblData = item->FindChild("aDrawblData3");
                if (pDrawblData) {
                    auto drawblItems = pDrawblData->FindChildren("Item");
                    for (int dIdx = 0; dIdx < (int)drawblItems.size(); ++dIdx) {
                        DrawableVariation dv;
                        dv.drawableIndex = dIdx;
                        const XmlLite::XmlNode* pPropMask = drawblItems[dIdx]->FindChild("propMask");
                        dv.propMask = pPropMask ? pPropMask->GetAttrInt("value", 0) : 0;

                        const XmlLite::XmlNode* pTexData = drawblItems[dIdx]->FindChild("aTexData");
                        if (pTexData) {
                            auto texItems = pTexData->FindChildren("Item");
                            for (int tIdx = 0; tIdx < (int)texItems.size(); ++tIdx) {
                                TextureVariation tv;
                                tv.texIndex = tIdx;
                                const XmlLite::XmlNode* pDist = texItems[tIdx]->FindChild("distribution");
                                tv.distribution = pDist ? (uint8_t)pDist->GetAttrInt("value", 255) : 255;
                                dv.textures.push_back(tv);
                            }
                        }
                        config.components[compId].drawables.push_back(dv);
                    }
                }
            }
        }
    }

    // 3. Parse compInfos for inclusions and exclusions
    const XmlLite::XmlNode* pCompInfos = root.FindChild("compInfos");
    if (pCompInfos) {
        for (const auto* item : pCompInfos->FindChildren("Item")) {
            const XmlLite::XmlNode* pCIdx = item->FindChild("pedXml_compIdx");
            const XmlLite::XmlNode* pDIdx = item->FindChild("pedXml_drawblIdx");
            const XmlLite::XmlNode* pExcl = item->FindChild("exclusions");
            const XmlLite::XmlNode* pIncl = item->FindChild("inclusions");

            if (pCIdx && pDIdx) {
                int cIdx = pCIdx->GetAttrInt("value", -1);
                int dIdx = pDIdx->GetAttrInt("value", -1);
                if (cIdx >= 0 && cIdx < PV_COMP_COUNT && dIdx >= 0 && dIdx < (int)config.components[cIdx].drawables.size()) {
                    auto& dv = config.components[cIdx].drawables[dIdx];
                    if (pExcl && !ParseU32(pExcl->text, dv.exclusions))
                        LogWarn(xmlPath + ": bad <exclusions> value '" + pExcl->text + "'");
                    if (pIncl && !ParseU32(pIncl->text, dv.inclusions))
                        LogWarn(xmlPath + ": bad <inclusions> value '" + pIncl->text + "'");
                }
            }
        }
    }

    // 4. Parse propInfo
    const XmlLite::XmlNode* pPropInfo = root.FindChild("propInfo");
    if (pPropInfo) {
        const XmlLite::XmlNode* pPropMeta = pPropInfo->FindChild("aPropMetaData");
        if (pPropMeta) {
            for (const auto* item : pPropMeta->FindChildren("Item")) {
                const XmlLite::XmlNode* pAnchor = item->FindChild("anchorId");
                const XmlLite::XmlNode* pPropId = item->FindChild("propId");
                if (pAnchor && pPropId) {
                    int anchor = pAnchor->GetAttrInt("value", 0);
                    int prop = pPropId->GetAttrInt("value", 0);
                    if (anchor >= 0 && anchor < PV_ANCHOR_COUNT) {
                        PropVariation pv;
                        pv.anchorId = anchor;
                        pv.propId = prop;
                        const XmlLite::XmlNode* pTexData = item->FindChild("texData");
                        if (pTexData) {
                            for (const auto* tItem : pTexData->FindChildren("Item")) {
                                TextureVariation tv;
                                const XmlLite::XmlNode* pDist = tItem->FindChild("distribution");
                                tv.distribution = pDist ? (uint8_t)pDist->GetAttrInt("value", 255) : 255;
                                pv.textures.push_back(tv);
                            }
                        }
                        config.props[anchor].push_back(pv);
                    }
                }
            }
        }
    }

    return true;
}

bool VariationManager::AutoDiscoverFromClumpAndTxd(int modelIndex, RpClump* clump, RwTexDictionary* pTxd, ModelVariationConfig& config) {
    if (!clump) return false;

    config.modelIndex = modelIndex;
    config.modelName = GetModelName(modelIndex);

    struct AtomicDiscoveryInfo {
        int atomicIndex = 0;
        std::string frameName;
        int compId = -1;
        int drawableIdx = 0;
    };

    std::vector<AtomicDiscoveryInfo> discoveredAtomics;
    int curAtomicIdx = 0;

    RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto* pList = reinterpret_cast<std::vector<AtomicDiscoveryInfo>*>(data);
        RwFrame* frame = RpAtomicGetFrame(atomic);
        const char* name = nullptr;

        // Try getting frame NodeName plugin if available
        if (frame) {
            name = GetFrameNodeName(frame);
        }

        std::string sName = name ? name : "";
        int compId = GetComponentFromPrefix(sName);

        if (compId >= 0) {
            // Parse drawable index from name: e.g. "head_001_r" -> 1
            int dIdx = 0;
            size_t firstUnderscore = sName.find('_');
            if (firstUnderscore != std::string::npos && firstUnderscore + 1 < sName.length()) {
                try {
                    dIdx = std::stoi(sName.substr(firstUnderscore + 1));
                } catch (...) {
                    dIdx = 0;
                }
            }
            pList->push_back({(int)pList->size(), sName, compId, dIdx});
        } else {
            pList->push_back({(int)pList->size(), sName, -1, 0});
        }
        return atomic;
    }, &discoveredAtomics);

    bool anyGtaVComponents = false;
    for (const auto& info : discoveredAtomics) {
        if (info.compId >= 0) {
            anyGtaVComponents = true;
            ComponentSlot& slot = config.components[info.compId];
            slot.isAvailable = true;

            // Ensure drawable exists
            while ((int)slot.drawables.size() <= info.drawableIdx) {
                DrawableVariation dv;
                dv.drawableIndex = (int)slot.drawables.size();
                slot.drawables.push_back(dv);
            }
            slot.drawables[info.drawableIdx].drawableName = info.frameName;
            slot.drawables[info.drawableIdx].atomicIndices.push_back(info.atomicIndex);
        }
    }

    if (!anyGtaVComponents) {
        return false;
    }

    config.hasVariations = true;
    config.hasDrawblVariations = true;

    // Discover textures in TXD matching component drawables
    if (pTxd) {
        RwLinkList* objectList = &pTxd->texturesInDict;
        if (!rwLinkListEmpty(objectList)) {
            RwLLLink* current = rwLinkListGetFirstLLLink(objectList);
            RwLLLink* end = rwLinkListGetTerminator(objectList);

            std::map<std::pair<int, int>, std::vector<RwTexture*>> discoveredTextures;

            while (current != end) {
                RwTexture* texture = rwLLLinkGetData(current, RwTexture, lInDictionary);
                if (texture && texture->name[0] != '\0') {
                    std::string texName = texture->name;
                    int compId = GetComponentFromPrefix(texName);
                    if (compId >= 0) {
                        // e.g. "head_diff_001_a_lat"
                        size_t diffPos = texName.find("_diff_");
                        if (diffPos != std::string::npos) {
                            try {
                                int dIdx = std::stoi(texName.substr(diffPos + 6));
                                discoveredTextures[{compId, dIdx}].push_back(texture);
                            } catch (...) {}
                        }
                    }
                }
                current = rwLLLinkGetNext(current);
            }

            for (auto& pair : discoveredTextures) {
                int compId = pair.first.first;
                int dIdx = pair.first.second;
                auto& texList = pair.second;

                // Sort textures alphabetically so _a_, _b_, _c_ variants are in sequential order
                std::sort(texList.begin(), texList.end(), [](RwTexture* a, RwTexture* b) {
                    return std::string(a->name) < std::string(b->name);
                });

                ComponentSlot& slot = config.components[compId];
                while ((int)slot.drawables.size() <= dIdx) {
                    DrawableVariation dv;
                    dv.drawableIndex = (int)slot.drawables.size();
                    slot.drawables.push_back(dv);
                }

                DrawableVariation& dv = slot.drawables[dIdx];
                if (dv.textures.empty()) {
                    for (size_t i = 0; i < texList.size(); ++i) {
                        TextureVariation tv;
                        tv.name = texList[i]->name;
                        tv.texIndex = (int)i;
                        tv.pRwTexture = texList[i];
                        tv.skinTone = ParseSkinToneFromTextureName(tv.name);
                        tv.distribution = 255;
                        dv.textures.push_back(tv);
                    }
                } else {
                    for (size_t i = 0; i < texList.size(); ++i) {
                        if (i < dv.textures.size()) {
                            dv.textures[i].name = texList[i]->name;
                            dv.textures[i].pRwTexture = texList[i];
                            dv.textures[i].skinTone = ParseSkinToneFromTextureName(dv.textures[i].name);
                        } else {
                            TextureVariation tv;
                            tv.name = texList[i]->name;
                            tv.texIndex = (int)i;
                            tv.pRwTexture = texList[i];
                            tv.skinTone = ParseSkinToneFromTextureName(tv.name);
                            tv.distribution = 255;
                            dv.textures.push_back(tv);
                        }
                    }
                    if (dv.textures.size() > texList.size()) {
                        dv.textures.resize(texList.size());
                    }
                }
                config.hasTexVariations = true;
            }
        }
    }

    // Collect available skin tones (ethnicity) for this model
    std::set<std::string> uniqueTones;
    const ComponentSlot& headSlot = config.components[PV_COMP_HEAD];
    for (const auto& dv : headSlot.drawables) {
        for (const auto& tv : dv.textures) {
            if (tv.skinTone != "uni" && !tv.skinTone.empty()) {
                uniqueTones.insert(tv.skinTone);
            }
        }
    }
    if (uniqueTones.empty()) {
        for (int c = 0; c < PV_COMP_COUNT; ++c) {
            for (const auto& dv : config.components[c].drawables) {
                for (const auto& tv : dv.textures) {
                    if (tv.skinTone != "uni" && !tv.skinTone.empty()) {
                        uniqueTones.insert(tv.skinTone);
                    }
                }
            }
        }
    }
    config.availableSkinTones.assign(uniqueTones.begin(), uniqueTones.end());

    std::string toneStr;
    for (const auto& t : config.availableSkinTones) toneStr += t + " ";
    LogDebug("Auto-discovered GTA V variations for model " + std::to_string(modelIndex) + " (" + config.modelName +
        "), Skin tones: [" + (toneStr.empty() ? "uni" : toneStr) + "]");
    return true;
}

ModelVariationConfig* VariationManager::GetOrLoadModelConfig(int modelIndex) {
    if (!m_settings.enableGtaVVariations) return nullptr;
    auto it = m_modelConfigs.find(modelIndex);
    if (it != m_modelConfigs.end()) {
        return &it->second;
    }

    ModelVariationConfig config;
    config.modelIndex = modelIndex;
    config.modelName = GetModelName(modelIndex);

    CBaseModelInfo* pBaseInfo = CModelInfo::GetModelInfo(modelIndex);
    if (!pBaseInfo) return nullptr;

    RpClump* clump = pBaseInfo->m_pRwClump;
    RwTexDictionary* pTxd = nullptr;

    if (pBaseInfo->m_nTxdIndex >= 0 && CTxdStore::ms_pTxdPool) {
        pTxd = CTxdStore::ms_pTxdPool->m_pObjects[pBaseInfo->m_nTxdIndex].m_pRwDictionary;
    }

    // Try finding <modelName>.xml (older conversions named it <modelName>.ymt.xml; still accepted)
    std::string xmlPaths[] = {
        config.modelName + ".xml",
        config.modelName + ".ymt.xml",
        "models/" + config.modelName + ".xml",
        "models/" + config.modelName + ".ymt.xml",
        "models/generic/" + config.modelName + ".xml",
        "models/generic/" + config.modelName + ".ymt.xml",
        "PedFuncs/" + config.modelName + ".xml",
        "PedFuncs/" + config.modelName + ".ymt.xml"
    };

    bool loadedXml = false;
    for (const auto& path : xmlPaths) {
        if (LoadConfigFromYmtXml(path, config)) {
            loadedXml = true;
            break;
        }
    }

    if (!loadedXml) {
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA("modloader/*", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                    std::string sub = std::string("modloader/") + fd.cFileName + "/" + config.modelName;
                    if (LoadConfigFromYmtXml(sub + ".xml", config) || LoadConfigFromYmtXml(sub + ".ymt.xml", config)) {
                        loadedXml = true;
                        break;
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    if (!loadedXml) {
        LogDebug("Model " + std::to_string(modelIndex) + " (" + config.modelName + "): no " + config.modelName +
                 ".xml found, using DFF/TXD auto-discovery only");
    }

    // Auto-discover/link atomics and textures from the DFF and TXD
    bool discovered = AutoDiscoverFromClumpAndTxd(modelIndex, clump, pTxd, config);
    if (loadedXml && !discovered) {
        LogWarn("Model " + std::to_string(modelIndex) + " (" + config.modelName +
                "): variation XML found but the DFF has no component atomics (head_000_*, uppr_000_*, ...). "
                "Was the DFF converted with tools/gtav_to_sa.py?");
    }
    if (config.hasVariations) {
        LogModelSummary(config);
        // Materials whose texture did not resolve render untextured and cannot take variations.
        std::pair<VariationManager*, std::string> ctx{this, config.modelName};
        RpClumpForAllAtomics(clump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
            auto* c = reinterpret_cast<std::pair<VariationManager*, std::string>*>(data);
            int missing = 0;
            if (atomic->geometry) {
                RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial* m, void* d) -> RpMaterial* {
                    if (!m->texture) ++*reinterpret_cast<int*>(d);
                    return m;
                }, &missing);
            }
            if (missing) {
                RwFrame* frame = RpAtomicGetFrame(atomic);
                const char* name = frame ? GetFrameNodeName(frame) : nullptr;
                c->first->LogWarn(c->second + ": atomic '" + (name ? name : "?") + "' has " + std::to_string(missing) +
                                  " material(s) whose texture is not in the TXD");
            }
            return atomic;
        }, &ctx);
    }
    if (!clump) LogWarn("Model " + std::to_string(modelIndex) + ": model info has no clump yet");
    if (!pTxd) LogWarn("Model " + std::to_string(modelIndex) + " (" + config.modelName + "): TXD not loaded, texture variations unavailable");

    m_modelConfigs[modelIndex] = std::move(config);
    return &m_modelConfigs[modelIndex];
}

void VariationManager::ApplyPedVariations(CPed* ped, bool forceRandom) {
    if (!ped || !ped->m_pRwClump) return;

    PedExtended& ext = extData.Get(ped);
    ModelVariationConfig* pConfig = GetOrLoadModelConfig(ped->m_nModelIndex);

    if (!pConfig || !pConfig->hasVariations) {
        // Fallback to legacy PedFuncs remaps
        FindLegacyRemaps(ped);
        return;
    }

    ext.hasGtaVVariations = true;

    if (!ext.variationsInitialized || forceRandom) {
        if (m_settings.randomizeOnSpawn) {
            RandomizePedVariations(ped);
        } else {
            ResetPedToDefaultVariation(ped, *pConfig);
        }
        if (IsLogging(LogLevel::Debug)) {
            std::string desc;
            for (int c = 0; c < PV_COMP_COUNT; ++c) {
                if (!pConfig->components[c].isAvailable || pConfig->components[c].drawables.empty()) continue;
                desc += std::string(GetComponentPrefix(c)) + "=" + std::to_string(ext.activeDrawable[c]) + "/" +
                        std::to_string(ext.activeTexture[c]) + " ";
            }
            LogDebug("Ped " + std::to_string(reinterpret_cast<uintptr_t>(ped)) + " model " + std::to_string(ped->m_nModelIndex) +
                     " tone=" + ext.currentSkinTone + " drawable/texture: " + desc);
        }
    }

    // Ensure all skinned atomics receive the ped's live RpHAnimHierarchy
    RpHAnimHierarchy* hier = nullptr;
    RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
        RpHAnimHierarchy** pHier = reinterpret_cast<RpHAnimHierarchy**>(data);
        if (!*pHier) {
            *pHier = RpSkinAtomicGetHAnimHierarchy(atomic);
        }
        return atomic;
    }, &hier);

    if (!hier) {
        hier = plugin::CallAndReturn<RpHAnimHierarchy*, 0x734A40, RpClump*>(ped->m_pRwClump);
    }

    if (hier) {
        RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
            RpHAnimHierarchy* h = reinterpret_cast<RpHAnimHierarchy*>(data);
            RpGeometry* geom = RpAtomicGetGeometry(atomic);
            if (geom && RpSkinGeometryGetSkin(geom)) {
                RpSkinAtomicSetHAnimHierarchy(atomic, h);
            }
            return atomic;
        }, hier);
    }

    // Apply atomic visibility
    auto pairData = std::make_pair(pConfig, &ext);
    RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto* pair = reinterpret_cast<std::pair<ModelVariationConfig*, PedExtended*>*>(data);
        ModelVariationConfig* pCfg = pair->first;
        PedExtended* pExt = pair->second;

        RwFrame* frame = RpAtomicGetFrame(atomic);
        const char* name = frame ? GetFrameNodeName(frame) : nullptr;
        std::string sName = name ? name : "";

        int compId = GetComponentFromPrefix(sName);
        if (compId >= 0 && compId < PV_COMP_COUNT) {
            int dIdx = 0;
            size_t firstUnderscore = sName.find('_');
            if (firstUnderscore != std::string::npos && firstUnderscore + 1 < sName.length()) {
                try { dIdx = std::stoi(sName.substr(firstUnderscore + 1)); } catch (...) {}
            }

            if (pExt->activeDrawable[compId] == dIdx) {
                RpAtomicSetFlags(atomic, RpAtomicGetFlags(atomic) | rpATOMICRENDER);
            } else {
                RpAtomicSetFlags(atomic, RpAtomicGetFlags(atomic) & ~rpATOMICRENDER);
            }
        }
        return atomic;
    }, &pairData);

    // Also run legacy remaps for textures that still use _remap
    FindLegacyRemaps(ped);
}

void VariationManager::RandomizePedVariations(CPed* ped) {
    if (!ped) return;
    PedExtended& ext = extData.Get(ped);
    ModelVariationConfig* pConfig = GetOrLoadModelConfig(ped->m_nModelIndex);
    if (!pConfig) return;

    // 1. Determine master skin tone (ethnicity) for this ped instance
    std::string chosenSkinTone = "uni";
    if (!pConfig->availableSkinTones.empty()) {
        int toneIdx = CGeneral::GetRandomNumberInRange(0, (int)pConfig->availableSkinTones.size());
        chosenSkinTone = pConfig->availableSkinTones[toneIdx];
    }
    ext.currentSkinTone = chosenSkinTone;

    // 2. Select compatible drawables and textures matching chosenSkinTone
    for (int compId = 0; compId < PV_COMP_COUNT; ++compId) {
        const ComponentSlot& slot = pConfig->components[compId];
        if (slot.isAvailable && !slot.drawables.empty()) {
            std::vector<int> validDrawables = slot.GetMatchingDrawableIndices(chosenSkinTone);
            if (validDrawables.empty()) {
                for (int i = 0; i < (int)slot.drawables.size(); ++i) validDrawables.push_back(i);
            }

            int dIdx = validDrawables[CGeneral::GetRandomNumberInRange(0, (int)validDrawables.size())];
            ext.activeDrawable[compId] = dIdx;

            const DrawableVariation& dv = slot.drawables[dIdx];
            std::vector<int> matchingTex = dv.GetMatchingTextureIndices(chosenSkinTone);
            if (matchingTex.empty()) {
                for (int i = 0; i < (int)dv.textures.size(); ++i) matchingTex.push_back(i);
            }

            if (!matchingTex.empty()) {
                int totalWeight = 0;
                for (int tIdx : matchingTex) {
                    totalWeight += (int)dv.textures[tIdx].distribution;
                }

                int chosenTIdx = matchingTex[0];
                if (totalWeight > 0) {
                    int roll = CGeneral::GetRandomNumberInRange(0, totalWeight);
                    int accumulated = 0;
                    for (int tIdx : matchingTex) {
                        accumulated += (int)dv.textures[tIdx].distribution;
                        if (roll < accumulated) {
                            chosenTIdx = tIdx;
                            break;
                        }
                    }
                } else {
                    chosenTIdx = matchingTex[CGeneral::GetRandomNumberInRange(0, (int)matchingTex.size())];
                }
                ext.activeTexture[compId] = chosenTIdx;
            } else {
                ext.activeTexture[compId] = 0;
            }
        } else {
            ext.activeDrawable[compId] = 0;
            ext.activeTexture[compId] = 0;
        }
    }

    // 3. Select props
    for (int anchor = 0; anchor < PV_ANCHOR_COUNT; ++anchor) {
        if (!pConfig->props[anchor].empty()) {
            int pIdx = CGeneral::GetRandomNumberInRange(-1, (int)pConfig->props[anchor].size());
            ext.activeProp[anchor] = pIdx;
            if (pIdx >= 0 && !pConfig->props[anchor][pIdx].textures.empty()) {
                ext.activePropTexture[anchor] = CGeneral::GetRandomNumberInRange(0, (int)pConfig->props[anchor][pIdx].textures.size());
            } else {
                ext.activePropTexture[anchor] = 0;
            }
        }
    }

    ext.variationsInitialized = true;
}

void VariationManager::ResetPedToDefaultVariation(CPed* ped, const ModelVariationConfig& config) {
    PedExtended& ext = extData.Get(ped);
    ext.currentSkinTone = config.availableSkinTones.empty() ? "uni" : config.availableSkinTones[0];
    for (int compId = 0; compId < PV_COMP_COUNT; ++compId) {
        ext.activeDrawable[compId] = 0;
        ext.activeTexture[compId] = 0;
    }
    for (int anchor = 0; anchor < PV_ANCHOR_COUNT; ++anchor) {
        ext.activeProp[anchor] = -1;
        ext.activePropTexture[anchor] = 0;
    }
    AlignPedComponentsToSkinTone(ped, ext.currentSkinTone);
    ext.variationsInitialized = true;
}

void VariationManager::AlignPedComponentsToSkinTone(CPed* ped, const std::string& tone) {
    if (!ped || tone.empty() || tone == "uni") return;
    PedExtended& ext = extData.Get(ped);
    ModelVariationConfig* pConfig = GetOrLoadModelConfig(ped->m_nModelIndex);
    if (!pConfig) return;

    for (int compId = 0; compId < PV_COMP_COUNT; ++compId) {
        const ComponentSlot& slot = pConfig->components[compId];
        if (!slot.isAvailable || slot.drawables.empty()) continue;

        int curDIdx = ext.activeDrawable[compId];
        if (curDIdx < 0 || curDIdx >= (int)slot.drawables.size() || !slot.drawables[curDIdx].SupportsSkinTone(tone)) {
            std::vector<int> validDrawables = slot.GetMatchingDrawableIndices(tone);
            if (!validDrawables.empty()) {
                curDIdx = validDrawables[0];
                ext.activeDrawable[compId] = curDIdx;
            }
        }

        const DrawableVariation& dv = slot.drawables[curDIdx];
        int curTIdx = ext.activeTexture[compId];
        bool textureMatches = false;
        if (curTIdx >= 0 && curTIdx < (int)dv.textures.size()) {
            const std::string& tTone = dv.textures[curTIdx].skinTone;
            if (tTone == "uni" || tTone == tone) {
                textureMatches = true;
            }
        }

        if (!textureMatches) {
            std::vector<int> matchingTex = dv.GetMatchingTextureIndices(tone);
            if (!matchingTex.empty()) {
                ext.activeTexture[compId] = matchingTex[0];
            }
        }
    }
}

bool VariationManager::SetPedComponentVariation(CPed* ped, int compId, int drawableId, int textureId) {
    if (!ped || compId < 0 || compId >= PV_COMP_COUNT) return false;
    PedExtended& ext = extData.Get(ped);
    ModelVariationConfig* pConfig = GetOrLoadModelConfig(ped->m_nModelIndex);
    if (!pConfig) return false;

    ext.activeDrawable[compId] = drawableId;
    ext.activeTexture[compId] = textureId;

    // If changing head or any component with a specific skin tone, update ped skin tone and align
    if (drawableId >= 0 && drawableId < (int)pConfig->components[compId].drawables.size()) {
        const auto& dv = pConfig->components[compId].drawables[drawableId];
        if (textureId >= 0 && textureId < (int)dv.textures.size()) {
            const std::string& texTone = dv.textures[textureId].skinTone;
            if (texTone != "uni" && texTone != ext.currentSkinTone) {
                ext.currentSkinTone = texTone;
                AlignPedComponentsToSkinTone(ped, texTone);
            }
        }
    }

    ApplyPedVariations(ped, false);
    return true;
}

bool VariationManager::SetPedSkinTone(CPed* ped, const std::string& tone) {
    if (!ped) return false;
    PedExtended& ext = extData.Get(ped);
    ext.currentSkinTone = tone;
    AlignPedComponentsToSkinTone(ped, tone);
    ApplyPedVariations(ped, false);
    return true;
}

const char* VariationManager::GetPedSkinTone(CPed* ped) {
    if (!ped) return "uni";
    PedExtended& ext = extData.Get(ped);
    return ext.currentSkinTone.c_str();
}

bool VariationManager::SetPedPropVariation(CPed* ped, int anchorId, int propId, int textureId) {
    if (!ped || anchorId < 0 || anchorId >= PV_ANCHOR_COUNT) return false;
    PedExtended& ext = extData.Get(ped);
    ext.activeProp[anchorId] = propId;
    ext.activePropTexture[anchorId] = textureId;
    return true;
}

void VariationManager::BeforePedRender(CPed* ped) {
    if (!ped || !ped->m_pRwClump || ped->m_pRwClump->object.type != rpCLUMP) return;

    PedExtended& ext = extData.Get(ped);
    ModelVariationConfig* pConfig = GetOrLoadModelConfig(ped->m_nModelIndex);

    if (pConfig && pConfig->hasVariations) {
        // Ensure rendered atomics have skin hierarchy bound
        RpHAnimHierarchy* hier = nullptr;
        RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
            RpHAnimHierarchy** pHier = reinterpret_cast<RpHAnimHierarchy**>(data);
            if (!*pHier) {
                *pHier = RpSkinAtomicGetHAnimHierarchy(atomic);
            }
            return atomic;
        }, &hier);

        if (!hier) {
            hier = plugin::CallAndReturn<RpHAnimHierarchy*, 0x734A40, RpClump*>(ped->m_pRwClump);
        }

        if (hier) {
            RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
                RpHAnimHierarchy* h = reinterpret_cast<RpHAnimHierarchy*>(data);
                if (RpAtomicGetFlags(atomic) & rpATOMICRENDER) {
                    RpGeometry* geom = RpAtomicGetGeometry(atomic);
                    if (geom && RpSkinGeometryGetSkin(geom) && !RpSkinAtomicGetHAnimHierarchy(atomic)) {
                        RpSkinAtomicSetHAnimHierarchy(atomic, h);
                    }
                }
                return atomic;
            }, hier);
        }

        auto pairData = std::make_pair(pConfig, &ext);
        RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
            if (!(RpAtomicGetFlags(atomic) & rpATOMICRENDER)) return atomic;

            auto* pair = reinterpret_cast<std::pair<ModelVariationConfig*, PedExtended*>*>(data);
            ModelVariationConfig* pCfg = pair->first;
            PedExtended* pExt = pair->second;

            RwFrame* frame = RpAtomicGetFrame(atomic);
            const char* name = frame ? GetFrameNodeName(frame) : nullptr;
            std::string sName = name ? name : "";

            int compId = GetComponentFromPrefix(sName);
            if (compId >= 0 && compId < PV_COMP_COUNT) {
                int dIdx = pExt->activeDrawable[compId];
                int tIdx = pExt->activeTexture[compId];

                if (dIdx >= 0 && dIdx < (int)pCfg->components[compId].drawables.size()) {
                    const DrawableVariation& dv = pCfg->components[compId].drawables[dIdx];
                    if (tIdx >= 0 && tIdx < (int)dv.textures.size() && dv.textures[tIdx].pRwTexture) {
                        RwTexture* targetTex = dv.textures[tIdx].pRwTexture;

                        if (atomic->geometry) {
                            RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial* material, void* texData) -> RpMaterial* {
                                RwTexture* pNewTex = reinterpret_cast<RwTexture*>(texData);
                                if (material->texture != pNewTex) {
                                    // Only swap materials that currently show a texture of the same
                                    // <comp>_diff_<NNN> family. A material whose texture failed to load
                                    // (null) or uses another sheet has UVs laid out for something else.
                                    bool shouldSwap = material->texture && pNewTex;
                                    if (shouldSwap) {
                                        std::string curName = material->texture->name;
                                        std::string newName = pNewTex->name;
                                        size_t curDiff = curName.find("_diff_");
                                        size_t newDiff = newName.find("_diff_");
                                        if (curDiff != std::string::npos && newDiff != std::string::npos) {
                                            size_t curUnder = curName.find('_', curDiff + 6);
                                            size_t newUnder = newName.find('_', newDiff + 6);
                                            if (curUnder != std::string::npos && newUnder != std::string::npos) {
                                                std::string curPrefix = curName.substr(0, curUnder);
                                                std::string newPrefix = newName.substr(0, newUnder);
                                                if (curPrefix != newPrefix) {
                                                    shouldSwap = false;
                                                }
                                            } else {
                                                shouldSwap = false;
                                            }
                                        } else {
                                            shouldSwap = false;
                                        }
                                    }
                                    if (shouldSwap) {
                                        g_renderTextureResets.push_back({&material->texture, material->texture});
                                        material->texture = pNewTex;
                                    }
                                }
                                return material;
                            }, targetTex);
                        }
                    }
                }
            }
            return atomic;
        }, &pairData);
    }

    // Legacy PedFuncs texture remaps
    if (ext.curRemapNum[0] >= 0) {
        RpClumpForAllAtomics(ped->m_pRwClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
            PedExtended* info = reinterpret_cast<PedExtended*>(data);
            if (atomic->geometry) {
                RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial* material, void* data) -> RpMaterial* {
                    PedExtended* info = reinterpret_cast<PedExtended*>(data);
                    g_renderTextureResets.push_back({&material->texture, material->texture});

                    for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
                        if (info->curRemapNum[i] >= 0 && info->originalRemap[i]) {
                            if (info->originalRemap[i] == material->texture && !info->remaps[i].empty()) {
                                auto it = info->remaps[i].begin();
                                std::advance(it, info->curRemapNum[i]);
                                material->texture = *it;
                            }
                        } else {
                            break;
                        }
                    }
                    return material;
                }, info);
            }
            return atomic;
        }, &ext);
    }
}

void VariationManager::AfterPedRender(CPed* /*ped*/) {
    for (auto& reset : g_renderTextureResets) {
        if (reset.pTexturePtr) {
            *reset.pTexturePtr = reset.pOriginalTexture;
        }
    }
    g_renderTextureResets.clear();
}

void VariationManager::FindLegacyRemaps(CPed* ped) {
    PedExtended& info = extData.Get(ped);

    // Scan once per model; rescanning used to append every _remap texture again on each call.
    if (info.legacyScannedModel == ped->m_nModelIndex) return;
    info.legacyScannedModel = ped->m_nModelIndex;
    for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
        info.curRemapNum[i] = -1;
        info.TotalRemapNum[i] = 0;
        info.originalRemap[i] = nullptr;
        info.remaps[i].clear();
    }

    CBaseModelInfo* pedModelInfo = CModelInfo::GetModelInfo(ped->m_nModelIndex);
    if (!pedModelInfo || pedModelInfo->m_nTxdIndex < 0 || !CTxdStore::ms_pTxdPool) return;

    TxdDef pedTxd = CTxdStore::ms_pTxdPool->m_pObjects[pedModelInfo->m_nTxdIndex];
    RwTexDictionary* pedTxdDic = pedTxd.m_pRwDictionary;
    if (!pedTxdDic) return;

    RwLinkList* objectList = &pedTxdDic->texturesInDict;
    if (rwLinkListEmpty(objectList)) return;

    RwLLLink* current = rwLinkListGetFirstLLLink(objectList);
    RwLLLink* end = rwLinkListGetTerminator(objectList);

    while (current != end) {
        RwTexture* texture = rwLLLinkGetData(current, RwTexture, lInDictionary);
        std::string name = texture->name;
        size_t found = name.find("_remap");
        if (found != std::string::npos) {
            std::string baseName = name.substr(0, found);
            int slot = -1;
            for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
                if (info.originalRemap[i]) {
                    if (baseName == info.originalRemap[i]->name) {
                        slot = i;
                        break;
                    }
                } else {
                    info.originalRemap[i] = RwTexDictionaryFindNamedTexture(pedTxdDic, baseName.c_str());
                    slot = i;
                    break;
                }
            }
            if (slot != -1) {
                info.remaps[slot].push_back(texture);
                info.TotalRemapNum[slot]++;
            }
        }
        current = rwLLLinkGetNext(current);
    }

    if (info.TotalRemapNum[0] > 0) {
        for (int i = 0; i < LEGACY_TEXTURE_LIMIT; ++i) {
            if (info.TotalRemapNum[i] > 0) {
                info.curRemapNum[i] = CGeneral::GetRandomNumberInRange(-1, info.TotalRemapNum[i]);
            }
        }
    }
}
