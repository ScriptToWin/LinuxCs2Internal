module;
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <string_view>

export module Resolver;

import Logger;
import Modules;
import Memory;
import Offsets;
import SchemaScan;

// Dinamik imza çözücü. Hardcoded offset'ler bu build'de yanlış olduğu için
// (bak: 0x47f3fd9 -> çöp data) libclient.so içinde runtime'da AOB taramasıyla
// global adresleri/fonksiyonu çözer. Legacy parrot projesinden (build 10877660)
// birebir alınan imzalar kullanılır.

struct Sig {
    std::string_view bytes;
    std::size_t offsetToAdd;
};

// x86_64 rip-relative çözümü artık Memory::ResolveRipRel ile yapılıyor.

static std::int32_t ReadInt32At(std::uintptr_t loc) {
    std::int32_t v = 0;
    if (loc)
        std::memcpy(&v, reinterpret_cast<const void*>(loc), sizeof(v));
    return v;
}

export void* ResolveInterface(std::string_view module, const char* name) {
    void* library = dlopen(module.data(), RTLD_NOLOAD | RTLD_NOW);
    if (!library)
        return nullptr;
    using CreateInterfaceFn = void* (*)(const char*, int*);
    auto* create = reinterpret_cast<CreateInterfaceFn>(dlsym(library, "CreateInterface"));
    if (!create)
        return nullptr;

    if (void* exact = create(name, nullptr))
        return exact;

    // Source 2 interface registrations commonly append a three-digit
    // revision. The reference resolver matches by prefix, so mirror that
    // behavior while retaining the normal exact-name attempt above.
    char candidate[160] = {};
    for (int revision = 0; revision < 1000; ++revision) {
        for (int width = 1; width <= 3; ++width) {
            std::snprintf(candidate, sizeof(candidate), "%s%0*d", name, width, revision);
            if (void* versioned = create(candidate, nullptr))
                return versioned;
        }
    }
    return nullptr;
}

std::uintptr_t ResolveEntityList() {
    void* resourceService = ResolveInterface(EngineModule, "GameResourceServiceClientV0");
    if (!resourceService)
        return 0;

    std::uintptr_t resource = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(resourceService) + 0x50,
                  &resource, sizeof(resource)) || !resource)
        return 0;

    // Matches the reference project's deref(resource, 0x50, 0x10): the
    // returned address is the first bucket pointer, not another object slot.
    return resource + 0x10;
}

export struct ClientOffsets {
    std::uintptr_t localPlayerController = 0; // absolute addr of CCSPlayerController* global
    std::uintptr_t entitySystem = 0;          // entity-list bucket array base
    std::uintptr_t setModel = 0;              // absolute fn addr
    std::uintptr_t updateSubClass = 0;        // absolute fn addr (C_BaseEntity::UpdateSubClassID)
    std::int32_t m_hPawn = 0;
    std::int8_t entityList = 0;
    bool resolved = false;
    bool updateSubClassValid = false;
};

export struct InputOffsets {
    std::uintptr_t inputSystem = 0;
    std::uint32_t buttonStateOffset = 0;
    bool resolved = false;
};

export InputOffsets& ResolveInputOffsets() {
    static InputOffsets offsets;
    static bool done = false;
    if (done)
        return offsets;

    void* inputSystem = ResolveInterface(InputModule, "InputSystemVersion0");
    if (!inputSystem)
        return offsets;

    std::uintptr_t vtable = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(inputSystem), &vtable, sizeof(vtable)) || !vtable)
        return offsets;

    std::uintptr_t buttonStateMethod = 0;
    if (!SafeRead(vtable + 19 * sizeof(void*), &buttonStateMethod, sizeof(buttonStateMethod)) ||
        !buttonStateMethod)
        return offsets;

    std::uint32_t buttonStateOffset = 0;
    if (!SafeRead(buttonStateMethod + 0x14, &buttonStateOffset, sizeof(buttonStateOffset)) ||
        buttonStateOffset >= 0x10000)
        return offsets;

    offsets.inputSystem = reinterpret_cast<std::uintptr_t>(inputSystem);
    offsets.buttonStateOffset = buttonStateOffset;
    offsets.resolved = true;
    done = true;
    static Logger log;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ResolveInputOffsets: input=%llx button_state=0x%X",
                  static_cast<unsigned long long>(offsets.inputSystem),
                  offsets.buttonStateOffset);
    log.info(buf);
    return offsets;
}

