#pragma once
#include "stream_bridge_native_decode.h"
#include <array>
namespace stream_bridge_native {
struct ImportSlot { std::uintptr_t* address=nullptr;std::uintptr_t expected=0,replacement=0; };
using Exchange = bool(*)(std::uintptr_t*,std::uintptr_t&,std::uintptr_t,void*) noexcept;
// Scalar transaction only. This does not certify an address as a native GOT.
// On conflict, restores only values still owned by this transaction.
bool publishSlots(const std::array<ImportSlot,4>&,Exchange,void*) noexcept;
void restoreSlots(const std::array<ImportSlot,4>&,Exchange,void*) noexcept;
// Cold/pre-main lifetime only. Independently verifies pinned process, public
// symbol targets and actual RW/non-executable mappings before four CAS stores.
// No mprotect, instruction patch, library unload or native file modification.
bool publishPinnedDecodeImports() noexcept;
void restorePinnedDecodeImports() noexcept;
bool decodeImportsPublished() noexcept;
}
