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
}
