/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.os;

import android.compat.annotation.UnsupportedAppUsage;

/**
 * Native implementation of the service manager.  Most clients will only
 * care about asInterface().
 *
 * @hide
 */
public final class ServiceManagerNative {
    private ServiceManagerNative() {}

    /**
     * Cast a Binder object into a service manager interface, generating
     * a proxy if needed.
     *
     * TODO: delete this method and have clients use
     *     IServiceManager.Stub.asInterface instead
     */
    // 把IBinder对象转成对应的service接口
    @UnsupportedAppUsage
    public static IServiceManager asInterface(IBinder obj) {
        if (obj == null) {
            return null;
        }

        // ServiceManager is never local
        // servicemanager IBinder -> BpBinder -> BinderProxy -> ServiceManagerProxy() -> ServiceManagerNative()
        return new ServiceManagerProxy(obj);
    }
}

// This class should be deleted and replaced with IServiceManager.Stub whenever
// mRemote is no longer used
class ServiceManagerProxy implements IServiceManager {
    public ServiceManagerProxy(IBinder remote) {
        mRemote = remote; // remote是传递进来的BinderProxy
        // IServiceManager.Stub.Proxy(obj)
        // mServiceManager是客户端调用的Proxy对象（asInterface中封装成的Proxy对象）
        // 在 IServiceManager.Stub.asInterface 内部又封装成了Proxy对象 IServiceManager.Stub.Proxy(remote)
        mServiceManager = IServiceManager.Stub.asInterface(remote);
    }

    public IBinder asBinder() {
        return mRemote;
    }

    @UnsupportedAppUsage
    public IBinder getService(String name) throws RemoteException {
        // Same as checkService (old versions of servicemanager had both methods).
        return mServiceManager.checkService(name);
    }

    public IBinder checkService(String name) throws RemoteException {
        return mServiceManager.checkService(name);
    }

    public void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority)
            throws RemoteException {
        // 调用Proxy的addService方法
        // 内部会调用mRemote.transcat方法（mRemote就是BinderProxy）
        mServiceManager.addService(name, service, allowIsolated, dumpPriority);
    }

    public String[] listServices(int dumpPriority) throws RemoteException {
        return mServiceManager.listServices(dumpPriority);
    }

    public void registerForNotifications(String name, IServiceCallback cb)
            throws RemoteException {
        mServiceManager.registerForNotifications(name, cb);
    }

    public void unregisterForNotifications(String name, IServiceCallback cb)
            throws RemoteException {
        throw new RemoteException();
    }

    public boolean isDeclared(String name) throws RemoteException {
        return mServiceManager.isDeclared(name);
    }

    public String[] getDeclaredInstances(String iface) throws RemoteException {
        return mServiceManager.getDeclaredInstances(iface);
    }

    public String updatableViaApex(String name) throws RemoteException {
        return mServiceManager.updatableViaApex(name);
    }

    public String[] getUpdatableNames(String apexName) throws RemoteException {
        return mServiceManager.getUpdatableNames(apexName);
    }

    public ConnectionInfo getConnectionInfo(String name) throws RemoteException {
        return mServiceManager.getConnectionInfo(name);
    }

    public void registerClientCallback(String name, IBinder service, IClientCallback cb)
            throws RemoteException {
        throw new RemoteException();
    }

    public void tryUnregisterService(String name, IBinder service) throws RemoteException {
        throw new RemoteException();
    }

    public ServiceDebugInfo[] getServiceDebugInfo() throws RemoteException {
        return mServiceManager.getServiceDebugInfo();
    }

    /**
     * Same as mServiceManager but used by apps.
     *
     * Once this can be removed, ServiceManagerProxy should be removed entirely.
     */
    @UnsupportedAppUsage
    private IBinder mRemote;

    private IServiceManager mServiceManager;
}
