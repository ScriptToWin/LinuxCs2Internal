module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <linux/input.h>
#include <linux/uinput.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unordered_map>
#include <vector>

export module Features;

import Config;
import Json;
import Logger;
import Memory;
import Offsets;
import Resolver;
import SchemaScan;

export struct FeatureConfig {
    bool bhopEnabled = false;
    bool thirdpersonEnabled = false;
    bool customCharacterEnabled = false;
    bool sniperCrosshair = false;
    bool skinsEnabled = true;
    std::uint32_t ctKnifeDef = 0;
    std::uint32_t tKnifeDef = 0;
    bool noFlash = false;
    float maxFlashAlpha = 0.0f;
    bool noSmoke = false;
    bool noSkybox = false;
    bool fullbright = false;
    bool onlyRenderOpaque = false;
    bool changeSmokeColor = false;
    bool glowChams = false;
    bool weaponGlowChams = false;
    float smokeRed = 255.0f;
    float smokeGreen = 0.0f;
    float smokeBlue = 0.0f;
    float glowRed = 0.0f;
    float glowGreen = 200.0f;
    float glowBlue = 255.0f;
};

export struct InputState {
    std::atomic<bool> homeDown{false};
    std::atomic<bool> vDown{false};
    std::atomic<bool> mouse4Down{false};
};

struct GlowOriginal {
    std::int32_t type = 0;
    std::int32_t range = 0;
    std::int32_t rangeMin = 0;
    std::array<std::uint8_t, 4> color{};
    std::array<std::uint8_t, 4> tint{};
    std::uint8_t eligible = 0;
    std::uint8_t enabled = 0;
    std::uint8_t tintEnabled = 0;
};

namespace {

using Clock = std::chrono::steady_clock;

std::string FeatureConfigPath() {
    const char* home = std::getenv("HOME");
    const std::string base = (home && *home) ? home : ".";
    return base + "/.config/hamzex/features.json";
}

template <typename T>
bool ReadValue(std::uintptr_t address, T& value) {
    return address && SafeRead(address, &value, sizeof(T));
}

template <typename T>
bool WriteValue(std::uintptr_t address, const T& value) {
    if (!address || IsBadReadPtr(reinterpret_cast<const void*>(address), sizeof(T)))
        return false;
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    return true;
}

void SetBoolFrom(const json::Value* object, const char* key, bool& target) {
    if (object && object->type == json::Type::Object)
        target = json::GetBool(*object, key, target);
}

void SetFloatFrom(const json::Value* object, const char* key, float& target) {
    if (object && object->type == json::Type::Object)
        target = static_cast<float>(json::GetDouble(*object, key, target));
}

void SetUIntFrom(const json::Value* object, const char* key, std::uint32_t& target) {
    if (object && object->type == json::Type::Object)
        target = static_cast<std::uint32_t>(json::GetLong(*object, key, target));
}

std::uintptr_t FindConvar(const char* name) {
    void* cvar = ResolveInterface(Tier0Module, "VEngineCvar0");
    if (!cvar)
        return 0;

    std::uintptr_t objects = 0;
    std::uint32_t count = 0;
    if (!ReadValue(reinterpret_cast<std::uintptr_t>(cvar) + 0x50, objects) ||
        !ReadValue(reinterpret_cast<std::uintptr_t>(cvar) + 0xA0, count) || !objects)
        return 0;
    count = std::min<std::uint32_t>(count, 8192);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t object = 0;
        if (!ReadValue(objects + static_cast<std::uintptr_t>(i) * 16, object) || !object)
            break;
        std::uintptr_t nameAddress = 0;
        if (!ReadValue(object, nameAddress) || !nameAddress)
            continue;
        char current[96] = {};
        if (ReadString(nameAddress, current, sizeof(current)) && std::strcmp(current, name) == 0)
            return object;
    }
    return 0;
}

struct Convars {
    std::uintptr_t drawSkybox = 0;
    std::uintptr_t fullbright = 0;
    std::uintptr_t onlyOpaque = 0;
    std::uintptr_t crosshairSniperWidth = 0;
    std::uintptr_t crosshairSize = 0;
    std::uintptr_t crosshairThickness = 0;
    std::uintptr_t crosshairGap = 0;
    std::uintptr_t crosshairAlpha = 0;
    std::uintptr_t crosshairDot = 0;
    std::uintptr_t crosshairColor = 0;
    std::uintptr_t crosshairColorR = 0;
    std::uintptr_t crosshairColorG = 0;
    std::uintptr_t crosshairColorB = 0;
};

struct RuntimeState {
    std::mutex mutex;
    std::mutex tickMutex;
    FeatureConfig config;
    bool loaded = false;
    std::atomic<bool> started{false};
    std::thread tickThread;
    int uinput = -1;
    bool uinputCreated = false;
    bool bhopActive = false;
    bool thirdpersonActive = false;
    bool previousMouse4 = false;
    bool previousV = false;
    bool previousThirdpersonConfig = false;
    std::array<std::uint8_t, 64> previousButtons{};
    std::array<std::uint8_t, 64> currentButtons{};
    bool gameInputValid = false;
    bool vPressPending = false;
    std::chrono::steady_clock::time_point lastJump{};
    bool haveOriginalThirdperson = false;
    std::uintptr_t originalObserverServices = 0;
    std::uint8_t originalObserverMode = 0;
    std::uint8_t originalObserverLastMode = 0;
    std::uint8_t originalObserverForcedMode = 0;
    bool haveOriginalObserverLastMode = false;
    bool haveOriginalObserverForcedMode = false;
    std::int32_t originalObserverTarget = 0;
    float originalChaseDistance = 0.0f;
    float originalChaseDistanceCalcTime = 0.0f;
    bool haveOriginalChaseDistanceCalcTime = false;
    bool haveOriginalCrosshair = false;
    float originalCrosshairSize = 0.0f;
    float originalCrosshairThickness = 0.0f;
    float originalCrosshairGap = 0.0f;
    std::int32_t originalCrosshairAlpha = 0;
    std::int32_t originalCrosshairDot = 0;
    std::int32_t originalCrosshairColor = 0;
    std::int32_t originalCrosshairColorR = 0;
    std::int32_t originalCrosshairColorG = 0;
    std::int32_t originalCrosshairColorB = 0;
    std::int32_t originalCrosshairSniperWidth = 0;
    bool haveOriginalSkybox = false;
    std::uint8_t originalSkybox = 1;
    bool haveOriginalFullbright = false;
    std::int32_t originalFullbright = 0;
    bool haveOriginalOpaque = false;
    std::uint8_t originalOpaque = 0;
    std::atomic<bool> sniperCrosshairActive{false};
    Convars convars;
    std::unordered_map<std::uintptr_t, GlowOriginal> originalGlows;
    std::vector<std::uintptr_t> glowEntities;
    std::vector<std::uintptr_t> weaponGlowEntities;
    Clock::time_point nextGlowScan{};
    Clock::time_point nextConvarScan{};
    bool convarsResolved = false;
    bool warnedOffsets = false;
    bool warnedUinput = false;
    bool warnedSchema = false;
    bool warnedPawn = false;
    bool warnedInput = false;
};

