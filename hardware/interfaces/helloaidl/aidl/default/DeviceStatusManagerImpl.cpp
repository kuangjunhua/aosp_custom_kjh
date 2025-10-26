#define LOG_TAG "HelloStableAidl"
#include "DeviceStatusManagerImpl.h"
#include <android/binder_ibinder.h>

#include <log/log.h>
#include <stdio.h>
#include <iostream>

namespace aidl::android::hardware::helloaidl {
    typedef struct {
        DeviceStatusManagerImpl* manager;
        std::shared_ptr<IDeviceStatusListener> listener;
    } BinderData;

    static void onBinderDied(void* cookie) {
        BinderData* data = static_cast<BinderData*>(cookie);

        //extract the data from cookie
        DeviceStatusManagerImpl* manager = data->manager;
        std::shared_ptr<IDeviceStatusListener> listener = data->listener;

        //get remote AIBinder
        AIBinder *binder = listener->asBinder().get();
        ALOGD("Binder died! Cookie: %p, remoe AIBinder:%p", cookie, binder);

        //remove the remote AIbinder from list.
        auto it = std::find(manager->getListeners().begin(), manager->getListeners().end(), listener);
        if( it != manager->getListeners().end() ){
            // 找到了，移除
            manager->getListeners().erase(it);
            ALOGD("remove AIBinder: %p from the mListeners", binder);
        }
    }

    void DeviceStatusManagerImpl::updateStatus(int32_t code, const std::string& msg) {
        for(const auto& listener : mListeners) {
            if(listener != nullptr) {
                // 跨进程调用
                listener->onStatusChanged(code, msg);
            }
        }
    }
    std::vector<std::shared_ptr<IDeviceStatusListener>> DeviceStatusManagerImpl::getListeners(void){
        return mListeners;
    }

    ::ndk::ScopedAStatus DeviceStatusManagerImpl::registerListener(const std::shared_ptr<::aidl::android::hardware::helloaidl::IDeviceStatusListener>& in_listener) {
        if(in_listener == nullptr) {
            ALOGE("registerListener: listener is null");
            return ::ndk::ScopedAStatus::fromStatus(STATUS_INVALID_OPERATION);
        }
        if(std::find(mListeners.begin(), mListeners.end(), in_listener) == mListeners.end()) {
            mListeners.push_back(in_listener);
            ALOGI("registerListener: listener registered, total listeners: %zu", mListeners.size());
            // 监听远程Binder死亡
            auto* cookie = new BinderData{this, in_listener};
            AIBinder_DeathRecipient* recipient = AIBinder_DeathRecipient_new(onBinderDied);
            binder_status_t status = AIBinder_linkToDeath(in_listener->asBinder().get(), recipient, cookie);
            updateStatus(3, "helloworld");
            ALOGD("[hal service]register listener success");
        } else {
            ALOGW("registerListener: listener already registered");
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus DeviceStatusManagerImpl::unregisterListener(const std::shared_ptr<::aidl::android::hardware::helloaidl::IDeviceStatusListener>& in_listener) {
        auto it = std::find(mListeners.begin(), mListeners.end(), in_listener);
        if(it != mListeners.end()) {
            // 找到了，移除
            mListeners.erase(it);
            ALOGI("unregisterListener: listener unregistered, total listeners: %zu", mListeners.size());
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus DeviceStatusManagerImpl::getCurrentStatus(::aidl::android::hardware::helloaidl::StatusSetting* _aidl_return) {
        *_aidl_return = mStatus;
        ALOGI("getCurrentStatus: devicename=%s, value=%d", mStatus.devicename.c_str(), mStatus.val);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus DeviceStatusManagerImpl::setStatus(const ::aidl::android::hardware::helloaidl::StatusSetting& in_s) {
        mStatus = in_s;
        ALOGI("setStatus: devicename=%s, value=%d", mStatus.devicename.c_str(), mStatus.val);
        return ::ndk::ScopedAStatus::ok();
    }
}