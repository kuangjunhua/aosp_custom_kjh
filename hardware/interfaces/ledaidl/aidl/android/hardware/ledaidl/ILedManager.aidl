package android.hardware.ledaidl;

@VintfStability
interface ILedManager {
    int getStatus();
    void open();
    void close();
}