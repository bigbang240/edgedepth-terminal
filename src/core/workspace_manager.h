#pragma once
#include "app_context.h"
#include "ui/widget.h"
#include <memory>
#include <vector>
namespace workspace {
// Run only at the main-thread safe point, before rendering any windows.
void tick(std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx,
          const Terminal::Pair& pair, bool enabled);
void menu();
void reset_default();
void flush();
// Capture live state before the replay context replaces it, including recent UI changes.
void checkpoint_live();
// True when the last tick ran with the workspace enabled (live, nothing
// replaying, no rebuild pending). After a hosted research replay ends, main.cpp
// keeps the live-only panels out of the frame until this is true again, which
// is the tick that re-applies the checkpoint taken when the replay began.
bool enabled();
}
