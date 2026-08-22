#pragma once

#include <cstddef>
#include <span>

namespace san9::code_hook {

bool Install(void* entry, void* replacement, std::span<const unsigned char> expected,
             void** trampoline);

} // namespace san9::code_hook
