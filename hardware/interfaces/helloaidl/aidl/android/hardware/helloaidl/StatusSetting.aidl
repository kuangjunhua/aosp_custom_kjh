package android.hardware.helloaidl;

@VintfStability
parcelable StatusSetting {
    String devicename;
    int val; // 0: 关闭 1: 开启
}