RuntimeState& State() {
    static RuntimeState state;
    return state;
}

InputState& GlobalInput() {
    static InputState input;
    return input;
}

void LoadConfig(RuntimeState& state) {
    state.config = FeatureConfig{};
    std::ifstream file(FeatureConfigPath());
    if (!file)
        return;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    json::Value root;
    if (!json::Parse(text.data(), text.size(), root) || root.type != json::Type::Object)
        return;

    const json::Value* movement = root.Find("movement");
    const json::Value* skins = root.Find("skins");
    const json::Value* unsafe = root.Find("unsafe");
    const auto& legacyConfig = config::Global();
    SetBoolFrom(movement, "bhop_enabled", state.config.bhopEnabled);
    SetBoolFrom(movement, "thirdperson_enabled", state.config.thirdpersonEnabled);
    SetBoolFrom(movement, "custom_character_enabled", state.config.customCharacterEnabled);
    SetBoolFrom(movement, "sniper_crosshair", state.config.sniperCrosshair);
    state.config.skinsEnabled = config::ConfigStore::Instance().isSkinEnabled();
    state.config.ctKnifeDef = legacyConfig.ctKnifeDef;
    state.config.tKnifeDef = legacyConfig.tKnifeDef;
    SetBoolFrom(skins, "enabled", state.config.skinsEnabled);
    SetUIntFrom(skins, "ct_knife_def", state.config.ctKnifeDef);
    SetUIntFrom(skins, "t_knife_def", state.config.tKnifeDef);
    SetBoolFrom(unsafe, "no_flash", state.config.noFlash);
    SetFloatFrom(unsafe, "max_flash_alpha", state.config.maxFlashAlpha);
    SetBoolFrom(unsafe, "no_smoke", state.config.noSmoke);
    SetBoolFrom(unsafe, "no_skybox", state.config.noSkybox);
    SetBoolFrom(unsafe, "fullbright", state.config.fullbright);
    SetBoolFrom(unsafe, "sc_only_render_opaque", state.config.onlyRenderOpaque);
    SetBoolFrom(unsafe, "change_smoke_color", state.config.changeSmokeColor);
    SetBoolFrom(unsafe, "glow_chams", state.config.glowChams);
    SetBoolFrom(unsafe, "weapon_glow_chams", state.config.weaponGlowChams);
    SetFloatFrom(unsafe, "smoke_red", state.config.smokeRed);
    SetFloatFrom(unsafe, "smoke_green", state.config.smokeGreen);
    SetFloatFrom(unsafe, "smoke_blue", state.config.smokeBlue);
    SetFloatFrom(unsafe, "glow_red", state.config.glowRed);
    SetFloatFrom(unsafe, "glow_green", state.config.glowGreen);
    SetFloatFrom(unsafe, "glow_blue", state.config.glowBlue);
}

void SaveConfig(const FeatureConfig& config) {
    const std::filesystem::path path(FeatureConfigPath());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return;
    file << "{\n"
         << "  \"movement\": {\n"
         << "    \"bhop_enabled\": " << (config.bhopEnabled ? "true" : "false") << ",\n"
         << "    \"thirdperson_enabled\": " << (config.thirdpersonEnabled ? "true" : "false") << ",\n"
         << "    \"custom_character_enabled\": " << (config.customCharacterEnabled ? "true" : "false") << ",\n"
         << "    \"sniper_crosshair\": " << (config.sniperCrosshair ? "true" : "false") << "\n"
         << "  },\n"
         << "  \"skins\": {\n"
         << "    \"enabled\": " << (config.skinsEnabled ? "true" : "false") << ",\n"
         << "    \"ct_knife_def\": " << config.ctKnifeDef << ",\n"
         << "    \"t_knife_def\": " << config.tKnifeDef << "\n"
         << "  },\n"
         << "  \"unsafe\": {\n"
         << "    \"no_flash\": " << (config.noFlash ? "true" : "false") << ",\n"
         << "    \"max_flash_alpha\": " << config.maxFlashAlpha << ",\n"
         << "    \"no_smoke\": " << (config.noSmoke ? "true" : "false") << ",\n"
         << "    \"no_skybox\": " << (config.noSkybox ? "true" : "false") << ",\n"
         << "    \"fullbright\": " << (config.fullbright ? "true" : "false") << ",\n"
         << "    \"sc_only_render_opaque\": " << (config.onlyRenderOpaque ? "true" : "false") << ",\n"
         << "    \"change_smoke_color\": " << (config.changeSmokeColor ? "true" : "false") << ",\n"
         << "    \"glow_chams\": " << (config.glowChams ? "true" : "false") << ",\n"
         << "    \"weapon_glow_chams\": " << (config.weaponGlowChams ? "true" : "false") << ",\n"
         << "    \"smoke_red\": " << config.smokeRed << ",\n"
         << "    \"smoke_green\": " << config.smokeGreen << ",\n"
         << "    \"smoke_blue\": " << config.smokeBlue << ",\n"
         << "    \"glow_red\": " << config.glowRed << ",\n"
         << "    \"glow_green\": " << config.glowGreen << ",\n"
         << "    \"glow_blue\": " << config.glowBlue << "\n"
         << "  }\n}\n";
}

