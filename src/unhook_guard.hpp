#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Streamline's DLSS-G plugin calls its linkSwapchainToCmdQueue when a swapchain is created through it. Other tools hook
// that function (REFramework does, for games that ship DLSS-G), and a hook that waits for its owner to finish starting up
// deadlocks if our swapchain is created during the game's startup. So for the duration of our own swapchain creation,
// whatever patched the start of that function is taken out: the original bytes from the DLL on disk go back in, and
// the patched bytes are put back afterwards, unchanged. Nothing is assumed about who patched it or how.

// The address of linkSwapchainToCmdQueue in a loaded sl.dlss_g.dll (found like REFramework does: by the string it
// logs), or nullptr.
void* find_link_swapchain_to_cmd_queue(HMODULE dlssg);

class TemporaryUnhook {
public:
    // fn inside `module`: compares its first bytes with the module's file on disk and restores those if a jump was
    // written over them.
    TemporaryUnhook(void* fn, HMODULE module);
    ~TemporaryUnhook();

    bool was_hooked() const { return m_active; }

private:
    static constexpr size_t BYTES = 16; // covers a 5 byte relative and a 14 byte absolute jump
    uint8_t* m_fn{};
    uint8_t m_patched[BYTES]{};
    bool m_active{};
};
