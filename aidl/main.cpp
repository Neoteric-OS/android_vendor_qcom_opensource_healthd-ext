/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "android.hardware.health-service.qti"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_interface_utils.h>
#include <cutils/klog.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <health/utils.h>
#include <health-impl/ChargerUtils.h>
#include <health-impl/Health.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#define ARRAY_SIZE(x)     (sizeof(x) / sizeof((x)[0]))

typedef enum soc_id {
        MSM_NEO_LA = 554,
        MSM_NEO_LE = 525,
        MSM_NEO_LA_V2 = 579,
        MSM_SERAPH = 673,
        MSM_SERAPHP = 672,
        MSM_ALISO_SSG = 739,
        MSM_ALISO_SSG2 = 740,
}soc_id_t;

static const enum soc_id target_no_psy[] = {
        MSM_NEO_LA,
        MSM_NEO_LE,
        MSM_NEO_LA_V2,
        MSM_SERAPH,
        MSM_SERAPHP,
        MSM_ALISO_SSG,
        MSM_ALISO_SSG2,
};

static int read_soc_id() {
    constexpr const char* kSocIdPaths[] = {
        "/sys/devices/soc0/soc_id",
        "/sys/devices/system/soc/soc0/id",
    };
    char buf[PROPERTY_VALUE_MAX];

    for (const auto* path : kSocIdPaths) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) {
            continue;
        }

        buf[len] = '\0';
        auto soc_id_str = android::base::Trim(buf);
        if (soc_id_str.empty()) {
            continue;
        }

        errno = 0;
        char* end = nullptr;
        long val = strtol(soc_id_str.c_str(), &end, 10);
        if (errno == 0 && end != soc_id_str.c_str() && *end == '\0' && val >= 0 && val <= INT_MAX) {
            return static_cast<int>(val);
        }
    }

    return -1;
}

using aidl::android::hardware::health::HalHealthLoop;
using aidl::android::hardware::health::Health;

#if !CHARGER_FORCE_NO_UI
using aidl::android::hardware::health::charger::ChargerCallback;
using aidl::android::hardware::health::charger::ChargerModeMain;
namespace aidl::android::hardware::health {
class ChargerCallbackImpl : public ChargerCallback {
  public:
    ChargerCallbackImpl(const std::shared_ptr<Health>& service) : ChargerCallback(service) {}
    bool ChargerEnableSuspend() override { return true; }
};
} //namespace aidl::android::hardware::health
#endif

static constexpr const char* gInstanceName = "default";
static constexpr std::string_view gChargerArg{"--charger"};

constexpr char *ucsiPSYName[]{
	(char *const)"ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi1",
	(char *const)"ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi2"
};

#define RETRY_COUNT    100

void qti_healthd_board_init(struct healthd_config *hc)
{
    int fd;
    unsigned char retries = RETRY_COUNT;
    int ret = 0;
    unsigned char buf;
    int soc_id_prop = 0;
    bool is_no_batt_psy;

    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYName[0]));
    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYName[1]));

    is_no_batt_psy = property_get_bool("persist.vendor.hal_health.no_batt_psy", false);
    soc_id_prop = read_soc_id();

    if (!is_no_batt_psy) {
        for (int idx = 0; idx < ARRAY_SIZE(target_no_psy); idx++) {
             if (soc_id_prop == target_no_psy[idx]) {
                KLOG_INFO(LOG_TAG, "no support for batt_psy with socid:%d \n",soc_id_prop);
                return;
           }
        }
        if (soc_id_prop < 0) {
            KLOG_INFO(LOG_TAG, "soc_id unavailable, continuing batt_psy wait path\n");
        }
    } else {
        KLOG_INFO(LOG_TAG, "no support for batt_psy\n");
        return;
    }

retry:
    if (!retries) {
        KLOG_ERROR(LOG_TAG, "Cannot open battery/capacity, fd=%d\n", fd);
        return;
    }

    fd = open("/sys/class/power_supply/battery/capacity", 0440);
    if (fd >= 0) {
        KLOG_INFO(LOG_TAG, "opened battery/capacity after %d retries\n", RETRY_COUNT - retries);
        while (retries) {
            ret = read(fd, &buf, 1);
            if(ret >= 0) {
                KLOG_INFO(LOG_TAG, "Read Batt Capacity after %d retries ret : %d\n", RETRY_COUNT - retries, ret);
                close(fd);
                return;
            }

            retries--;
            usleep(100000);
        }

        KLOG_ERROR(LOG_TAG, "Failed to read Battery Capacity ret=%d\n", ret);
        close(fd);
        return;
    }

    retries--;
    usleep(100000);
    goto retry;
}

int main(int argc, char** argv) {
#ifdef __ANDROID_RECOVERY__
    android::base::InitLogging(argv, android::base::KernelLogger);
#endif
    auto config = std::make_unique<healthd_config>();
    qti_healthd_board_init(config.get());
    ::android::hardware::health::InitHealthdConfig(config.get());
    auto binder = ndk::SharedRefBase::make<Health>(gInstanceName, std::move(config));

    if (argc >= 2 && argv[1] == gChargerArg) {
#if !CHARGER_FORCE_NO_UI
        KLOG_INFO(LOG_TAG, "Starting charger mode with UI.");
        auto charger_callback = std::make_shared<aidl::android::hardware::health::ChargerCallbackImpl>(binder);
        return ChargerModeMain(binder, charger_callback);
#endif
        KLOG_INFO(LOG_TAG, "Starting charger mode without UI.");
    } else {
        KLOG_INFO(LOG_TAG, "Starting health HAL.");
    }

    auto hal_health_loop = std::make_shared<HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
