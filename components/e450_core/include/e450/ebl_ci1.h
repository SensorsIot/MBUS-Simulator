#pragma once

#include <span>
#include <vector>

#include "e450/telegram.h"
#include "e450/value_model.h"

namespace e450::ebl {

// Consumer Information 1 — the 5 s push list as EBL Liestal parametrises the
// E450. Thirteen objects in transmission order; FSD Appendix A.
//
// This is the *only* list v1 emits. The 15 s list (which carries the meter
// serial 0-0:96.1.0), the 1 min tariff list and the 15 min M-Bus channel list
// are out of scope — FSD §4.3. Anything tested against this therefore never learns
// a meter identity, which is a known and accepted difference from the field.
inline constexpr std::size_t CI1_OBJECT_COUNT = 13;

std::span<const CaptureObject> ci1_objects();

// Snapshot the model into the 13 positional values the object list declares.
std::vector<TelegramValue> ci1_values(const ValueModel &model);

}  // namespace e450::ebl
