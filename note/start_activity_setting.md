# 启动 Settings 全链路：从点击图标到首帧绘制（ATMS/WMS/SurfaceFlinger 对齐源码）

本文件用于把一次“点击桌面设置图标 → Settings 首页显示”的完整链路固化下来，包含：

- 日志线索（`note/log/start_setting.log`）
- Framework 内部函数级调用链（从 `startActivity` 到 `ActivityThread.handleLaunchActivity`）
- WMS 侧关键机制（Starting Window / Traversal / SurfacePlacement / Input）
- SurfaceFlinger 侧 transaction/buffer 处理（SurfaceControl → SurfaceComposerClient → SurfaceFlinger）
- 每个关键步骤对应的源码位置与“完整函数实现片段”（以关键函数为粒度，而非整文件全文）

---

## 1. 关键日志（入口与分界点）

日志文件：`note/log/start_setting.log`

### 1.1 ATMS 打印 START（启动请求被系统接收并处理）

```text
05-10 11:08:23.231   490   505 I ActivityTaskManager: START u0 {act=android.intent.action.MAIN cat=[android.intent.category.LAUNCHER] flg=0x10200000 cmp=com.android.settings/.Settings bnds=[288,1145][576,1565]} with LAUNCH_MULTIPLE from uid 10115 (BAL_ALLOW_ALLOWLISTED_COMPONENT) result code=0
```

要点：

- `cmp=com.android.settings/.Settings`：桌面入口显式组件
- `with LAUNCH_MULTIPLE`：来自 `ActivityInfo.launchMode`
- `(BAL_ALLOW_ALLOWLISTED_COMPONENT)`：BAL（后台拉起）评估结果，允许
- `result code=0`：`START_SUCCESS`

### 1.2 WM Shell 打印 Transition requested（启动过渡交给 Shell 播放）

```text
05-10 11:08:23.212   687   731 V WindowManagerShell: Transition requested: ... TransitionRequestInfo { type = 1, triggerTask = TaskInfo{... baseIntent=... cmp=com.android.settings/.Settings ... origActivity=...Settings realActivity=...SettingsHomepageActivity ...}, remoteTransition=... QuickstepLaunch }
```

要点：

- `type = 1`：OPEN transition
- `triggerTask = TaskInfo{...}`：来自 `Task.fillTaskInfo(...)`
- `origActivity` 与 `realActivity` 不同：说明入口是 alias/targetActivity 映射
- `remoteTransition ... QuickstepLaunch`：启动动画由 Launcher/Quickstep 接管

---

## 2. 总体调用链（从点击到 onCreate）

```text
Launcher 点击图标
  -> Instrumentation.execStartActivity (app 进程)
    -> IActivityTaskManager.startActivity (Binder)
      -> ActivityTaskManagerService.startActivityAsUser (system_server / ATMS)
        -> ActivityStartController.obtainStarter(...).execute()
          -> ActivityStarter.execute()
            -> ActivityStarter.executeRequest()  // 打印 "START u0 ... result code=..."
              -> ActivityStarter.startActivityUnchecked()
                -> TransitionController.createAndStartCollecting(TRANSIT_OPEN)
                -> ActivityStarter.startActivityInner() // 选 Task / 复用 Task / 设定 LaunchFlags
                  -> ActivityTaskSupervisor.startSpecificActivity()
                    -> realStartActivityLocked()
                      -> ClientTransaction + LaunchActivityItem + ResumeActivityItem
                        -> scheduleTransaction (到 app 进程)
                          -> TransactionExecutor.executeCallbacks()
                            -> LaunchActivityItem.execute()
                              -> ActivityThread.handleLaunchActivity()
                                -> performLaunchActivity()
                                  -> Activity.onCreate()（SettingsHomepageActivity.onCreate）
```

---

## 3. Framework 内部函数级调用链（源码对齐 + 关键函数完整实现）

### 3.1 App 进程：Instrumentation.execStartActivity（跨进程调用 ATMS）

源码：`frameworks/base/core/java/android/app/Instrumentation.java`

```java
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
        int result = ActivityTaskManager.getService().startActivity(whoThread,
                who.getOpPackageName(), who.getAttributionTag(), intent,
                intent.resolveTypeIfNeeded(who.getContentResolver()), token,
                target != null ? target.mEmbeddedID : null, requestCode, 0, null, options);
        notifyStartActivityResult(result, options);
        checkStartActivityResult(result, intent);
    } catch (RemoteException e) {
        throw new RuntimeException("Failure from system", e);
    }
    return null;
}
```