void Emit(RuntimeState& state, std::uint16_t type, std::uint16_t code, int value) {
    if (state.uinput < 0 || !state.uinputCreated)
        return;
    input_event event{};
    gettimeofday(&event.time, nullptr);
    event.type = type;
    event.code = code;
    event.value = value;
    (void)::write(state.uinput, &event, sizeof(event));
}

void Sync(RuntimeState& state) {
    Emit(state, EV_SYN, SYN_REPORT, 0);
}

void WheelUp(RuntimeState& state) {
    Emit(state, EV_REL, REL_WHEEL, 1);
    Sync(state);
}

void OpenUinput(RuntimeState& state) {
    if (state.uinputCreated || state.uinput >= 0)
        return;
    state.uinput = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (state.uinput < 0) {
        if (!state.warnedUinput) {
            static Logger log;
            log.warn("Features: /dev/uinput unavailable; bhop input disabled");
            state.warnedUinput = true;
        }
        return;
    }
    ioctl(state.uinput, UI_SET_EVBIT, EV_SYN);
    ioctl(state.uinput, UI_SET_EVBIT, EV_KEY);
    ioctl(state.uinput, UI_SET_EVBIT, EV_REL);
    ioctl(state.uinput, UI_SET_RELBIT, REL_X);
    ioctl(state.uinput, UI_SET_RELBIT, REL_Y);
    ioctl(state.uinput, UI_SET_RELBIT, REL_WHEEL);
    ioctl(state.uinput, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(state.uinput, UI_SET_KEYBIT, KEY_A);
    ioctl(state.uinput, UI_SET_KEYBIT, KEY_D);

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x0451;
    setup.id.product = 0xe008;
    setup.id.version = 1;
    std::strncpy(setup.name, "Hamzex bhop input", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(state.uinput, UI_DEV_SETUP, &setup) != 0 || ioctl(state.uinput, UI_DEV_CREATE) != 0) {
        if (!state.warnedUinput) {
            static Logger log;
            log.warn("Features: could not create the uinput bhop device");
            state.warnedUinput = true;
        }
        ::close(state.uinput);
        state.uinput = -1;
        return;
    }
    state.uinputCreated = true;
}

constexpr std::size_t Mouse4ButtonCode = 320;
constexpr std::size_t KeyA = 11;
constexpr std::size_t KeyD = 14;
constexpr std::size_t KeyS = 29;
constexpr std::size_t KeyW = 36;
constexpr std::size_t KeyV = 33;

void UpdateGameInput(RuntimeState& state) {
    const auto& offsets = ResolveInputOffsets();
    if (!offsets.resolved) {
        state.gameInputValid = false;
        if (!state.warnedInput) {
            static Logger log;
            log.warn("Features: CS2 input offsets are not ready; Mouse4 fallback is active");
            state.warnedInput = true;
        }
        return;
    }

    std::array<std::uint8_t, 64> next{};
    if (!ReadValue(offsets.inputSystem + offsets.buttonStateOffset, next)) {
        state.gameInputValid = false;
        return;
    }
    if (state.gameInputValid) {
        const auto byte = KeyV / 8;
        const auto mask = static_cast<std::uint8_t>(1u << (KeyV % 8));
        if ((next[byte] & mask) != 0 && (state.currentButtons[byte] & mask) == 0)
            state.vPressPending = true;
        state.previousButtons = state.currentButtons;
    } else {
        state.previousButtons = next;
    }
    state.currentButtons = next;
    state.gameInputValid = true;
}

bool GameButtonDown(const RuntimeState& state, std::size_t code) {
    const std::size_t byte = code / 8;
    const std::size_t bit = code % 8;
    return byte < state.currentButtons.size() &&
           (state.currentButtons[byte] & (static_cast<std::uint8_t>(1u) << bit)) != 0;
}

bool GameButtonJustPressed(const RuntimeState& state, std::size_t code) {
    const std::size_t byte = code / 8;
    const std::size_t bit = code % 8;
    if (byte >= state.currentButtons.size())
        return false;
    const auto mask = static_cast<std::uint8_t>(1u << bit);
    return (state.currentButtons[byte] & mask) != 0 &&
           (state.previousButtons[byte] & mask) == 0;
}

void ResolveConvars(RuntimeState& state) {
    if (Clock::now() < state.nextConvarScan)
        return;
    state.nextConvarScan = Clock::now() + std::chrono::milliseconds(500);
    if (!state.convars.drawSkybox)
        state.convars.drawSkybox = FindConvar("r_drawskybox");
    if (!state.convars.fullbright)
        state.convars.fullbright = FindConvar("mat_fullbright");
    if (!state.convars.onlyOpaque)
        state.convars.onlyOpaque = FindConvar("sc_only_render_opaque");
    if (!state.convars.crosshairSniperWidth)
        state.convars.crosshairSniperWidth = FindConvar("cl_crosshair_sniper_width");
    if (!state.convars.crosshairSize)
        state.convars.crosshairSize = FindConvar("cl_crosshairsize");
    if (!state.convars.crosshairThickness)
        state.convars.crosshairThickness = FindConvar("cl_crosshairthickness");
    if (!state.convars.crosshairGap)
        state.convars.crosshairGap = FindConvar("cl_crosshairgap");
    if (!state.convars.crosshairAlpha)
        state.convars.crosshairAlpha = FindConvar("cl_crosshairalpha");
    if (!state.convars.crosshairDot)
        state.convars.crosshairDot = FindConvar("cl_crosshairdot");
    if (!state.convars.crosshairColor)
        state.convars.crosshairColor = FindConvar("cl_crosshaircolor");
    if (!state.convars.crosshairColorR)
        state.convars.crosshairColorR = FindConvar("cl_crosshaircolor_r");
    if (!state.convars.crosshairColorG)
        state.convars.crosshairColorG = FindConvar("cl_crosshaircolor_g");
    if (!state.convars.crosshairColorB)
        state.convars.crosshairColorB = FindConvar("cl_crosshaircolor_b");
    state.convarsResolved = state.convars.drawSkybox || state.convars.fullbright ||
                            state.convars.onlyOpaque ||
                            state.convars.crosshairSniperWidth || state.convars.crosshairSize ||
                            state.convars.crosshairThickness || state.convars.crosshairGap ||
                            state.convars.crosshairAlpha || state.convars.crosshairDot ||
                            state.convars.crosshairColor || state.convars.crosshairColorR ||
                            state.convars.crosshairColorG || state.convars.crosshairColorB;
}

bool NativeCrosshairReady(const Convars& convars) {
    return convars.crosshairSniperWidth && convars.crosshairSize &&
           convars.crosshairThickness && convars.crosshairGap && convars.crosshairAlpha &&
           convars.crosshairDot && convars.crosshairColor && convars.crosshairColorR &&
           convars.crosshairColorG && convars.crosshairColorB;
}

void RestoreNativeCrosshair(RuntimeState& state) {
    if (!state.haveOriginalCrosshair)
        return;
    const auto& c = state.convars;
    WriteValue(c.crosshairSize + 0x58, state.originalCrosshairSize);
    WriteValue(c.crosshairThickness + 0x58, state.originalCrosshairThickness);
    WriteValue(c.crosshairGap + 0x58, state.originalCrosshairGap);
    WriteValue(c.crosshairAlpha + 0x58, state.originalCrosshairAlpha);
    WriteValue(c.crosshairDot + 0x58, state.originalCrosshairDot);
    WriteValue(c.crosshairColor + 0x58, state.originalCrosshairColor);
    WriteValue(c.crosshairColorR + 0x58, state.originalCrosshairColorR);
    WriteValue(c.crosshairColorG + 0x58, state.originalCrosshairColorG);
    WriteValue(c.crosshairColorB + 0x58, state.originalCrosshairColorB);
    WriteValue(c.crosshairSniperWidth + 0x58, state.originalCrosshairSniperWidth);
    state.haveOriginalCrosshair = false;
}

bool IsSniperDefinition(std::uint32_t definition) {
    return definition == 9 ||    // AWP
           definition == 11 ||   // G3SG1
           definition == 38 ||   // SCAR-20
           definition == 40;     // SSG 08
}

bool ActiveWeaponIsSniper(std::uintptr_t system, const ClientOffsets& client,
                          std::uintptr_t pawn, const SkinSchemaOffsets& schema) {
    if (!system || !client.entitySystem || !pawn ||
        !schema.weapon_services_offset || !schema.active_weapon_offset ||
        !schema.attribute_manager_offset || !schema.item_in_container_offset ||
        !schema.item_def_index_offset)
        return false;

    std::uintptr_t services = 0;
    if (!ReadValue(pawn + schema.weapon_services_offset, services) || !services ||
        IsBadReadPtr(reinterpret_cast<const void*>(services), sizeof(void*)))
        return false;

    std::uint32_t handle = 0;
    if (!ReadValue(services + schema.active_weapon_offset, handle))
        return false;
    const auto weapon = reinterpret_cast<std::uintptr_t>(EntityFromHandle(
        system, client.entityList, handle, schema.entity_identity_size));
    if (!weapon || IsBadReadPtr(reinterpret_cast<const void*>(weapon), 8))
        return false;

    const auto econView = weapon + schema.attribute_manager_offset +
                          schema.item_in_container_offset;
    std::uint32_t definition = 0;
    if (!ReadValue(econView + schema.item_def_index_offset, definition))
        return false;
    return IsSniperDefinition(definition & 0xFFFFu);
}

void ApplySniperCrosshair(RuntimeState& state, const FeatureConfig& config,
                          std::uintptr_t system, const ClientOffsets& client,
                          std::uintptr_t pawn, const SkinSchemaOffsets& schema) {
    const bool active = config.sniperCrosshair &&
                        ActiveWeaponIsSniper(system, client, pawn, schema);
    state.sniperCrosshairActive.store(active, std::memory_order_relaxed);
    if (!NativeCrosshairReady(state.convars) || !active) {
        RestoreNativeCrosshair(state);
        return;
    }

    const auto& c = state.convars;
    if (!state.haveOriginalCrosshair) {
        const bool captured =
            ReadValue(c.crosshairSize + 0x58, state.originalCrosshairSize) &&
            ReadValue(c.crosshairThickness + 0x58, state.originalCrosshairThickness) &&
            ReadValue(c.crosshairGap + 0x58, state.originalCrosshairGap) &&
            ReadValue(c.crosshairAlpha + 0x58, state.originalCrosshairAlpha) &&
            ReadValue(c.crosshairDot + 0x58, state.originalCrosshairDot) &&
            ReadValue(c.crosshairColor + 0x58, state.originalCrosshairColor) &&
            ReadValue(c.crosshairColorR + 0x58, state.originalCrosshairColorR) &&
            ReadValue(c.crosshairColorG + 0x58, state.originalCrosshairColorG) &&
            ReadValue(c.crosshairColorB + 0x58, state.originalCrosshairColorB) &&
            ReadValue(c.crosshairSniperWidth + 0x58, state.originalCrosshairSniperWidth);
        if (!captured)
            return;
        state.haveOriginalCrosshair = true;
    }

    const float size = 2.0f;
    const float thickness = 1.0f;
    const float gap = 0.0f;
    const std::int32_t alpha = 255;
    const std::int32_t dot = 1;
    const std::int32_t color = 5;
    const std::int32_t channel = 255;
    const std::int32_t sniperWidth = 1;
    WriteValue(c.crosshairSize + 0x58, size);
    WriteValue(c.crosshairThickness + 0x58, thickness);
    WriteValue(c.crosshairGap + 0x58, gap);
    WriteValue(c.crosshairAlpha + 0x58, alpha);
    WriteValue(c.crosshairDot + 0x58, dot);
    WriteValue(c.crosshairColor + 0x58, color);
    WriteValue(c.crosshairColorR + 0x58, channel);
    WriteValue(c.crosshairColorG + 0x58, channel);
    WriteValue(c.crosshairColorB + 0x58, channel);
    WriteValue(c.crosshairSniperWidth + 0x58, sniperWidth);

    // CS2 suppresses the native reticle while scoped. Clearing only the
    // render-state flag leaves the weapon zoom untouched and lets the normal
    // crosshair draw over the scope view. The game refreshes this field again
    // when the scope state changes, so this is deliberately reapplied.
    if (pawn && schema.is_scoped_offset) {
        std::uint8_t scoped = 0;
        if (ReadValue(pawn + schema.is_scoped_offset, scoped) && scoped) {
            const std::uint8_t unmasked = 0;
            WriteValue(pawn + schema.is_scoped_offset, unmasked);
        }
    }
}

std::array<std::uint8_t, 4> GlowColor(const FeatureConfig& config) {
    return {
        static_cast<std::uint8_t>(std::clamp(config.glowRed, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(config.glowGreen, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(config.glowBlue, 0.0f, 255.0f)),
        255};
}

void ApplyGlow(RuntimeState& state, const FeatureConfig& config, std::uintptr_t entity,
               const SkinSchemaOffsets& schema, bool enabled) {
    if (!schema.GlowReady())
        return;
    const auto glow = entity + schema.glow_property_offset;
    auto it = state.originalGlows.find(entity);
    if (enabled) {
        if (it == state.originalGlows.end()) {
            GlowOriginal original;
            if (!ReadValue(glow + schema.glow_type_offset, original.type) ||
                !ReadValue(glow + schema.glow_range_offset, original.range) ||
                !ReadValue(glow + schema.glow_range_min_offset, original.rangeMin) ||
                !ReadValue(glow + schema.glow_color_offset, original.color) ||
                !ReadValue(glow + schema.glow_eligible_offset, original.eligible) ||
                !ReadValue(glow + schema.glow_enabled_offset, original.enabled) ||
                !ReadValue(entity + schema.client_tint_offset, original.tint) ||
                !ReadValue(entity + schema.client_tint_enabled_offset, original.tintEnabled))
                return;
            it = state.originalGlows.emplace(entity, original).first;
        }
        const std::int32_t glowType = 3;
        const std::int32_t glowRange = 5000;
        const std::int32_t glowRangeMin = 0;
        const auto color = GlowColor(config);
        const std::uint8_t enabled = 1;
        WriteValue(glow + schema.glow_type_offset, glowType);
        WriteValue(glow + schema.glow_range_offset, glowRange);
        WriteValue(glow + schema.glow_range_min_offset, glowRangeMin);
        WriteValue(glow + schema.glow_color_offset, color);
        WriteValue(glow + schema.glow_eligible_offset, enabled);
        WriteValue(glow + schema.glow_enabled_offset, enabled);
        WriteValue(entity + schema.client_tint_offset, color);
        WriteValue(entity + schema.client_tint_enabled_offset, enabled);
        return;
    }

    if (it == state.originalGlows.end())
        return;
    const auto& original = it->second;
    WriteValue(glow + schema.glow_type_offset, original.type);
    WriteValue(glow + schema.glow_range_offset, original.range);
    WriteValue(glow + schema.glow_range_min_offset, original.rangeMin);
    WriteValue(glow + schema.glow_color_offset, original.color);
    WriteValue(glow + schema.glow_eligible_offset, original.eligible);
    WriteValue(glow + schema.glow_enabled_offset, original.enabled);
    WriteValue(entity + schema.client_tint_offset, original.tint);
    WriteValue(entity + schema.client_tint_enabled_offset, original.tintEnabled);
    state.originalGlows.erase(it);
}

void RestoreAllGlow(RuntimeState& state, const SkinSchemaOffsets& schema) {
    if (!schema.GlowReady()) {
        state.originalGlows.clear();
        state.glowEntities.clear();
        state.weaponGlowEntities.clear();
        return;
    }
    for (const auto& [entity, original] : state.originalGlows) {
        if (IsBadReadPtr(reinterpret_cast<const void*>(entity), 8))
            continue;
        const auto glow = entity + schema.glow_property_offset;
        WriteValue(glow + schema.glow_type_offset, original.type);
        WriteValue(glow + schema.glow_range_offset, original.range);
        WriteValue(glow + schema.glow_range_min_offset, original.rangeMin);
        WriteValue(glow + schema.glow_color_offset, original.color);
        WriteValue(glow + schema.glow_eligible_offset, original.eligible);
        WriteValue(glow + schema.glow_enabled_offset, original.enabled);
        WriteValue(entity + schema.client_tint_offset, original.tint);
        WriteValue(entity + schema.client_tint_enabled_offset, original.tintEnabled);
    }
    state.originalGlows.clear();
    state.glowEntities.clear();
    state.weaponGlowEntities.clear();
}

bool IsWeaponGlowTarget(std::string_view className) {
    return className.find("Weapon") != std::string_view::npos ||
           className.find("Knife") != std::string_view::npos ||
           className.find("C4") != std::string_view::npos ||
           className.find("ViewModel") != std::string_view::npos;
}

void ForEachEntity(std::uintptr_t system, std::int8_t listOffset, int identitySize,
                   const std::function<void(std::uintptr_t, std::string_view)>& callback) {
    if (!system)
        return;
    const auto list = system + static_cast<std::intptr_t>(listOffset);
    for (std::uint32_t chunkIndex = 0; chunkIndex < 64; ++chunkIndex) {
        std::uintptr_t chunk = 0;
        if (!ReadValue(list + chunkIndex * sizeof(void*), chunk) || !chunk)
            continue;
        for (std::uint32_t item = 0; item < 512; ++item) {
            const auto identity = chunk + (item * static_cast<std::uint32_t>(identitySize));
            std::uintptr_t entity = 0;
            std::uint32_t handle = 0;
            if (!ReadValue(identity, entity) || !ReadValue(identity + 0x10, handle) || !entity)
                continue;
            if ((handle & 0x7FFFu) != chunkIndex * 512u + item)
                continue;

            std::uintptr_t vtable = 0;
            std::uintptr_t rtti = 0;
            std::uintptr_t name = 0;
            char className[96] = {};
            if (!ReadValue(entity, vtable) || vtable < 8 ||
                !ReadValue(vtable - 8, rtti) || !ReadValue(rtti + 8, name) ||
                !ReadString(name, className, sizeof(className)))
                continue;
            callback(entity, className);
        }
    }
}

bool LocalPawn(const ClientOffsets& client, std::size_t identitySize, std::uintptr_t& pawn,
               std::uintptr_t& controller, std::uint32_t& pawnHandle) {
    controller = reinterpret_cast<std::uintptr_t>(TryController(client.localPlayerController, ClientModule));
    if (!controller)
        controller = reinterpret_cast<std::uintptr_t>(TryController(client.localPlayerController + 1, ClientModule));
    if (!controller || !ReadValue(controller + client.m_hPawn, pawnHandle))
        return false;
    const auto system = client.entitySystem;
    if (!system)
        return false;
    pawn = reinterpret_cast<std::uintptr_t>(EntityFromHandle(system, client.entityList,
                                                              pawnHandle, identitySize));
    return pawn != 0 && !IsBadReadPtr(reinterpret_cast<const void*>(pawn), 8);
}

void ApplyUnsafe(RuntimeState& state, const FeatureConfig& config, std::uintptr_t pawn,
                 std::uintptr_t system, const ClientOffsets& client, const SkinSchemaOffsets& schema) {
    ResolveConvars(state);
    if (state.convars.drawSkybox) {
        std::uint8_t value = 1;
        if (!state.haveOriginalSkybox)
            state.haveOriginalSkybox = ReadValue(state.convars.drawSkybox + 0x58, state.originalSkybox);
        value = config.noSkybox ? 0 : (state.haveOriginalSkybox ? state.originalSkybox : 1);
        WriteValue(state.convars.drawSkybox + 0x58, value);
    }
    if (state.convars.fullbright) {
        if (!state.haveOriginalFullbright)
            state.haveOriginalFullbright = ReadValue(state.convars.fullbright + 0x58, state.originalFullbright);
        const auto value = config.fullbright ? 1 : (state.haveOriginalFullbright ? state.originalFullbright : 0);
        WriteValue(state.convars.fullbright + 0x58, value);
    }
    if (state.convars.onlyOpaque) {
        if (!state.haveOriginalOpaque)
            state.haveOriginalOpaque = ReadValue(state.convars.onlyOpaque + 0x58, state.originalOpaque);
        const auto value = config.onlyRenderOpaque ? static_cast<std::uint8_t>(1) : state.originalOpaque;
        WriteValue(state.convars.onlyOpaque + 0x58, value);
    }
    if (config.noFlash && schema.flash_alpha_offset) {
        const float alpha = std::clamp(config.maxFlashAlpha, 0.0f, 255.0f);
        WriteValue(pawn + schema.flash_alpha_offset, alpha);
    }
    ApplySniperCrosshair(state, config, system, client, pawn, schema);

    const bool glowNeeded = schema.GlowReady() &&
                            (config.glowChams || config.weaponGlowChams ||
                             !state.originalGlows.empty());
    const bool crosshairNeeded = state.sniperCrosshairActive.load(std::memory_order_relaxed) ||
                                 state.haveOriginalCrosshair;
    if (!config.noSmoke && !config.changeSmokeColor && !glowNeeded &&
        !crosshairNeeded)
        return;

    if (schema.GlowReady() && (config.glowChams || config.weaponGlowChams) &&
        Clock::now() >= state.nextGlowScan) {
        state.glowEntities.clear();
        state.weaponGlowEntities.clear();
        ForEachEntity(system, client.entityList, schema.entity_identity_size,
            [&](std::uintptr_t entity, std::string_view className) {
                if (className.find("C_CSPlayerPawn") != std::string_view::npos)
                    state.glowEntities.push_back(entity);
                if (IsWeaponGlowTarget(className))
                    state.weaponGlowEntities.push_back(entity);
            });
        state.nextGlowScan = Clock::now() + std::chrono::milliseconds(100);
    }
    if (schema.GlowReady() && config.glowChams) {
        for (const auto entity : state.glowEntities)
            ApplyGlow(state, config, entity, schema, true);
    }
    if (schema.GlowReady() && config.weaponGlowChams) {
        for (const auto entity : state.weaponGlowEntities)
            ApplyGlow(state, config, entity, schema, true);
    }
    if (!config.glowChams && !config.weaponGlowChams && !state.originalGlows.empty()) {
        RestoreAllGlow(state, schema);
    }

    const bool entityScanNeeded = config.noSmoke || config.changeSmokeColor;
    if (!entityScanNeeded)
        return;

    ForEachEntity(system, client.entityList, schema.entity_identity_size,
        [&](std::uintptr_t entity, std::string_view className) {
            const bool isSmoke = className == "24C_SmokeGrenadeProjectile" ||
                                 className.find("SmokeGrenadeProjectile") != std::string_view::npos;
            if (isSmoke) {
                if (config.noSmoke && schema.smoke_effect_offset) {
                    const std::uint8_t disabled = 1;
                    WriteValue(entity + schema.smoke_effect_offset, disabled);
                }
                if (config.changeSmokeColor && schema.smoke_color_offset) {
                    const std::array<float, 3> color{
                        std::clamp(config.smokeRed, 0.0f, 255.0f),
                        std::clamp(config.smokeGreen, 0.0f, 255.0f),
                        std::clamp(config.smokeBlue, 0.0f, 255.0f)};
                    for (std::size_t i = 0; i < color.size(); ++i)
                        WriteValue(entity + schema.smoke_color_offset + i * sizeof(float), color[i]);
                }
            }
        });
}

void ApplyThirdperson(RuntimeState& state, const FeatureConfig& config, std::uintptr_t pawn,
                      std::uint32_t pawnHandle, const SkinSchemaOffsets& schema) {
    const bool vDown = state.gameInputValid
                           ? GameButtonDown(state, KeyV)
                           : GlobalInput().vDown.load(std::memory_order_relaxed);
    const bool wasVDown = state.previousV;
    if (config.thirdpersonEnabled != state.previousThirdpersonConfig)
        state.thirdpersonActive = config.thirdpersonEnabled;
    if (!config.thirdpersonEnabled)
        state.thirdpersonActive = false;
    const bool vPressed = state.gameInputValid ? state.vPressPending : (vDown && !wasVDown);
    state.vPressPending = false;
    if (config.thirdpersonEnabled && vPressed)
        state.thirdpersonActive = !state.thirdpersonActive;
    state.previousV = vDown;
    state.previousThirdpersonConfig = config.thirdpersonEnabled;
    std::int32_t health = 0;
    std::uint8_t team = 0;
    const bool playable = pawn && schema.health_offset && schema.team_num_offset &&
                          ReadValue(pawn + schema.health_offset, health) &&
                          ReadValue(pawn + schema.team_num_offset, team) &&
                          health > 0 && (team == 2 || team == 3);
    auto restoreSnapshot = [&] {
        if (!state.haveOriginalThirdperson || !state.originalObserverServices)
            return;
        const auto services = state.originalObserverServices;
        if (schema.observer_mode_offset)
            WriteValue(services + schema.observer_mode_offset, state.originalObserverMode);
        if (schema.observer_target_offset)
            WriteValue(services + schema.observer_target_offset, state.originalObserverTarget);
        if (schema.observer_chase_distance_offset)
            WriteValue(services + schema.observer_chase_distance_offset, state.originalChaseDistance);
        if (state.haveOriginalChaseDistanceCalcTime &&
            schema.observer_chase_distance_calc_time_offset)
            WriteValue(services + schema.observer_chase_distance_calc_time_offset,
                       state.originalChaseDistanceCalcTime);
        if (state.haveOriginalObserverLastMode && schema.observer_last_mode_offset)
            WriteValue(services + schema.observer_last_mode_offset,
                       state.originalObserverLastMode);
        if (state.haveOriginalObserverForcedMode && schema.observer_forced_mode_offset)
            WriteValue(services + schema.observer_forced_mode_offset,
                       state.originalObserverForcedMode);
        state.haveOriginalThirdperson = false;
        state.originalObserverServices = 0;
        state.haveOriginalObserverLastMode = false;
        state.haveOriginalObserverForcedMode = false;
        state.haveOriginalChaseDistanceCalcTime = false;
    };
    if (!playable) {
        state.thirdpersonActive = false;
        restoreSnapshot();
    }
    if (!pawn || !schema.observer_services_offset)
        return;
    std::uintptr_t services = 0;
    if (!ReadValue(pawn + schema.observer_services_offset, services) || !services ||
        IsBadReadPtr(reinterpret_cast<const void*>(services), sizeof(void*))) {
        restoreSnapshot();
        return;
    }
    if (state.haveOriginalThirdperson && state.originalObserverServices != services)
        restoreSnapshot();
    const auto modeAddress = services + schema.observer_mode_offset;
    if (state.thirdpersonActive && schema.observer_target_offset) {
        if (!state.haveOriginalThirdperson) {
            state.originalObserverServices = services;
            state.haveOriginalThirdperson =
                ReadValue(modeAddress, state.originalObserverMode) &&
                ReadValue(services + schema.observer_target_offset, state.originalObserverTarget);
            if (schema.observer_last_mode_offset)
                state.haveOriginalObserverLastMode =
                    ReadValue(services + schema.observer_last_mode_offset,
                              state.originalObserverLastMode);
            if (schema.observer_forced_mode_offset)
                state.haveOriginalObserverForcedMode =
                    ReadValue(services + schema.observer_forced_mode_offset,
                              state.originalObserverForcedMode);
            if (schema.observer_chase_distance_offset)
                ReadValue(services + schema.observer_chase_distance_offset, state.originalChaseDistance);
            if (schema.observer_chase_distance_calc_time_offset)
                state.haveOriginalChaseDistanceCalcTime =
                    ReadValue(services + schema.observer_chase_distance_calc_time_offset,
                              state.originalChaseDistanceCalcTime);
            if (!state.haveOriginalThirdperson)
                return;
        }
        const std::uint8_t chase = 3;
        std::uint8_t currentMode = 0;
        std::int32_t currentTarget = 0;
        if (!ReadValue(modeAddress, currentMode) ||
            !ReadValue(services + schema.observer_target_offset, currentTarget))
            return;
        if (currentMode != chase)
            WriteValue(modeAddress, chase);
        const auto target = static_cast<std::int32_t>(pawnHandle);
        if (currentTarget != target)
            WriteValue(services + schema.observer_target_offset, target);
        if (schema.observer_chase_distance_offset) {
            const float distance = 150.0f;
            WriteValue(services + schema.observer_chase_distance_offset, distance);
        }
        if (schema.observer_chase_distance_calc_time_offset) {
            const float forceRecalculate = -1.0f;
            WriteValue(services + schema.observer_chase_distance_calc_time_offset,
                       forceRecalculate);
        }
        if (schema.observer_last_mode_offset) {
            const std::uint8_t chaseLastMode = 3;
            WriteValue(services + schema.observer_last_mode_offset, chaseLastMode);
        }
        if (schema.observer_forced_mode_offset) {
            const std::uint8_t forced = 1;
            WriteValue(services + schema.observer_forced_mode_offset, forced);
        }
    } else if (state.haveOriginalThirdperson) {
        restoreSnapshot();
    }
}

void ApplyMovement(RuntimeState& state, const FeatureConfig& config, std::uintptr_t pawn,
                   const SkinSchemaOffsets& schema) {
    const bool mouse4 = state.gameInputValid
                            ? GameButtonDown(state, Mouse4ButtonCode)
                            : GlobalInput().mouse4Down.load(std::memory_order_relaxed);
    const bool mouse4Pressed = state.gameInputValid
                                   ? GameButtonJustPressed(state, Mouse4ButtonCode)
                                   : (mouse4 && !state.previousMouse4);
    if (!config.bhopEnabled) {
        state.bhopActive = false;
    }
    if (config.bhopEnabled && mouse4Pressed) {
        state.bhopActive = !state.bhopActive;
        state.lastJump = {};
    }
    state.previousMouse4 = mouse4;

    std::int32_t health = 0;
    std::uint32_t flags = 0;
    float vx = 0.0f;
    float vy = 0.0f;
    if (!pawn || !ReadValue(pawn + schema.health_offset, health) ||
        !ReadValue(pawn + schema.flags_offset, flags) ||
        !ReadValue(pawn + schema.velocity_offset + 0x10, vx) ||
        !ReadValue(pawn + schema.velocity_offset + 0x18, vy)) {
        return;
    }
    const bool grounded = health > 0 && (flags & 1u) != 0;
    const bool directionalInput = state.gameInputValid &&
                                  (GameButtonDown(state, KeyW) || GameButtonDown(state, KeyA) ||
                                   GameButtonDown(state, KeyS) || GameButtonDown(state, KeyD));
    const bool moving = std::hypot(vx, vy) > 10.0f || directionalInput;
    const auto now = Clock::now();

    if (!grounded || !moving) {
        state.lastJump = {};
        return;
    }

    if (state.bhopActive &&
        (state.lastJump.time_since_epoch().count() == 0 ||
         now - state.lastJump >= std::chrono::milliseconds(30))) {
        WheelUp(state);
        state.lastJump = now;
    }

}

void FeatureWorker();

}  // namespace

export class FeatureRuntime {
public:
    static FeatureRuntime& Global();
    void Start();
    void Stop();
    void Tick(bool renderStage = false);
    FeatureConfig Snapshot();
    bool SniperCrosshairVisible();
    void Update(const FeatureConfig& config);
    InputState& Input();

private:
    FeatureRuntime() = default;
};

namespace {

void FeatureWorker() {
    auto& state = State();
    while (state.started.load(std::memory_order_relaxed)) {
        FeatureRuntime::Global().Tick(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

}  // namespace

FeatureRuntime& FeatureRuntime::Global() {
    static FeatureRuntime runtime;
    return runtime;
}

void FeatureRuntime::Start() {
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.started)
            return;
        LoadConfig(state);
        state.loaded = true;
        state.started = true;
        OpenUinput(state);
    }
    state.tickThread = std::thread(FeatureWorker);
}

void FeatureRuntime::Stop() {
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (!state.started)
            return;
        state.started = false;
    }
    if (state.tickThread.joinable())
        state.tickThread.join();
    std::lock_guard lock(state.mutex);
    if (state.uinputCreated)
        ioctl(state.uinput, UI_DEV_DESTROY);
    if (state.uinput >= 0)
        ::close(state.uinput);
    state.uinput = -1;
    state.uinputCreated = false;
    state.sniperCrosshairActive.store(false, std::memory_order_relaxed);
}

FeatureConfig FeatureRuntime::Snapshot() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.loaded) {
        LoadConfig(state);
        state.loaded = true;
    }
    return state.config;
}

