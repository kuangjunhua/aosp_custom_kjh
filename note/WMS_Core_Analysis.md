# Android WindowManagerService (WMS) 核心架构解析手册

## 1. WMS 核心类层次结构 (Container Tree)
WMS 的数据管理是基于树状结构的，所有容器类都继承自 `WindowContainer`。

| 类名 | 职责描述 | 重要方法 |
| :--- | :--- | :--- |
| **WindowManagerService** | WMS 的核心服务类，负责与客户端通信、管理全局锁、调度布局。 | `addWindow`, `relayoutWindow`, `removeWindow`, `updateFocusedWindowLocked` |
| **DisplayContent** | 对应一个逻辑显示屏（如主屏、外接屏），管理该屏幕下的所有窗口、任务和配置。 | `assignWindowLayers`, `computeImeTarget`, `updateOrientation`, `getInputMonitor` |
| **WindowToken** | 窗口令牌，将一组相关的窗口（如一个 Activity 的所有窗口）组织在一起。 | `addWindow`, `removeImmediately` |
| **ActivityRecord** | 继承自 WindowToken，代表应用层的一个 Activity，管理 Activity 的生命周期与窗口关联。 | `attachStartingWindow`, `updateReportedVisibilityLocked` |
| **Task** | 代表一个任务栈，包含多个 ActivityRecord 或嵌套的 Task。 | `reparent`, `setWindowingMode` |
| **WindowState** | WMS 中最细粒度的单位，代表一个具体的窗口实例，持有 Surface 信息。 | `removeIfPossible`, `relayoutVisibleWindow`, `openInputChannel` |

## 2. 关键辅助类与职责

| 类名 | 职责描述 | 重要方法 |
| :--- | :--- | :--- |
| **DisplayPolicy** | 窗口布局策略类，定义状态栏、导航栏、刘海屏等对布局的影响。 | `layoutWindowsLw`, `adjustWindowParamsLw`, `validateAddingWindowLw` |
| **WindowSurfacePlacer** | 布局刷新调度器，负责触发全局的窗口大小计算和 Surface 状态更新。 | `performSurfacePlacement`, `requestTraversal` |
| **RootWindowContainer** | 窗口树的根节点，管理多个 DisplayContent。 | `getDisplayContent`, `resumeFocusedStacksTopActivities` |
| **WindowStateAnimator** | 负责单个窗口的动画播放和 SurfaceControl 的具体操作。 | `createSurfaceLocked`, `prepareSurfaceLocked`, `setWallpaperOffset` |
| **SurfaceAnimator** | 通用的动画框架，用于给 WindowContainer 添加动画。 | `startAnimation`, `cancelAnimation` |

## 3. 核心流程与方法协作

### A. 窗口添加流程 (Add Window)
1. **WindowManagerService.addWindow**:
    - 校验权限（`DisplayPolicy.checkAddPermission`）。
    - 查找或创建 `WindowToken`。
    - 创建 `WindowState` 对象。
2. **WindowState.openInputChannel**:
    - 向 `InputManagerService` 注册输入通道。
3. **DisplayContent.assignWindowLayers**:
    - 重新计算该屏幕下所有窗口的 Z-Order 层级。

### B. 布局与刷新流程 (Relayout/Traversal)
这是 WMS 最复杂的协作流程，通常由 `requestTraversal` 触发。
1. **WindowSurfacePlacer.performSurfacePlacement**:
    - 这是布局的入口。
2. **DisplayContent.layoutWindowsLw**:
    - 遍历窗口树，调用 `DisplayPolicy.layoutWindowLw` 计算每个窗口的各种 Frame（坐标）。
3. **WindowContainer.assignChildLayers**:
    - 根据窗口类型和状态分配 Surface 的层级。
4. **WindowStateAnimator.prepareSurfaceLocked**:
    - 将计算好的坐标、透明度、裁剪区域应用到底层的 `SurfaceControl`。

### C. 窗口移除流程 (Remove Window)
1. **WindowManagerService.removeWindow**:
    - 确认窗口存在并准备移除。
2. **WindowState.removeIfPossible**:
    - 判断是否需要播放退出动画。如果需要，标记 `mAnimatingExit`。
3. **WindowState.removeImmediately**:
    - 销毁 `SurfaceControl`。
    - 清理输入通道。
    - 从父容器（`WindowToken`）中移除自己。
4. **WindowManagerService.postWindowRemoveCleanupLocked**:
    - 更新系统焦点。
    - 触发新的布局遍历。

## 4. WMS 协作关系图 (逻辑链路)

```text
ViewRootImpl (应用端)
      |
      v (Session / IWindowSession.aidl)
WindowManagerService (addWindow / relayoutWindow)
      |
      +-----> WindowState (管理窗口状态)
      |          |
      |          +-----> WindowStateAnimator (Surface 控制)
      |
      +-----> DisplayContent (屏幕级管理)
      |          |
      |          +-----> DisplayPolicy (布局规则)
      |          +-----> InputMonitor (输入事件监控)
      |
      +-----> WindowSurfacePlacer (刷新节拍控制器)
                 |
                 v
           SurfaceFlinger (系统合成显示)
```

## 5. 掌握建议
1. **理解锁机制**：WMS 几乎所有操作都受 `mGlobalLock` (WindowManagerGlobalLock) 保护，这是分析卡顿问题的关键。
2. **关注 Z-Order**：掌握窗口如何通过 `mBaseLayer` 和 `mSubLayer` 决定最终在屏幕上的覆盖顺序。
3. **掌握 Frame 的含义**：区分 `mFrame`, `mVisibleFrame`, `mContentFrame` 等，它们决定了应用最终看到的绘图区域。
4. **追踪 SurfaceControl**：理解 WMS 如何作为“管家”，将 `SurfaceControl` 传递给应用，但保留对其层级和位置的最终控制权。

---
**核心文件参考：**
- `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowState.java`
- `frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java`
- `frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java`
