/*
 * Copyright (C) 2023 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "SmartCharge.h"

#include <GetServiceSupport.h>
#include <SafeStoi.h>

#include <android-base/properties.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <chrono>
#include <dlfcn.h>
#include <functional>
#include <sstream>
#include <type_traits>

namespace aidl {
namespace vendor {
namespace samsung_ext {
namespace framework {
namespace battery {

using ::android::base::GetProperty;
using ::android::base::SetProperty;
using ::android::base::WaitForPropertyCreation;

using namespace std::chrono_literals;

static const char kSmartChargeConfigProp[] = "persist.ext.smartcharge.config";
static const char kSmartChargeEnabledProp[] = "persist.ext.smartcharge.enabled";
static const char kSmartChargeOverrideProp[] = "ro.hardware.battery";
static const char kComma = ',';

template <typename T>
using is_integral_or_bool =
    std::enable_if_t<std::is_integral_v<T> || std::is_same_v<T, bool>, bool>;

static inline bool isValidBool(const int val) {
  return val == true || val == false;
}
static inline bool verifyConfig(const int lower, const int upper) {
  return !(upper <= lower || upper > 95 || (0 <= lower && lower < 50));
}

template <typename T, is_integral_or_bool<T> = true>
struct ConfigPair {
  T first, second;
  std::string toString(void) {
    return std::to_string(first) + kComma + std::to_string(second);
  }
};

template <typename U>
bool fromString(const std::string &v, ConfigPair<U> *pair) {
  std::stringstream ss(v);
  std::string res;

  if (v.find(kComma) != std::string::npos) {
    getline(ss, res, kComma);
    pair->first = stoi_safe(res);
    getline(ss, res, kComma);
    pair->second = stoi_safe(res);
    return true;
  }
  return false;
}

template <>
bool fromString(const std::string &v, ConfigPair<bool> *pair) {
  ConfigPair<int> tmp{};
  if (fromString<int>(v, &tmp) && isValidBool(tmp.first) &&
      isValidBool(tmp.second)) {
    pair->first = tmp.first;
    pair->second = tmp.second;
    return true;
  }
  return false;
}

template <typename U>
bool getAndParse(const char *prop, ConfigPair<U> *pair) {
  if (WaitForPropertyCreation(prop, 500ms)) {
    std::string propval = GetProperty(prop, "");
    if (!propval.empty()) {
      return fromString(propval, pair);
    }
  }
  return false;
}

const static auto kDisabledCfgStr = ConfigPair<bool>{0, 0}.toString();

static void onServiceDied(void *cookie) {
  reinterpret_cast<SmartCharge *>(cookie)->loadHealthImpl();
}

void SmartCharge::loadHealthImpl(void) {
  bool linkToDeathSuccess;
  std::string reason;

  // Try aidl
  health_aidl = waitServiceDefault<IHealthAIDL>();
  if (health_aidl == nullptr) {
    // hidl
    health_hidl = ::android::hardware::health::V2_0::get_health_service();
    if (health_hidl != nullptr) {
      healthState = USE_HEALTH_HIDL;
      ALOGD("%s: Connected to health HIDL V2.0 HAL", __func__);
      hidl_death_recp = new hidl_health_death_recipient(health_hidl);
      auto ret = health_hidl->linkToDeath(hidl_death_recp, reinterpret_cast<uint64_t>(this));
      linkToDeathSuccess = ret.isOk();
      reason = ret.description();
    } else {
      LOG_ALWAYS_FATAL("Failed to connect to any valid health HAL");
      __builtin_unreachable();
    }
  } else {
    healthState = USE_HEALTH_AIDL;
    ALOGD("%s: Connected to health AIDL HAL", __func__);
    aidl_death_recp = ndk::ScopedAIBinder_DeathRecipient(
        AIBinder_DeathRecipient_new(onServiceDied)
    );
    auto ret = AIBinder_linkToDeath(health_aidl->asBinder().get(), aidl_death_recp.get(), this);
    linkToDeathSuccess = ret == STATUS_OK;
    reason = ndk::ScopedAStatus(AStatus_fromStatus(ret)).getDescription();
  }
  if (!linkToDeathSuccess)
    ALOGW("%s: linkToDeath failed: %s", __func__, reason.c_str());
}

bool SmartCharge::loadAndParseConfigProp(void) {
  ConfigPair<int> ret{};
  if (getAndParse(kSmartChargeConfigProp, &ret) &&
      verifyConfig(ret.first, ret.second)) {
    upper = ret.second;
    lower = ret.first;
    ALOGD("%s: upper: %d, lower: %d", __func__, upper, lower);
  } else {
    upper = -1;
    lower = -1;
    ALOGW("%s: Parsing config failed", __func__);
    return false;
  }
  return true;
}

void SmartCharge::loadImplLibrary(void) {
  const std::string path = "/system_ext/lib64/hw/battery."
        + GetProperty(kSmartChargeOverrideProp, "default") + ".so";

  ALOGI("%s: Try dlopen '%s'", __func__, path.c_str());
  handle = dlopen(path.c_str(), RTLD_NOW);
  if (handle) {
    setChargableFunc = reinterpret_cast<void(*)(const bool)>(
        dlsym(handle, "setChargable"));

    if (setChargableFunc) {
      ALOGD("%s: setChargable function loaded", __func__);
    } else {
      ALOGE("%s: Failed to find setChargable symbol", __func__);
      // Unused handle, close it
      dlclose(handle);
    }
  } else {
    ALOGE("%s: %s", __func__, dlerror() ?: "unknown");
  }
  if (!setChargableFunc) {
    LOG_ALWAYS_FATAL("Failed to load setChargable function from impl library");
  }
}

void SmartCharge::loadEnabledAndStart(void) {
  ConfigPair<bool> ret{};

  if (getAndParse(kSmartChargeEnabledProp, &ret)) {
    if (ret.first) {
      ALOGD("%s: Starting loop, withrestart: %d", __func__, ret.second);
      kRun.store(true);
      createLoopThread(ret.second);
    } else
      ALOGD("%s: Not starting loop", __func__);
  } else {
    ALOGE("%s: Enabled prop value invalid, resetting to valid one", __func__);
    SetProperty(kSmartChargeEnabledProp, kDisabledCfgStr);
  }
}

SmartCharge::SmartCharge(void) {
  bool ret;

  loadHealthImpl();
  loadImplLibrary();

  ret = loadAndParseConfigProp();
  if (ret) {
    loadEnabledAndStart();
  }
}

enum ChargeStatus {
  ON,
  OFF,
  NOOP,
};

void SmartCharge::startLoop(bool withrestart) {
  bool initdone = false;
  ChargeStatus tmp = ChargeStatus::NOOP;
  ChargeStatus status = ChargeStatus::NOOP;
  ALOGD("%s: ++", __func__);
  std::unique_lock<std::mutex> lock(kCVLock);
  while (kRun.load()) {
    int percent;
    bool isDischarging, transactFailed = false;

    switch (healthState) {
    case USE_HEALTH_AIDL: {
      using android::hardware::health::BatteryStatus;

      BatteryStatus status = BatteryStatus::UNKNOWN;
      auto ret = health_aidl->getCapacity(&percent);
      if (!ret.isOk()) {
	transactFailed = true;
      }
      ret = health_aidl->getChargeStatus(&status);
      if (!ret.isOk()) {
	transactFailed = true;
      }
      isDischarging = status == BatteryStatus::DISCHARGING;
      break;
    }
    case USE_HEALTH_HIDL: {
      using ::android::hardware::health::V2_0::Result;
      using ::android::hardware::health::V1_0::BatteryStatus;

      Result res = Result::UNKNOWN;
      BatteryStatus status = BatteryStatus::UNKNOWN;
      health_hidl->getCapacity([&res, &percent](Result hal_res, int32_t hal_value) {
        res = hal_res;
        percent = hal_value;
      });
      if (res != Result::SUCCESS)
        transactFailed = true;
      health_hidl->getChargeStatus([&res, &status](Result hal_res, BatteryStatus hal_value) {
        res = hal_res;
        status = hal_value;
      });
      if (res != Result::SUCCESS)
        transactFailed = true;
      isDischarging = status == BatteryStatus::DISCHARGING;
      break;
    }
    }
    if (transactFailed) {
      kRun.store(false);
      SetProperty(kSmartChargeEnabledProp, kDisabledCfgStr);
      ALOGE("%s: exit loop: Trasaction failed", __func__);
      break;
    }
    if (!isDischarging) {
      if (percent > upper)
        tmp = ChargeStatus::OFF;
      else if (withrestart && percent < lower)
        tmp = ChargeStatus::ON;
      else if (!withrestart && percent <= upper - 1)
        tmp = ChargeStatus::ON;

      if (tmp != status || !initdone) {
        switch (tmp) {
        case ChargeStatus::OFF:
          setChargableFunc(false);
          break;
        case ChargeStatus::ON:
          setChargableFunc(true);
          break;
        default:
          break;
        }
        status = tmp;
        initdone = true;
      }
    }

    if (cv.wait_for(lock, 5s) == std::cv_status::no_timeout) {
      // cv signaled, exit now
      break;
    }
  }
  lock.unlock();
  ALOGD("%s: --", __func__);
}

void SmartCharge::createLoopThread(bool restart) {
  const std::lock_guard<std::mutex> _(thread_lock);
  ALOGD("%s: create thread", __func__);
  kLoopThread = std::make_shared<std::thread>(&SmartCharge::startLoop, this, restart);
}

ndk::ScopedAStatus SmartCharge::setChargeLimit(int32_t upper_, int32_t lower_) {
  ALOGD("%s: upper: %d, lower: %d, kRun: %d", __func__, upper_, lower_,
        kRun.load());
  if (!verifyConfig(lower_, upper_))
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  if (kRun.load())
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
  auto pair = ConfigPair<int>{lower_ < 0 ? -1 : lower_, upper_};
  SetProperty(kSmartChargeConfigProp, pair.toString());
  {
    std::unique_lock<std::mutex> _(config_lock);
    lower = lower_ < 0 ? -1 : lower_;
    upper = upper_;
  }
  ALOGD("%s: Exit", __func__);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SmartCharge::activate(bool enable, bool restart) {
  auto pair = ConfigPair<bool>{enable, restart};
  {
    std::unique_lock<std::mutex> _(config_lock);
    ALOGD("%s: upper: %d, lower: %d, enable: %d, restart: %d, kRun: %d",
          __func__, upper, lower, enable, restart, kRun.load());
    if (!verifyConfig(lower, upper))
      return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    if (lower == -1 && restart)
      return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  }
  if (kRun.load() == enable)
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  SetProperty(kSmartChargeEnabledProp, pair.toString());
  if (enable) {
    bool kThreadRunning;
    kRun.store(true);
    {
      const std::lock_guard<std::mutex> _(thread_lock);
      kThreadRunning = !!kLoopThread.get();
    }
    if (kThreadRunning) {
      ALOGW("Thread is running?");
    } else {
      createLoopThread(restart);
    }
  } else {
    kRun.store(false);
    setChargableFunc(true);
    if (kLoopThread) {
      const std::lock_guard<std::mutex> _(thread_lock);
      if (kLoopThread->joinable()) {
        cv.notify_one();
        kLoopThread->join();
      }
      kLoopThread.reset();
    } else {
      ALOGW("No threads to stop?");
    }
  }
  ALOGD("%s: Exit", __func__);
  return ndk::ScopedAStatus::ok();
}

using ::android::hardware::interfacesEqual;

void hidl_health_death_recipient::serviceDied(uint64_t cookie,
                                              const wp<::android::hidl::base::V1_0::IBase>& who) {
    if (mHealth != nullptr && interfacesEqual(mHealth, who.promote())) {
        onServiceDied(reinterpret_cast<void*>(cookie));
    }
}

} // namespace battery
} // namespace framework
} // namespace samsung_ext
} // namespace vendor
} // namespace aidl