bool FeatureRuntime::SniperCrosshairVisible() {
    return State().sniperCrosshairActive.load(std::memory_order_relaxed);
}

void FeatureRuntime::Update(const FeatureConfig& config) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    state.config = config;
    if (!config.sniperCrosshair)
        state.sniperCrosshairActive.store(false, std::memory_order_relaxed);
    SaveConfig(state.config);
    static Logger log;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "Features: config updated thirdperson=%d custom_character=%d sniper_crosshair=%d skins=%d weapon_glow=%d",
                  config.thirdpersonEnabled ? 1 : 0,
                  config.customCharacterEnabled ? 1 : 0,
                  config.sniperCrosshair ? 1 : 0,
                  config.skinsEnabled ? 1 : 0,
                  config.weaponGlowChams ? 1 : 0);
    log.info(buf);
}

InputState& FeatureRuntime::Input() {
    return GlobalInput();
}

void FeatureRuntime::Tick(bool renderStage) {
    auto& state = State();
    if (!state.started)
        return;
    std::lock_guard tickLock(state.tickMutex);
    if (!state.started)
        return;
    const auto config = Snapshot();
    if (config.bhopEnabled) {
        std::lock_guard lock(state.mutex);
        OpenUinput(state);
    }
    UpdateGameInput(state);
    const auto& client = ResolveClientOffsets();
    if (!client.resolved || !client.localPlayerController || !client.entitySystem) {
        state.sniperCrosshairActive.store(false, std::memory_order_relaxed);
        if (!state.warnedOffsets) {
            static Logger log;
            log.warn("Features: client offsets are not ready");
            state.warnedOffsets = true;
        }
        return;
    }
    const auto& schema = ResolveSkin();
    if (!schema.MovementReady()) {
        state.sniperCrosshairActive.store(false, std::memory_order_relaxed);
        if (!state.warnedSchema) {
            static Logger log;
            log.warn("Features: movement schema fields are not ready");
            state.warnedSchema = true;
        }
        return;
    }
    std::uintptr_t pawn = 0;
    std::uintptr_t controller = 0;
    std::uint32_t pawnHandle = 0;
    if (!LocalPawn(client, schema.entity_identity_size, pawn, controller, pawnHandle)) {
        state.sniperCrosshairActive.store(false, std::memory_order_relaxed);
        if (!state.warnedPawn) {
            static Logger log;
            log.warn("Features: local pawn could not be resolved from the live entity list");
            state.warnedPawn = true;
        }
        return;
    }
    const auto system = client.entitySystem;
    if (!system)
        return;

    std::lock_guard lock(state.mutex);
    ApplyUnsafe(state, config, pawn, system, client, schema);
    ApplyMovement(state, config, pawn, schema);
    // Apply on the worker as well as render-start. Some Linux client builds
    // do not dispatch the expected render-stage callback through the vtable
    // slot, while the observer fields remain valid outside that callback.
    if (schema.observer_services_offset && schema.observer_mode_offset &&
        schema.observer_target_offset)
        ApplyThirdperson(state, config, pawn, pawnHandle, schema);
}