export ClientOffsets& ResolveClientOffsets() {
    static ClientOffsets o;
    static bool done = false;
    static std::uint64_t attempts = 0;
    if (done)
        return o;
    // A library can expose its interface before all executable segments and
    // schema data are ready. Retry periodically instead of caching a failed
    // scan forever after a game update or an early hook callback.
    if (attempts++ != 0 && (attempts % 60) != 0)
        return o;

    static Logger log;
    log.info("ResolveClientOffsets: scanning libclient.so (dynamic signatures)");

    // kSetModel (Function)
    {
        // Linux CBaseModelEntity::SetModel prologue. Keep the complete
        // body signature so a coincidental short match cannot be called as a
        // model setter and leave the local pawn unchanged.
        constexpr const char* patterns[] = {
            "55 48 89 E5 53 48 89 FB 48 83 EC ? 48 8D 05 ? ? ? ? 48 8B 38 48 8B 07 FF 50 ? 48 89 DF 48 8B 5D ? C9 48 89 C6 E9 ? ? ? ? CC CC CC CC 55",
            "55 48 89 E5 53 48 89 FB 48 83 EC 08 48 8D 05 ? ? ? ? 48 8B 38 48 8B 07 FF 50 68"
        };
        for (const auto* pattern : patterns) {
            o.setModel = PatternScan(ClientModule, pattern);
            if (o.setModel)
                break;
        }
        if (!o.setModel)
            log.error("ResolveClientOffsets: kSetModel NOT FOUND");
    }
    // kLocalPlayerController. The reference layout resolves the displacement
    // at +3 with an eight-byte instruction size.
    {
        const Sig s{ "48 83 3D ? ? ? ? 00 0F 95 C0 C3", 3 };
        const auto m = PatternScan(ClientModule, s.bytes);
        o.localPlayerController = m ? m + 8 + ReadInt32At(m + s.offsetToAdd) : 0;
        if (!m)
            log.error("ResolveClientOffsets: kLocalPlayerController NOT FOUND");
    }
    // The reference project obtains the entity-list bucket array from the
    // engine resource service; its first bucket is at the returned address.
    o.entitySystem = ResolveEntityList();
    o.entityList = 0;

    // m_hPawn is a schema field, not a build-stable client signature.
    o.m_hPawn = ResolveSkin().controller_pawn_offset;
    // kUpdateSubClass (Function) — for knife model swap
    {
        const Sig s{ "55 48 89 E5 41 57 49 89 FF 41 56 41 55 41 54 53 48 81 EC 48 01 00 00 48 8B 07", 0 };
        const auto m = PatternScan(ClientModule, s.bytes);
        o.updateSubClass = m;
        if (!m)
            log.error("ResolveClientOffsets: kUpdateSubClass NOT FOUND");
    }

    const bool ok =
        o.localPlayerController &&
        o.entitySystem &&
        o.m_hPawn > 0 && o.m_hPawn < 0x1000 &&
        IsAddressInModule(ClientModule, o.localPlayerController);
    o.resolved = ok;
    if (ok)
        done = true;
    o.updateSubClassValid = o.updateSubClass &&
                            IsAddressInModule(ClientModule, o.updateSubClass);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "ResolveClientOffsets: resolved=%d lpc=%llx es=%llx setmodel=%llx m_hPawn=%d entityList=%d",
                  ok ? 1 : 0,
                  static_cast<unsigned long long>(o.localPlayerController),
                  static_cast<unsigned long long>(o.entitySystem),
                  static_cast<unsigned long long>(o.setModel),
                  static_cast<int>(o.m_hPawn),
                  static_cast<int>(o.entityList));
    log.info(buf);
    return o;
}
