// Copyright 2025 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef HIGHWAY_HWY_CONTRIB_THREAD_POOL_SPIN_H_
#define HIGHWAY_HWY_CONTRIB_THREAD_POOL_SPIN_H_

// Relatively power-efficient spin lock for low-latency synchronization.

#include <stdint.h>

#include <atomic>

#include "hwy/base.h"
#include "hwy/cache_control.h"  // Pause

#ifndef HWY_ENABLE_MONITORX  // allow override
// Clang 3.9 suffices for mwaitx, but the target pragma requires 9.0.
#if HWY_ARCH_X86 && ((HWY_COMPILER_CLANG >= 900) || \
                     (HWY_COMPILER_GCC_ACTUAL >= 502) || defined(__MWAITX__))
#define HWY_ENABLE_MONITORX 1
#else
#define HWY_ENABLE_MONITORX 0
#endif
#endif  // HWY_ENABLE_MONITORX

#ifndef HWY_ENABLE_UMONITOR  // allow override
#if HWY_ARCH_X86 && ((HWY_COMPILER_CLANG >= 900) || \
                     (HWY_COMPILER_GCC_ACTUAL >= 901) || defined(__WAITPKG__))
#define HWY_ENABLE_UMONITOR 1
#else
#define HWY_ENABLE_UMONITOR 0
#endif
#endif  // HWY_ENABLE_UMONITOR

#if HWY_ENABLE_MONITORX || HWY_ENABLE_UMONITOR
#include <x86intrin.h>

#include "hwy/x86_cpuid.h"
#endif  // HWY_ENABLE_MONITORX || HWY_ENABLE_UMONITOR

namespace hwy {

// Returned by `UntilDifferent` in a single register.
struct SpinResult {
  // We also use u32 because that is all that futex.h supports.
  uint32_t current;
  // Number of retries before returning, useful for checking that the
  // monitor/wait did not just return immediately.
  uint32_t reps;
};

// User-space monitor/wait are supported on Zen2+ AMD and SPR+ Intel. Spin waits
// are rarely called from SIMD code, hence we do not integrate this into
// `HWY_TARGET` and its runtime dispatch mechanism. Returned by `Type()`, also
// used by callers to set the `disabled` argument for `DetectSpin`.
enum class SpinType {
  kMonitorX,  // AMD
  kUMonitor,  // Intel
  kPause
};

// For printing which is in use.
static inline const char* ToString(SpinType type) {
  switch (type) {
    case SpinType::kMonitorX:
      return "MonitorX_C1";
    case SpinType::kUMonitor:
      return "UMonitor_C0.2";
    case SpinType::kPause:
      return "Pause";
  }

  return nullptr;
}

// Indirect function calls turn out to be too expensive because this is called
// multiple times per ThreadPool barrier. We will instead inline the spin and
// barrier using policy classes. This one is always available; use it as a
// reference for the interface. Note that Pause varies across CPUs: it can be
// a no-op, or wait 140 cycles.
struct SpinPause {
  SpinType Type() const { return SpinType::kPause; }

  // Spins until `watched != prev` and returns the new value, similar to
  // `BlockUntilDifferent` in `futex.h`.
  SpinResult UntilDifferent(const uint32_t prev,
                            std::atomic<uint32_t>& watched) const {
    for (uint32_t reps = 0;; ++reps) {
      const uint32_t current = watched.load(std::memory_order_acquire);
      if (current != prev) return SpinResult{current, reps};
      hwy::Pause();
    }
  }

  // Returns number of retries until `watched == expected`.
  size_t UntilEqual(const uint32_t expected,
                    std::atomic<uint32_t>& watched) const {
    for (size_t reps = 0;; ++reps) {
      const uint32_t current = watched.load(std::memory_order_acquire);
      if (current == expected) return reps;
      hwy::Pause();
    }
  }
};

#if HWY_ENABLE_MONITORX || HWY_IDE
HWY_PUSH_ATTRIBUTES("mwaitx")

// AMD's user-mode monitor/wait (Zen2+).
class SpinMonitorX {
 public:
  SpinType Type() const { return SpinType::kMonitorX; }

  SpinResult UntilDifferent(const uint32_t prev,
                            std::atomic<uint32_t>& watched) const {
    for (uint32_t reps = 0;; ++reps) {
      uint32_t current = watched.load(std::memory_order_acquire);
      if (current != prev) return SpinResult{current, reps};
      // No extensions/hints currently defined.
      _mm_monitorx(&watched, 0, 0);
      // Double-checked 'lock' to avoid missed events:
      current = watched.load(std::memory_order_acquire);
      if (current != prev) return SpinResult{current, reps};
      _mm_mwaitx(kExtensions, kHints, /*cycles=*/0);
    }
  }

