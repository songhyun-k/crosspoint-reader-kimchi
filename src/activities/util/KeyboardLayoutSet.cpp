#include "KeyboardLayoutSet.h"

namespace keyboard_layouts {

uint16_t enabled() { return bitAt(0); }

freeink::ui::KeyboardLayoutId startingLayout() { return freeink::ui::KeyboardLayoutId::QwertyEn; }

// Even a stale layout id cannot switch an input screen away from QWERTY.
freeink::ui::KeyboardLayoutId next(const freeink::ui::KeyboardLayoutId) { return startingLayout(); }

}  // namespace keyboard_layouts