### 3.2 system_server/ATMS：ActivityTaskManagerService.startActivityAsUser（创建 starter 并执行）

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java`

```java
private int startActivityAsUser(IApplicationThread caller, String callingPackage,
        @Nullable String callingFeatureId, Intent intent, String resolvedType,
        IBinder resultTo, String resultWho, int requestCode, int startFlags,
        ProfilerInfo profilerInfo, Bundle bOptions, int userId, boolean validateIncomingUser) {

    final SafeActivityOptions opts = SafeActivityOptions.fromBundle(bOptions);

    assertPackageMatchesCallingUid(callingPackage);
    enforceNotIsolatedCaller("startActivityAsUser");

    userId = getActivityStartController().checkTargetUser(userId, validateIncomingUser,
            Binder.getCallingPid(), Binder.getCallingUid(), "startActivityAsUser");

    return getActivityStartController().obtainStarter(intent, "startActivityAsUser")
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

### 3.3 ActivityStartController.obtainStarter（starter 工厂/池）

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityStartController.java`

```java
ActivityStarter obtainStarter(Intent intent, String reason) {
    return mFactory.obtain().setIntent(intent).setReason(reason);
}
```

### 3.4 ActivityStarter.execute：打印 result code=... 的位置

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java`

```java
int execute() {
    try {
        onExecutionStarted();

        if (mRequest.intent != null && mRequest.intent.hasFileDescriptors()) {
            throw new IllegalArgumentException("File descriptors passed in Intent");
        }
        final LaunchingState launchingState;
        synchronized (mService.mGlobalLock) {
            final ActivityRecord caller = ActivityRecord.forTokenLocked(mRequest.resultTo);
            final int callingUid = mRequest.realCallingUid == Request.DEFAULT_REAL_CALLING_UID
                    ?  Binder.getCallingUid() : mRequest.realCallingUid;
            launchingState = mSupervisor.getActivityMetricsLogger().notifyActivityLaunching(
                    mRequest.intent, caller, callingUid);
        }

        if (mRequest.activityInfo == null) {
            mRequest.resolveActivity(mSupervisor);
        }

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
        synchronized (mService.mGlobalLock) {
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
                res = executeRequest(mRequest);
            } finally {
                mRequest.logMessage.append(" result code=").append(res);
                Slog.i(TAG, mRequest.logMessage.toString());
                mRequest.logMessage.setLength(0);
            }

            Binder.restoreCallingIdentity(origId);

            if (globalConfigWillChange) {
                mService.mAmInternal.enforceCallingPermission(
                        android.Manifest.permission.CHANGE_CONFIGURATION,
                        "updateConfiguration()");
                if (rootTask != null) {
                    rootTask.mConfigWillChange = false;
                }
                ProtoLog.v(WM_DEBUG_CONFIGURATION,
                            "Updating to new configuration after starting activity.");
                mService.updateConfigurationLocked(mRequest.globalConfig, null, false);
            }

            final ActivityOptions originalOptions = mRequest.activityOptions != null
                    ? mRequest.activityOptions.getOriginalOptions() : null;
            final ActivityRecord launchingRecord = mDoResume ? mLastStartActivityRecord : null;
            final boolean newActivityCreated = mStartActivity == launchingRecord;
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
        onExecutionComplete();
    }
}
```

### 3.5 ActivityStarter.executeRequest：拼接 “START u0 {...} with LAUNCH_* ...”

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java`

```java
private int executeRequest(Request request) {
    if (TextUtils.isEmpty(request.reason)) {
        throw new IllegalArgumentException("Need to specify a reason.");
    }
    mLastStartReason = request.reason;
    mLastStartActivityTimeMs = System.currentTimeMillis();
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

    ActivityRecord sourceRecord = null;
    ActivityRecord resultRecord = null;
    if (resultTo != null) {
        sourceRecord = ActivityRecord.isInAnyTask(resultTo);
        if (sourceRecord != null) {
            if (requestCode >= 0 && !sourceRecord.finishing) {
                resultRecord = sourceRecord;
            }
        }
    }

    final int launchFlags = intent.getFlags();
    if ((launchFlags & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0 && sourceRecord != null) {
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
            callingPackage = sourceRecord.launchedFromPackage;
            callingFeatureId = sourceRecord.launchedFromFeatureId;
        }
    }

    if (err == ActivityManager.START_SUCCESS && intent.getComponent() == null) {
        err = ActivityManager.START_INTENT_NOT_RESOLVED;
    }

    if (err == ActivityManager.START_SUCCESS && aInfo == null) {
        err = ActivityManager.START_CLASS_NOT_FOUND;
    }

    boolean abort;
    try {
        abort = !mSupervisor.checkStartAnyActivityPermission(intent, aInfo, resultWho,
                requestCode, callingPid, callingUid, callingPackage, callingFeatureId,
                request.ignoreTargetSecurity, inTask != null, callerApp, resultRecord,
                resultRecord == null ? null : resultRecord.getRootTask());
    } catch (SecurityException e) {
        throw e;
    }
    abort |= !mService.mIntentFirewall.checkStartActivity(intent, callingUid,
            callingPid, resolvedType, aInfo.applicationInfo);
    abort |= !mService.getPermissionPolicyInternal().checkStartActivity(intent, callingUid,
            callingPackage);

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

    if (abort) {
        if (resultRecord != null) {
            resultRecord.sendResult(INVALID_UID, resultWho, requestCode, RESULT_CANCELED,
                    null, null);
        }
        ActivityOptions.abort(checkedOptions);
        return START_ABORTED;
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

    WindowProcessController homeProcess = mService.mHomeProcess;
    boolean isHomeProcess = homeProcess != null
            && aInfo.applicationInfo.uid == homeProcess.mUid;
    if (balCode != BAL_BLOCK && !isHomeProcess) {
        mService.resumeAppSwitches();
    }

    mLastStartActivityResult = startActivityUnchecked(r, sourceRecord, voiceSession,
            request.voiceInteractor, startFlags, checkedOptions,
            inTask, inTaskFragment, balCode, intentGrants, realCallingUid);

    if (request.outActivity != null) {
        request.outActivity[0] = mLastStartActivityRecord;
    }

    return mLastStartActivityResult;
}
```

### 3.6 startActivityUnchecked：创建 transition 并进入 startActivityInner

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java`

```java
private int startActivityUnchecked(final ActivityRecord r, ActivityRecord sourceRecord,
        IVoiceInteractionSession voiceSession, IVoiceInteractor voiceInteractor,
        int startFlags, ActivityOptions options, Task inTask,
        TaskFragment inTaskFragment, @BalCode int balCode,
        NeededUriGrants intentGrants, int realCallingUid) {
    int result = START_CANCELED;
    final Task startedActivityRootTask;

    final TransitionController transitionController = r.mTransitionController;
    Transition newTransition = transitionController.isShellTransitionsEnabled()
            ? transitionController.createAndStartCollecting(TRANSIT_OPEN) : null;
    RemoteTransition remoteTransition = r.takeRemoteTransition();
    try {
        mService.deferWindowLayout();
        transitionController.collect(r);
        try {
            Trace.traceBegin(Trace.TRACE_TAG_WINDOW_MANAGER, "startActivityInner");
            result = startActivityInner(r, sourceRecord, voiceSession, voiceInteractor,
                    startFlags, options, inTask, inTaskFragment, balCode,
                    intentGrants, realCallingUid);
        } finally {
            Trace.traceEnd(Trace.TRACE_TAG_WINDOW_MANAGER);
            startedActivityRootTask = handleStartResult(r, options, result, newTransition,
                    remoteTransition);
        }
    } finally {
        mService.continueWindowLayout();
    }
    postStartActivityProcessing(r, result, startedActivityRootTask);

    return result;
}
```

### 3.7 进程是否已存在：ActivityTaskSupervisor.startSpecificActivity（Warm/Hot 分支）

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java`

```java
void startSpecificActivity(ActivityRecord r, boolean andResume, boolean checkConfig) {
    final WindowProcessController wpc =
            mService.getProcessController(r.processName, r.info.applicationInfo.uid);

    boolean knownToBeDead = false;
    if (wpc != null && wpc.hasThread()) {
        try {
            realStartActivityLocked(r, wpc, andResume, checkConfig);
            return;
        } catch (RemoteException e) {
            Slog.w(TAG, "Exception when starting activity "
                    + r.intent.getComponent().flattenToShortString(), e);
        }

        knownToBeDead = true;
        mService.mProcessNames.remove(wpc.mName, wpc.mUid);
        mService.mProcessMap.remove(wpc.getPid());
    } else if (r.intent.isSandboxActivity(mService.mContext)) {
        Slog.e(TAG, "Abort sandbox activity launching as no sandbox process to host it.");
        r.finishIfPossible("No sandbox process for the activity", false);
        r.launchFailed = true;
        r.detachFromProcess();
        return;
    }

    r.notifyUnknownVisibilityLaunchedForKeyguardTransition();

    final boolean isTop = andResume && r.isTopRunningActivity();
    mService.startProcessAsync(r, knownToBeDead, isTop,
            isTop ? HostingRecord.HOSTING_TYPE_TOP_ACTIVITY
                    : HostingRecord.HOSTING_TYPE_ACTIVITY);
}
```

### 3.8 事务派发到 App：LaunchActivityItem → TransactionExecutor → ActivityThread

#### 3.8.1 LaunchActivityItem.execute（App 端回调入口）

源码：`frameworks/base/core/java/android/app/servertransaction/LaunchActivityItem.java`

```java
public void execute(ClientTransactionHandler client, IBinder token,
        PendingTransactionActions pendingActions) {
    Trace.traceBegin(TRACE_TAG_ACTIVITY_MANAGER, "activityStart");
    ActivityClientRecord r = new ActivityClientRecord(token, mIntent, mIdent, mInfo,
            mOverrideConfig, mReferrer, mVoiceInteractor, mState, mPersistentState,
            mPendingResults, mPendingNewIntents, mActivityOptions, mIsForward, mProfilerInfo,
            client, mAssistToken, mShareableActivityToken, mLaunchedFromBubble,
            mTaskFragmentToken);
    client.handleLaunchActivity(r, pendingActions, mDeviceId, null);
    Trace.traceEnd(TRACE_TAG_ACTIVITY_MANAGER);
}
```

#### 3.8.2 TransactionExecutor.executeCallbacks（按顺序执行 callback + 生命周期）

源码：`frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java`

```java
public void execute(ClientTransaction transaction) {
    final IBinder token = transaction.getActivityToken();
    if (token != null) {
        final Map<IBinder, ClientTransactionItem> activitiesToBeDestroyed =
                mTransactionHandler.getActivitiesToBeDestroyed();
        final ClientTransactionItem destroyItem = activitiesToBeDestroyed.get(token);
        if (destroyItem != null) {
            if (transaction.getLifecycleStateRequest() == destroyItem) {
                activitiesToBeDestroyed.remove(token);
            }
            if (mTransactionHandler.getActivityClient(token) == null) {
                Slog.w(TAG, tId(transaction) + "Skip pre-destroyed transaction:\n"
                        + transactionToString(transaction, mTransactionHandler));
                return;
            }
        }
    }

    executeCallbacks(transaction);
    executeLifecycleState(transaction);
    mPendingActions.clear();
}

public void executeCallbacks(ClientTransaction transaction) {
    final List<ClientTransactionItem> callbacks = transaction.getCallbacks();
    if (callbacks == null || callbacks.isEmpty()) {
        return;
    }
    final IBinder token = transaction.getActivityToken();
    ActivityClientRecord r = mTransactionHandler.getActivityClient(token);

    final ActivityLifecycleItem finalStateRequest = transaction.getLifecycleStateRequest();
    final int finalState = finalStateRequest != null ? finalStateRequest.getTargetState()
            : UNDEFINED;
    final int lastCallbackRequestingState = lastCallbackRequestingState(transaction);

    final int size = callbacks.size();
    for (int i = 0; i < size; ++i) {
        final ClientTransactionItem item = callbacks.get(i);
        final int postExecutionState = item.getPostExecutionState();

        if (item.shouldHaveDefinedPreExecutionState()) {
            final int closestPreExecutionState = mHelper.getClosestPreExecutionState(r,
                    item.getPostExecutionState());
            if (closestPreExecutionState != UNDEFINED) {
                cycleToPath(r, closestPreExecutionState, transaction);
            }
        }
        item.execute(mTransactionHandler, token, mPendingActions);
        item.postExecute(mTransactionHandler, token, mPendingActions);
        if (r == null) {
            r = mTransactionHandler.getActivityClient(token);
        }

        if (postExecutionState != UNDEFINED && r != null) {
            final boolean shouldExcludeLastTransition =
                    i == lastCallbackRequestingState && finalState == postExecutionState;
            cycleToPath(r, postExecutionState, shouldExcludeLastTransition, transaction);
        }
    }
}
```

#### 3.8.3 ActivityThread.handleLaunchActivity（最终跑到 performLaunchActivity/onCreate）

源码：`frameworks/base/core/java/android/app/ActivityThread.java`

```java
public Activity handleLaunchActivity(ActivityClientRecord r,
        PendingTransactionActions pendingActions, int deviceId, Intent customIntent) {
    unscheduleGcIdler();
    mSomeActivitiesChanged = true;

    if (r.profilerInfo != null) {
        mProfiler.setProfiler(r.profilerInfo);
        mProfiler.startProfiling();
    }

    applyPendingApplicationInfoChanges(r.activityInfo.packageName);
    mConfigurationController.handleConfigurationChanged(null, null);
    updateDeviceIdForNonUIContexts(deviceId);

    if (ThreadedRenderer.sRendererEnabled
            && (r.activityInfo.flags & ActivityInfo.FLAG_HARDWARE_ACCELERATED) != 0) {
        HardwareRenderer.preload();
    }
    WindowManagerGlobal.initialize();

    GraphicsEnvironment.hintActivityLaunch();
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
        ActivityClient.getInstance().finishActivity(r.token, Activity.RESULT_CANCELED,
                null, Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
    }

    return a;
}
```

### 3.9 origActivity vs realActivity：alias/targetActivity 映射的来源

#### 3.9.1 Task.setIntent：根据 ActivityInfo.targetActivity 写入 realActivity/origActivity

源码：`frameworks/base/services/core/java/com/android/server/wm/Task.java`

```java
private void setIntent(Intent _intent, ActivityInfo info) {
    if (!isLeafTask()) return;

    mNeverRelinquishIdentity = (info.flags & FLAG_RELINQUISH_TASK_IDENTITY) == 0;
    affinity = info.taskAffinity;
    if (intent == null) {
        rootAffinity = affinity;
        mRequiredDisplayCategory = info.requiredDisplayCategory;
    }
    effectiveUid = info.applicationInfo.uid;
    mIsEffectivelySystemApp = info.applicationInfo.isSystemApp();
    stringName = null;

    if (info.targetActivity == null) {
        if (_intent != null) {
            if (_intent.getSelector() != null || _intent.getSourceBounds() != null) {
                _intent = new Intent(_intent);
                _intent.setSelector(null);
                _intent.setSourceBounds(null);
            }
        }
        ProtoLog.v(WM_DEBUG_TASKS, "Setting Intent of %s to %s", this, _intent);
        intent = _intent;
        realActivity = _intent != null ? _intent.getComponent() : null;
        origActivity = null;
    } else {
        ComponentName targetComponent = new ComponentName(
                info.packageName, info.targetActivity);
        if (_intent != null) {
            Intent targetIntent = new Intent(_intent);
            targetIntent.setSelector(null);
            targetIntent.setSourceBounds(null);
            ProtoLog.v(WM_DEBUG_TASKS, "Setting Intent of %s to target %s", this, targetIntent);
            intent = targetIntent;
            realActivity = targetComponent;
            origActivity = _intent.getComponent();
        } else {
            intent = null;
            realActivity = targetComponent;
            origActivity = new ComponentName(info.packageName, info.name);
        }
    }
    mWindowLayoutAffinity =
            info.windowLayout == null ? null : info.windowLayout.windowLayoutAffinity;
    ...
}
```

#### 3.9.2 Task.fillTaskInfo：给 Shell/外部观察者提供 baseIntent/orig/real/top

源码：`frameworks/base/services/core/java/com/android/server/wm/Task.java`

```java
void fillTaskInfo(TaskInfo info, boolean stripExtras, @Nullable TaskDisplayArea tda) {
    info.launchCookies.clear();
    info.addLaunchCookie(mLaunchCookie);
    final ActivityRecord top = mTaskSupervisor.mTaskInfoHelper.fillAndReturnTop(this, info);

    info.userId = isLeafTask() ? mUserId : mCurrentUser;
    info.taskId = mTaskId;
    info.displayId = getDisplayId();
    info.displayAreaFeatureId = tda != null ? tda.mFeatureId : FEATURE_UNDEFINED;
    final Intent baseIntent = getBaseIntent();
    final int baseIntentFlags = baseIntent == null ? 0 : baseIntent.getFlags();
    info.baseIntent = baseIntent == null
            ? new Intent()
            : stripExtras ? baseIntent.cloneFilter() : new Intent(baseIntent);
    info.baseIntent.setFlags(baseIntentFlags);

    info.isRunning = top != null;
    info.topActivity = top != null ? top.mActivityComponent : null;
    info.origActivity = origActivity;
    info.realActivity = realActivity;
    info.lastActiveTime = lastActiveTime;
    info.taskDescription = new ActivityManager.TaskDescription(getTaskDescription());
    info.supportsMultiWindow = supportsMultiWindowInDisplayArea(tda);
    info.configuration.setTo(getConfiguration());
    info.configuration.windowConfiguration.setActivityType(getActivityType());
    info.configuration.windowConfiguration.setWindowingMode(getWindowingMode());
    info.token = mRemoteToken.toWindowContainerToken();
    ...
}
```

---

## 4. WMS 相关处理逻辑（StartingWindow / Traversal / SurfacePlacement / Input）

### 4.1 Starting Window：何时显示 Splash/快照，何时转移/不显示

#### 4.1.1 StartingSurfaceController.showStartingWindow：入口（支持延迟批量）

源码：`frameworks/base/services/core/java/com/android/server/wm/StartingSurfaceController.java`

```java
void showStartingWindow(ActivityRecord target, ActivityRecord prev,
        boolean newTask, boolean isTaskSwitch, ActivityRecord source) {
    if (mDeferringAddStartingWindow) {
        addDeferringRecord(target, prev, newTask, isTaskSwitch, source);
    } else {
        target.showStartingWindow(prev, newTask, isTaskSwitch, true /* startActivity */,
                source);
    }
}
```

#### 4.1.2 ActivityRecord.showStartingWindow：筛选场景与主题/样式

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java`

```java
void showStartingWindow(ActivityRecord prev, boolean newTask, boolean taskSwitch,
        boolean processRunning, boolean startActivity, ActivityRecord sourceRecord,
        ActivityOptions candidateOptions) {
    if (mTaskOverlay) {
        return;
    }
    final ActivityOptions startOptions = candidateOptions != null
            ? candidateOptions : mPendingOptions;
    if (startOptions != null
            && startOptions.getAnimationType() == ActivityOptions.ANIM_SCENE_TRANSITION) {
        return;
    }

    final int splashScreenTheme = startActivity ? getSplashscreenTheme(startOptions) : 0;
    final int resolvedTheme = evaluateStartingWindowTheme(prev, packageName, theme,
            splashScreenTheme);

    mSplashScreenStyleSolidColor = shouldUseSolidColorSplashScreen(sourceRecord, startActivity,
            startOptions, resolvedTheme);

    final boolean activityCreated =
            mState.ordinal() >= STARTED.ordinal() && mState.ordinal() <= STOPPED.ordinal();
    final boolean newSingleActivity = !newTask && !activityCreated
            && task.getActivity((r) -> !r.finishing && r != this) == null;

    final boolean scheduled = addStartingWindow(packageName, resolvedTheme,
            prev, newTask || newSingleActivity, taskSwitch, processRunning,
            allowTaskSnapshot(), activityCreated, mSplashScreenStyleSolidColor, allDrawn);
    if (DEBUG_STARTING_WINDOW_VERBOSE && scheduled) {
        Slog.d(TAG, "Scheduled starting window for " + this);
    }
}
```

#### 4.1.3 ActivityRecord.addStartingWindow：决定 Snapshot/Splash/None 并异步创建

源码：`frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java`

```java
boolean addStartingWindow(String pkg, int resolvedTheme, ActivityRecord from, boolean newTask,
        boolean taskSwitch, boolean processRunning, boolean allowTaskSnapshot,
        boolean activityCreated, boolean isSimple,
        boolean activityAllDrawn) {
    if (!okToDisplay()) {
        return false;
    }

    if (mStartingData != null) {
        return false;
    }

    final WindowState mainWin = findMainWindow();
    if (mainWin != null && mainWin.mWinAnimator.getShown()) {
        return false;
    }

    final TaskSnapshot snapshot =
            mWmService.mTaskSnapshotController.getSnapshot(task.mTaskId, task.mUserId,
                    false, false);
    final int type = getStartingWindowType(newTask, taskSwitch, processRunning,
            allowTaskSnapshot, activityCreated, activityAllDrawn, snapshot);

    final boolean useLegacy = type == STARTING_WINDOW_TYPE_SPLASH_SCREEN
            && mWmService.mStartingSurfaceController.isExceptionApp(packageName, mTargetSdk,
                () -> {
                    ActivityInfo activityInfo = intent.resolveActivityInfo(
                            mAtmService.mContext.getPackageManager(),
                            PackageManager.GET_META_DATA);
                    return activityInfo != null ? activityInfo.applicationInfo : null;
                });

    final int typeParameter = StartingSurfaceController
            .makeStartingWindowTypeParameter(newTask, taskSwitch, processRunning,
                    allowTaskSnapshot, activityCreated, isSimple, useLegacy, activityAllDrawn,
                    type, packageName, mUserId);

    if (type == STARTING_WINDOW_TYPE_SNAPSHOT) {
        if (isActivityTypeHome()) {
            mWmService.mTaskSnapshotController.removeSnapshotCache(task.mTaskId);
            if ((mDisplayContent.mAppTransition.getTransitFlags()
                    & WindowManager.TRANSIT_FLAG_KEYGUARD_GOING_AWAY_NO_ANIMATION) == 0) {
                return false;
            }
        }
        return createSnapshot(snapshot, typeParameter);
    }

    if (resolvedTheme == 0 && theme != 0) {
        return false;
    }

    if (from != null && transferStartingWindow(from)) {
        return true;
    }

    if (type != STARTING_WINDOW_TYPE_SPLASH_SCREEN) {
        return false;
    }

    mStartingData = new SplashScreenStartingData(mWmService, resolvedTheme, typeParameter);
    scheduleAddStartingWindow();
    return true;
}
```

### 4.2 Traversal：requestTraversal → performSurfacePlacement 的调度与合并

#### 4.2.1 WindowSurfacePlacer.requestTraversal：把一次 traversal 投递到 AnimationHandler

源码：`frameworks/base/services/core/java/com/android/server/wm/WindowSurfacePlacer.java`

```java
void requestTraversal() {
    if (mTraversalScheduled) {
        return;
    }

    mTraversalScheduled = true;
    if (mDeferDepth > 0) {
        mDeferredRequests++;
        if (DEBUG) Slog.i(TAG, "Defer requestTraversal " + Debug.getCallers(3));
        return;
    }
    mService.mAnimationHandler.post(mPerformSurfacePlacement);
}
```

#### 4.2.2 WindowSurfacePlacer.performSurfacePlacement：最多循环 6 次，直到不再需要 layout

源码同上：

```java
final void performSurfacePlacement(boolean force) {
    if (mDeferDepth > 0 && !force) {
        mDeferredRequests++;
        return;
    }
    int loopCount = 6;
    do {
        mTraversalScheduled = false;
        performSurfacePlacementLoop();
        mService.mAnimationHandler.removeCallbacks(mPerformSurfacePlacement);
        loopCount--;
    } while (mTraversalScheduled && loopCount > 0);
    mService.mRoot.mWallpaperActionPending = false;
}
```

### 4.3 RootWindowContainer.performSurfacePlacementNoTrace：open/close SurfaceTransaction 与 applySurfaceChanges

源码：`frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java`

```java
void performSurfacePlacementNoTrace() {
    if (mWmService.mFocusMayChange) {
        mWmService.mFocusMayChange = false;
        mWmService.updateFocusedWindowLocked(
                UPDATE_FOCUS_WILL_PLACE_SURFACES, false);
    }

    mScreenBrightnessOverride = PowerManager.BRIGHTNESS_INVALID_FLOAT;
    mUserActivityTimeout = -1;
    mObscureApplicationContentOnSecondaryDisplays = false;
    mSustainedPerformanceModeCurrent = false;
    mWmService.mTransactionSequence++;

    final DisplayContent defaultDisplay = mWmService.getDefaultDisplayContentLocked();
    final WindowSurfacePlacer surfacePlacer = mWmService.mWindowPlacerLocked;

    Trace.traceBegin(TRACE_TAG_WINDOW_MANAGER, "applySurfaceChanges");
    mWmService.openSurfaceTransaction();
    try {
        applySurfaceChangesTransaction();
    } catch (RuntimeException e) {
        Slog.wtf(TAG, "Unhandled exception in Window Manager", e);
    } finally {
        mWmService.closeSurfaceTransaction("performLayoutAndPlaceSurfaces");
        Trace.traceEnd(TRACE_TAG_WINDOW_MANAGER);
    }

    mWmService.mAtmService.mTaskOrganizerController.dispatchPendingEvents();
    mWmService.mAtmService.mTaskFragmentOrganizerController.dispatchPendingEvents();
    mWmService.mSyncEngine.onSurfacePlacement();
    mWmService.mAnimator.executeAfterPrepareSurfacesRunnables();

    checkAppTransitionReady(surfacePlacer);

    final RecentsAnimationController recentsAnimationController =
            mWmService.getRecentsAnimationController();
    if (recentsAnimationController != null) {
        recentsAnimationController.checkAnimationReady(defaultDisplay.mWallpaperController);
    }
    mWmService.mAtmService.mBackNavigationController
            .checkAnimationReady(defaultDisplay.mWallpaperController);

    if (mWmService.mFocusMayChange) {
        mWmService.mFocusMayChange = false;
        mWmService.updateFocusedWindowLocked(UPDATE_FOCUS_PLACING_SURFACES,
                false);
    }

    if (isLayoutNeeded()) {
        defaultDisplay.pendingLayoutChanges |= FINISH_LAYOUT_REDO_LAYOUT;
    }

    handleResizingWindows();

    if (mOrientationChangeComplete) {
        if (mWmService.mWindowsFreezingScreen != WINDOWS_FREEZING_SCREENS_NONE) {
            mWmService.mWindowsFreezingScreen = WINDOWS_FREEZING_SCREENS_NONE;
            mWmService.mLastFinishedFreezeSource = mLastWindowFreezeSource;
            mWmService.mH.removeMessages(WINDOW_FREEZE_TIMEOUT);
        }
        mWmService.stopFreezingDisplayLocked();
    }

    forAllDisplays(dc -> {
        dc.getInputMonitor().updateInputWindowsLw(true);
        dc.updateSystemGestureExclusion();
        dc.updateKeepClearAreas();
        dc.updateTouchExcludeRegion();
    });

    mWmService.enableScreenIfNeededLocked();
    mWmService.scheduleAnimationLocked();
}
```

---

## 5. SurfaceFlinger 相关处理逻辑（SurfaceControl / Transaction / BLAST）

### 5.1 Transaction 从 WMS/客户端到 SurfaceFlinger 的跨进程路径

#### 5.1.1 Java：SurfaceControl.Transaction.apply → nativeApplyTransaction

源码：`frameworks/base/core/java/android/view/SurfaceControl.java`

```java
public void apply() {
    apply(false);
}

public void apply(boolean sync) {
    applyResizedSurfaces();
    notifyReparentedSurfaces();
    nativeApplyTransaction(mNativeObject, sync);
}
```

#### 5.1.2 JNI：nativeApplyTransaction → SurfaceComposerClient::Transaction::apply

源码：`frameworks/base/core/jni/android_view_SurfaceControl.cpp`

```cpp
static void nativeApplyTransaction(JNIEnv* env, jclass clazz, jlong transactionObj, jboolean sync) {
    auto transaction = reinterpret_cast<SurfaceComposerClient::Transaction*>(transactionObj);
    transaction->apply(sync);
}
```

#### 5.1.3 native(gui)：SurfaceComposerClient::Transaction::apply → ISurfaceComposer::setTransactionState

源码：`frameworks/native/libs/gui/SurfaceComposerClient.cpp`

```cpp
status_t SurfaceComposerClient::Transaction::apply(bool synchronous, bool oneWay) {
    if (mStatus != NO_ERROR) {
        return mStatus;
    }

    std::shared_ptr<SyncCallback> syncCallback = std::make_shared<SyncCallback>();
    if (synchronous) {
        syncCallback->init();
        addTransactionCommittedCallback(SyncCallback::getCallback(syncCallback),
                                        /*callbackContext=*/nullptr);
    }

    bool hasListenerCallbacks = !mListenerCallbacks.empty();
    std::vector<ListenerCallbacks> listenerCallbacks;
    for (const auto& [listener, callbackInfo] : mListenerCallbacks) {
        auto& [callbackIds, surfaceControls] = callbackInfo;
        if (callbackIds.empty()) {
            continue;
        }

        if (surfaceControls.empty()) {
            listenerCallbacks.emplace_back(IInterface::asBinder(listener), std::move(callbackIds));
        } else {
            for (const auto& surfaceControl : surfaceControls) {
                layer_state_t* s = getLayerState(surfaceControl);
                if (!s) {
                    ALOGE("failed to get layer state");
                    continue;
                }
                std::vector<CallbackId> callbacks(callbackIds.begin(), callbackIds.end());
                s->what |= layer_state_t::eHasListenerCallbacksChanged;
                s->listeners.emplace_back(IInterface::asBinder(listener), callbacks);
            }
        }
    }

    cacheBuffers();

    Vector<ComposerState> composerStates;
    Vector<DisplayState> displayStates;
    uint32_t flags = 0;

    for (auto const& kv : mComposerStates) {
        composerStates.add(kv.second);
    }

    displayStates = std::move(mDisplayStates);

    if (mAnimation) {
        flags |= ISurfaceComposer::eAnimation;
    }
    if (oneWay) {
        if (synchronous) {
            ALOGE("Transaction attempted to set synchronous and one way at the same time"
                  " this is an invalid request. Synchronous will win for safety");
        } else {
            flags |= ISurfaceComposer::eOneWay;
        }
    }

    if (mEarlyWakeupStart && !mEarlyWakeupEnd) {
        flags |= ISurfaceComposer::eEarlyWakeupStart;
    }
    if (mEarlyWakeupEnd && !mEarlyWakeupStart) {
        flags |= ISurfaceComposer::eEarlyWakeupEnd;
    }

    sp<IBinder> applyToken = mApplyToken ? mApplyToken : sApplyToken;

    sp<ISurfaceComposer> sf(ComposerService::getComposerService());
    sf->setTransactionState(mFrameTimelineInfo, composerStates, displayStates, flags, applyToken,
                            mInputWindowCommands, mDesiredPresentTime, mIsAutoTimestamp,
                            mUncacheBuffers, hasListenerCallbacks, listenerCallbacks, mId,
                            mMergedTransactionIds);
    mId = generateId();

    clear();

    if (synchronous) {
        syncCallback->wait();
    }

    mStatus = NO_ERROR;
    return NO_ERROR;
}
```

#### 5.1.4 SurfaceFlinger：setTransactionState（权限/解析/入队，等待主线程 apply）

源码：`frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`

```cpp
status_t SurfaceFlinger::setTransactionState(
        const FrameTimelineInfo& frameTimelineInfo, Vector<ComposerState>& states,
        const Vector<DisplayState>& displays, uint32_t flags, const sp<IBinder>& applyToken,
        InputWindowCommands inputWindowCommands, int64_t desiredPresentTime, bool isAutoTimestamp,
        const std::vector<client_cache_t>& uncacheBuffers, bool hasListenerCallbacks,
        const std::vector<ListenerCallbacks>& listenerCallbacks, uint64_t transactionId,
        const std::vector<uint64_t>& mergedTransactionIds) {
    ATRACE_CALL();

    IPCThreadState* ipc = IPCThreadState::self();
    const int originPid = ipc->getCallingPid();
    const int originUid = ipc->getCallingUid();
    uint32_t permissions = LayerStatePermissions::getTransactionPermissions(originPid, originUid);
    for (auto composerState : states) {
        composerState.state.sanitize(permissions);
    }

    for (DisplayState display : displays) {
        display.sanitize(permissions);
    }

    if (!inputWindowCommands.empty() &&
        (permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER) == 0) {
        ALOGE("Only privileged callers are allowed to send input commands.");
        inputWindowCommands.clear();
    }

    if (flags & (eEarlyWakeupStart | eEarlyWakeupEnd)) {
        const bool hasPermission =
                (permissions & layer_state_t::Permission::ACCESS_SURFACE_FLINGER) ||
                callingThreadHasPermission(sWakeupSurfaceFlinger);
        if (!hasPermission) {
            ALOGE("Caller needs permission android.permission.WAKEUP_SURFACE_FLINGER to use "
                  "eEarlyWakeup[Start|End] flags");
            flags &= ~(eEarlyWakeupStart | eEarlyWakeupEnd);
        }
    }

    const int64_t postTime = systemTime();

    std::vector<uint64_t> uncacheBufferIds;
    uncacheBufferIds.reserve(uncacheBuffers.size());
    for (const auto& uncacheBuffer : uncacheBuffers) {
        sp<GraphicBuffer> buffer = ClientCache::getInstance().erase(uncacheBuffer);
        if (buffer != nullptr) {
            uncacheBufferIds.push_back(buffer->getId());
        }
    }

    std::vector<ResolvedComposerState> resolvedStates;
    resolvedStates.reserve(states.size());
    for (auto& state : states) {
        resolvedStates.emplace_back(std::move(state));
        auto& resolvedState = resolvedStates.back();
        if (resolvedState.state.hasBufferChanges() && resolvedState.state.hasValidBuffer() &&
            resolvedState.state.surface) {
            sp<Layer> layer = LayerHandle::getLayer(resolvedState.state.surface);
            std::string layerName = (layer) ?
                    layer->getDebugName() : std::to_string(resolvedState.state.layerId);
            resolvedState.externalTexture =
                    getExternalTextureFromBufferData(*resolvedState.state.bufferData,
                                                     layerName.c_str(), transactionId);
            if (resolvedState.externalTexture) {
                resolvedState.state.bufferData->buffer = resolvedState.externalTexture->getBuffer();
            }
            mBufferCountTracker.increment(resolvedState.state.surface->localBinder());
        }
        resolvedState.layerId = LayerHandle::getLayerId(resolvedState.state.surface);
        if (resolvedState.state.what & layer_state_t::eReparent) {
            resolvedState.parentId =
                    getLayerIdFromSurfaceControl(resolvedState.state.parentSurfaceControlForChild);
        }
        if (resolvedState.state.what & layer_state_t::eRelativeLayerChanged) {
            resolvedState.relativeParentId =
                    getLayerIdFromSurfaceControl(resolvedState.state.relativeLayerSurfaceControl);
        }
        if (resolvedState.state.what & layer_state_t::eInputInfoChanged) {
            wp<IBinder>& touchableRegionCropHandle =
                    resolvedState.state.windowInfoHandle->editInfo()->touchableRegionCropHandle;
            resolvedState.touchCropId =
                    LayerHandle::getLayerId(touchableRegionCropHandle.promote());
        }
    }

    TransactionState state{frameTimelineInfo,
                           resolvedStates,
                           displays,
                           flags,
                           applyToken,
                           std::move(inputWindowCommands),
                           desiredPresentTime,
                           isAutoTimestamp,
                           std::move(uncacheBufferIds),
                           postTime,
                           hasListenerCallbacks,
                           listenerCallbacks,
                           originPid,
                           originUid,
                           transactionId,
                           mergedTransactionIds};

    const auto schedule = [](uint32_t flags) {
        if (flags & eEarlyWakeupEnd) return TransactionSchedule::EarlyEnd;
        if (flags & eEarlyWakeupStart) return TransactionSchedule::EarlyStart;
        return TransactionSchedule::Late;
    }(state.flags);

    const auto frameHint = state.isFrameActive() ? FrameHint::kActive : FrameHint::kNone;
    mTransactionHandler.queueTransaction(std::move(state));
    setTransactionFlags(eTransactionFlushNeeded, schedule, applyToken, frameHint);
    return NO_ERROR;
}
```

### 5.2 BLASTSyncEngine：为何启动/转场要“等 ready 后合并一次提交”

源码：`frameworks/base/services/core/java/com/android/server/wm/BLASTSyncEngine.java`

```java
private void finishNow() {
    if (mTraceName != null) {
        Trace.asyncTraceEnd(TRACE_TAG_WINDOW_MANAGER, mTraceName, mSyncId);
    }
    ProtoLog.v(WM_DEBUG_SYNC_ENGINE, "SyncGroup %d: Finished!", mSyncId);
    SurfaceControl.Transaction merged = mWm.mTransactionFactory.get();
    if (mOrphanTransaction != null) {
        merged.merge(mOrphanTransaction);
    }
    for (WindowContainer wc : mRootMembers) {
        wc.finishSync(merged, this, false);
    }

    final ArraySet<WindowContainer> wcAwaitingCommit = new ArraySet<>();
    for (WindowContainer wc : mRootMembers) {
        wc.waitForSyncTransactionCommit(wcAwaitingCommit);
    }
    class CommitCallback implements Runnable {
        boolean ran = false;
        public void onCommitted(SurfaceControl.Transaction t) {
            synchronized (mWm.mGlobalLock) {
                if (ran) {
                    return;
                }
                mHandler.removeCallbacks(this);
                ran = true;
                for (WindowContainer wc : wcAwaitingCommit) {
                    wc.onSyncTransactionCommitted(t);
                }
                t.apply();
                wcAwaitingCommit.clear();
            }
        }

        @Override
        public void run() {
            Trace.traceBegin(TRACE_TAG_WINDOW_MANAGER, "onTransactionCommitTimeout");
            Slog.e(TAG, "WM sent Transaction to organized, but never received" +
                   " commit callback. Application ANR likely to follow.");
            Trace.traceEnd(TRACE_TAG_WINDOW_MANAGER);
            synchronized (mWm.mGlobalLock) {
                onCommitted(merged.mNativeObject != 0
                        ? merged : mWm.mTransactionFactory.get());
            }
        }
    };
    CommitCallback callback = new CommitCallback();
    merged.addTransactionCommittedListener(Runnable::run,
            () -> callback.onCommitted(new SurfaceControl.Transaction()));
    mHandler.postDelayed(callback, BLAST_TIMEOUT_DURATION);

    Trace.traceBegin(TRACE_TAG_WINDOW_MANAGER, "onTransactionReady");
    mListener.onTransactionReady(mSyncId, merged);
    Trace.traceEnd(TRACE_TAG_WINDOW_MANAGER);
    mActiveSyncs.remove(this);
    mHandler.removeCallbacks(mOnTimeout);
    ...
}
```

---

## 6. 与日志对应的“可定位点”

- `ActivityTaskManager: START ... (BAL_ALLOW_...) result code=...`
  - `ActivityStarter.execute()` finally 打印：`result code=...`
  - `ActivityStarter.executeRequest()` 拼接 `START u... {intent} with LAUNCH_* from uid ...` + `balCodeToString(...)`

- `WindowManagerShell: Transition requested ... triggerTask=TaskInfo{...}`
  - `TransitionController.requestStartTransition()`：构造 `TransitionRequestInfo`，并调用 `mTransitionPlayer.requestStartTransition(...)`
  - `Task.fillTaskInfo(...)`：生成 `baseIntent/origActivity/realActivity/topActivity`

- “入口组件 .Settings，但实际显示 HomepageActivity”
  - `Task.setIntent(...)`：`ActivityInfo.targetActivity` 非空时写入 `realActivity` 与 `origActivity`

---

## 7. 建议的学习顺序（最少路径）

1. `Instrumentation.execStartActivity` → `ActivityTaskManagerService.startActivityAsUser`
2. `ActivityStarter.execute` / `executeRequest` / `startActivityUnchecked`
3. `ActivityTaskSupervisor.startSpecificActivity` / `realStartActivityLocked`
4. `LaunchActivityItem` → `TransactionExecutor` → `ActivityThread.handleLaunchActivity`
5. `WindowSurfacePlacer.requestTraversal` → `RootWindowContainer.performSurfacePlacementNoTrace`
6. `SurfaceControl.Transaction.apply` → `SurfaceComposerClient::Transaction::apply` → `SurfaceFlinger::setTransactionState`
