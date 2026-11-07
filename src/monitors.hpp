#pragma once
#include <vector>

#include "input/input_types.hpp"

namespace monitors {

// This PC's monitors in desktop coordinates, straight from the OS. Empty when the OS cannot
// tell (for example a headless Wayland session): the daemon then falls back to what the GUI
// reported earlier.
std::vector<InputRect> Native();

} // namespace monitors
