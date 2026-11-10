module;
// POSIX + errno makroları GMF'te kalır: export module öncesinde include edilmelidir
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <string_view>

export module Memory;

import Modules;

// CGameEntitySystem chunked list walk sabitleri
inline constexpr std::uint32_t IdentitiesPerChunk = 512;
inline constexpr int NumberOfChunks = 64;
inline constexpr std::size_t EntityIdentitySize = 112;
inline constexpr std::size_t EntityIdentityEntityOffset = 0;
inline constexpr std::size_t EntityIdentityHandleOffset = 16;
inline constexpr std::uint32_t HandleValidMask = 0x7FFFu;

export bool SafeRead(std::uintptr_t addr, void* dst, std::size_t size);

export std::uintptr_t ReadPointer(std::uintptr_t slot) {
    std::uintptr_t value = 0;
    return slot && SafeRead(slot, &value, sizeof(value)) ? value : 0;
}

// Geçersiz adresi pipe write trick ile güvenle tespit et (crash yerine EFAULT).
export bool IsBadReadPtr(const void* ptr, std::size_t size) {
    if (!ptr)
        return true;
    static int rfd = -1;
    static int wfd = -1;
    if (wfd < 0) {
        int filedes[2];
        if (pipe(filedes) < 0)
            return true;
        rfd = filedes[0];
        wfd = filedes[1];
        fcntl(rfd, F_SETFL, O_NONBLOCK);
        fcntl(wfd, F_SETFL, O_NONBLOCK);
    }
    const ssize_t result = write(wfd, ptr, size);
    if (result < 0) {
        if (errno == EFAULT)
            return true;
        if (errno == EAGAIN) {
            char buf[4096];
            while (read(rfd, buf, sizeof(buf)) > 0) {}
        }
    }
    return false;
}

// CEntityHandle -> entity pointer (CGameEntitySystem chunked list walk).
// entityListOffset: CGameEntitySystem::entityList üye offset'i (dinamik çözülür).
void* EntityFromHandleImpl(std::uintptr_t system, std::int8_t entityListOffset,
                           std::uint32_t handle, std::size_t identitySize) {
    if (!system)
        return nullptr;
    const auto index = handle & HandleValidMask;
    if (index == HandleValidMask || index == 0)
        return nullptr;

    const auto list = reinterpret_cast<const std::uint8_t*>(system) + entityListOffset;
    const auto chunkIndex = index / IdentitiesPerChunk;
    if (chunkIndex >= NumberOfChunks)
        return nullptr;
    void* chunk = nullptr;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(list) +
                      static_cast<std::size_t>(chunkIndex) * sizeof(void*),
                  &chunk, sizeof(chunk)) || !chunk)
        return nullptr;
    const auto identityAddress = reinterpret_cast<std::uintptr_t>(chunk) +
                                 (index % IdentitiesPerChunk) * identitySize;
    if (IsBadReadPtr(reinterpret_cast<const void*>(identityAddress), identitySize))
        return nullptr;

    std::uint32_t storedHandle = 0;
    if (!SafeRead(identityAddress + EntityIdentityHandleOffset,
                  &storedHandle, sizeof(storedHandle)) || storedHandle != handle)
        return nullptr;
    void* entity = nullptr;
    if (!SafeRead(identityAddress + EntityIdentityEntityOffset, &entity, sizeof(entity)))
        return nullptr;
    return entity;
}

export void* EntityFromHandle(std::uintptr_t system, std::int8_t entityListOffset,
                              std::uint32_t handle) {
    return EntityFromHandleImpl(system, entityListOffset, handle, EntityIdentitySize);
}

export void* EntityFromHandle(std::uintptr_t system, std::int8_t entityListOffset,
                              std::uint32_t handle, std::size_t identitySize) {
    return EntityFromHandleImpl(system, entityListOffset, handle,
                                 identitySize ? identitySize : EntityIdentitySize);
}

// x86_64 rip-relative: hedef = dispKonumu + 4 + disp32
export std::uintptr_t ResolveRipRel(std::uintptr_t dispLoc) {
    if (!dispLoc)
        return 0;
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(dispLoc), sizeof(disp));
    return dispLoc + 4 + static_cast<std::uintptr_t>(disp);
}

// Bogus pointer'ı crash'e yol açmadan güvenle kopyalar (IsBadReadPtr ile korunur).
export bool SafeRead(std::uintptr_t addr, void* dst, std::size_t size) {
    if (!addr || IsBadReadPtr(reinterpret_cast<const void*>(addr), size))
        return false;
    std::memcpy(dst, reinterpret_cast<const void*>(addr), size);
    return true;
}

export bool ReadI16(std::uintptr_t addr, std::int16_t* out) { return SafeRead(addr, out, sizeof(*out)); }
export bool ReadI32(std::uintptr_t addr, std::int32_t* out) { return SafeRead(addr, out, sizeof(*out)); }
export bool ReadPtr(std::uintptr_t addr, std::uintptr_t* out) { return SafeRead(addr, out, sizeof(*out)); }

export bool ReadString(std::uintptr_t addr, char* dst, std::size_t cap) {
    if (!addr || cap == 0)
        return false;
    char chunk[64];
    std::size_t filled = 0;
    for (;;) {
        if (!SafeRead(addr + filled, chunk, sizeof(chunk)))
            return false;
        for (std::size_t i = 0; i < sizeof(chunk); ++i) {
            if (filled + i + 1 >= cap)
                return false;
            dst[filled + i] = chunk[i];
            if (chunk[i] == '\0')
                return true;
        }
        filled += sizeof(chunk);
    }
}

// Resolver'ın localPlayerController global'ı +1 RIP quirk'ine tabidir
// (gerçek pointer base+1'de). İşaretçiyi okur, vtable'ın belirtilen modülde
// olduğunu doğrular; geçersizse nullptr döner.
export void* TryController(std::uintptr_t slot, std::string_view moduleName) {
    const auto p = ReadPointer(slot);
    if (!p || IsBadReadPtr(reinterpret_cast<const void*>(p), 8))
        return nullptr;
    std::uintptr_t vtbl = 0;
    if (!SafeRead(p, &vtbl, sizeof(vtbl)))
        return nullptr;
    if (!IsAddressInModule(moduleName, vtbl))
        return nullptr;
    return reinterpret_cast<void*>(p);
}
