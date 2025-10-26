#define LOG_TAG "LedAidlDemo"
#include "LedManagerImpl.h"
#include <android/binder_ibinder.h>

#include <log/log.h>
#include <stdio.h>
#include <iostream>

namespace aidl::android::hardware::ledaidl {
    ::ndk::ScopedAStatus LedManagerImpl::getStatus(int32_t* _aidl_return){
        *_aidl_return = mStatus;
        ALOGD("[Service] get status: %d", mStatus);
        return ::ndk::ScopedAStatus::ok();
    };
    ::ndk::ScopedAStatus LedManagerImpl::open(){
        mStatus = 1;
        ALOGD("[Service] open~~~");
        return ::ndk::ScopedAStatus::ok();
    };
    ::ndk::ScopedAStatus LedManagerImpl::close(){
        mStatus = 0;
        ALOGD("[Service] close~~~");
        return ::ndk::ScopedAStatus::ok();
    };
}