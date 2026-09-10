# macos/ -- the macOS target

Windows 版的架构在 macOS 上的对应实现:**叠加在原始锁屏之上**,不替换锁屏、
不触碰壁纸系统、不做任何系统注册。

## 架构

```
app/    Swift 壳:窗口、生命周期、锁检测(CGSSessionScreenIsLocked)、
        SkyLight 挂载(Space @ level 400,浮在原始锁屏之上)
bridge/ C ABI shim:Swift <-> adapter 机制层的窄接口
../adapter/  共享机制层(纯 C++):相机、槽位分类、场景表、配置解析
spine/  spine-cpp 运行时(4.2 分支)放置处 —— 不入库,见 spine/README.md
```

## 安全性质

覆盖窗口只在会话锁定时挂载,解锁即摘除;`ignoresMouseEvents` + 永不为
key/main 窗口;退出应用 = 窗口消失。不触碰 WallpaperAgent / pluginkit /
凭据路径。

## 私有 API 风险

SkyLight 的 `SLSSpaceCreate / SLSSpaceSetAbsoluteLevel /
SLSSpaceAddWindowsAndRemoveFromSpaces` 是逆向的私有接口(绝对层级枚举:
ScreenLock=300,NotificationCenterAtScreenLock=400)。每次 macOS 大版本
升级需要回归。存在性证明:widgetScreen 类应用与 Lakr233/SkyLightWindow
(MIT)在现行系统上工作。

## 构建

```bash
make            # bridge-test + app
make app && build/ShittimMac          # 窗口化验证(不锁屏)
build/ShittimMac --sky                # 锁屏时挂载(显式开启)
```

`bridge-test` 在不依赖 spine-cpp 的前提下验证机制层在 macOS clang 上的
行为:配置解析、atlas PNG 解码直通(`adapter/image_mac.h`,含预乘还原)、
软光栅化三角形输出。
