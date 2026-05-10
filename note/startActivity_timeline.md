# startActivity(Intent) 启动 Activity：从 app 调用到 onCreate/onStart/onResume（含关键回报链路）

本文按**时间线**把一次典型的 `Activity.startActivity(Intent)` 启动流程串起来：  
**app 侧调用 → Instrumentation → system_server(ATMS/WMS/AMS) 决策与调度 → ClientTransaction 下发 → app 进程 ActivityThread 执行 → Activity onCreate/onStart/onResume → client→server 回报(activityResumed/activityIdle 等)**。

> 说明：本文基于当前源码树（AOSP 定制目录）中的实现路径，引用均为可点击源码链接。

---

## 0. 总览（你可以先记住这一条主干）

1. **app 侧**：`Activity.startActivity()` → `startActivityForResult()` → `Instrumentation.execStartActivity()`  
2. **system_server**：ATMS `startActivityAsUser()` → `ActivityStarter.execute()` → `executeRequest()` → `startActivityInner()` → `resumeFocusedTasksTopActivities()` → `realStartActivityLocked()`  
3. **client(app 进程)**：`IApplicationThread.scheduleTransaction()` → `H(EXECUTE_TRANSACTION)` → `TransactionExecutor`  
   - `LaunchActivityItem`：`handleLaunchActivity()` → `performLaunchActivity()` → `Instrumentation.newActivity()` → `Activity.attach()` → `callActivityOnCreate()`  
   - 生命周期：`handleStartActivity()`(onStart/onPostCreate/restore) → `handleResumeActivity()`(onResume + 窗口可见)  
4. **回报**：`activityResumed()`、主线程 idle 后 `activityIdle()` 回报给 system_server，推动后续收尾/可见性/超时处理。

---

## 1. 起点：app 侧调用 Activity.startActivity(Intent)

### T0：Activity.startActivity(Intent)

