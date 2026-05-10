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

执行要点：
- `startActivity(intent)` → `startActivity(intent, null)`

### T1：Activity.startActivity(Intent, Bundle)

继续转到 `startActivityForResult`（requestCode = -1 表示不需要 result）：  
- [Activity.startActivity(Intent, Bundle)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Activity.java#L6066-L6074)

执行要点：
- `options != null`：`startActivityForResult(intent, -1, options)`  
- `options == null`：`startActivityForResult(intent, -1)`

### T2：Activity.startActivityForResult(Intent, int, Bundle)

常规 Activity（非嵌套 `mParent == null`）走这里：  
- [Activity.startActivityForResult](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Activity.java#L5610-L5646)

执行要点：
- `options = transferSpringboardActivityOptions(options)`：转场 options 透传/补齐  
- 调用 **Instrumentation**：  
  `mInstrumentation.execStartActivity(this, mMainThread.getApplicationThread(), mToken, this, intent, requestCode, options)`

> 关键点：这里开始从“应用框架层 API”转入“Instrumentation → Binder → system_server”。

---

## 2. app 侧 Instrumentation：准备 Intent 并跨进程进入 ATMS

### T3：Instrumentation.execStartActivity（app 进程）

- [Instrumentation.execStartActivity(Context, IBinder, IBinder, Activity, Intent, int, Bundle)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/Instrumentation.java#L1837-L1886)

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

关键决策点（常见“为什么启动失败/被拦截”的根源）：
- `assertPackageMatchesCallingUid(callingPackage)`：callingPackage 与 calling uid 校验  
- `enforceNotIsolatedCaller(...)`：隔离进程限制  
- `checkTargetUser(...)`：跨用户合法性  
- `obtainStarter(intent, reason).setXXX(...).execute()`：进入 ActivityStarter 主流程

### T5：ActivityStartController.obtainStarter（拿到 ActivityStarter）

- [ActivityStartController.obtainStarter](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStartController.java#L133-L136)

执行要点：
- 从工厂/对象池拿 `ActivityStarter`，设置 intent/reason，后续通过链式 setCaller/setCallingPackage/... 填充参数。

---

## 4. system_server：ActivityStarter.execute（解析/权限/拦截/构建 ActivityRecord）

### T6：ActivityStarter.execute

- [ActivityStarter.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L684-L796)

主干步骤：
1. 拒绝 FD 泄漏：`intent.hasFileDescriptors()`  
2. 性能打点：`notifyActivityLaunching(...)`  
3. 解析 ActivityInfo：`mRequest.resolveActivity(...)`（隐式 intent 解析）  
4. heavy-weight 处理：`resolveToHeavyWeightSwitcherIfNeeded()`（可能改写 intent）  
5. 进入核心：`executeRequest(mRequest)`

### T7：ActivityStarter.executeRequest（最重要的“决策区”）

- [ActivityStarter.executeRequest](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java#L901-L1331)

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

分支：
- 目标进程已存在且有线程：`realStartActivityLocked(r, proc, andResume, checkConfig)`  
- 否则：`mService.startProcessAsync(...)` 走 AMS 拉起进程，之后回到 realStart 再发 transaction

### T11：ActivityTaskSupervisor.realStartActivityLocked（发 ClientTransaction）

- [ActivityTaskSupervisor.realStartActivityLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java#L785-L1027)

执行要点（最关键的一步之一）：
- 构造 `ClientTransaction`，添加 `LaunchActivityItem`（携带 intent/info/config/state/results/options 等）  
- 设置最终 lifecycle：通常是 `ResumeActivityItem`（andResume=true）  
- `scheduleTransaction(...)` 发往 app 进程的 `IApplicationThread`

---

## 7. client：收 transaction 并执行（Activity 实例化 + onCreate/onStart/onResume）

### T12：IApplicationThread.scheduleTransaction → 主线程 EXECUTE_TRANSACTION

- binder 入口：[ActivityThread.ApplicationThread.scheduleTransaction](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L1972-L1974)  
- 主线程执行点：[ActivityThread.H(EXECUTE_TRANSACTION)](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L2468-L2478)

### T13：TransactionExecutor：先 callbacks，再 final lifecycle

- [TransactionExecutor.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java#L70-L101)

顺序：
1. `executeCallbacks(transaction)`：先跑 `LaunchActivityItem`  
2. `executeLifecycleState(transaction)`：再跑 `ResumeActivityItem` 等最终状态

### T14：LaunchActivityItem → ActivityThread.handleLaunchActivity → performLaunchActivity（onCreate）

- [LaunchActivityItem.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/LaunchActivityItem.java#L94-L106)  
- [ActivityThread.handleLaunchActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3938-L3986)  
- [ActivityThread.performLaunchActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3695-L3832)

`performLaunchActivity` 内部关键时序：
1. `Instrumentation.newActivity(...)` 实例化 Activity  
2. `LoadedApk.makeApplicationInner(...)`（确保 Application 存在）  
3. `Activity.attach(...)`（注入 token/ATMS/WMS 交互对象、Context、Window 等）  
4. `Instrumentation.callActivityOnCreate(...)` → `Activity.onCreate(...)`  

### T15：ON_START：ActivityThread.handleStartActivity（onStart/onPostCreate/restore）

- [TransactionExecutor.performLifecycleSequence: ON_START](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java#L224-L227)  
- [ActivityThread.handleStartActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L3834-L3892)

执行要点：
- `activity.performStart(...)` → `Activity.onStart()`  
- 若 `PendingTransactionActions` 指定：`onRestoreInstanceState`、`onPostCreate`

### T16：ON_RESUME：ActivityThread.handleResumeActivity（onResume + addView/makeVisible）

- [ResumeActivityItem.execute](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/servertransaction/ResumeActivityItem.java#L53-L60)  
- [ActivityThread.handleResumeActivity](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L5037-L5160)

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

server 落点：
- [ActivityClientController.activityResumed](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityClientController.java#L182-L189)  
  → [ActivityRecord.activityResumedLocked](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java#L6257-L6270)

### T18：主线程空闲回报：activityIdle（常被当作“启动完成/可进行后续调度”的标志）

client 侧触发：
- `ActivityThread.handleResumeActivity` 末尾把 record 加入 `mNewActivities`，IdleHandler 统一上报：  
  - [ActivityThread.Idler.queueIdle](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityThread.java#L2512-L2536)  
  - `ActivityClient.activityIdle(token, config, stopProfiling)`：见 [ActivityClient.activityIdle](file:///d:/kjh/framework/aosp_custom_kjh/frameworks/base/core/java/android/app/ActivityClient.java#L44-L51)

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

