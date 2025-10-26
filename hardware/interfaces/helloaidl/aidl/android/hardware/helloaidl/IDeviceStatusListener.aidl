package android.hardware.helloaidl;

@VintfStability
interface IDeviceStatusListener {
    /* 当状态改变时触发 API 调用 */
    /* oneway 异步调用，非阻塞 */
    oneway void onStatusChanged(int statusCode, String statusMsg);
}