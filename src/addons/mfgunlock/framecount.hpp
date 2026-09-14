/*
 * Advertising and forcing the requested frame multiplier.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * Two different kinds of game need two different things.
 *
 * Cyberpunk 2077 and GTA V Enhanced have their own 2x/3x/4x selector. Once the
 * runtime reports the added capacity, the game asks for 3x or 4x by itself.
 * Forcing anything would override the player's own choice, so this is OFF by
 * default. STALKER 2 also has a native selector, but builds it from
 * slDLSSGGetState::numFramesToGenerateMax. That return value is raised to the
 * wrapper ceiling verified by addon.cpp so 3x/4x actually appear in its menu.
 *
 * Deep Rock Galactic (and anything else where frame generation is just a
 * on/off toggle) will only ever ask for 1 generated frame, no matter how
 * capable the runtime claims to be. Nothing downstream can help: sl.dlss_g
 * loops numFramesToGenerate times, so a request of 1 produces exactly one
 * generated frame. This is the job the NVIDIA App does for 50-series users via
 * a driver registry setting -- overriding the request, not the capability.
 *
 * The lever is slDLSSGSetOptions. It is not exported: the app obtains it
 * through slGetFeatureFunction (which sl.interposer.dll does export), so we
 * hook that, hand back a wrapper, and replace numFramesToGenerate in transit.
 *
 * DLSSGOptions::numFramesToGenerate counts GENERATED frames, not total:
 * 2x -> 1, 3x -> 2, 4x -> 3 (sl_dlss_g.h). The struct is passed by const
 * reference; the wrapper builds an addon-owned options copy and never writes to
 * the caller's structure.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <include/reshade.hpp>

#include "./force_policy.hpp"
#include "./hdr_compat.hpp"
#include "./ngx_hook.hpp"
#include "./pacing_policy.hpp"
#include "./runtime_version.hpp"

namespace mfgunlock::framecount {

// 0 = leave the game's request alone. 2..6 = force that exact multiplier.
inline std::atomic<unsigned int> g_force_multiplier{0};
inline std::atomic_bool g_hooked{false};
inline std::atomic_bool g_intercepted{false};
inline std::atomic_bool g_game_request_seen{false};
inline std::atomic<unsigned int> g_last_requested{0};
inline std::atomic<unsigned int> g_last_forced{0};
inline std::atomic_bool g_effective_request_seen{false};
inline std::atomic<unsigned int> g_last_effective_generated{0};
inline std::atomic<unsigned int> g_fixed_override_status{
    static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kNative)};
inline std::atomic_bool g_declined_no_pacing{false};
inline std::atomic<unsigned int> g_force_failed_for{0};
inline std::atomic<unsigned int> g_last_result{0};
inline std::atomic_bool g_native_request_seen{false};
inline std::atomic<unsigned int> g_native_requested{0};
inline std::atomic<unsigned int> g_native_result{0};
inline std::atomic_bool g_state_seen{false};
inline std::atomic<unsigned int> g_state_result{0};
inline std::atomic<unsigned int> g_dlssg_status{0};
inline std::atomic_bool g_status_ok_logged{false};
inline std::atomic_bool g_failure_status_logged{false};
inline std::atomic<unsigned int> g_actual_frames_presented{0};
inline std::atomic<unsigned int> g_max_actual_frames_presented{0};
inline std::atomic<unsigned long long> g_state_samples{0};
inline std::atomic<unsigned int> g_seen_present_counts{0};
inline std::atomic_bool g_addon_enabled{true};

// DLSS-G 4.5/Streamline v5 can select the generated-frame count itself. The
// provider owns its pacing, refresh-rate detection and multiplier hysteresis;
// this addon only requests eDynamic after GetState explicitly reports support.
// A target of zero asks NVIDIA to follow the active display refresh rate.
inline std::atomic_bool g_dynamic_mfg_enabled{false};
inline std::atomic<unsigned int> g_dynamic_target_fps{0};
inline std::atomic_bool g_dynamic_d3d12{false};
inline std::atomic_bool g_dynamic_support_seen{false};
inline std::atomic_bool g_dynamic_supported{false};
inline std::atomic_bool g_dynamic_applied{false};
inline std::atomic_bool g_dynamic_fell_back{false};
inline std::atomic_bool g_dynamic_runtime_declined{false};
inline std::atomic<unsigned int> g_dynamic_result{0};
inline std::atomic<unsigned int> g_dynamic_set_failures{0};
inline std::atomic_bool g_dynamic_change_pending{false};
inline std::atomic_bool g_vsync_support_seen{false};
inline std::atomic_bool g_vsync_supported{false};
inline std::atomic<unsigned int> g_dynamic_state_probe_failures{0};
inline std::atomic_bool g_reflex_hooked{false};
inline std::atomic_bool g_reflex_options_seen{false};
inline std::atomic_bool g_reflex_limit_applied{false};
inline std::atomic<unsigned int> g_reflex_native_limit_us{0};
inline std::atomic<unsigned int> g_reflex_effective_limit_us{0};
inline std::atomic<unsigned int> g_reflex_limit_result{0};
inline std::atomic_bool g_reflex_limit_failure_logged{false};
// A Reflex limiter controls application-rendered frames, not final displayed
// frames. It therefore cannot enforce Dynamic MFG's output target while VSync
// makes Streamline ignore dynamicTargetFrameRate. Keep it as an explicit
// advanced source-frame cap instead of silently changing the game's limiter.
inline std::atomic_bool g_dynamic_reflex_source_cap{false};

// Dynamic MFG is release-supported only on the validated 310.9.1 / 2.14.1
// stack. The provider capability bit remains authoritative, while these loaded
// module checks prevent an older OTA Streamline wrapper from masquerading as
// the manually installed 2.14.1 runtime.
inline std::atomic_bool g_dlssg_version_seen{false};
inline std::atomic_bool g_dlssg_310_9_1_seen{false};
inline std::atomic_bool g_dlssg_other_version_seen{false};
inline std::atomic<uint64_t> g_last_dlssg_version{0};
inline std::atomic_bool g_streamline_version_seen{false};
inline std::atomic_bool g_streamline_2_14_1_active{false};
inline std::atomic<uint64_t> g_active_streamline_version{0};
inline std::atomic_bool g_streamline_function_owner_seen{false};

inline void ObserveDlssgProviderVersion(HMODULE module) {
  const auto version = runtimeversion::FromModule(module);
  if (!version.valid) return;
  const uint64_t packed = runtimeversion::Pack(version);
  const bool supported = runtimeversion::Is(packed, 310, 9, 1);
  if (supported || !g_dlssg_310_9_1_seen.load(std::memory_order_acquire)) {
    g_last_dlssg_version.store(packed, std::memory_order_release);
  }
  g_dlssg_version_seen.store(true, std::memory_order_release);
  bool first_version_class = false;
  if (supported) {
    first_version_class = !g_dlssg_310_9_1_seen.exchange(
        true, std::memory_order_acq_rel);
  } else {
    first_version_class = !g_dlssg_other_version_seen.exchange(
        true, std::memory_order_acq_rel);
  }
  if (first_version_class) {
    std::stringstream stream;
    stream << "mfgunlock: verified mapped DLSS-G provider candidate version "
           << version.major << '.' << version.minor << '.' << version.patch
           << '.' << version.revision
           << (supported ? " (Dynamic MFG release stack)."
                         : " (general compatibility only; Dynamic MFG is not release-supported)." );
    reshade::log::message(supported ? reshade::log::level::info
                                    : reshade::log::level::warning,
                          stream.str().c_str());
  }
}

inline void ObserveStreamlinePluginVersion(HMODULE module,
                                           bool authoritative = false) {
  if (!authoritative &&
      g_streamline_function_owner_seen.load(std::memory_order_acquire)) {
    return;
  }
  const auto version = runtimeversion::FromModule(module);
  if (!version.valid) return;
  if (authoritative) {
    g_streamline_function_owner_seen.store(true, std::memory_order_release);
  }
  const uint64_t packed = runtimeversion::Pack(version);
  const uint64_t previous = g_active_streamline_version.exchange(
      packed, std::memory_order_acq_rel);
  g_streamline_version_seen.store(true, std::memory_order_release);
  const bool supported = runtimeversion::Is(packed, 2, 14, 1);
  g_streamline_2_14_1_active.store(supported, std::memory_order_release);
  if (previous != packed) {
    std::vector<char> path(32768);
    const DWORD path_length = GetModuleFileNameA(
        module, path.data(), static_cast<DWORD>(path.size()));
    std::stringstream stream;
    stream << "mfgunlock: observed Streamline DLSS-G wrapper version "
           << version.major << '.' << version.minor << '.' << version.patch
           << '.' << version.revision;
    if (path_length != 0 && path_length < path.size()) {
      stream << " from " << path.data();
    }
    stream
           << (supported ? " (Dynamic MFG release stack)."
                          : " (general compatibility only; Dynamic MFG is not release-supported)." );
    reshade::log::message(supported ? reshade::log::level::info
                                    : reshade::log::level::warning,
                          stream.str().c_str());
  }
}

// Published by addon.cpp only after the active Streamline wrapper's pacing and
// hard ceiling have both been verified. Games such as STALKER 2 build their
// native 2x/3x/4x selector from DLSSGState::numFramesToGenerateMax rather than
// from the NGX parameter block, so the SetOptions hook alone cannot expose the
// additional choices.
inline std::atomic<unsigned int> g_advertised_max_generated{0};
inline std::atomic_bool g_capacity_advertised{false};
inline std::atomic<unsigned int> g_runtime_max_generated{0};

// The game owns Streamline provider selection by default. The two explicit
// overrides only adjust NVIDIA's documented slInit OTA flags and require a
// restart: local mode clears both flags so a game-folder runtime can be tested,
// while OTA mode restores both flags so Streamline can use its downloaded set.
// Neither mode mixes individual DLLs or bypasses Streamline's validation.
enum class RuntimeSelectionMode : unsigned int {
  kGameDefault = 0,
  kPreferLocal = 1,
  kForceOta = 2,
};
inline std::atomic<unsigned int> g_runtime_selection_mode{
    static_cast<unsigned int>(RuntimeSelectionMode::kGameDefault)};
inline std::atomic_bool g_runtime_selection_observed{false};
inline std::atomic<unsigned long long> g_runtime_flags_before{0};
inline std::atomic<unsigned long long> g_runtime_flags_after{0};
inline std::atomic<unsigned int> g_runtime_selection_result{0};

// Current Streamline providers own working native pacing and need no metering
// mutation. These callbacks are a guard only when the user explicitly requests
// the legacy software-flip compatibility path. By SetOptions time the plugin is
// loaded, so a requested legacy patch gets one last bounded attempt; if it still
// cannot be verified, the overridden count is declined rather than risking a freeze.
inline void (*g_ensure_pacing)() = nullptr;
inline bool (*g_pacing_ready)() = nullptr;

namespace internal {

using SetOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
using GetStateFn =
    sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
using GetFeatureFunctionFn = sl::Result (*)(sl::Feature, const char*, void*&);
using InitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using ReflexSetOptionsFn = PFun_slReflexSetOptions*;

inline std::atomic<SetOptionsFn> g_real_set_options{nullptr};
inline std::atomic<GetStateFn> g_real_get_state{nullptr};
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;
inline std::atomic<ReflexSetOptionsFn> g_real_reflex_set_options{nullptr};
inline std::atomic_bool g_set_options_wrapped_logged{false};
inline std::atomic_bool g_get_state_wrapped_logged{false};
inline std::atomic_bool g_reflex_wrapped_logged{false};

inline SRWLOCK g_reflex_options_lock = SRWLOCK_INIT;
inline sl::ReflexOptions g_last_native_reflex_options{};
inline bool g_last_native_reflex_options_valid = false;

inline bool ShouldApplyReflexTarget() {
  return pacing::ShouldApplyReflexSourceCap(
      g_dynamic_mfg_enabled.load(std::memory_order_relaxed),
      g_dynamic_d3d12.load(std::memory_order_relaxed),
      g_dynamic_support_seen.load(std::memory_order_acquire),
      g_dynamic_supported.load(std::memory_order_relaxed),
      g_dynamic_applied.load(std::memory_order_relaxed),
      g_dynamic_reflex_source_cap.load(std::memory_order_relaxed),
      g_dynamic_target_fps.load(std::memory_order_relaxed));
}

inline bool DynamicVersionStackReady() {
  return g_streamline_2_14_1_active.load(std::memory_order_acquire) &&
         g_dlssg_310_9_1_seen.load(std::memory_order_acquire);
}

inline bool IsTransientDynamicFailure(sl::Result result) {
  return result == sl::Result::eErrorDeviceNotCreated ||
         result == sl::Result::eErrorNGXFailed ||
         result == sl::Result::eErrorNotInitialized ||
         result == sl::Result::eErrorInitNotCalled ||
         result == sl::Result::eErrorFeatureManagerInvalidState ||
         result == sl::Result::eErrorInvalidState;
}

inline void ObserveStreamlineFunctionOwner(const void* function) {
  HMODULE owner = nullptr;
  const auto version = runtimeversion::FromAddress(function, &owner);
  if (!version.valid || owner == nullptr) return;

  std::vector<wchar_t> path(32768);
  const DWORD length =
      GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return;
  for (DWORD index = 0; index < length; ++index) {
    if (path[index] >= L'A' && path[index] <= L'Z') {
      path[index] = static_cast<wchar_t>(path[index] - L'A' + L'a');
    }
  }
  // Another ReShade addon can sit between this hook and Streamline. Do not
  // mistake that addon's file version for the active NVIDIA wrapper version;
  // retain the directly discovered sl.dlss_g candidate in that case.
  const bool is_streamline_owner =
      std::wcsstr(path.data(), L"sl.dlss_g") != nullptr ||
      std::wcsstr(path.data(), L"\\models\\sl_dlss_g_0\\") != nullptr ||
      std::wcsstr(path.data(), L"sl.interposer") != nullptr;
  if (is_streamline_owner) ObserveStreamlinePluginVersion(owner, true);
}

inline sl::Result SubmitReflexOptions(const sl::ReflexOptions& native_options) {
  const auto real = g_real_reflex_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;

  sl::ReflexOptions forwarded = native_options;
  const bool apply_target = ShouldApplyReflexTarget();
  if (apply_target) {
    forwarded.frameLimitUs = pacing::TargetFpsToFrameLimitUs(
        g_dynamic_target_fps.load(std::memory_order_relaxed));
  }

  const sl::Result result = real(forwarded);
  g_reflex_native_limit_us.store(native_options.frameLimitUs,
                                 std::memory_order_relaxed);
  g_reflex_effective_limit_us.store(forwarded.frameLimitUs,
                                    std::memory_order_relaxed);
  g_reflex_limit_result.store(static_cast<unsigned int>(result),
                              std::memory_order_relaxed);
  const bool was_applied = g_reflex_limit_applied.exchange(
      apply_target && result == sl::Result::eOk, std::memory_order_acq_rel);

  if (apply_target && result == sl::Result::eOk && !was_applied) {
    g_reflex_limit_failure_logged.store(false, std::memory_order_relaxed);
    std::stringstream s;
    s << "mfgunlock: advanced Reflex source-frame cap applied ("
      << native_options.frameLimitUs << " us -> " << forwarded.frameLimitUs
      << " us). This limits application-rendered frames; it is not a Dynamic "
         "MFG output-FPS target.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  } else if (apply_target && result != sl::Result::eOk &&
             !g_reflex_limit_failure_logged.exchange(true,
                                                      std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: Reflex rejected the advanced source-frame cap with sl::Result "
      << static_cast<unsigned int>(result)
      << "; Dynamic MFG remains active and the game's native Reflex settings are preserved.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
  } else if (!apply_target && was_applied && result == sl::Result::eOk) {
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: restored the game's native Reflex frame-limit setting.");
  }
  return result;
}

inline void RefreshReflexTarget() {
  if (g_real_reflex_set_options.load(std::memory_order_acquire) == nullptr) return;
  sl::ReflexOptions native_options{};
  AcquireSRWLockShared(&g_reflex_options_lock);
  const bool valid = g_last_native_reflex_options_valid;
  if (valid) native_options = g_last_native_reflex_options;
  ReleaseSRWLockShared(&g_reflex_options_lock);
  if (valid) SubmitReflexOptions(native_options);
}

inline sl::Result HookedReflexSetOptions(const sl::ReflexOptions& options) {
  const auto real = g_real_reflex_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;
  if (options.structType != sl::ReflexOptions::s_structType ||
      options.structVersion != sl::kStructVersion1) {
    return real(options);
  }

  AcquireSRWLockExclusive(&g_reflex_options_lock);
  g_last_native_reflex_options = options;
  g_last_native_reflex_options_valid = true;
  ReleaseSRWLockExclusive(&g_reflex_options_lock);
  g_reflex_options_seen.store(true, std::memory_order_release);
  return SubmitReflexOptions(options);
}

inline sl::Result CallSetOptions(const sl::ViewportHandle& viewport,
                                 const sl::DLSSGOptions& options,
                                 uint32_t generated_frames = 0,
                                 bool override_generated_frames = false) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_addon_enabled.load(std::memory_order_relaxed))
    return real(viewport, options);

  const bool game_enabled = options.mode != sl::DLSSGMode::eOff;
  const bool dynamic =
      game_enabled && g_dynamic_mfg_enabled.load(std::memory_order_relaxed) &&
      g_dynamic_d3d12.load(std::memory_order_relaxed) &&
      DynamicVersionStackReady() &&
      g_dynamic_support_seen.load(std::memory_order_acquire) &&
      g_dynamic_supported.load(std::memory_order_relaxed) &&
      !g_dynamic_runtime_declined.load(std::memory_order_relaxed);

  const auto call_original = [&]() {
    const sl::Result result = real(viewport, options);
    if (result == sl::Result::eOk && game_enabled) {
      if (!g_dynamic_mfg_enabled.load(std::memory_order_relaxed)) {
        g_dynamic_change_pending.store(false, std::memory_order_release);
      }
      // Configuration changes originate on the overlay thread, but
      // Streamline/Reflex calls belong on the game's own submission thread.
      // Restore a previously overridden source cap only at this safe boundary.
      if (!g_dynamic_mfg_enabled.load(std::memory_order_relaxed) &&
          g_reflex_limit_applied.load(std::memory_order_acquire)) {
        RefreshReflexTarget();
      }
    }
    return result;
  };

  if (!dynamic && !override_generated_frames)
    return call_original();

  sl::DLSSGOptions forwarded{};
  const float target = static_cast<float>(
      g_dynamic_target_fps.load(std::memory_order_relaxed));
  if (!hdrcompat::BuildAdvancedOptions(options, forwarded, generated_frames,
                                       override_generated_frames,
                                       dynamic, target)) {
    if (override_generated_frames) {
      static std::atomic_bool warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: refusing to modify an unknown DLSSGOptions ABI; forwarding the game's request unchanged.");
      }
    }
    if (dynamic) {
      g_dynamic_applied.store(false, std::memory_order_relaxed);
      g_dynamic_runtime_declined.store(true, std::memory_order_release);
      g_dynamic_change_pending.store(false, std::memory_order_release);
      g_dynamic_result.store(
          static_cast<unsigned int>(sl::Result::eErrorUnsupportedInterface),
          std::memory_order_relaxed);
      if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: Dynamic MFG was not submitted because the game supplied an unknown DLSSGOptions ABI; fixed mode remains active.");
      }
    }
    return call_original();
  }

  sl::Result result = real(viewport, forwarded);
  if (result == sl::Result::eOk) {
    if (dynamic) {
      g_dynamic_result.store(static_cast<unsigned int>(result),
                             std::memory_order_relaxed);
      g_dynamic_set_failures.store(0, std::memory_order_relaxed);
      g_dynamic_fell_back.store(false, std::memory_order_relaxed);
      g_dynamic_change_pending.store(false, std::memory_order_release);
    }
    if (!g_dynamic_mfg_enabled.load(std::memory_order_relaxed) &&
        g_reflex_limit_applied.load(std::memory_order_acquire)) {
      RefreshReflexTarget();
    }
    const bool first_dynamic_apply =
        dynamic && !g_dynamic_applied.exchange(true, std::memory_order_acq_rel);
    if (first_dynamic_apply) {
      std::stringstream s;
      s << "mfgunlock: NVIDIA Dynamic MFG accepted (target "
        << (target == 0.0f ? "active-display refresh" : std::to_string(target) + " FPS")
        << "); multiplier selection and pacing remain provider-controlled.";
      reshade::log::message(reshade::log::level::info, s.str().c_str());
      RefreshReflexTarget();
    }
    return result;
  }

  // A rejected call is retried once with the same options below: transient
  // feature-manager state is the usual cause, and giving up on the first
  // failure would silently lose a working configuration.
  if (dynamic) {
    const sl::Result retry = real(viewport, forwarded);
    if (retry == sl::Result::eOk) {
      g_dynamic_result.store(static_cast<unsigned int>(retry),
                             std::memory_order_relaxed);
      g_dynamic_set_failures.store(0, std::memory_order_relaxed);
      g_dynamic_fell_back.store(false, std::memory_order_relaxed);
      g_dynamic_change_pending.store(false, std::memory_order_release);
      const bool first_dynamic_apply =
          !g_dynamic_applied.exchange(true, std::memory_order_acq_rel);
      if (first_dynamic_apply) {
        reshade::log::message(
            reshade::log::level::info,
            "mfgunlock: Dynamic MFG was accepted on the bounded retry after transient runtime state.");
        RefreshReflexTarget();
      }
      return retry;
    }
    result = retry;
  }
  // Dynamic accounting only. A failed fixed-multiplier override must not touch
  // Dynamic state: doing so would record a Dynamic rejection that never
  // happened and could park g_dynamic_runtime_declined, blocking Dynamic MFG
  // from being attempted again until the next FG off/on cycle. The fixed
  // override's own callers report that failure.
  if (dynamic) {
    g_dynamic_result.store(static_cast<unsigned int>(result),
                           std::memory_order_relaxed);
    constexpr unsigned int kMaxTransientDynamicFailures = 4;
    const unsigned int failures =
        g_dynamic_set_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool retry_later = IsTransientDynamicFailure(result) &&
                             failures < kMaxTransientDynamicFailures;
    g_dynamic_change_pending.store(retry_later, std::memory_order_release);
    g_dynamic_runtime_declined.store(!retry_later, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_release);
    RefreshReflexTarget();
    if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
      std::stringstream s;
      s << "mfgunlock: NVIDIA Dynamic MFG was rejected with sl::Result "
        << static_cast<unsigned int>(result)
        << "; restoring the game's fixed mode. "
        << (retry_later
                ? "The next game-side SetOptions call will retry because this result can be transient."
                : "Toggle frame generation or the addon option to retry.");
      reshade::log::message(reshade::log::level::warning, s.str().c_str());
    }
  }
  return call_original();
}

// Which sl.dlss_g the game ends up using is influenced here at slInit, not by
// the mere presence of a replacement DLL on disk.
//
// Streamline can load plugins the driver has downloaded into
// C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\... but only if the
// app asks for it at slInit. Cyberpunk and Deep Rock Galactic do, which is why
// both silently run a 2.12.129.0 plugin regardless of the (much older) copies
// sitting in their folders. GTA V Enhanced does not, so it is stuck with
// whatever is on disk beside it -- and that copy was clamped to 3 generated
// frames.
//
// Both flags are in the SDK's own default; the app can actively drop them. We
// preserve the game's request unless the user selected an explicit policy.
inline sl::Result HookedInit(const sl::Preferences& pref, uint64_t sdk_version) {
  if (g_real_init == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_addon_enabled.load(std::memory_order_relaxed))
    return g_real_init(pref, sdk_version);

  constexpr uint64_t kOta = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                            static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);
  const auto mode = static_cast<RuntimeSelectionMode>(
      g_runtime_selection_mode.load(std::memory_order_relaxed));
  if (mode == RuntimeSelectionMode::kGameDefault)
    return g_real_init(pref, sdk_version);
  if (pref.structType != sl::Preferences::s_structType ||
      pref.structVersion != sl::kStructVersion1) {
    reshade::log::message(
        reshade::log::level::warning,
        "mfgunlock: runtime selection was not applied because sl::Preferences uses an unknown ABI; preserving the game's slInit request.");
    return g_real_init(pref, sdk_version);
  }

  const uint64_t before = static_cast<uint64_t>(pref.flags);
  const uint64_t after = mode == RuntimeSelectionMode::kPreferLocal
                             ? (before & ~kOta)
                             : (before | kOta);
  sl::Preferences forwarded = pref;
  forwarded.flags = static_cast<sl::PreferenceFlags>(after);
  const sl::Result result = g_real_init(forwarded, sdk_version);

  g_runtime_flags_before.store(before, std::memory_order_relaxed);
  g_runtime_flags_after.store(after, std::memory_order_relaxed);
  g_runtime_selection_result.store(static_cast<unsigned int>(result),
                                   std::memory_order_relaxed);
  g_runtime_selection_observed.store(true, std::memory_order_release);

  std::stringstream s;
  s << "mfgunlock: slInit runtime policy "
    << (mode == RuntimeSelectionMode::kPreferLocal ? "prefer-local" : "force-OTA")
    << " changed flags 0x" << std::hex << before << " -> 0x" << after << std::dec
    << (mode == RuntimeSelectionMode::kPreferLocal
            ? " (OTA download and downloaded-plugin loading disabled)"
            : " (OTA download and downloaded-plugin loading enabled)")
    << (result == sl::Result::eOk ? "." : " -- but slInit returned an error, so this had no effect.");
  reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                  : reshade::log::level::warning,
                        s.str().c_str());
  return result;
}

inline sl::Result HookedSetOptions(const sl::ViewportHandle& viewport,
                                   const sl::DLSSGOptions& options) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr)
    return sl::Result::eErrorNotInitialized;
  if (!g_addon_enabled.load(std::memory_order_relaxed))
    return real(viewport, options);

  const bool game_enabled = options.mode != sl::DLSSGMode::eOff;
  if (!game_enabled) {
    // A deliberate game-side off/on cycle is a safe retry boundary after a
    // transient or provider-version rejection.
    g_dynamic_runtime_declined.store(false, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_relaxed);
  }
  const bool dynamic_ready =
      game_enabled && g_dynamic_mfg_enabled.load(std::memory_order_relaxed) &&
      g_dynamic_d3d12.load(std::memory_order_relaxed) &&
      DynamicVersionStackReady() &&
      g_dynamic_support_seen.load(std::memory_order_acquire) &&
      g_dynamic_supported.load(std::memory_order_relaxed) &&
      !g_dynamic_runtime_declined.load(std::memory_order_relaxed);
  const unsigned int multiplier =
      g_force_multiplier.load(std::memory_order_relaxed);
  const uint32_t requested = options.numFramesToGenerate;
  if (game_enabled) {
    g_last_requested.store(options.numFramesToGenerate, std::memory_order_relaxed);
    g_game_request_seen.store(true, std::memory_order_release);
  }

  const forcepolicy::RequestDecision decision = forcepolicy::Resolve(
      requested, multiplier, dynamic_ready, game_enabled);

  // Frame Generation Off is never rewritten. A selected fixed override remains
  // pending and is applied only when the game submits its next enabled call.
  if (!game_enabled) {
    const sl::Result result = CallSetOptions(viewport, options);
    g_last_forced.store(0, std::memory_order_relaxed);
    g_effective_request_seen.store(false, std::memory_order_release);
    g_fixed_override_status.store(
        static_cast<unsigned int>(
            forcepolicy::IsFixedMultiplier(multiplier)
                ? forcepolicy::FixedOverrideStatus::kPending
                : forcepolicy::FixedOverrideStatus::kNative),
        std::memory_order_release);
    return result;
  }

  // Dynamic has priority only while its validated runtime path is actively
  // eligible. Fixed selection remains saved for the next non-Dynamic call.
  if (decision.source == forcepolicy::RequestSource::kDynamic) {
    const sl::Result result = CallSetOptions(viewport, options);
    g_last_forced.store(0, std::memory_order_relaxed);
    if (g_dynamic_applied.load(std::memory_order_acquire)) {
      g_effective_request_seen.store(false, std::memory_order_release);
      g_fixed_override_status.store(
          static_cast<unsigned int>(
              forcepolicy::FixedOverrideStatus::kDynamicPriority),
          std::memory_order_release);
    } else {
      if (result == sl::Result::eOk) {
        g_last_effective_generated.store(requested, std::memory_order_relaxed);
        g_effective_request_seen.store(true, std::memory_order_release);
      } else {
        g_effective_request_seen.store(false, std::memory_order_release);
      }
      g_fixed_override_status.store(
          static_cast<unsigned int>(
              forcepolicy::IsFixedMultiplier(multiplier)
                  ? forcepolicy::FixedOverrideStatus::kPending
                  : forcepolicy::FixedOverrideStatus::kNative),
          std::memory_order_release);
    }
    return result;
  }

  if (decision.source == forcepolicy::RequestSource::kNative) {
    const sl::Result result = CallSetOptions(viewport, options);
    g_last_forced.store(0, std::memory_order_relaxed);
    g_declined_no_pacing.store(false, std::memory_order_relaxed);
    g_fixed_override_status.store(
        static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kNative),
        std::memory_order_release);
    if (result == sl::Result::eOk) {
      g_last_effective_generated.store(requested, std::memory_order_relaxed);
      g_effective_request_seen.store(true, std::memory_order_release);
    } else {
      g_effective_request_seen.store(false, std::memory_order_release);
    }
    const unsigned int previous =
        g_native_requested.exchange(requested, std::memory_order_relaxed);
    const unsigned int raw_result = static_cast<unsigned int>(result);
    const unsigned int previous_result =
        g_native_result.exchange(raw_result, std::memory_order_relaxed);
    const bool first =
        !g_native_request_seen.exchange(true, std::memory_order_relaxed);
    if (first || previous != requested || previous_result != raw_result) {
      std::stringstream s;
      s << "mfgunlock: native slDLSSGSetOptions requested numFramesToGenerate="
        << requested << " (" << (requested + 1)
        << "x) and returned sl::Result " << raw_result << ".";
      reshade::log::message(result == sl::Result::eOk
                                ? reshade::log::level::info
                                : reshade::log::level::warning,
                            s.str().c_str());
    }
    return result;
  }

  const uint32_t desired = decision.downstream_generated_frames;
  if (!decision.OverridesGeneratedFrames()) {
    const sl::Result result = CallSetOptions(viewport, options);
    g_last_result.store(static_cast<unsigned int>(result),
                        std::memory_order_relaxed);
    g_last_forced.store(0, std::memory_order_relaxed);
    if (result == sl::Result::eOk) {
      g_last_effective_generated.store(requested, std::memory_order_relaxed);
      g_effective_request_seen.store(true, std::memory_order_release);
    } else {
      g_effective_request_seen.store(false, std::memory_order_release);
    }
    g_fixed_override_status.store(
        static_cast<unsigned int>(
            forcepolicy::FixedOverrideStatus::kMatchedGameRequest),
        std::memory_order_release);
    g_declined_no_pacing.store(false, std::memory_order_relaxed);
    g_force_failed_for.store(0, std::memory_order_relaxed);
    return result;
  }

  if (options.structVersion < sl::kStructVersion1 ||
      options.structVersion > sl::kStructVersion5) {
    static std::atomic_bool warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: refusing to override an unknown DLSSGOptions ABI; forwarding the game's request unchanged.");
    }
    const sl::Result result = CallSetOptions(viewport, options);
    g_last_result.store(static_cast<unsigned int>(result),
                        std::memory_order_relaxed);
    g_last_forced.store(0, std::memory_order_relaxed);
    if (result == sl::Result::eOk) {
      g_last_effective_generated.store(requested, std::memory_order_relaxed);
      g_effective_request_seen.store(true, std::memory_order_release);
    } else {
      g_effective_request_seen.store(false, std::memory_order_release);
    }
    g_fixed_override_status.store(
        static_cast<unsigned int>(
            forcepolicy::FixedOverrideStatus::kUnsupportedAbi),
        std::memory_order_release);
    return result;
  }

  // Native pacing is ready without a patch. This branch becomes false only
  // when the user explicitly selected legacy software-flip compatibility and
  // that old provider field has not been verified yet.
  if (desired > 1) {
    if (g_pacing_ready != nullptr && !g_pacing_ready() && g_ensure_pacing != nullptr) {
      g_ensure_pacing();
    }
    if (g_pacing_ready != nullptr && !g_pacing_ready()) {
      if (!g_declined_no_pacing.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: NOT forcing the multiplier -- legacy software-flip compatibility "
            "was requested, but its provider field could not be verified. Leaving the "
            "game's own request alone.");
      }
      const sl::Result result = CallSetOptions(viewport, options);
      g_last_result.store(static_cast<unsigned int>(result),
                          std::memory_order_relaxed);
      g_last_forced.store(0, std::memory_order_relaxed);
      if (result == sl::Result::eOk) {
        g_last_effective_generated.store(requested, std::memory_order_relaxed);
        g_effective_request_seen.store(true, std::memory_order_release);
      } else {
        g_effective_request_seen.store(false, std::memory_order_release);
      }
      g_fixed_override_status.store(
          static_cast<unsigned int>(
              forcepolicy::FixedOverrideStatus::kBlockedByPacing),
          std::memory_order_release);
      return result;
    }
  }

  // Forward through a local copy of the options so the requested count can be
  // overridden without modifying the game-owned structure.
  sl::Result result = CallSetOptions(viewport, options, desired, true);

  // A rejected call means frame generation just stays off, which looks exactly
  // like "the mod broke FG". Never leave the game worse than we found it: if
  // the runtime refuses the overridden count, put the original request through so
  // the player still gets the frame generation they asked for.
  if (result != sl::Result::eOk) {
    // eErrorFeatureManagerInvalidState is not "your count is too high" -- it can
    // simply mean DLSS-G was not ready yet. Retry once with the SAME overridden
    // count: if that succeeds the first failure was transient state, and
    // dropping straight back to the game's request would have silently given up
    // a working 4x. Only if the retry fails too is the count really refused.
    const sl::Result retry = CallSetOptions(viewport, options, desired, true);

    if (retry == sl::Result::eOk) {
      if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGSetOptions returned " << static_cast<unsigned int>(result)
          << " on the first attempt but accepted numFramesToGenerate=" << desired
          << " on retry -- that first failure was feature-manager state, not the count.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
      g_last_result.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
      g_last_forced.store(desired, std::memory_order_relaxed);
      g_last_effective_generated.store(desired, std::memory_order_relaxed);
      g_effective_request_seen.store(true, std::memory_order_release);
      g_fixed_override_status.store(
          static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kApplied),
          std::memory_order_release);
      g_declined_no_pacing.store(false, std::memory_order_relaxed);
      g_force_failed_for.store(0, std::memory_order_relaxed);
      return retry;
    }

    if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGSetOptions refused numFramesToGenerate=" << desired
        << " twice (sl::Result " << static_cast<unsigned int>(result) << " then "
        << static_cast<unsigned int>(retry) << "); falling back to the game's own request of "
        << requested << ". The count itself is being refused, not a transient state.";
      reshade::log::message(reshade::log::level::warning, s.str().c_str());
    }
    g_last_result.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
    g_last_forced.store(0, std::memory_order_relaxed);
    const sl::Result fallback = CallSetOptions(viewport, options);
    if (fallback == sl::Result::eOk) {
      g_last_effective_generated.store(requested, std::memory_order_relaxed);
      g_effective_request_seen.store(true, std::memory_order_release);
    } else {
      g_effective_request_seen.store(false, std::memory_order_release);
    }
    g_fixed_override_status.store(
        static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kRejected),
        std::memory_order_release);
    return fallback;
  }

  if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: overriding DLSS-G numFramesToGenerate from " << requested
      << " (" << (requested + 1) << "x) to " << desired << " ("
      << multiplier << "x). slDLSSGSetOptions accepted it.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  g_last_result.store(static_cast<unsigned int>(result), std::memory_order_relaxed);
  g_last_forced.store(desired, std::memory_order_relaxed);
  g_last_effective_generated.store(desired, std::memory_order_relaxed);
  g_effective_request_seen.store(true, std::memory_order_release);
  g_fixed_override_status.store(
      static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kApplied),
      std::memory_order_release);
  g_declined_no_pacing.store(false, std::memory_order_relaxed);
  g_force_failed_for.store(0, std::memory_order_relaxed);
  return result;
}

inline sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                 const sl::DLSSGOptions* options) {
  const auto real = g_real_get_state.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_addon_enabled.load(std::memory_order_relaxed))
    return real(viewport, state, options);

  const size_t caller_version = state.structVersion;
  sl::DLSSGState extended_state{};
  sl::DLSSGState* observed_state = &state;
  sl::Result result = sl::Result::eErrorNotInitialized;
  constexpr unsigned int kMaxDynamicProbeFailures = 8;
  const bool probe_dynamic_state =
      g_dynamic_d3d12.load(std::memory_order_relaxed) &&
      g_streamline_2_14_1_active.load(std::memory_order_acquire) &&
      caller_version >= sl::kStructVersion1 &&
      caller_version < sl::kStructVersion4 &&
      g_dynamic_state_probe_failures.load(std::memory_order_relaxed) <
          kMaxDynamicProbeFailures;
  if (probe_dynamic_state) {
    // Old games often allocate only the state version they compiled against.
    // Query v4 into addon-owned storage so no field beyond the caller's ABI is
    // touched, then copy back only fields guaranteed by that caller version.
    extended_state.next = state.next;
    result = real(viewport, extended_state, options);
    if (result == sl::Result::eOk) {
      g_dynamic_state_probe_failures.store(0, std::memory_order_relaxed);
      observed_state = &extended_state;
      state.estimatedVRAMUsageInBytes = extended_state.estimatedVRAMUsageInBytes;
      state.status = extended_state.status;
      state.minWidthOrHeight = extended_state.minWidthOrHeight;
      state.numFramesActuallyPresented = extended_state.numFramesActuallyPresented;
      if (caller_version >= sl::kStructVersion2) {
        state.numFramesToGenerateMax = extended_state.numFramesToGenerateMax;
        state.bReserved4 = extended_state.bReserved4;
        state.bIsVsyncSupportAvailable = extended_state.bIsVsyncSupportAvailable;
        state.inputsProcessingCompletionFence = extended_state.inputsProcessingCompletionFence;
        state.lastPresentInputsProcessingCompletionFenceValue =
            extended_state.lastPresentInputsProcessingCompletionFenceValue;
      }
    } else {
      // An older provider may reject the newer state ABI. Preserve native
      // behavior. Bound retries so capability probing cannot double every
      // GetState call forever on a genuinely old runtime.
      const bool structurally_unsupported =
          result == sl::Result::eErrorInvalidParameter ||
          result == sl::Result::eErrorUnsupportedInterface ||
          result == sl::Result::eErrorFeatureNotSupported;
      if (structurally_unsupported) {
        g_dynamic_state_probe_failures.store(kMaxDynamicProbeFailures,
                                             std::memory_order_relaxed);
      } else {
        g_dynamic_state_probe_failures.fetch_add(1, std::memory_order_relaxed);
      }
      result = real(viewport, state, options);
      observed_state = &state;
    }
  } else {
    result = real(viewport, state, options);
  }
  const unsigned int raw_result = static_cast<unsigned int>(result);
  g_state_result.store(raw_result, std::memory_order_relaxed);
  if (result == sl::Result::eOk) {
    const unsigned int status = static_cast<unsigned int>(observed_state->status);
    g_dlssg_status.store(status, std::memory_order_relaxed);
    const unsigned int presented = observed_state->numFramesActuallyPresented;
    const unsigned int previous =
        g_actual_frames_presented.exchange(presented, std::memory_order_relaxed);
    unsigned int observed_max = g_max_actual_frames_presented.load(std::memory_order_relaxed);
    while (presented > observed_max &&
           !g_max_actual_frames_presented.compare_exchange_weak(
               observed_max, presented, std::memory_order_relaxed)) {
    }
    g_state_samples.fetch_add(1, std::memory_order_relaxed);
    const bool first = !g_state_seen.exchange(true, std::memory_order_relaxed);
    bool log_status = false;
    if (status == 0) {
      log_status = !g_status_ok_logged.exchange(true, std::memory_order_relaxed);
    } else {
      log_status = !g_failure_status_logged.exchange(true, std::memory_order_relaxed);
    }
    if (log_status) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGGetState runtime status is 0x" << std::hex << status << std::dec
        << (status == 0 ? " (OK)." : " (DLSS-G reported one or more failure flags).");
      reshade::log::message(status == 0 ? reshade::log::level::info
                                        : reshade::log::level::warning,
                            s.str().c_str());
    }
    if (first || previous != presented) {
      const unsigned int bit = presented < 32 ? (1u << presented) : 0u;
      const unsigned int seen =
          bit == 0 ? ~0u : g_seen_present_counts.fetch_or(bit, std::memory_order_relaxed);
      if (first || (seen & bit) == 0) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGGetState reports " << presented
          << " frame(s) actually presented since its previous call.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
    }

    if (observed_state->structVersion >= sl::kStructVersion4 &&
        (observed_state->bIsDynamicMFGSupported == sl::Boolean::eTrue ||
         observed_state->bIsDynamicMFGSupported == sl::Boolean::eFalse)) {
      const bool supported = observed_state->bIsDynamicMFGSupported ==
                             sl::Boolean::eTrue;
      const bool previous =
          g_dynamic_supported.exchange(supported, std::memory_order_acq_rel);
      const bool was_seen =
          g_dynamic_support_seen.exchange(true, std::memory_order_release);
      if (!was_seen || previous != supported) {
        reshade::log::message(
            reshade::log::level::info,
            supported
                ? "mfgunlock: slDLSSGGetState confirms NVIDIA Dynamic MFG support."
                : "mfgunlock: slDLSSGGetState reports NVIDIA Dynamic MFG unsupported; fixed MFG remains active.");
      }
    }
    if (observed_state->structVersion >= sl::kStructVersion2 &&
        (observed_state->bIsVsyncSupportAvailable == sl::Boolean::eTrue ||
         observed_state->bIsVsyncSupportAvailable == sl::Boolean::eFalse)) {
      const bool supported = observed_state->bIsVsyncSupportAvailable ==
                             sl::Boolean::eTrue;
      const bool previous =
          g_vsync_supported.exchange(supported, std::memory_order_acq_rel);
      const bool was_seen =
          g_vsync_support_seen.exchange(true, std::memory_order_release);
      if (!was_seen || previous != supported) {
        reshade::log::message(
            reshade::log::level::info,
            supported
                ? "mfgunlock: the active DLSS-G runtime reports VSync support available."
                : "mfgunlock: the active DLSS-G runtime reports VSync support unavailable.");
      }
    }
  }
  if (result != sl::Result::eOk || caller_version < sl::kStructVersion2) return result;

  const unsigned int reported = observed_state->numFramesToGenerateMax;
  g_runtime_max_generated.store(reported, std::memory_order_relaxed);

  const unsigned int wanted = g_advertised_max_generated.load(std::memory_order_relaxed);
  if (wanted < 2 || reported >= wanted) return result;

  state.numFramesToGenerateMax = wanted;
  if (!g_capacity_advertised.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: slDLSSGGetState reported a maximum of " << reported
      << " generated frame(s); advertising the verified Streamline ceiling of " << wanted
      << " so the game's native multiplier selector can expose up to " << (wanted + 1) << "x.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return result;
}

inline sl::Result HookedGetFeatureFunction(sl::Feature feature, const char* function_name,
                                           void*& function) {
  const sl::Result result = g_real_get_feature_function(feature, function_name, function);
  if (result != sl::Result::eOk || function_name == nullptr || function == nullptr) return result;
  if (feature == sl::kFeatureDLSS_G &&
      std::strcmp(function_name, "slDLSSGSetOptions") == 0 &&
      function != reinterpret_cast<void*>(&HookedSetOptions)) {
    ObserveStreamlineFunctionOwner(function);
    g_real_set_options.store(reinterpret_cast<SetOptionsFn>(function),
                             std::memory_order_release);
    function = reinterpret_cast<void*>(&HookedSetOptions);
    if (!g_set_options_wrapped_logged.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: wrapped slDLSSGSetOptions; frame-multiplier override is live.");
    }
  } else if (feature == sl::kFeatureDLSS_G &&
             std::strcmp(function_name, "slDLSSGGetState") == 0 &&
             function != reinterpret_cast<void*>(&HookedGetState)) {
    ObserveStreamlineFunctionOwner(function);
    g_real_get_state.store(reinterpret_cast<GetStateFn>(function),
                           std::memory_order_release);
    function = reinterpret_cast<void*>(&HookedGetState);
    if (!g_get_state_wrapped_logged.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: wrapped slDLSSGGetState; native multiplier-menu override is live.");
    }
  } else if (feature == sl::kFeatureReflex &&
             std::strcmp(function_name, "slReflexSetOptions") == 0 &&
             function != reinterpret_cast<void*>(&HookedReflexSetOptions)) {
    g_real_reflex_set_options.store(reinterpret_cast<ReflexSetOptionsFn>(function),
                                    std::memory_order_release);
    function = reinterpret_cast<void*>(&HookedReflexSetOptions);
    g_reflex_hooked.store(true, std::memory_order_release);
    if (!g_reflex_wrapped_logged.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: wrapped slReflexSetOptions; the optional advanced source-frame cap is available.");
    }
  }
  return result;
}

inline std::vector<hook::HookItem> g_installed_hooks;
inline std::atomic_bool g_installing{false};

}  // namespace internal

inline void NotifyDynamicD3D12(bool d3d12) {
  g_dynamic_d3d12.store(d3d12, std::memory_order_relaxed);
}

inline void NotifyFixedMultiplierChanged(unsigned int) {
  g_last_forced.store(0, std::memory_order_relaxed);
  g_effective_request_seen.store(false, std::memory_order_release);
  g_declined_no_pacing.store(false, std::memory_order_relaxed);
  g_force_failed_for.store(0, std::memory_order_relaxed);
  // Both selecting a fixed value and returning control to the game take effect
  // only on the next enabled game-side SetOptions call.
  g_fixed_override_status.store(
      static_cast<unsigned int>(forcepolicy::FixedOverrideStatus::kPending),
      std::memory_order_release);
}

inline void NotifyDynamicModeChanged() {
  g_dynamic_applied.store(false, std::memory_order_relaxed);
  g_dynamic_fell_back.store(false, std::memory_order_relaxed);
  g_dynamic_runtime_declined.store(false, std::memory_order_release);
  g_dynamic_result.store(0, std::memory_order_relaxed);
  g_dynamic_set_failures.store(0, std::memory_order_relaxed);
  g_dynamic_state_probe_failures.store(0, std::memory_order_relaxed);
  g_dynamic_change_pending.store(true, std::memory_order_release);
  g_reflex_limit_failure_logged.store(false, std::memory_order_relaxed);
  g_effective_request_seen.store(false, std::memory_order_release);
  g_fixed_override_status.store(
      static_cast<unsigned int>(
          forcepolicy::IsFixedMultiplier(
              g_force_multiplier.load(std::memory_order_relaxed))
               ? forcepolicy::FixedOverrideStatus::kPending
               : forcepolicy::FixedOverrideStatus::kNative),
       std::memory_order_release);
}

// Must land before the game asks for the function pointer, which it does once
// during DLSS-G setup -- hence installing from the earliest event we get rather
// than waiting for a present.
inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  bool expected = false;
  if (!internal::g_installing.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer == nullptr ||
      GetProcAddress(interposer, "slGetFeatureFunction") == nullptr) {
    internal::g_installing.store(false, std::memory_order_release);
    return;
  }

   std::vector<hook::HookItem> hooks = {
       {"slGetFeatureFunction", reinterpret_cast<void**>(&internal::g_real_get_feature_function),
        reinterpret_cast<void*>(&internal::HookedGetFeatureFunction)}};
   // slInit is only detoured for an explicit provider-selection override.
  if (g_runtime_selection_mode.load(std::memory_order_relaxed) !=
      static_cast<unsigned int>(RuntimeSelectionMode::kGameDefault)) {
    hooks.push_back(
        {"slInit", reinterpret_cast<void**>(&internal::g_real_init),
         reinterpret_cast<void*>(&internal::HookedInit)});
  }
  if (!hook::Install(interposer, hooks, "sl.interposer.dll")) {
    internal::g_installing.store(false, std::memory_order_release);
    return;
  }
  internal::g_installed_hooks = std::move(hooks);
  g_hooked.store(true, std::memory_order_release);
  internal::g_installing.store(false, std::memory_order_release);
}

inline void Uninstall() {
  if (!g_hooked.load(std::memory_order_acquire)) return;
  if (!internal::g_installed_hooks.empty()) hook::Uninstall(internal::g_installed_hooks);
  internal::g_installed_hooks.clear();
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::framecount
