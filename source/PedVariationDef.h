#pragma once

#include "plugin.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// GTA V 12 Component Slots
enum ePedVariationComponent : int {
    PV_COMP_HEAD = 0,  // Head / Face
    PV_COMP_BERD = 1,  // Beard / Facial Hair / Mask
    PV_COMP_HAIR = 2,  // Hair
    PV_COMP_UPPR = 3,  // Upper Body / Shirt / Torso
    PV_COMP_LOWR = 4,  // Lower Body / Pants / Skirt
    PV_COMP_HAND = 5,  // Hands / Gloves
    PV_COMP_FEET = 6,  // Feet / Shoes
    PV_COMP_TEEF = 7,  // Teeth / Scarves / Necklaces
    PV_COMP_ACCS = 8,  // Accessories / Undershirt
    PV_COMP_TASK = 9,  // Task / Body Armor / Badges
    PV_COMP_DECL = 10, // Decals / Tattoos / Emblems
    PV_COMP_JBIB = 11, // Auxiliary / Jackets / Outerwear
    PV_COMP_COUNT = 12
};

// GTA V Prop Anchor Slots
enum ePedVariationAnchor : int {
    PV_ANCHOR_HEAD = 0,        // Hats / Helmets
    PV_ANCHOR_EYES = 1,        // Glasses
    PV_ANCHOR_EARS = 2,        // Ear accessories
    PV_ANCHOR_LEFT_WRIST = 6,  // Watches
    PV_ANCHOR_RIGHT_WRIST = 7, // Bracelets
    PV_ANCHOR_COUNT = 8
};

// Structure holding texture variation info
struct TextureVariation {
    std::string name;
    int texIndex = 0;
    uint8_t distribution = 255;
    std::string skinTone = "uni"; // e.g. "whi", "lat", "bla", "uni"
    RwTexture* pRwTexture = nullptr;
};

// Structure holding drawable variation info
struct DrawableVariation {
    int drawableIndex = 0;
    std::string drawableName;
    uint32_t propMask = 0;
    uint32_t inclusions = 0;
    uint32_t exclusions = 0;
    std::vector<TextureVariation> textures;
    std::vector<int> atomicIndices; // Indices of atomics in clump matching this drawable

    bool SupportsSkinTone(const std::string& tone) const {
        if (tone == "uni" || tone.empty()) return true;
        if (textures.empty()) return true;
        for (const auto& t : textures) {
            if (t.skinTone == "uni" || t.skinTone == tone) {
                return true;
            }
        }
        return false;
    }

    std::vector<int> GetMatchingTextureIndices(const std::string& tone) const {
        std::vector<int> matching;
        for (int i = 0; i < (int)textures.size(); ++i) {
            if (tone == "uni" || tone.empty() || textures[i].skinTone == "uni" || textures[i].skinTone == tone) {
                matching.push_back(i);
            }
        }
        return matching;
    }
};

// Structure holding a component slot's variations
struct ComponentSlot {
    int componentId = 0;
    bool isAvailable = false;
    std::vector<DrawableVariation> drawables;

    std::vector<int> GetMatchingDrawableIndices(const std::string& tone) const {
        std::vector<int> matching;
        for (int i = 0; i < (int)drawables.size(); ++i) {
            if (drawables[i].SupportsSkinTone(tone)) {
                matching.push_back(i);
            }
        }
        return matching;
    }
};

// Structure holding prop variations
struct PropVariation {
    int anchorId = 0;
    int propId = 0;
    uint32_t propFlags = 0;
    std::vector<TextureVariation> textures;
    std::vector<int> atomicIndices;
};

// Structure holding full variation model profile for a ped model
struct ModelVariationConfig {
    int modelIndex = -1;
    std::string modelName;
    bool hasVariations = false;
    bool hasTexVariations = false;
    bool hasDrawblVariations = false;

    std::vector<std::string> availableSkinTones; // e.g. ["whi", "lat"]

    ComponentSlot components[PV_COMP_COUNT];
    std::vector<PropVariation> props[PV_ANCHOR_COUNT];

    ModelVariationConfig() {
        for (int i = 0; i < PV_COMP_COUNT; ++i) {
            components[i].componentId = i;
            components[i].isAvailable = false;
        }
    }
};

inline std::string ParseSkinToneFromTextureName(const std::string& name) {
    size_t lastUnder = name.rfind('_');
    if (lastUnder != std::string::npos && lastUnder + 1 < name.length()) {
        std::string tag = name.substr(lastUnder + 1);
        if (tag.length() == 3) {
            bool allAlpha = true;
            for (char c : tag) {
                if (!isalpha(static_cast<unsigned char>(c))) {
                    allAlpha = false;
                    break;
                }
            }
            if (allAlpha) {
                for (char& c : tag) c = tolower(static_cast<unsigned char>(c));
                return tag;
            }
        }
    }
    return "uni";
}

inline const char* GetComponentPrefix(int compId) {
    switch (compId) {
        case PV_COMP_HEAD: return "head";
        case PV_COMP_BERD: return "berd";
        case PV_COMP_HAIR: return "hair";
        case PV_COMP_UPPR: return "uppr";
        case PV_COMP_LOWR: return "lowr";
        case PV_COMP_HAND: return "hand";
        case PV_COMP_FEET: return "feet";
        case PV_COMP_TEEF: return "teef";
        case PV_COMP_ACCS: return "accs";
        case PV_COMP_TASK: return "task";
        case PV_COMP_DECL: return "decl";
        case PV_COMP_JBIB: return "jbib";
        default: return "none";
    }
}

inline int GetComponentFromPrefix(const std::string& name) {
    if (name.rfind("head", 0) == 0) return PV_COMP_HEAD;
    if (name.rfind("berd", 0) == 0) return PV_COMP_BERD;
    if (name.rfind("hair", 0) == 0) return PV_COMP_HAIR;
    if (name.rfind("uppr", 0) == 0) return PV_COMP_UPPR;
    if (name.rfind("lowr", 0) == 0) return PV_COMP_LOWR;
    if (name.rfind("hand", 0) == 0) return PV_COMP_HAND;
    if (name.rfind("feet", 0) == 0) return PV_COMP_FEET;
    if (name.rfind("teef", 0) == 0) return PV_COMP_TEEF;
    if (name.rfind("accs", 0) == 0) return PV_COMP_ACCS;
    if (name.rfind("task", 0) == 0) return PV_COMP_TASK;
    if (name.rfind("decl", 0) == 0) return PV_COMP_DECL;
    if (name.rfind("jbib", 0) == 0) return PV_COMP_JBIB;
    return -1;
}

inline int GetAnchorFromPrefix(const std::string& name) {
    if (name.rfind("p_head", 0) == 0) return PV_ANCHOR_HEAD;
    if (name.rfind("p_eyes", 0) == 0) return PV_ANCHOR_EYES;
    if (name.rfind("p_ears", 0) == 0) return PV_ANCHOR_EARS;
    if (name.rfind("p_lwrist", 0) == 0) return PV_ANCHOR_LEFT_WRIST;
    if (name.rfind("p_rwrist", 0) == 0) return PV_ANCHOR_RIGHT_WRIST;
    return -1;
}