  size_t UntilEqual(const uint32_t expected,
                    std::atomic<uint32_t>& watched) const {
    for (size_t reps = 0;; ++reps) {
      uint32_t current = watched.load(std::memory_order_acquire);
      if (current == expected) return reps;
      // No extensions/hints currently defined.
      _mm_monitorx(&watched, 0, 0);
      // Double-checked 'lock' to avoid missed events:
      current = watched.load(std::memory_order_acquire);
      if (current == expected) return reps;
      _mm_mwaitx(kExtensions, kHints, /*cycles=*/0);
    }
  }

 private:
  // 0xF would be C0. Its wakeup latency is less than 0.1 us shorter, and
  // package power is sometimes actually higher than with Pause. The
  // difference in spurious wakeups is minor.
  static constexpr unsigned kHints = 0x0;  // C1: a bit deeper than C0
  // No timeout required, we assume the mwaitx does not miss stores, see
  // https://www.usenix.org/system/files/usenixsecurity23-zhang-ruiyi.pdf.]
  static constexpr unsigned kExtensions = 0;
};

HWY_POP_ATTRIBUTES
#endif  // HWY_ENABLE_MONITORX

#if HWY_ENABLE_UMONITOR || HWY_IDE
HWY_PUSH_ATTRIBUTES("waitpkg")

// Intel's user-mode monitor/wait (SPR+).
class SpinUMonitor {
 public:
  SpinType Type() const { return SpinType::kUMonitor; }

  SpinResult UntilDifferent(const uint32_t prev,
                            std::atomic<uint32_t>& watched) const {
    for (uint32_t reps = 0;; ++reps) {
      uint32_t current = watched.load(std::memory_order_acquire);
      if (current != prev) return SpinResult{current, reps};
      _umonitor(&watched);
      // Double-checked 'lock' to avoid missed events:
      current = watched.load(std::memory_order_acquire);
      if (current != prev) return SpinResult{current, reps};
      _umwait(kControl, kDeadline);
    }
  }

  size_t UntilEqual(const uint32_t expected,
                    std::atomic<uint32_t>& watched) const {
    for (size_t reps = 0;; ++reps) {
      uint32_t current = watched.load(std::memory_order_acquire);
      if (current == expected) return reps;
      _umonitor(&watched);
      // Double-checked 'lock' to avoid missed events:
      current = watched.load(std::memory_order_acquire);
      if (current == expected) return reps;
      _umwait(kControl, kDeadline);
    }
  }

 private:
  // 1 would be C0.1. C0.2 has 20x fewer spurious wakeups and additional 4%
  // package power savings vs Pause on SPR. It comes at the cost of
  // 0.4-0.6us higher wake latency, but the total is comparable to Zen4.
  static constexpr unsigned kControl = 0;              // C0.2 for deeper sleep
  static constexpr uint64_t kDeadline = ~uint64_t{0};  // no timeout, see above
};

HWY_POP_ATTRIBUTES
#endif  // HWY_ENABLE_UMONITOR

// TODO(janwas): add WFE on Arm. May wake at 10 kHz, but still worthwhile.

// Returns the best-available type whose bit in `disabled` is not set. Example:
// to disable kUMonitor, pass `1 << static_cast<int>(SpinType::kUMonitor)`.
// Ignores `disabled` for `kPause` if it is the only supported and enabled type.
// Somewhat expensive, typically called during initialization.
static inline SpinType DetectSpin(int disabled = 0) {
  const auto HWY_MAYBE_UNUSED enabled = [disabled](SpinType type) {
    return (disabled & (1 << static_cast<int>(type))) == 0;
  };

#if HWY_ENABLE_MONITORX
  if (enabled(SpinType::kMonitorX) && x86::IsAMD()) {
    uint32_t abcd[4];
    x86::Cpuid(0x80000001U, 0, abcd);
    if (x86::IsBitSet(abcd[2], 29)) return SpinType::kMonitorX;
  }
#endif  // HWY_ENABLE_MONITORX

#if HWY_ENABLE_UMONITOR
  if (enabled(SpinType::kUMonitor) && x86::MaxLevel() >= 7) {
    uint32_t abcd[4];
    x86::Cpuid(7, 0, abcd);
    if (x86::IsBitSet(abcd[2], 5)) return SpinType::kUMonitor;
  }
#endif  // HWY_ENABLE_UMONITOR

  if (!enabled(SpinType::kPause)) {
    HWY_WARN("Ignoring attempt to disable Pause, it is the only option left.");
  }
  return SpinType::kPause;
}

// Calls `func(spin)` for the given `spin_type`.
template <class Func>
void CallWithSpin(SpinType spin_type, const Func& func) {
  switch (spin_type) {
#if HWY_ENABLE_MONITORX
    case SpinType::kMonitorX:
      func(SpinMonitorX());
      break;
#endif
#if HWY_ENABLE_UMONITOR
    case SpinType::kUMonitor:
      func(SpinUMonitor());
      break;
#endif
    case SpinType::kPause:
    default:
      func(SpinPause());
      break;
  }
}

}  // namespace hwy

#endif  // HIGHWAY_HWY_CONTRIB_THREAD_POOL_SPIN_H_