入口方法仅做转发：  
- [Activity.startActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Activity.java#L6039-L6041)

<details><summary>源码：Activity.startActivity(Intent)</summary>

```java
@Override
public void startActivity(Intent intent) {
    this.startActivity(intent, null);
}
```

</details>

执行要点：
- `startActivity(intent)` → `startActivity(intent, null)`

### T1：Activity.startActivity(Intent, Bundle)

继续转到 `startActivityForResult`（requestCode = -1 表示不需要 result）：  
- [Activity.startActivity(Intent, Bundle)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Activity.java#L6066-L6074)

<details><summary>源码：Activity.startActivity(Intent, Bundle)</summary>

```java
@Override
public void startActivity(Intent intent, @Nullable Bundle options) {
    getAutofillClientController().onStartActivity(intent, mIntent);
    if (options != null) {
        startActivityForResult(intent, -1, options);
    } else {
        // Note we want to go through this call for compatibility with
        // applications that may have overridden the method.
        startActivityForResult(intent, -1);
    }
}
```

</details>

执行要点：
- `options != null`：`startActivityForResult(intent, -1, options)`  
- `options == null`：`startActivityForResult(intent, -1)`

### T2：Activity.startActivityForResult(Intent, int, Bundle)

常规 Activity（非嵌套 `mParent == null`）走这里：  
- [Activity.startActivityForResult](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Activity.java#L5610-L5646)

<details><summary>源码：Activity.startActivityForResult(Intent, int, Bundle)</summary>

```java
public void startActivityForResult(@RequiresPermission Intent intent, int requestCode,
        @Nullable Bundle options) {
    if (mParent == null) {
        // 不是嵌套的Activity，常规使用时走到这个分支
        options = transferSpringboardActivityOptions(options);
        Instrumentation.ActivityResult ar =
            mInstrumentation.execStartActivity(
                this, mMainThread.getApplicationThread(), mToken, this,
                intent, requestCode, options);
        if (ar != null) {
            mMainThread.sendActivityResult(
                mToken, mEmbeddedID, requestCode, ar.getResultCode(),
                ar.getResultData());
        }
        if (requestCode >= 0) {
            // If this start is requesting a result, we can avoid making
            // the activity visible until the result is received.  Setting
            // this code during onCreate(Bundle savedInstanceState) or onResume() will keep the
            // activity hidden during this time, to avoid flickering.
            // This can only be done when a result is requested because
            // that guarantees we will get information back when the
            // activity is finished, no matter what happens to it.
            mStartedActivity = true;
        }

        cancelInputsAndStartExitTransition(options);
        // TODO Consider clearing/flushing other event sources and events for child windows.
    } else {
        if (options != null) {
            mParent.startActivityFromChild(this, intent, requestCode, options);
        } else {
            // Note we want to go through this method for compatibility with
            // existing applications that may have overridden it.
            mParent.startActivityFromChild(this, intent, requestCode);
        }
    }
}
```

</details>

执行要点：
- `options = transferSpringboardActivityOptions(options)`：转场 options 透传/补齐  
- 调用 **Instrumentation**：  
  `mInstrumentation.execStartActivity(this, mMainThread.getApplicationThread(), mToken, this, intent, requestCode, options)`

> 关键点：这里开始从“应用框架层 API”转入“Instrumentation → Binder → system_server”。

---

## 2. app 侧 Instrumentation：准备 Intent 并跨进程进入 ATMS

### T3：Instrumentation.execStartActivity（app 进程）

- [Instrumentation.execStartActivity(Context, IBinder, IBinder, Activity, Intent, int, Bundle)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Instrumentation.java#L1837-L1886)

<details><summary>源码：Instrumentation.execStartActivity(...)</summary>

```java
@UnsupportedAppUsage
public ActivityResult execStartActivity(
        Context who, IBinder contextThread, IBinder token, Activity target,
        Intent intent, int requestCode, Bundle options) {
    IApplicationThread whoThread = (IApplicationThread) contextThread;
    Uri referrer = target != null ? target.onProvideReferrer() : null;
    if (referrer != null) {
        intent.putExtra(Intent.EXTRA_REFERRER, referrer);
    }
    if (mActivityMonitors != null) {
        synchronized (mSync) {
            final int N = mActivityMonitors.size();
            for (int i=0; i<N; i++) {
                final ActivityMonitor am = mActivityMonitors.get(i);
                ActivityResult result = null;
                if (am.ignoreMatchingSpecificIntents()) {
                    if (options == null) {
                        options = ActivityOptions.makeBasic().toBundle();
                    }
                    result = am.onStartActivity(who, intent, options);
                }
                if (result != null) {
                    am.mHits++;
                    return result;
                } else if (am.match(who, null, intent)) {
                    am.mHits++;
                    if (am.isBlocking()) {
                        return requestCode >= 0 ? am.getResult() : null;
                    }
                    break;
                }
            }
        }
    }
    try {
        intent.migrateExtraStreamToClipData(who);
        intent.prepareToLeaveProcess(who);
        // 跨进程调用到ATMS 启动Activity
        int result = ActivityTaskManager.getService().startActivity(whoThread,
                who.getOpPackageName(), who.getAttributionTag(), intent,
                intent.resolveTypeIfNeeded(who.getContentResolver()), token,
                target != null ? target.mEmbeddedID : null, requestCode, 0, null, options);
        // 通知结果
        notifyStartActivityResult(result, options);
        // 判断结果（很多报错的调试信息在其中）
        checkStartActivityResult(result, intent);
    } catch (RemoteException e) {
        throw new RuntimeException("Failure from system", e);
    }
    return null;
}
```

</details>

执行要点（按顺序）：
1. 注入 referrer：`target.onProvideReferrer()` → `intent.putExtra(EXTRA_REFERRER, ...)`
2. ActivityMonitor 拦截（测试/监控场景）  
3. `intent.migrateExtraStreamToClipData(who)` + `intent.prepareToLeaveProcess(who)`  
4. Binder 调用进入 system_server（ATMS）：  
   `ActivityTaskManager.getService().startActivity(whoThread, callingPackage, callingFeatureId, intent, resolvedType, token, resultWho, requestCode, ...)`
5. `checkStartActivityResult(result, intent)`：把 START_* 结果转成异常/校验（app 侧常见抛错点）

---

## 3. system_server：ATMS 入口与 Starter 创建

### T4：ActivityTaskManagerService.startActivity / startActivityAsUser（system_server）

入口桥接：  
- [ActivityTaskManagerService.startActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java#L1229-L1236)

真正入口：  
- [ActivityTaskManagerService.startActivityAsUser](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java#L1263-L1310)

<details><summary>源码：ATMS.startActivity(...) / startActivityAsUser(...)</summary>

```java
@Override
public final int startActivity(IApplicationThread caller, String callingPackage,
        String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
        String resultWho, int requestCode, int startFlags, ProfilerInfo profilerInfo,
        Bundle bOptions) {
    return startActivityAsUser(caller, callingPackage, callingFeatureId, intent, resolvedType,
            resultTo, resultWho, requestCode, startFlags, profilerInfo, bOptions,
            UserHandle.getCallingUserId());
}

@Override
public int startActivityAsUser(IApplicationThread caller, String callingPackage,
        String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
        String resultWho, int requestCode, int startFlags, ProfilerInfo profilerInfo,
        Bundle bOptions, int userId) {
    return startActivityAsUser(caller, callingPackage, callingFeatureId, intent, resolvedType,
            resultTo, resultWho, requestCode, startFlags, profilerInfo, bOptions, userId,
            true /*validateIncomingUser*/);
}

private int startActivityAsUser(IApplicationThread caller, String callingPackage,
        @Nullable String callingFeatureId, Intent intent, String resolvedType,
        IBinder resultTo, String resultWho, int requestCode, int startFlags,
        ProfilerInfo profilerInfo, Bundle bOptions, int userId, boolean validateIncomingUser) {

    final SafeActivityOptions opts = SafeActivityOptions.fromBundle(bOptions);

    assertPackageMatchesCallingUid(callingPackage);
    // 确保调用者不是隔离进程
    enforceNotIsolatedCaller("startActivityAsUser");

    if (intent != null && intent.isSandboxActivity(mContext)) {
        SdkSandboxManagerLocal sdkSandboxManagerLocal = LocalManagerRegistry.getManager(
                SdkSandboxManagerLocal.class);
        sdkSandboxManagerLocal.enforceAllowedToHostSandboxedActivity(
                intent, Binder.getCallingUid(), callingPackage
        );
    }

    if (Process.isSdkSandboxUid(Binder.getCallingUid())) {
        SdkSandboxManagerLocal sdkSandboxManagerLocal = LocalManagerRegistry.getManager(
                SdkSandboxManagerLocal.class);
        if (sdkSandboxManagerLocal == null) {
            throw new IllegalStateException("SdkSandboxManagerLocal not found when starting"
                    + " an activity from an SDK sandbox uid.");
        }
        sdkSandboxManagerLocal.enforceAllowedToStartActivity(intent);
    }

    userId = getActivityStartController().checkTargetUser(userId, validateIncomingUser,
            Binder.getCallingPid(), Binder.getCallingUid(), "startActivityAsUser");

    // TODO: Switch to user app stacks here.
    // 拿到activityStarter对象，设置各种启动参数，最后调用execute方法真正启动Activity
    return getActivityStartController().obtainStarter(intent, "startActivityAsUser") // 这里调用之后返回的是ActivityStarter对象
            .setCaller(caller)
            .setCallingPackage(callingPackage)
            .setCallingFeatureId(callingFeatureId)
            .setResolvedType(resolvedType)
            .setResultTo(resultTo)
            .setResultWho(resultWho)
            .setRequestCode(requestCode)
            .setStartFlags(startFlags)
            .setProfilerInfo(profilerInfo)
            .setActivityOptions(opts)
            .setUserId(userId)
            .execute();
}
```

</details>

关键决策点（常见“为什么启动失败/被拦截”的根源）：
- `assertPackageMatchesCallingUid(callingPackage)`：callingPackage 与 calling uid 校验  
- `enforceNotIsolatedCaller(...)`：隔离进程限制  
- `checkTargetUser(...)`：跨用户合法性  
- `obtainStarter(intent, reason).setXXX(...).execute()`：进入 ActivityStarter 主流程

### T5：ActivityStartController.obtainStarter（拿到 ActivityStarter）

- [ActivityStartController.obtainStarter](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStartController.java#L133-L136)

<details><summary>源码：ActivityStartController.obtainStarter(Intent, String)</summary>

```java
ActivityStarter obtainStarter(Intent intent, String reason) {
    // 从DefaultFactory工厂中获取一个ActivityStarter对象(实际是从内部维护的一个池子里面去取一个并返回)
    return mFactory.obtain().setIntent(intent).setReason(reason);
}
```

</details>

执行要点：
- 从工厂/对象池拿 `ActivityStarter`，设置 intent/reason，后续通过链式 setCaller/setCallingPackage/... 填充参数。

---

## 4. system_server：ActivityStarter.execute（解析/权限/拦截/构建 ActivityRecord）

### T6：ActivityStarter.execute

- [ActivityStarter.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L684-L796)

<details><summary>源码：ActivityStarter.execute()</summary>

```java
int execute() {
    try {
        // 阶段1：启动标记  mInExecution = true
        onExecutionStarted();

        // 阶段2
        // 拒绝包含文件描述符的 Intent（防止 FD 泄漏攻击）
        if (mRequest.intent != null && mRequest.intent.hasFileDescriptors()) {
            throw new IllegalArgumentException("File descriptors passed in Intent");
        }
        // 阶段 3: 性能追踪
        final LaunchingState launchingState;
        synchronized (mService.mGlobalLock) {
            final ActivityRecord caller = ActivityRecord.forTokenLocked(mRequest.resultTo);
            final int callingUid = mRequest.realCallingUid == Request.DEFAULT_REAL_CALLING_UID
                    ?  Binder.getCallingUid() : mRequest.realCallingUid;
            // 通知 ActivityMetricsLogger 开始记录启动性能
            launchingState = mSupervisor.getActivityMetricsLogger().notifyActivityLaunching(
                    mRequest.intent, caller, callingUid);
        }

        // 阶段 4: 解析目标 Activity
        if (mRequest.activityInfo == null) {
            // mRequest是ActivityStarter.Request对象，封装了启动请求的所有参数
            mRequest.resolveActivity(mSupervisor);
        }

        // 阶段 5: 关机检查点
        // 记录关机/重启请求的原始意图（用于调试）
        if (mRequest.intent != null) {
            String intentAction = mRequest.intent.getAction();
            String callingPackage = mRequest.callingPackage;
            if (intentAction != null && callingPackage != null
                    && (Intent.ACTION_REQUEST_SHUTDOWN.equals(intentAction)
                            || Intent.ACTION_SHUTDOWN.equals(intentAction)
                            || Intent.ACTION_REBOOT.equals(intentAction))) {
                ShutdownCheckPoints.recordCheckPoint(intentAction, callingPackage, null);
            }
        }

        int res;
        // 阶段 6: 执行核心请求
        synchronized (mService.mGlobalLock) {
            // 配置变更预处理
            final boolean globalConfigWillChange = mRequest.globalConfig != null
                    && mService.getGlobalConfiguration().diff(mRequest.globalConfig) != 0;
            final Task rootTask = mRootWindowContainer.getTopDisplayFocusedRootTask();
            if (rootTask != null) {
                rootTask.mConfigWillChange = globalConfigWillChange;
            }
            ProtoLog.v(WM_DEBUG_CONFIGURATION, "Starting activity when config "
                    + "will change = %b", globalConfigWillChange);

            final long origId = Binder.clearCallingIdentity();

            res = resolveToHeavyWeightSwitcherIfNeeded();
            if (res != START_SUCCESS) {
                return res;
            }

            try {
                // 构建ActivityRecord，实际启动Activity的关键步骤
                res = executeRequest(mRequest);
            } finally {
                mRequest.logMessage.append(" result code=").append(res);
                Slog.i(TAG, mRequest.logMessage.toString());
                mRequest.logMessage.setLength(0);
            }

            Binder.restoreCallingIdentity(origId);
            // 配置变更后处理
            if (globalConfigWillChange) {
                // If the caller also wants to switch to a new configuration, do so now.
                // This allows a clean switch, as we are waiting for the current activity
                // to pause (so we will not destroy it), and have not yet started the
                // next activity.
                mService.mAmInternal.enforceCallingPermission(
                        android.Manifest.permission.CHANGE_CONFIGURATION,
                        "updateConfiguration()");
                if (rootTask != null) {
                    rootTask.mConfigWillChange = false;
                }
                ProtoLog.v(WM_DEBUG_CONFIGURATION,
                            "Updating to new configuration after starting activity.");
                // 更新系统配置
                mService.updateConfigurationLocked(mRequest.globalConfig, null, false);
            }

            // The original options may have additional info about metrics. The mOptions is not
            // used here because it may be cleared in setTargetRootTaskIfNeeded.
            final ActivityOptions originalOptions = mRequest.activityOptions != null
                    ? mRequest.activityOptions.getOriginalOptions() : null;
            // Only track the launch time of activity that will be resumed.
            final ActivityRecord launchingRecord = mDoResume ? mLastStartActivityRecord : null;
            // If the new record is the one that started, a new activity has created.
            final boolean newActivityCreated = mStartActivity == launchingRecord;
            // Notify ActivityMetricsLogger that the activity has launched.
            // ActivityMetricsLogger will then wait for the windows to be drawn and populate
            // WaitResult.
            // 通知性能追踪结束
            mSupervisor.getActivityMetricsLogger().notifyActivityLaunched(launchingState, res,
                    newActivityCreated, launchingRecord, originalOptions);
            if (mRequest.waitResult != null) {
                mRequest.waitResult.result = res;
                res = waitResultIfNeeded(mRequest.waitResult, mLastStartActivityRecord,
                        launchingState);
            }
            return getExternalResult(res);
        }
    } finally {
        // 通知控制器流程结束
        onExecutionComplete();// 回收 Starter 到对象池
    }
}
```

</details>

主干步骤：
1. 拒绝 FD 泄漏：`intent.hasFileDescriptors()`  
2. 性能打点：`notifyActivityLaunching(...)`  
3. 解析 ActivityInfo：`mRequest.resolveActivity(...)`（隐式 intent 解析）  
4. heavy-weight 处理：`resolveToHeavyWeightSwitcherIfNeeded()`（可能改写 intent）  
5. 进入核心：`executeRequest(mRequest)`

### T7：ActivityStarter.executeRequest（最重要的“决策区”）

- [ActivityStarter.executeRequest](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L901-L1331)

<details><summary>源码：ActivityStarter.executeRequest(Request)</summary>

```java
private int executeRequest(Request request) {
    if (TextUtils.isEmpty(request.reason)) {
        throw new IllegalArgumentException("Need to specify a reason.");
    }
    mLastStartReason = request.reason;
    mLastStartActivityTimeMs = System.currentTimeMillis();
    // Reset the ActivityRecord#mCurrentLaunchCanTurnScreenOn state of last start activity in
    // case the state is not yet consumed during rapid activity launch.
    if (mLastStartActivityRecord != null) {
        mLastStartActivityRecord.setCurrentLaunchCanTurnScreenOn(false);
    }
    mLastStartActivityRecord = null;

    final IApplicationThread caller = request.caller;
    Intent intent = request.intent;
    NeededUriGrants intentGrants = request.intentGrants;
    String resolvedType = request.resolvedType;
    ActivityInfo aInfo = request.activityInfo;
    ResolveInfo rInfo = request.resolveInfo;
    final IVoiceInteractionSession voiceSession = request.voiceSession;
    final IBinder resultTo = request.resultTo;
    String resultWho = request.resultWho;
    int requestCode = request.requestCode;
    int callingPid = request.callingPid;
    int callingUid = request.callingUid;
    String callingPackage = request.callingPackage;
    String callingFeatureId = request.callingFeatureId;
    final int realCallingPid = request.realCallingPid;
    final int realCallingUid = request.realCallingUid;
    final int startFlags = request.startFlags;
    final SafeActivityOptions options = request.activityOptions;
    Task inTask = request.inTask;
    TaskFragment inTaskFragment = request.inTaskFragment;

    int err = ActivityManager.START_SUCCESS;
    // Pull the optional Ephemeral Installer-only bundle out of the options early.
    final Bundle verificationBundle =
            options != null ? options.popAppVerificationBundle() : null;

    WindowProcessController callerApp = null;
    if (caller != null) {
        callerApp = mService.getProcessController(caller);
        if (callerApp != null) {
            callingPid = callerApp.getPid();
            callingUid = callerApp.mInfo.uid;
        } else {
            Slog.w(TAG, "Unable to find app for caller " + caller + " (pid=" + callingPid
                    + ") when starting: " + intent.toString());
            err = START_PERMISSION_DENIED;
        }
    }

    final int userId = aInfo != null && aInfo.applicationInfo != null
            ? UserHandle.getUserId(aInfo.applicationInfo.uid) : 0;
    final int launchMode = aInfo != null ? aInfo.launchMode : 0;
    if (err == ActivityManager.START_SUCCESS) {
        request.logMessage.append("START u").append(userId).append(" {")
                .append(intent.toShortString(true, true, true, false))
                .append("} with ").append(launchModeToString(launchMode))
                .append(" from uid ").append(callingUid);
        if (callingUid != realCallingUid
                && realCallingUid != Request.DEFAULT_REAL_CALLING_UID) {
            request.logMessage.append(" (realCallingUid=").append(realCallingUid).append(")");
        }
    }
    // 源Record：当下启动新activity的record
    ActivityRecord sourceRecord = null;
    ActivityRecord resultRecord = null;
    if (resultTo != null) {
        sourceRecord = ActivityRecord.isInAnyTask(resultTo);
        if (DEBUG_RESULTS) {
            Slog.v(TAG_RESULTS, "Will send result to " + resultTo + " " + sourceRecord);
        }
        if (sourceRecord != null) {
            if (requestCode >= 0 && !sourceRecord.finishing) {
                resultRecord = sourceRecord;
            }
        }
    }

    final int launchFlags = intent.getFlags();
    if ((launchFlags & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0 && sourceRecord != null) {
        // Transfer the result target from the source activity to the new one being started,
        // including any failures.
        // 需要拿到结果
        if (requestCode >= 0) {
            SafeActivityOptions.abort(options);
            return ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT;
        }
        resultRecord = sourceRecord.resultTo;
        if (resultRecord != null && !resultRecord.isInRootTaskLocked()) {
            resultRecord = null;
        }
        resultWho = sourceRecord.resultWho;
        requestCode = sourceRecord.requestCode;
        sourceRecord.resultTo = null;
        if (resultRecord != null) {
            resultRecord.removeResultsLocked(sourceRecord, resultWho, requestCode);
        }
        if (sourceRecord.launchedFromUid == callingUid) {
            // The new activity is being launched from the same uid as the previous activity
            // in the flow, and asking to forward its result back to the previous.  In this
            // case the activity is serving as a trampoline between the two, so we also want
            // to update its launchedFromPackage to be the same as the previous activity.
            // Note that this is safe, since we know these two packages come from the same
            // uid; the caller could just as well have supplied that same package name itself
            // . This specifially deals with the case of an intent picker/chooser being
            // launched in the app flow to redirect to an activity picked by the user, where
            // we want the final activity to consider it to have been launched by the
            // previous app activity.
            callingPackage = sourceRecord.launchedFromPackage;
            callingFeatureId = sourceRecord.launchedFromFeatureId;
        }
    }

    if (err == ActivityManager.START_SUCCESS && intent.getComponent() == null) {
        // We couldn't find a class that can handle the given Intent.
        // That's the end of that!
        err = ActivityManager.START_INTENT_NOT_RESOLVED;
    }

    if (err == ActivityManager.START_SUCCESS && aInfo == null) {
        // We couldn't find the specific class specified in the Intent.
        // Also the end of the line.
        err = ActivityManager.START_CLASS_NOT_FOUND;
    }

    if (err == ActivityManager.START_SUCCESS && sourceRecord != null
            && sourceRecord.getTask().voiceSession != null) {
        // If this activity is being launched as part of a voice session, we need to ensure
        // that it is safe to do so.  If the upcoming activity will also be part of the voice
        // session, we can only launch it if it has explicitly said it supports the VOICE
        // category, or it is a part of the calling app.
        if ((launchFlags & FLAG_ACTIVITY_NEW_TASK) == 0
                && sourceRecord.info.applicationInfo.uid != aInfo.applicationInfo.uid) {
            try {
                intent.addCategory(Intent.CATEGORY_VOICE);
                if (!mService.getPackageManager().activitySupportsIntentAsUser(
                        intent.getComponent(), intent, resolvedType, userId)) {
                    Slog.w(TAG, "Activity being started in current voice task does not support "
                            + "voice: " + intent);
                    err = ActivityManager.START_NOT_VOICE_COMPATIBLE;
                }
            } catch (RemoteException e) {
                Slog.w(TAG, "Failure checking voice capabilities", e);
                err = ActivityManager.START_NOT_VOICE_COMPATIBLE;
            }
        }
    }

    if (err == ActivityManager.START_SUCCESS && voiceSession != null) {
        // If the caller is starting a new voice session, just make sure the target
        // is actually allowing it to run this way.
        try {
            if (!mService.getPackageManager().activitySupportsIntentAsUser(
                    intent.getComponent(), intent, resolvedType, userId)) {
                Slog.w(TAG,
                        "Activity being started in new voice task does not support: " + intent);
                err = ActivityManager.START_NOT_VOICE_COMPATIBLE;
            }
        } catch (RemoteException e) {
            Slog.w(TAG, "Failure checking voice capabilities", e);
            err = ActivityManager.START_NOT_VOICE_COMPATIBLE;
        }
    }

    final Task resultRootTask = resultRecord == null
            ? null : resultRecord.getRootTask();

    if (err != START_SUCCESS) {
        if (resultRecord != null) {
            resultRecord.sendResult(INVALID_UID, resultWho, requestCode, RESULT_CANCELED,
                    null /* data */, null /* dataGrants */);
        }
        SafeActivityOptions.abort(options);
        return err;
    }

    boolean abort;
    try {
        // 检测启动activity的权限
        abort = !mSupervisor.checkStartAnyActivityPermission(intent, aInfo, resultWho,
                requestCode, callingPid, callingUid, callingPackage, callingFeatureId,
                request.ignoreTargetSecurity, inTask != null, callerApp, resultRecord,
                resultRootTask);
    } catch (SecurityException e) {
        // Return activity not found for the explicit intent if the caller can't see the target
        // to prevent the disclosure of package existence.
        final Intent originalIntent = request.ephemeralIntent;
        if (originalIntent != null && (originalIntent.getComponent() != null
                || originalIntent.getPackage() != null)) {
            final String targetPackageName = originalIntent.getComponent() != null
                    ? originalIntent.getComponent().getPackageName()
                    : originalIntent.getPackage();
            if (mService.getPackageManagerInternalLocked()
                    .filterAppAccess(targetPackageName, callingUid, userId)) {
                if (resultRecord != null) {
                    resultRecord.sendResult(INVALID_UID, resultWho, requestCode,
                            RESULT_CANCELED, null /* data */, null /* dataGrants */);
                }
                SafeActivityOptions.abort(options);
                return ActivityManager.START_CLASS_NOT_FOUND;
            }
        }
        throw e;
    }
    abort |= !mService.mIntentFirewall.checkStartActivity(intent, callingUid,
            callingPid, resolvedType, aInfo.applicationInfo);
    abort |= !mService.getPermissionPolicyInternal().checkStartActivity(intent, callingUid,
            callingPackage);

    // Merge the two options bundles, while realCallerOptions takes precedence.
    ActivityOptions checkedOptions = options != null
            ? options.getOptions(intent, aInfo, callerApp, mSupervisor) : null;

    @BalCode int balCode = BAL_ALLOW_DEFAULT;
    if (!abort) {
        try {
            Trace.traceBegin(Trace.TRACE_TAG_WINDOW_MANAGER,
                    "shouldAbortBackgroundActivityStart");
            BackgroundActivityStartController balController =
                    mController.getBackgroundActivityLaunchController();
            balCode =
                    balController.checkBackgroundActivityStart(
                            callingUid,
                            callingPid,
                            callingPackage,
                            realCallingUid,
                            realCallingPid,
                            callerApp,
                            request.originatingPendingIntent,
                            request.backgroundStartPrivileges,
                            intent,
                            checkedOptions);
            if (balCode != BAL_ALLOW_DEFAULT) {
                request.logMessage.append(" (").append(
                                BackgroundActivityStartController.balCodeToString(balCode))
                        .append(")");
            }
        } finally {
            Trace.traceEnd(Trace.TRACE_TAG_WINDOW_MANAGER);
        }
    }

    if (request.allowPendingRemoteAnimationRegistryLookup) {
        checkedOptions = mService.getActivityStartController()
                .getPendingRemoteAnimationRegistry()
                .overrideOptionsIfNeeded(callingPackage, checkedOptions);
    }
    if (mService.mController != null) {
        try {
            // The Intent we give to the watcher has the extra data stripped off, since it
            // can contain private information.
            Intent watchIntent = intent.cloneFilter();
            abort |= !mService.mController.activityStarting(watchIntent,
                    aInfo.applicationInfo.packageName);
        } catch (RemoteException e) {
            mService.mController = null;
        }
    }

    mInterceptor.setStates(userId, realCallingPid, realCallingUid, startFlags, callingPackage,
            callingFeatureId);
    if (mInterceptor.intercept(intent, rInfo, aInfo, resolvedType, inTask, inTaskFragment,
            callingPid, callingUid, checkedOptions)) {
        // activity start was intercepted, e.g. because the target user is currently in quiet
        // mode (turn off work) or the target application is suspended
        intent = mInterceptor.mIntent;
        rInfo = mInterceptor.mRInfo;
        aInfo = mInterceptor.mAInfo;
        resolvedType = mInterceptor.mResolvedType;
        inTask = mInterceptor.mInTask;
        callingPid = mInterceptor.mCallingPid;
        callingUid = mInterceptor.mCallingUid;
        checkedOptions = mInterceptor.mActivityOptions;

        // The interception target shouldn't get any permission grants
        // intended for the original destination
        intentGrants = null;
    }

    if (abort) {
        if (resultRecord != null) {
            resultRecord.sendResult(INVALID_UID, resultWho, requestCode, RESULT_CANCELED,
                    null /* data */, null /* dataGrants */);
        }
        // We pretend to the caller that it was really started, but they will just get a
        // cancel result.
        ActivityOptions.abort(checkedOptions);
        return START_ABORTED;
    }

    // If permissions need a review before any of the app components can run, we
    // launch the review activity and pass a pending intent to start the activity
    // we are to launching now after the review is completed.
    if (aInfo != null) {
        if (mService.getPackageManagerInternalLocked().isPermissionsReviewRequired(
                aInfo.packageName, userId)) {
            final IIntentSender target = mService.getIntentSenderLocked(
                    ActivityManager.INTENT_SENDER_ACTIVITY, callingPackage, callingFeatureId,
                    callingUid, userId, null, null, 0, new Intent[]{intent},
                    new String[]{resolvedType}, PendingIntent.FLAG_CANCEL_CURRENT
                            | PendingIntent.FLAG_ONE_SHOT, null);

            Intent newIntent = new Intent(Intent.ACTION_REVIEW_PERMISSIONS);

            int flags = intent.getFlags();
            flags |= Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS;

            /*
             * Prevent reuse of review activity: Each app needs their own review activity. By
             * default activities launched with NEW_TASK or NEW_DOCUMENT try to reuse activities
             * with the same launch parameters (extras are ignored). Hence to avoid possible
             * reuse force a new activity via the MULTIPLE_TASK flag.
             *
             * Activities that are not launched with NEW_TASK or NEW_DOCUMENT are not re-used,
             * hence no need to add the flag in this case.
             */
            if ((flags & (FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_NEW_DOCUMENT)) != 0) {
                flags |= Intent.FLAG_ACTIVITY_MULTIPLE_TASK;
            }
            newIntent.setFlags(flags);

            newIntent.putExtra(Intent.EXTRA_PACKAGE_NAME, aInfo.packageName);
            newIntent.putExtra(Intent.EXTRA_INTENT, new IntentSender(target));
            if (resultRecord != null) {
                newIntent.putExtra(Intent.EXTRA_RESULT_NEEDED, true);
            }
            intent = newIntent;

            // The permissions review target shouldn't get any permission
            // grants intended for the original destination
            intentGrants = null;

            resolvedType = null;
            callingUid = realCallingUid;
            callingPid = realCallingPid;

            rInfo = mSupervisor.resolveIntent(intent, resolvedType, userId, 0,
                    computeResolveFilterUid(
                            callingUid, realCallingUid, request.filterCallingUid),
                    realCallingPid);
            aInfo = mSupervisor.resolveActivity(intent, rInfo, startFlags,
                    null /*profilerInfo*/);

            if (DEBUG_PERMISSIONS_REVIEW) {
                final Task focusedRootTask =
                        mRootWindowContainer.getTopDisplayFocusedRootTask();
                Slog.i(TAG, "START u" + userId + " {" + intent.toShortString(true, true,
                        true, false) + "} from uid " + callingUid + " on display "
                        + (focusedRootTask == null ? DEFAULT_DISPLAY
                                : focusedRootTask.getDisplayId()));
            }
        }
    }

    // If we have an ephemeral app, abort the process of launching the resolved intent.
    // Instead, launch the ephemeral installer. Once the installer is finished, it
    // starts either the intent we resolved here [on install error] or the ephemeral
    // app [on install success].
    if (rInfo != null && rInfo.auxiliaryInfo != null) {
        intent = createLaunchIntent(rInfo.auxiliaryInfo, request.ephemeralIntent,
                callingPackage, callingFeatureId, verificationBundle, resolvedType, userId);
        resolvedType = null;
        callingUid = realCallingUid;
        callingPid = realCallingPid;

        // The ephemeral installer shouldn't get any permission grants
        // intended for the original destination
        intentGrants = null;

        aInfo = mSupervisor.resolveActivity(intent, rInfo, startFlags, null /*profilerInfo*/);
    }
    // TODO (b/187680964) Correcting the caller/pid/uid when start activity from shortcut
    // Pending intent launched from systemui also depends on caller app
    if (callerApp == null && realCallingPid > 0) {
        final WindowProcessController wpc = mService.mProcessMap.getProcess(realCallingPid);
        if (wpc != null) {
            callerApp = wpc;
        }
    }
    final ActivityRecord r = new ActivityRecord.Builder(mService)
            .setCaller(callerApp)
            .setLaunchedFromPid(callingPid)
            .setLaunchedFromUid(callingUid)
            .setLaunchedFromPackage(callingPackage)
            .setLaunchedFromFeature(callingFeatureId)
            .setIntent(intent)
            .setResolvedType(resolvedType)
            .setActivityInfo(aInfo)
            .setConfiguration(mService.getGlobalConfiguration())
            .setResultTo(resultRecord)
            .setResultWho(resultWho)
            .setRequestCode(requestCode)
            .setComponentSpecified(request.componentSpecified)
            .setRootVoiceInteraction(voiceSession != null)
            .setActivityOptions(checkedOptions)
            .setSourceRecord(sourceRecord)
            .build();

    mLastStartActivityRecord = r;

    if (r.appTimeTracker == null && sourceRecord != null) {
        // If the caller didn't specify an explicit time tracker, we want to continue
        // tracking under any it has.
        r.appTimeTracker = sourceRecord.appTimeTracker;
    }

    // Only allow app switching to be resumed if activity is not a restricted background
    // activity and target app is not home process, otherwise any background activity
    // started in background task can stop home button protection mode.
    // As the targeted app is not a home process and we don't need to wait for the 2nd
    // activity to be started to resume app switching, we can just enable app switching
    // directly.
    WindowProcessController homeProcess = mService.mHomeProcess;
    boolean isHomeProcess = homeProcess != null
            && aInfo.applicationInfo.uid == homeProcess.mUid;
    if (balCode != BAL_BLOCK && !isHomeProcess) {
        mService.resumeAppSwitches();
    }
    // 
    mLastStartActivityResult = startActivityUnchecked(r, sourceRecord, voiceSession,
            request.voiceInteractor, startFlags, checkedOptions,
            inTask, inTaskFragment, balCode, intentGrants, realCallingUid);

    if (request.outActivity != null) {
        request.outActivity[0] = mLastStartActivityRecord;
    }

    return mLastStartActivityResult;
}
```

</details>

关键决策点（按出现顺序）：
- 结果转发：`FLAG_ACTIVITY_FORWARD_RESULT`（requestCode 冲突会直接返回错误）  
- 目标解析失败：`START_INTENT_NOT_RESOLVED / START_CLASS_NOT_FOUND`  
- 启动权限总闸：`mSupervisor.checkStartAnyActivityPermission(...)`（exported/permission/appop 等）  
- IntentFirewall / PermissionPolicy：策略层拦截  
- BAL（后台启动限制）：`BackgroundActivityStartController.checkBackgroundActivityStart(...)`  
- Interceptor：quiet mode / suspended / work profile 等拦截与替换 intent  
- Permissions Review/InstantApp：可能改写 intent 先走审查/安装器  
- 构建 ActivityRecord：`new ActivityRecord.Builder(...).build()`  
- 进入任务栈与调度：`startActivityUnchecked(...)`

---

## 5. system_server：任务栈选择/复用/清栈/置顶/触发 resume

### T8：startActivityUnchecked → startActivityInner

- [ActivityStarter.startActivityInner](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L1640-L1859)

<details><summary>源码：ActivityStarter.startActivityInner(...)</summary>

```java
int startActivityInner(final ActivityRecord r, ActivityRecord sourceRecord,
        IVoiceInteractionSession voiceSession, IVoiceInteractor voiceInteractor,
        int startFlags, ActivityOptions options, Task inTask,
        TaskFragment inTaskFragment, @BalCode int balCode,
        NeededUriGrants intentGrants, int realCallingUid) {

    setInitialState(r, options, inTask, inTaskFragment, startFlags, sourceRecord,
            voiceSession, voiceInteractor, balCode, realCallingUid);
    // 设置一些启动标记
    computeLaunchingTaskFlags();
    mIntent.setFlags(mLaunchFlags);

    boolean dreamStopping = false;

    for (ActivityRecord stoppingActivity : mSupervisor.mStoppingActivities) {
        if (stoppingActivity.getActivityType()
                == WindowConfiguration.ACTIVITY_TYPE_DREAM) {
            dreamStopping = true;
            break;
        }
    }

    // Get top task at beginning because the order may be changed when reusing existing task.
    final Task prevTopRootTask = mPreferredTaskDisplayArea.getFocusedRootTask();
    final Task prevTopTask = prevTopRootTask != null ? prevTopRootTask.getTopLeafTask() : null;
    // 获取可复用的 Task
    final Task reusedTask = getReusableTask();

    // If requested, freeze the task list
    if (mOptions != null && mOptions.freezeRecentTasksReordering()
            && mSupervisor.mRecentTasks.isCallerRecents(r.launchedFromUid)
            && !mSupervisor.mRecentTasks.isFreezeTaskListReorderingSet()) {
        mFrozeTaskList = true;
        mSupervisor.mRecentTasks.setFreezeTaskListReordering();
    }

    // Compute if there is an existing task that should be used for.
    final Task targetTask = reusedTask != null ? reusedTask : computeTargetTask();
    final boolean newTask = targetTask == null;
    mTargetTask = targetTask;

    computeLaunchParams(r, sourceRecord, targetTask);

    // Check if starting activity on given task or on a new task is allowed.
    // 是否允许启动
    int startResult = isAllowedToStart(r, newTask, targetTask);
    if (startResult != START_SUCCESS) {
        if (r.resultTo != null) {
            r.resultTo.sendResult(INVALID_UID, r.resultWho, r.requestCode, RESULT_CANCELED,
                    null /* data */, null /* dataGrants */);
        }
        return startResult;
    }

    if (targetTask != null) {
        // 任务栈 300 个任务
        if (targetTask.getTreeWeight() > MAX_TASK_WEIGHT_FOR_ADDING_ACTIVITY) {
            Slog.e(TAG, "Remove " + targetTask + " because it has contained too many"
                    + " activities or windows (abort starting " + r
                    + " from uid=" + mCallingUid);
            targetTask.removeImmediately("bulky-task");
            return START_ABORTED;
        }
        // When running transient transition, the transient launch target should keep on top.
        // So disallow the transient hide activity to move itself to front, e.g. trampoline.
        if (!mAvoidMoveToFront && (mService.mHomeProcess == null
                || mService.mHomeProcess.mUid != realCallingUid)
                && r.mTransitionController.isTransientHide(targetTask)) {
            mAvoidMoveToFront = true;
        }
        // 记录在启动/重用目标 task 前，栈上方原来是谁，以便后续处理过渡动画和任务顺序相关的逻辑
        mPriorAboveTask = TaskDisplayArea.getRootTaskAbove(targetTask.getRootTask());
    }

    final ActivityRecord targetTaskTop = newTask
            ? null : targetTask.getTopNonFinishingActivity();
    if (targetTaskTop != null) {
        // Removes the existing singleInstance activity in another task (if any) while
        // launching a singleInstance activity on sourceRecord's task.
        if (LAUNCH_SINGLE_INSTANCE == mLaunchMode && mSourceRecord != null
                && targetTask == mSourceRecord.getTask()) {
            final ActivityRecord activity = mRootWindowContainer.findActivity(mIntent,
                    mStartActivity.info, false);
            if (activity != null && activity.getTask() != targetTask) {
                activity.destroyIfPossible("Removes redundant singleInstance");
            }
        }
        recordTransientLaunchIfNeeded(targetTaskTop);
        // Recycle the target task for this launch.
        startResult = recycleTask(targetTask, targetTaskTop, reusedTask, intentGrants);
        if (startResult != START_SUCCESS) {
            return startResult;
        }
    } else {
        mAddingToTask = true;
    }

    // If the activity being launched is the same as the one currently at the top, then
    // we need to check if it should only be launched once.
    final Task topRootTask = mPreferredTaskDisplayArea.getFocusedRootTask();
    if (topRootTask != null) {
        startResult = deliverToCurrentTopIfNeeded(topRootTask, intentGrants);
        if (startResult != START_SUCCESS) {
            return startResult;
        }
    }

    if (mTargetRootTask == null) {
        mTargetRootTask = getOrCreateRootTask(mStartActivity, mLaunchFlags, targetTask,
                mOptions);
    }
    if (newTask) {
        final Task taskToAffiliate = (mLaunchTaskBehind && mSourceRecord != null)
                ? mSourceRecord.getTask() : null;
        setNewTask(taskToAffiliate);
    } else if (mAddingToTask) {
        addOrReparentStartingActivity(targetTask, "adding to task");
    }

    // After activity is attached to task, but before actual start
    recordTransientLaunchIfNeeded(mLastStartActivityRecord);

    if (!mAvoidMoveToFront && mDoResume) {
        mTargetRootTask.getRootTask().moveToFront("reuseOrNewTask", targetTask);
        if (!mTargetRootTask.isTopRootTaskInDisplayArea() && mService.isDreaming()
                && !dreamStopping) {
            // Launching underneath dream activity (fullscreen, always-on-top). Run the launch-
            // -behind transition so the Activity gets created and starts in visible state.
            mLaunchTaskBehind = true;
            r.mLaunchTaskBehind = true;
        }
    }

    mService.mUgmInternal.grantUriPermissionUncheckedFromIntent(intentGrants,
            mStartActivity.getUriPermissionsLocked());
    if (mStartActivity.resultTo != null && mStartActivity.resultTo.info != null) {
        // we need to resolve resultTo to a uid as grantImplicitAccess deals explicitly in UIDs
        final PackageManagerInternal pmInternal =
                mService.getPackageManagerInternalLocked();
        final int resultToUid = pmInternal.getPackageUid(
                mStartActivity.resultTo.info.packageName, 0 /* flags */,
                mStartActivity.mUserId);
        pmInternal.grantImplicitAccess(mStartActivity.mUserId, mIntent,
                UserHandle.getAppId(mStartActivity.info.applicationInfo.uid) /*recipient*/,
                resultToUid /*visible*/, true /*direct*/);
    } else if (mStartActivity.mShareIdentity) {
        final PackageManagerInternal pmInternal =
                mService.getPackageManagerInternalLocked();
        pmInternal.grantImplicitAccess(mStartActivity.mUserId, mIntent,
                UserHandle.getAppId(mStartActivity.info.applicationInfo.uid) /*recipient*/,
                r.launchedFromUid /*visible*/, true /*direct*/);
    }
    // 先获取当前任务栈的顶部的Task，然后调用mTargetRootTask.startActivityLocked(...)，将Activity加入Task并准备启动
    final Task startedTask = mStartActivity.getTask();
    if (newTask) {
        EventLogTags.writeWmCreateTask(mStartActivity.mUserId, startedTask.mTaskId,
                startedTask.getRootTaskId(), startedTask.getDisplayId());
    }
    mStartActivity.logStartActivity(EventLogTags.WM_CREATE_ACTIVITY, startedTask);

    mStartActivity.getTaskFragment().clearLastPausedActivity();

    mRootWindowContainer.startPowerModeLaunchIfNeeded(
            false /* forceSend */, mStartActivity);

    final boolean isTaskSwitch = startedTask != prevTopTask;
    // 将Activity加入Task并准备启动
    mTargetRootTask.startActivityLocked(mStartActivity, topRootTask, newTask, isTaskSwitch,
            mOptions, sourceRecord);
    if (mDoResume) {
        final ActivityRecord topTaskActivity = startedTask.topRunningActivityLocked();
        if (!mTargetRootTask.isTopActivityFocusable()
                || (topTaskActivity != null && topTaskActivity.isTaskOverlay()
                && mStartActivity != topTaskActivity)) {
            // If the activity is not focusable, we can't resume it, but still would like to
            // make sure it becomes visible as it starts (this will also trigger entry
            // animation). An example of this are PIP activities.
            // Also, we don't want to resume activities in a task that currently has an overlay
            // as the starting activity just needs to be in the visible paused state until the
            // over is removed.
            // Passing {@code null} as the start parameter ensures all activities are made
            // visible.
            mTargetRootTask.ensureActivitiesVisible(null /* starting */,
                    0 /* configChanges */, !PRESERVE_WINDOWS);
            // Go ahead and tell window manager to execute app transition for this activity
            // since the app transition will not be triggered through the resume channel.
            mTargetRootTask.mDisplayContent.executeAppTransition();
        } else {
            // If the target root-task was not previously focusable (previous top running
            // activity on that root-task was not visible) then any prior calls to move the
            // root-task to the will not update the focused root-task.  If starting the new
            // activity now allows the task root-task to be focusable, then ensure that we
            // now update the focused root-task accordingly.
            if (!mAvoidMoveToFront && mTargetRootTask.isTopActivityFocusable()
                    && !mRootWindowContainer.isTopDisplayFocusedRootTask(mTargetRootTask)) {
                mTargetRootTask.moveToFront("startActivityInner");
            }
            // 启动 Activity
            mRootWindowContainer.resumeFocusedTasksTopActivities(
                    mTargetRootTask, mStartActivity, mOptions, mTransientLaunch);
        }
    }
    mRootWindowContainer.updateUserRootTask(mStartActivity.mUserId, mTargetRootTask);

    // Update the recent tasks list immediately when the activity starts
    mSupervisor.mRecentTasks.add(startedTask);
    mSupervisor.handleNonResizableTaskIfNeeded(startedTask,
            mPreferredWindowingMode, mPreferredTaskDisplayArea, mTargetRootTask);

    // If Activity's launching into PiP, move the mStartActivity immediately to pinned mode.
    // Note that mStartActivity and source should be in the same Task at this point.
    if (mOptions != null && mOptions.isLaunchIntoPip()
            && sourceRecord != null && sourceRecord.getTask() == mStartActivity.getTask()
            && balCode != BAL_BLOCK) {
        mRootWindowContainer.moveActivityToPinnedRootTask(mStartActivity,
                sourceRecord, "launch-into-pip");
    }

    return START_SUCCESS;
}
```

</details>

这里解决“放到哪个 task/rootTask、是否复用、是否清栈、是否仅送 newIntent、是否创建新 task”等核心问题：
- `getReusableTask()`：singleTask/singleInstance、NEW_TASK 等复用策略  
  - [ActivityStarter.getReusableTask](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L2891-L2948)
- `deliverToCurrentTopIfNeeded()`：singleTop/same top 直接送 newIntent，不新建实例  
  - [deliverToCurrentTopIfNeeded](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L2381-L2424)
- `complyActivityFlags(...)`：CLEAR_TOP/CLEAR_TASK/REORDER_TO_FRONT 等  
  - [complyActivityFlags](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L2430-L2556)
- 把 Activity 加入任务/准备转场与 starting window：  
  - `mTargetRootTask.startActivityLocked(...)` 调用点：[startActivityInner](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L1805-L1808)  
  - 实现：[Task.startActivityLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/Task.java#L5107-L5222)
- 触发 resume：  
  - `mRootWindowContainer.resumeFocusedTasksTopActivities(...)`：[startActivityInner](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L1837-L1840)

### T9：RootWindowContainer/Task/TaskFragment resume 链路（挑选 next 并决定是否拉进程）

- [RootWindowContainer.resumeFocusedTasksTopActivities](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java#L2296-L2363)  
- [Task.resumeTopActivityUncheckedLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/Task.java#L4980-L5075)  
- [TaskFragment.resumeTopActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/TaskFragment.java#L1186-L1553)

关键点：
- 若 `next` 进程不存在：`mTaskSupervisor.startSpecificActivity(next, ...)`  
  - 调用点：[TaskFragment.resumeTopActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/TaskFragment.java#L1539-L1550)

---

## 6. system_server：startSpecificActivity → realStartActivityLocked（下发 transaction）

### T10：ActivityTaskSupervisor.startSpecificActivity

- [ActivityTaskSupervisor.startSpecificActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java#L1057-L1094)

<details><summary>源码：ActivityTaskSupervisor.startSpecificActivity(...)</summary>

```java
// 启动activity
void startSpecificActivity(ActivityRecord r, boolean andResume, boolean checkConfig) {
    // Is this activity's application already running?
    final WindowProcessController wpc =
            mService.getProcessController(r.processName, r.info.applicationInfo.uid);

    boolean knownToBeDead = false;
    if (wpc != null && wpc.hasThread()) {
        try {
            // 进程已经存在了，直接启动activity
            realStartActivityLocked(r, wpc, andResume, checkConfig);
            return;
        } catch (RemoteException e) {
            Slog.w(TAG, "Exception when starting activity "
                    + r.intent.getComponent().flattenToShortString(), e);
        }

        // If a dead object exception was thrown -- fall through to
        // restart the application.
        knownToBeDead = true;
        // Remove the process record so it won't be considered as alive.
        mService.mProcessNames.remove(wpc.mName, wpc.mUid);
        mService.mProcessMap.remove(wpc.getPid());
    } else if (r.intent.isSandboxActivity(mService.mContext)) {
        Slog.e(TAG, "Abort sandbox activity launching as no sandbox process to host it.");
        r.finishIfPossible("No sandbox process for the activity", false /* oomAdj */);
        r.launchFailed = true;
        r.detachFromProcess();
        return;
    }

    r.notifyUnknownVisibilityLaunchedForKeyguardTransition();

    final boolean isTop = andResume && r.isTopRunningActivity();
    // mService 是 ActivityTaskManagerService
    mService.startProcessAsync(r, knownToBeDead, isTop,
            isTop ? HostingRecord.HOSTING_TYPE_TOP_ACTIVITY
                    : HostingRecord.HOSTING_TYPE_ACTIVITY);
}
```

</details>

分支：
- 目标进程已存在且有线程：`realStartActivityLocked(r, proc, andResume, checkConfig)`  
- 否则：`mService.startProcessAsync(...)` 走 AMS 拉起进程，之后回到 realStart 再发 transaction

### T11：ActivityTaskSupervisor.realStartActivityLocked（发 ClientTransaction）

- [ActivityTaskSupervisor.realStartActivityLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java#L785-L1027)

<details><summary>源码：ActivityTaskSupervisor.realStartActivityLocked(...)</summary>

```java
boolean realStartActivityLocked(ActivityRecord r, WindowProcessController proc,
        boolean andResume, boolean checkConfig) throws RemoteException {

    if (!mRootWindowContainer.allPausedActivitiesComplete()) {
        // While there are activities pausing we skipping starting any new activities until
        // pauses are complete. NOTE: that we also do this for activities that are starting in
        // the paused state because they will first be resumed then paused on the client side.
        ProtoLog.v(WM_DEBUG_STATES,
                "realStartActivityLocked: Skipping start of r=%s some activities pausing...",
                r);
        return false;
    }

    final Task task = r.getTask();
    final Task rootTask = task.getRootTask();

    beginDeferResume();
    // The LaunchActivityItem also contains process configuration, so the configuration change
    // from WindowProcessController#setProcess can be deferred. The major reason is that if
    // the activity has FixedRotationAdjustments, it needs to be applied with configuration.
    // In general, this reduces a binder transaction if process configuration is changed.
    proc.pauseConfigurationDispatch();

    try {
        r.startFreezingScreenLocked(proc, 0);

        // schedule launch ticks to collect information about slow apps.
        r.startLaunchTickingLocked();
        r.lastLaunchTime = SystemClock.uptimeMillis();
        r.setProcess(proc);

        // Ensure activity is allowed to be resumed after process has set.
        if (andResume && !r.canResumeByCompat()) {
            andResume = false;
        }

        r.notifyUnknownVisibilityLaunchedForKeyguardTransition();

        // Have the window manager re-evaluate the orientation of the screen based on the new
        // activity order.  Note that as a result of this, it can call back into the activity
        // manager with a new orientation.  We don't care about that, because the activity is
        // not currently running so we are just restarting it anyway.
        if (checkConfig) {
            // Deferring resume here because we're going to launch new activity shortly.
            // We don't want to perform a redundant launch of the same record while ensuring
            // configurations and trying to resume top activity of focused root task.
            mRootWindowContainer.ensureVisibilityAndConfig(r, r.getDisplayId(),
                    false /* markFrozenIfConfigChanged */, true /* deferResume */);
        }

        if (mKeyguardController.checkKeyguardVisibility(r) && r.allowMoveToFront()) {
            // We only set the visibility to true if the activity is not being launched in
            // background, and is allowed to be visible based on keyguard state. This avoids
            // setting this into motion in window manager that is later cancelled due to later
            // calls to ensure visible activities that set visibility back to false.
            r.setVisibility(true);
        }

        final int applicationInfoUid =
                (r.info.applicationInfo != null) ? r.info.applicationInfo.uid : -1;
        if ((r.mUserId != proc.mUserId) || (r.info.applicationInfo.uid != applicationInfoUid)) {
            Slog.wtf(TAG,
                    "User ID for activity changing for " + r
                            + " appInfo.uid=" + r.info.applicationInfo.uid
                            + " info.ai.uid=" + applicationInfoUid
                            + " old=" + r.app + " new=" + proc);
        }

        // Send the controller to client if the process is the first time to launch activity.
        // So the client can save binder transactions of getting the controller from activity
        // task manager service.
        final IActivityClientController activityClientController =
                proc.hasEverLaunchedActivity() ? null : mService.mActivityClientController;
        r.launchCount++;

        if (DEBUG_ALL) Slog.v(TAG, "Launching: " + r);

        final LockTaskController lockTaskController = mService.getLockTaskController();
        if (task.mLockTaskAuth == LOCK_TASK_AUTH_LAUNCHABLE
                || task.mLockTaskAuth == LOCK_TASK_AUTH_LAUNCHABLE_PRIV
                || (task.mLockTaskAuth == LOCK_TASK_AUTH_ALLOWLISTED
                        && lockTaskController.getLockTaskModeState()
                                == LOCK_TASK_MODE_LOCKED)) {
            lockTaskController.startLockTaskMode(task, false, 0 /* blank UID */);
        }

        try {
            if (!proc.hasThread()) {
                throw new RemoteException();
            }
            List<ResultInfo> results = null;
            List<ReferrerIntent> newIntents = null;
            if (andResume) {
                // We don't need to deliver new intents and/or set results if activity is going
                // to pause immediately after launch.
                results = r.results;
                newIntents = r.newIntents;
            }
            if (DEBUG_SWITCH) Slog.v(TAG_SWITCH,
                    "Launching: " + r + " savedState=" + r.getSavedState()
                            + " with results=" + results + " newIntents=" + newIntents
                            + " andResume=" + andResume);
            EventLogTags.writeWmRestartActivity(r.mUserId, System.identityHashCode(r),
                    task.mTaskId, r.shortComponentName);
            if (r.isActivityTypeHome()) {
                // Home process is the root process of the task.
                updateHomeProcess(task.getBottomMostActivity().app);
            }
            mService.getPackageManagerInternalLocked().notifyPackageUse(
                    r.intent.getComponent().getPackageName(), NOTIFY_PACKAGE_USE_ACTIVITY);
            r.forceNewConfig = false;
            mService.getAppWarningsLocked().onStartActivity(r);

            // Because we could be starting an Activity in the system process this may not go
            // across a Binder interface which would create a new Configuration. Consequently
            // we have to always create a new Configuration here.
            final Configuration procConfig = proc.prepareConfigurationForLaunchingActivity();
            final MergedConfiguration mergedConfiguration = new MergedConfiguration(
                    procConfig, r.getMergedOverrideConfiguration());
            r.setLastReportedConfiguration(mergedConfiguration);

            logIfTransactionTooLarge(r.intent, r.getSavedState());

            final TaskFragment organizedTaskFragment = r.getOrganizedTaskFragment();
            if (organizedTaskFragment != null) {
                // Sending TaskFragmentInfo to client to ensure the info is updated before
                // the activity creation.
                mService.mTaskFragmentOrganizerController.dispatchPendingInfoChangedEvent(
                        organizedTaskFragment);
            }

            // Create activity launch transaction.
            final ClientTransaction clientTransaction = ClientTransaction.obtain(
                    proc.getThread(), r.token);

            final boolean isTransitionForward = r.isTransitionForward();
            final IBinder fragmentToken = r.getTaskFragment().getFragmentToken();

            final int deviceId = getDeviceIdForDisplayId(r.getDisplayId());
            clientTransaction.addCallback(LaunchActivityItem.obtain(new Intent(r.intent),
                    System.identityHashCode(r), r.info,
                    // TODO: Have this take the merged configuration instead of separate global
                    // and override configs.
                    mergedConfiguration.getGlobalConfiguration(),
                    mergedConfiguration.getOverrideConfiguration(), deviceId,
                    r.getFilteredReferrer(r.launchedFromPackage), task.voiceInteractor,
                    proc.getReportedProcState(), r.getSavedState(), r.getPersistentSavedState(),
                    results, newIntents, r.takeOptions(), isTransitionForward,
                    proc.createProfilerInfoIfNeeded(), r.assistToken, activityClientController,
                    r.shareableActivityToken, r.getLaunchedFromBubble(), fragmentToken));

            // Set desired final state.
            final ActivityLifecycleItem lifecycleItem;
            if (andResume) {
                lifecycleItem = ResumeActivityItem.obtain(isTransitionForward,
                        r.shouldSendCompatFakeFocus());
            } else {
                lifecycleItem = PauseActivityItem.obtain();
            }
            clientTransaction.setLifecycleStateRequest(lifecycleItem);

            // Schedule transaction.
            mService.getLifecycleManager().scheduleTransaction(clientTransaction);

            if (procConfig.seq > mRootWindowContainer.getConfiguration().seq) {
                // If the seq is increased, there should be something changed (e.g. registered
                // activity configuration).
                proc.setLastReportedConfiguration(procConfig);
            }
            if ((proc.mInfo.privateFlags & ApplicationInfo.PRIVATE_FLAG_CANT_SAVE_STATE) != 0
                    && mService.mHasHeavyWeightFeature) {
                // This may be a heavy-weight process! Note that the package manager will ensure
                // that only activity can run in the main process of the .apk, which is the only
                // thing that will be considered heavy-weight.
                if (proc.mName.equals(proc.mInfo.packageName)) {
                    if (mService.mHeavyWeightProcess != null
                            && mService.mHeavyWeightProcess != proc) {
                        Slog.w(TAG, "Starting new heavy weight process " + proc
                                + " when already running "
                                + mService.mHeavyWeightProcess);
                    }
                    mService.setHeavyWeightProcess(r);
                }
            }

        } catch (RemoteException e) {
            if (r.launchFailed) {
                // This is the second time we failed -- finish activity and give up.
                Slog.e(TAG, "Second failure launching "
                        + r.intent.getComponent().flattenToShortString() + ", giving up", e);
                proc.appDied("2nd-crash");
                r.finishIfPossible("2nd-crash", false /* oomAdj */);
                return false;
            }

            // This is the first time we failed -- restart process and
            // retry.
            r.launchFailed = true;
            r.detachFromProcess();
            throw e;
        }
    } finally {
        endDeferResume();
        proc.resumeConfigurationDispatch();
    }

    r.launchFailed = false;

    // TODO(lifecycler): Resume or pause requests are done as part of launch transaction,
    // so updating the state should be done accordingly.
    if (andResume && readyToResume()) {
        // As part of the process of launching, ActivityThread also performs
        // a resume.
        rootTask.minimalResumeActivityLocked(r);
    } else {
        // This activity is not starting in the resumed state... which should look like we asked
        // it to pause+stop (but remain visible), and it has done so and reported back the
        // current icicle and other state.
        ProtoLog.v(WM_DEBUG_STATES, "Moving to PAUSED: %s "
                + "(starting in paused state)", r);
        r.setState(PAUSED, "realStartActivityLocked");
        mRootWindowContainer.executeAppTransitionForAllDisplay();
    }
    // Perform OOM scoring after the activity state is set, so the process can be updated with
    // the latest state.
    proc.onStartActivity(mService.mTopProcessState, r.info);

    // Launch the new version setup screen if needed.  We do this -after-
    // launching the initial activity (that is, home), so that it can have
    // a chance to initialize itself while in the background, making the
    // switch back to it faster and look better.
    if (mRootWindowContainer.isTopDisplayFocusedRootTask(rootTask)) {
        mService.getActivityStartController().startSetupActivity();
    }

    // Update any services we are bound to that might care about whether
    // their client may have activities.
    if (r.app != null) {
        r.app.updateServiceConnectionActivities();
    }

    return true;
}
```

</details>

执行要点（最关键的一步之一）：
- 构造 `ClientTransaction`，添加 `LaunchActivityItem`（携带 intent/info/config/state/results/options 等）  
- 设置最终 lifecycle：通常是 `ResumeActivityItem`（andResume=true）  
- `scheduleTransaction(...)` 发往 app 进程的 `IApplicationThread`

---

## 7. client：收 transaction 并执行（Activity 实例化 + onCreate/onStart/onResume）

### T12：IApplicationThread.scheduleTransaction → 主线程 EXECUTE_TRANSACTION

- binder 入口：[ActivityThread.ApplicationThread.scheduleTransaction](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L1972-L1974)  
- 主线程执行点：[ActivityThread.H(EXECUTE_TRANSACTION)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L2468-L2478)

<details><summary>源码：ActivityThread.ApplicationThread.scheduleTransaction(ClientTransaction)</summary>

```java
@Override
public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
    ActivityThread.this.scheduleTransaction(transaction);
}
```

</details>

<details><summary>源码：ClientTransactionHandler.scheduleTransaction(ClientTransaction)</summary>

```java
/** Prepare and schedule transaction for execution. */
void scheduleTransaction(ClientTransaction transaction) {
    transaction.preExecute(this);
    sendMessage(ActivityThread.H.EXECUTE_TRANSACTION, transaction);
}
```

</details>

<details><summary>源码：ActivityThread.H.handleMessage(EXECUTE_TRANSACTION)</summary>

```java
case EXECUTE_TRANSACTION: // startActivity 20
    final ClientTransaction transaction = (ClientTransaction) msg.obj;
    mTransactionExecutor.execute(transaction);
    if (isSystem()) {
        // Client transactions inside system process are recycled on the client side
        // instead of ClientLifecycleManager to avoid being cleared before this
        // message is handled.
        transaction.recycle();
    }
    // TODO(lifecycler): Recycle locally scheduled transactions.
    break;
```

</details>

### T13：TransactionExecutor：先 callbacks，再 final lifecycle

- [TransactionExecutor.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java#L70-L101)

<details><summary>源码：TransactionExecutor.execute(ClientTransaction)</summary>

```java
public void execute(ClientTransaction transaction) {
    if (DEBUG_RESOLVER) Slog.d(TAG, tId(transaction) + "Start resolving transaction");

    final IBinder token = transaction.getActivityToken();
    if (token != null) {
        final Map<IBinder, ClientTransactionItem> activitiesToBeDestroyed =
                mTransactionHandler.getActivitiesToBeDestroyed();
        final ClientTransactionItem destroyItem = activitiesToBeDestroyed.get(token);
        if (destroyItem != null) {
            if (transaction.getLifecycleStateRequest() == destroyItem) {
                // It is going to execute the transaction that will destroy activity with the
                // token, so the corresponding to-be-destroyed record can be removed.
                activitiesToBeDestroyed.remove(token);
            }
            if (mTransactionHandler.getActivityClient(token) == null) {
                // The activity has not been created but has been requested to destroy, so all
                // transactions for the token are just like being cancelled.
                Slog.w(TAG, tId(transaction) + "Skip pre-destroyed transaction:\n"
                        + transactionToString(transaction, mTransactionHandler));
                return;
            }
        }
    }

    if (DEBUG_RESOLVER) Slog.d(TAG, transactionToString(transaction, mTransactionHandler));

    executeCallbacks(transaction);

    executeLifecycleState(transaction);
    mPendingActions.clear();
    if (DEBUG_RESOLVER) Slog.d(TAG, tId(transaction) + "End resolving transaction");
}
```

</details>

顺序：
1. `executeCallbacks(transaction)`：先跑 `LaunchActivityItem`  
2. `executeLifecycleState(transaction)`：再跑 `ResumeActivityItem` 等最终状态

### T14：LaunchActivityItem → ActivityThread.handleLaunchActivity → performLaunchActivity（onCreate）

- [LaunchActivityItem.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/LaunchActivityItem.java#L94-L106)  
- [ActivityThread.handleLaunchActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3938-L3986)  
- [ActivityThread.performLaunchActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3695-L3832)

<details><summary>源码：LaunchActivityItem.execute(...)</summary>

```java
@Override
public void execute(ClientTransactionHandler client, IBinder token,
        PendingTransactionActions pendingActions) {
    Trace.traceBegin(TRACE_TAG_ACTIVITY_MANAGER, "activityStart");
    ActivityClientRecord r = new ActivityClientRecord(token, mIntent, mIdent, mInfo,
            mOverrideConfig, mReferrer, mVoiceInteractor, mState, mPersistentState,
            mPendingResults, mPendingNewIntents, mActivityOptions, mIsForward, mProfilerInfo,
            client, mAssistToken, mShareableActivityToken, mLaunchedFromBubble,
            mTaskFragmentToken);
    // 23. 调用 frameworks\base\core\java\android\app\ActivityThread.java 中的 handleLaunchActivity 方法
    client.handleLaunchActivity(r, pendingActions, mDeviceId, null /* customIntent */);
    Trace.traceEnd(TRACE_TAG_ACTIVITY_MANAGER);
}
```

</details>

<details><summary>源码：ActivityThread.handleLaunchActivity(...)</summary>

```java
@Override
public Activity handleLaunchActivity(ActivityClientRecord r,
        PendingTransactionActions pendingActions, int deviceId, Intent customIntent) {
    // If we are getting ready to gc after going to the background, well
    // we are back active so skip it.
    unscheduleGcIdler();
    mSomeActivitiesChanged = true;

    if (r.profilerInfo != null) {
        mProfiler.setProfiler(r.profilerInfo);
        mProfiler.startProfiling();
    }

    // Make sure we are running with the most recent config and resource paths.
    applyPendingApplicationInfoChanges(r.activityInfo.packageName);
    mConfigurationController.handleConfigurationChanged(null, null);
    updateDeviceIdForNonUIContexts(deviceId);

    if (localLOGV) Slog.v(
        TAG, "Handling launch of " + r);

    // Initialize before creating the activity
    if (ThreadedRenderer.sRendererEnabled
            && (r.activityInfo.flags & ActivityInfo.FLAG_HARDWARE_ACCELERATED) != 0) {
        HardwareRenderer.preload();
    }
    // 确保 WindowManagerGlobal 已初始化
    WindowManagerGlobal.initialize();

    // Hint the GraphicsEnvironment that an activity is launching on the process.
    GraphicsEnvironment.hintActivityLaunch();
    // 24. 
    final Activity a = performLaunchActivity(r, customIntent);

    if (a != null) {
        r.createdConfig = new Configuration(mConfigurationController.getConfiguration());
        reportSizeConfigurations(r);
        if (!r.activity.mFinished && pendingActions != null) {
            pendingActions.setOldState(r.state);
            pendingActions.setRestoreInstanceState(true);
            pendingActions.setCallOnPostCreate(true);
        }
    } else {
        // If there was an error, for any reason, tell the activity manager to stop us.
        ActivityClient.getInstance().finishActivity(r.token, Activity.RESULT_CANCELED,
                null /* resultData */, Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
    }

    return a;
}
```

</details>

<details><summary>源码：ActivityThread.performLaunchActivity(...)</summary>

```java
/**  Core implementation of activity launch. */
private Activity performLaunchActivity(ActivityClientRecord r, Intent customIntent) {
    ActivityInfo aInfo = r.activityInfo;
    if (r.packageInfo == null) {
        // 获取应用包信息
        r.packageInfo = getPackageInfo(aInfo.applicationInfo, mCompatibilityInfo,
                Context.CONTEXT_INCLUDE_CODE);
    }

    ComponentName component = r.intent.getComponent();
    if (component == null) {
        component = r.intent.resolveActivity(
            mInitialApplication.getPackageManager());
        r.intent.setComponent(component);
    }

    if (r.activityInfo.targetActivity != null) {
        component = new ComponentName(r.activityInfo.packageName,
                r.activityInfo.targetActivity);
    }
    // 创建一个ContextImpl对象
    ContextImpl appContext = createBaseContextForActivity(r);
    Activity activity = null;
    try {
        java.lang.ClassLoader cl = appContext.getClassLoader();
        activity = mInstrumentation.newActivity(
                cl, component.getClassName(), r.intent);
        StrictMode.incrementExpectedActivityCount(activity.getClass());
        r.intent.setExtrasClassLoader(cl);
        r.intent.prepareToEnterProcess(isProtectedComponent(r.activityInfo),
                appContext.getAttributionSource());
        if (r.state != null) {
            r.state.setClassLoader(cl);
        }
    } catch (Exception e) {
        if (!mInstrumentation.onException(activity, e)) {
            throw new RuntimeException(
                "Unable to instantiate activity " + component
                + ": " + e.toString(), e);
        }
    }

    try {
        // 创建一个 Application对象
        Application app = r.packageInfo.makeApplicationInner(false, mInstrumentation);

        if (localLOGV) Slog.v(TAG, "Performing launch of " + r);
        if (localLOGV) Slog.v(
                TAG, r + ": app=" + app
                + ", appName=" + app.getPackageName()
                + ", pkg=" + r.packageInfo.getPackageName()
                + ", comp=" + r.intent.getComponent().toShortString()
                + ", dir=" + r.packageInfo.getAppDir());

        // updatePendingActivityConfiguration() reads from mActivities to update
        // ActivityClientRecord which runs in a different thread. Protect modifications to
        // mActivities to avoid race.
        synchronized (mResourcesManager) {
            mActivities.put(r.token, r);
        }

        if (activity != null) {
            CharSequence title = r.activityInfo.loadLabel(appContext.getPackageManager());
            Configuration config =
                    new Configuration(mConfigurationController.getCompatConfiguration());
            if (r.overrideConfig != null) {
                config.updateFrom(r.overrideConfig);
            }
            if (DEBUG_CONFIGURATION) Slog.v(TAG, "Launching activity "
                    + r.activityInfo.name + " with config " + config);
            Window window = null;
            if (r.mPendingRemoveWindow != null && r.mPreserveWindow) {
                window = r.mPendingRemoveWindow;
                r.mPendingRemoveWindow = null;
                r.mPendingRemoveWindowManager = null;
            }

            // Activity resources must be initialized with the same loaders as the
            // application context.
            appContext.getResources().addLoaders(
                    app.getResources().getLoaders().toArray(new ResourcesLoader[0]));

            appContext.setOuterContext(activity);
            // 调用 frameworks/base/core/java/android/app/Activity#attach() 方法
            activity.attach(appContext, this, getInstrumentation(), r.token,
                    r.ident, app, r.intent, r.activityInfo, title, r.parent,
                    r.embeddedID, r.lastNonConfigurationInstances, config,
                    r.referrer, r.voiceInteractor, window, r.activityConfigCallback,
                    r.assistToken, r.shareableActivityToken);

            if (customIntent != null) {
                activity.mIntent = customIntent;
            }
            r.lastNonConfigurationInstances = null;
            checkAndBlockForNetworkAccess();
            activity.mStartedActivity = false;
            int theme = r.activityInfo.getThemeResource();
            if (theme != 0) {
                activity.setTheme(theme);
            }

            if (r.mActivityOptions != null) {
                activity.mPendingOptions = r.mActivityOptions;
                r.mActivityOptions = null;
            }
            activity.mLaunchedFromBubble = r.mLaunchedFromBubble;
            activity.mCalled = false;
            // Assigning the activity to the record before calling onCreate() allows
            // ActivityThread#getActivity() lookup for the callbacks triggered from
            // ActivityLifecycleCallbacks#onActivityCreated() or
            // ActivityLifecycleCallback#onActivityPostCreated().
            r.activity = activity;
            if (r.isPersistable()) {
                mInstrumentation.callActivityOnCreate(activity, r.state, r.persistentState);
            } else {
                mInstrumentation.callActivityOnCreate(activity, r.state);
            }
            if (!activity.mCalled) {
                throw new SuperNotCalledException(
                    "Activity " + r.intent.getComponent().toShortString() +
                    " did not call through to super.onCreate()");
            }
            r.mLastReportedWindowingMode = config.windowConfiguration.getWindowingMode();
        }
        r.setState(ON_CREATE);

    } catch (SuperNotCalledException e) {
        throw e;

    } catch (Exception e) {
        if (!mInstrumentation.onException(activity, e)) {
            throw new RuntimeException(
                "Unable to start activity " + component
                + ": " + e.toString(), e);
        }
    }

    return activity;
}
```

</details>

`performLaunchActivity` 内部关键时序：
1. `Instrumentation.newActivity(...)` 实例化 Activity  
2. `LoadedApk.makeApplicationInner(...)`（确保 Application 存在）  
3. `Activity.attach(...)`（注入 token/ATMS/WMS 交互对象、Context、Window 等）  
4. `Instrumentation.callActivityOnCreate(...)` → `Activity.onCreate(...)`  

### T15：ON_START：ActivityThread.handleStartActivity（onStart/onPostCreate/restore）

- [TransactionExecutor.performLifecycleSequence: ON_START](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java#L224-L227)  
- [ActivityThread.handleStartActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3834-L3892)

<details><summary>源码：ActivityThread.handleStartActivity(...)</summary>

```java
@Override
public void handleStartActivity(ActivityClientRecord r,
        PendingTransactionActions pendingActions, ActivityOptions activityOptions) {
    final Activity activity = r.activity;
    if (!r.stopped) {
        throw new IllegalStateException("Can't start activity that is not stopped.");
    }
    if (r.activity.mFinished) {
        // TODO(lifecycler): How can this happen?
        return;
    }

    unscheduleGcIdler();
    if (activityOptions != null) {
        activity.mPendingOptions = activityOptions;
    }

    // Start
    activity.performStart("handleStartActivity");
    r.setState(ON_START);

    if (pendingActions == null) {
        // No more work to do.
        return;
    }

    // Restore instance state
    if (pendingActions.shouldRestoreInstanceState()) {
        if (r.isPersistable()) {
            if (r.state != null || r.persistentState != null) {
                mInstrumentation.callActivityOnRestoreInstanceState(activity, r.state,
                        r.persistentState);
            }
        } else if (r.state != null) {
            mInstrumentation.callActivityOnRestoreInstanceState(activity, r.state);
        }
    }

    // Call postOnCreate()
    if (pendingActions.shouldCallOnPostCreate()) {
        activity.mCalled = false;
        Trace.traceBegin(Trace.TRACE_TAG_WINDOW_MANAGER, "onPostCreate");
        if (r.isPersistable()) {
            mInstrumentation.callActivityOnPostCreate(activity, r.state,
                    r.persistentState);
        } else {
            mInstrumentation.callActivityOnPostCreate(activity, r.state);
        }
        Trace.traceEnd(Trace.TRACE_TAG_WINDOW_MANAGER);
        if (!activity.mCalled) {
            throw new SuperNotCalledException(
                    "Activity " + r.intent.getComponent().toShortString()
                            + " did not call through to super.onPostCreate()");
        }
    }

    updateVisibility(r, true /* show */);
    mSomeActivitiesChanged = true;
}
```

</details>

执行要点：
- `activity.performStart(...)` → `Activity.onStart()`  
- 若 `PendingTransactionActions` 指定：`onRestoreInstanceState`、`onPostCreate`

### T16：ON_RESUME：ActivityThread.handleResumeActivity（onResume + addView/makeVisible）

- [ResumeActivityItem.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/ResumeActivityItem.java#L53-L60)  
- [ActivityThread.handleResumeActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L5037-L5160)

<details><summary>源码：ActivityThread.handleResumeActivity(...)</summary>

```java
@Override
public void handleResumeActivity(ActivityClientRecord r, boolean finalStateRequest,
        boolean isForward, boolean shouldSendCompatFakeFocus, String reason) {
    // If we are getting ready to gc after going to the background, well
    // we are back active so skip it.
    unscheduleGcIdler();
    mSomeActivitiesChanged = true;

    // TODO Push resumeArgs into the activity for consideration
    // skip below steps for double-resume and r.mFinish = true case.
    if (!performResumeActivity(r, finalStateRequest, reason)) {
        return;
    }
    if (mActivitiesToBeDestroyed.containsKey(r.token)) {
        // Although the activity is resumed, it is going to be destroyed. So the following
        // UI operations are unnecessary and also prevents exception because its token may
        // be gone that window manager cannot recognize it. All necessary cleanup actions
        // performed below will be done while handling destruction.
        return;
    }

    final Activity a = r.activity;

    if (localLOGV) {
        Slog.v(TAG, "Resume " + r + " started activity: " + a.mStartedActivity
                + ", hideForNow: " + r.hideForNow + ", finished: " + a.mFinished);
    }

    final int forwardBit = isForward
            ? WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION : 0;

    // If the window hasn't yet been added to the window manager,
    // and this guy didn't finish itself or start another activity,
    // then go ahead and add the window.
    boolean willBeVisible = !a.mStartedActivity;
    if (!willBeVisible) {
        willBeVisible = ActivityClient.getInstance().willActivityBeVisible(
                a.getActivityToken());
    }
    if (r.window == null && !a.mFinished && willBeVisible) {
        r.window = r.activity.getWindow();
        View decor = r.window.getDecorView();
        decor.setVisibility(View.INVISIBLE);
        ViewManager wm = a.getWindowManager();
        WindowManager.LayoutParams l = r.window.getAttributes();
        a.mDecor = decor;
        l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        l.softInputMode |= forwardBit;
        if (r.mPreserveWindow) {
            a.mWindowAdded = true;
            r.mPreserveWindow = false;
            // Normally the ViewRoot sets up callbacks with the Activity
            // in addView->ViewRootImpl#setView. If we are instead reusing
            // the decor view we have to notify the view root that the
            // callbacks may have changed.
            ViewRootImpl impl = decor.getViewRootImpl();
            if (impl != null) {
                impl.notifyChildRebuilt();
            }
        }
        if (a.mVisibleFromClient) {
            if (!a.mWindowAdded) {
                a.mWindowAdded = true;
                wm.addView(decor, l);
            } else {
                // The activity will get a callback for this {@link LayoutParams} change
                // earlier. However, at that time the decor will not be set (this is set
                // in this method), so no action will be taken. This call ensures the
                // callback occurs with the decor set.
                a.onWindowAttributesChanged(l);
            }
        }

        // If the window has already been added, but during resume
        // we started another activity, then don't yet make the
        // window visible.
    } else if (!willBeVisible) {
        if (localLOGV) Slog.v(TAG, "Launch " + r + " mStartedActivity set");
        r.hideForNow = true;
    }

    // Get rid of anything left hanging around.
    cleanUpPendingRemoveWindows(r, false /* force */);

    // The window is now visible if it has been added, we are not
    // simply finishing, and we are not starting another activity.
    if (!r.activity.mFinished && willBeVisible && r.activity.mDecor != null && !r.hideForNow) {
        if (localLOGV) Slog.v(TAG, "Resuming " + r + " with isForward=" + isForward);
        ViewRootImpl impl = r.window.getDecorView().getViewRootImpl();
        WindowManager.LayoutParams l = impl != null
                ? impl.mWindowAttributes : r.window.getAttributes();
        if ((l.softInputMode
                & WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION)
                != forwardBit) {
            l.softInputMode = (l.softInputMode
                    & (~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION))
                    | forwardBit;
            if (r.activity.mVisibleFromClient) {
                ViewManager wm = a.getWindowManager();
                View decor = r.window.getDecorView();
                wm.updateViewLayout(decor, l);
            }
        }

        r.activity.mVisibleFromServer = true;
        mNumVisibleActivities++;
        if (r.activity.mVisibleFromClient) {
            r.activity.makeVisible();
        }

        if (shouldSendCompatFakeFocus) {
            // Attaching to a window is asynchronous with the activity being resumed,
            // so it's possible we will need to send a fake focus event after attaching
            if (impl != null) {
                impl.dispatchCompatFakeFocus();
            } else {
                r.window.getDecorView().fakeFocusAfterAttachingToWindow();
            }
        }
    }

    mNewActivities.add(r);
    if (localLOGV) Slog.v(TAG, "Scheduling idle handler for " + r);
    Looper.myQueue().addIdleHandler(new Idler());
}
```

</details>

执行要点：
- `performResumeActivity(...)` → `Activity.performResume(...)` → `Activity.onResume()`  
  - `r.setState(ON_RESUME)` 见 [ActivityThread.performResumeActivity 片段](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L5003-L5008)
- Window 相关：`wm.addView(decor, l)` / `activity.makeVisible()`（界面真正挂上）

---

## 8. client → server 回报链路（启动“完成”与收尾调度的关键）

> 这部分非常适合用来定位“卡住/黑屏/切换慢/状态不同步”的问题。

### T17：onResume 完成回报：activityResumed

client 侧触发：
- `ResumeActivityItem.postExecute()` → `ActivityClient.activityResumed(...)`  
  - [ResumeActivityItem.postExecute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/ResumeActivityItem.java#L62-L67)  
  - [ActivityClient.activityResumed](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityClient.java#L53-L60)

<details><summary>源码：ResumeActivityItem.postExecute(...)</summary>

```java
@Override
public void postExecute(ClientTransactionHandler client, IBinder token,
        PendingTransactionActions pendingActions) {
    // TODO(lifecycler): Use interface callback instead of actual implementation.
    ActivityClient.getInstance().activityResumed(token, client.isHandleSplashScreenExit(token));
}
```

</details>

<details><summary>源码：ActivityClient.activityResumed(...)</summary>

```java
/** Reports {@link Activity#onResume()} is done. */
public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
    try {
        getActivityClientController().activityResumed(token, handleSplashScreenExit);
    } catch (RemoteException e) {
        e.rethrowFromSystemServer();
    }
}
```

</details>

<details><summary>源码：ActivityClientController.activityResumed(...)</summary>

```java
@Override
public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
    final long origId = Binder.clearCallingIdentity();
    synchronized (mGlobalLock) {
        ActivityRecord.activityResumedLocked(token, handleSplashScreenExit);
    }
    Binder.restoreCallingIdentity(origId);
}
```

</details>

<details><summary>源码：ActivityRecord.activityResumedLocked(...)</summary>

```java
static void activityResumedLocked(IBinder token, boolean handleSplashScreenExit) {
    final ActivityRecord r = ActivityRecord.forTokenLocked(token);
    ProtoLog.i(WM_DEBUG_STATES, "Resumed activity; dropping state of: %s", r);
    if (r == null) {
        // If an app reports resumed after a long delay, the record on server side might have
        // been removed (e.g. destroy timeout), so the token could be null.
        return;
    }
    r.setCustomizeSplashScreenExitAnimation(handleSplashScreenExit);
    r.setSavedState(null /* savedState */);

    r.mDisplayContent.handleActivitySizeCompatModeIfNeeded(r);
    r.mDisplayContent.mUnknownAppVisibilityController.notifyAppResumedFinished(r);
}
```

</details>

server 落点：
- [ActivityClientController.activityResumed](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L182-L189)  
  → [ActivityRecord.activityResumedLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java#L6257-L6270)

### T18：主线程空闲回报：activityIdle（常被当作“启动完成/可进行后续调度”的标志）

client 侧触发：
- `ActivityThread.handleResumeActivity` 末尾把 record 加入 `mNewActivities`，IdleHandler 统一上报：  
  - [ActivityThread.Idler.queueIdle](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L2512-L2536)  
  - `ActivityClient.activityIdle(token, config, stopProfiling)`：见 [ActivityClient.activityIdle](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityClient.java#L44-L51)

<details><summary>源码：ActivityThread.Idler.queueIdle()</summary>

```java
@Override
public final boolean queueIdle() {
    boolean stopProfiling = false;
    if (mBoundApplication != null && mProfiler.profileFd != null
            && mProfiler.autoStopProfiler) {
        stopProfiling = true;
    }
    final ActivityClient ac = ActivityClient.getInstance();
    while (mNewActivities.size() > 0) {
        final ActivityClientRecord a = mNewActivities.remove(0);
        if (localLOGV) {
            Slog.v(TAG, "Reporting idle of " + a + " finished="
                    + (a.activity != null && a.activity.mFinished));
        }
        if (a.activity != null && !a.activity.mFinished) {
            ac.activityIdle(a.token, a.createdConfig, stopProfiling);
            a.createdConfig = null;
        }
    }
    if (stopProfiling) {
        mProfiler.stopProfiling();
    }
    return false;
}
```

</details>

<details><summary>源码：ActivityClient.activityIdle(...)</summary>

```java
/** Reports the main thread is idle after the activity is resumed. */
public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
    try {
        getActivityClientController().activityIdle(token, config, stopProfiling);
    } catch (RemoteException e) {
        e.rethrowFromSystemServer();
    }
}
```

</details>

<details><summary>源码：ActivityClientController.activityIdle(...)</summary>

```java
@Override
public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
    final long origId = Binder.clearCallingIdentity();
    try {
        synchronized (mGlobalLock) {
            Trace.traceBegin(TRACE_TAG_WINDOW_MANAGER, "activityIdle");
            final ActivityRecord r = ActivityRecord.forTokenLocked(token);
            if (r == null) {
                return;
            }
            mTaskSupervisor.activityIdleInternal(r, false /* fromTimeout */,
                    false /* processPausingActivities */, config);
            if (stopProfiling && r.hasProcess()) {
                r.app.clearProfilerIfNeeded();
            }
        }
    } finally {
        Trace.traceEnd(TRACE_TAG_WINDOW_MANAGER);
        Binder.restoreCallingIdentity(origId);
    }
}
```

</details>

<details><summary>源码：ActivityTaskSupervisor.activityIdleInternal(...)</summary>

```java
void activityIdleInternal(ActivityRecord r, boolean fromTimeout,
        boolean processPausingActivities, Configuration config) {
    if (DEBUG_ALL) Slog.v(TAG, "Activity idle: " + r);

    if (r != null) {
        if (DEBUG_IDLE) Slog.d(TAG_IDLE, "activityIdleInternal: Callers="
                + Debug.getCallers(4));
        mHandler.removeMessages(IDLE_TIMEOUT_MSG, r);
        r.finishLaunchTickingLocked();
        if (fromTimeout) {
            reportActivityLaunched(fromTimeout, r, INVALID_DELAY, -1 /* launchState */);
        }

        // This is a hack to semi-deal with a race condition
        // in the client where it can be constructed with a
        // newer configuration from when we asked it to launch.
        // We'll update with whatever configuration it now says
        // it used to launch.
        if (config != null) {
            r.setLastReportedGlobalConfiguration(config);
        }

        // We are now idle.  If someone is waiting for a thumbnail from
        // us, we can now deliver.
        r.idle = true;

        // Check if able to finish booting when device is booting and all resumed activities
        // are idle.
        if ((mService.isBooting() && mRootWindowContainer.allResumedActivitiesIdle())
                || fromTimeout) {
            checkFinishBootingLocked();
        }

        // When activity is idle, we consider the relaunch must be successful, so let's clear
        // the flag.
        r.mRelaunchReason = RELAUNCH_REASON_NONE;
    }

    if (mRootWindowContainer.allResumedActivitiesIdle()) {
        if (r != null) {
            mService.scheduleAppGcsLocked();
            mRecentTasks.onActivityIdle(r);
        }

        if (mLaunchingActivityWakeLock.isHeld()) {
            mHandler.removeMessages(LAUNCH_TIMEOUT_MSG);
            if (VALIDATE_WAKE_LOCK_CALLER && Binder.getCallingUid() != SYSTEM_UID) {
                throw new IllegalStateException("Calling must be system uid");
            }
            mLaunchingActivityWakeLock.release();
        }
        mRootWindowContainer.ensureActivitiesVisible(null, 0, !PRESERVE_WINDOWS);
    }

    // Atomically retrieve all of the other things to do.
    processStoppingAndFinishingActivities(r, processPausingActivities, "idle");

    if (DEBUG_IDLE) {
        Slogf.i(TAG, "activityIdleInternal(): r=%s, mStartingUsers=%s", r, mStartingUsers);
    }

    if (!mStartingUsers.isEmpty()) {
        final ArrayList<UserState> startingUsers = new ArrayList<>(mStartingUsers);
        mStartingUsers.clear();
        // Complete user switch.
        for (int i = 0; i < startingUsers.size(); i++) {
            UserState userState = startingUsers.get(i);
            Slogf.i(TAG, "finishing switch of user %d", userState.mHandle.getIdentifier());
            mService.mAmInternal.finishUserSwitch(userState);
        }
    }

    mService.mH.post(() -> mService.mAmInternal.trimApplications());
}
```

</details>

server 落点：
- [ActivityClientController.activityIdle](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L160-L180)  
  → [ActivityTaskSupervisor.activityIdleInternal](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java#L1402-L1475)

`activityIdleInternal` 是一个“收尾枢纽”，典型动作包括：
- 结束 launch ticking、更新 last reported config、标记 record idle  
- boot 阶段可能 `checkFinishBootingLocked()`  
- 释放 launching wakelock、`ensureActivitiesVisible(...)`  
- 处理 stopping/finishing 队列（推进 stop/destroy）  
- `trimApplications()`

---

## 9.（补充）常见回报：pause/stop/destroy 的 client→server

这些不是“启动一条链路必经”，但在任务切换/返回/finish 时非常关键：

- onPause 完成回报：`PauseActivityItem.postExecute → ActivityClient.activityPaused`  
  - [PauseActivityItem.postExecute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/PauseActivityItem.java#L58-L66)  
  - server：[ActivityClientController.activityPaused](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L209-L221)  
  - 服务器侧推进 pause 完成：见 [ActivityRecord.activityPaused](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java#L6341-L6359)

- onStop 完成回报：`PendingTransactionActions.StopInfo.run → ActivityClient.activityStopped`  
  - [PendingTransactionActions.StopInfo.run](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/PendingTransactionActions.java#L129-L157)  
  - server：[ActivityClientController.activityStopped](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L223-L271)

- onDestroy 完成回报：`ActivityThread.handleDestroyActivity(finishing) → ActivityClient.activityDestroyed`  
  - client：[ActivityThread.handleDestroyActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L5693-L5761)（finishing 分支回报在 L5756-L5759）  
  - server：[ActivityClientController.activityDestroyed](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L273-L289)

