package android.hardware.helloaidl;

import android.hardware.helloaidl.IDeviceStatusListener;
import android.hardware.helloaidl.StatusSetting;


@VintfStability
interface IDeviceStatusManager{
    /* in 表示输入 */
    void registerListener(in IDeviceStatusListener listener);
    void unregisterListener(in IDeviceStatusListener listener);
    StatusSetting getCurrentStatus();
    void setStatus(in StatusSetting s);
}