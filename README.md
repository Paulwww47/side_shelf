# Side Shelf · 侧边暂存栏

[![release](https://img.shields.io/github/v/release/Paulwww47/side_shelf?sort=semver)](https://github.com/Paulwww47/side_shelf/releases/latest)
[![build](https://github.com/Paulwww47/side_shelf/actions/workflows/build.yml/badge.svg)](https://github.com/Paulwww47/side_shelf/actions/workflows/build.yml)
[![license](https://img.shields.io/github/license/Paulwww47/side_shelf)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6)
![Qt](https://img.shields.io/badge/Qt-6.9-41CD52)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)

常驻屏幕右侧的小把手（药丸形侧边条），用来临时中转文件与剪贴板里的非文字内容。

<p align="center">
  <img src="docs/panel.png" width="520" alt="预览面板：图片按原始比例瀑布流排布，文件显示系统图标">
</p>
<p align="center"><sub>中键打开预览面板 —— 图片按原始比例瀑布流排布，文件显示系统图标</sub></p>

## 为什么用它

- **退出即清空**：只做临时中转，不是剪贴板历史管理器，不往磁盘里堆东西
- **绝不碰你的原始文件**：清空与删除只清理程序自己在 `stash/` 里的副本，
  普通位置的文件只记录路径
- **拖进来就寄存**：从压缩软件 / 资源管理器 zip 视图直接拖出的文件会先复制
  到 `stash/` 再登记，源临时目录后来被清理也不会变成死路径
- **同一张图只留一份**：剪贴板同时提供临时文件路径与位图时，按解码后像素
  判等，预览里不会出现两条一模一样的条目
- **全屏不打扰**：检测到有程序全屏（游戏、视频）时自动完全隐藏

## 下载

到 [Releases](https://github.com/Paulwww47/side_shelf/releases/latest) 下载
`side-shelf-<版本>-windows-x64.zip`，解压后双击 `side_shelf.exe` 即可常驻运行。

发布包已内含全部 Qt 运行库，**无需安装 Python 或任何运行时**，可直接拷贝到
其他 Windows 10/11 x64 电脑使用。每个发布包都附带 `SHA256SUMS.txt`。

> 首次运行 Windows SmartScreen 可能提示「未知发布者」——本程序未做代码签名，
> 点「更多信息」→「仍要运行」即可。

退出方式：把鼠标移到屏幕最右边缘让侧边栏滑出 → 点右键 → 退出。

## 操作说明

| 操作 | 效果 |
| --- | --- |
| 鼠标移到屏幕最右边缘 | 把手从边缘滑出，居中的数量与上下两个功能小圆跟随浮现 |
| 点击把手 | 剪贴板中有文件 / 图片（非文字内容）时，暂存到侧边栏 |
| 点击把手**中键** | 屏幕中央弹出米白色预览窗口：图片按原始比例以瀑布流排布，文件显示系统图标；滚轮浏览，点击窗口外任意处关闭，再按一次中键也可关闭 |
| 预览窗口中**左键**点击条目 | 该条**带平滑过渡动画**放大为**原尺寸**展示（被点卡片从缩略图位置展开，窗口同步形变；超出屏幕时铺满屏幕可用区域、避开任务栏，非真正全屏）；其余条目在放大项**右侧自上而下单列排布**，点击列中其他条目可**带动画切换**放大项；图片保持白底卡片，文件显示大图标与文件名 |
| 放大状态下**左键**点击放大项 | 平滑缩小回到瀑布流状态（恢复原窗口大小与滚动位置）；放大状态下按 Esc 也先退回瀑布流，再按一次 Esc 才关闭窗口 |
| 预览窗口中**右键**点击条目 | 从暂存列表移除该项；底部出现 **5 秒撤销条**，倒计时结束、关闭面板、清空、再次新增或再次删除时提交删除 |
| 预览窗口中**中键**点击条目 | 用默认程序打开该文件（窗口保持打开） |
| 把文件拖到把手上 | 暂存这些文件（也可拖入图片）；**从压缩软件（7-Zip / WinRAR / Bandizip / 资源管理器 zip 视图）里直接拖出的文件会先复制到暂存区再登记**——相当于「压缩软件解压 + 本程序寄存」一步完成，不因临时文件被清理而失效，清空 / 退出时随暂存一起删除 |
| 按住把手上下拖动 | 调整垂直位置（位置会被记住） |
| 按住**上方**小圆拖动 | 把全部暂存内容作为文件拖出去（**复制**，拖出后暂存保留） |
| 点击**下方**小圆 | 清空全部暂存 |
| 右键菜单 → 自动监测剪贴板 | 开启后自动暂存此后复制的文件 / 图片，直到手动关闭 |
| 右键菜单 → 开机自启 | 勾选 / 取消 Windows 登录后自动启动本程序 |
| 右键菜单 / 在侧边栏上滚动滚轮 | 调节透明度（默认 75%，只作用于主体填充色，描边与数字始终清晰） |

数量直接显示在灰蓝色把手中央，与上下暖白色功能按钮对齐。空时隐藏数字，
超过 99 项显示 `99+`，悬停可查看精确数量；存储成功时闪绿色，清空时闪红色。

<p align="center">
  <img src="docs/shelf-empty.png" width="88" alt="空态">
  &nbsp;&nbsp;
  <img src="docs/shelf.png" width="88" alt="常规">
  &nbsp;&nbsp;
  <img src="docs/shelf-99plus.png" width="88" alt="99+">
</p>
<p align="center"><sub>把手三种状态：空态 · 常规 · 超过 99 项</sub></p>

<p align="center">
  <img src="docs/panel-zoom.png" width="360" alt="单项放大">
  <img src="docs/panel-zoom-file.png" width="220" alt="放大文件项">
  <img src="docs/panel-undo.png" width="360" alt="删除后的 5 秒撤销条">
</p>
<p align="center"><sub>左键放大单项 · 放大后的文件项 · 右键删除后的 5 秒撤销条</sub></p>

## 行为约定

- **退出即清空**：暂存内容只在本次运行内有效，退出程序 / 重启电脑后自动清空；
  只有透明度与垂直位置会被记住。
- **全屏禁用**：当有程序全屏（游戏、视频等）时，侧边栏自动完全隐藏，不再响应
  屏幕边缘；退出全屏后恢复。普通的最大化窗口不受影响。
- **自动监测剪贴板**：在右键菜单中开启后，自动暂存此后复制到剪贴板的文件 /
  图片（同一内容自动去重）；全屏时暂停，退出全屏后继续；**重启后默认关闭**，
  需要时重新开启。
- **多显示器**：把手固定位于主显示器右边缘。
- 只处理剪贴板里的**非文字**内容（文件、图片）；纯文字不会被暂存。
- **拖入即寄存**：把文件拖到把手上即可暂存。其中位于系统临时目录的文件
  （从压缩软件里直接拖出解压的文件、部分邮件附件 / 浏览器拖出的文件等）会被
  **复制一份**到 `stash` 文件夹保存，因此压缩软件关闭、临时目录被清理后仍可
  正常使用；副本随清空 / 退出一起删除，需要长期保留时请先从把手把文件拖出到
  目标文件夹。普通位置的文件仍只记录路径，不占用额外磁盘空间。**从剪贴板复制
  到的临时目录文件同样如此**（例如在压缩软件里 Ctrl+C 复制文件），否则源程序
  清理临时目录后该条目就是一条死路径。
- **同一张图只留一份**：微信等程序复制图片时会同时提供临时文件路径与位图，
  两者按解码后的像素判等视为同一张图，只保留暂存在 `stash` 里的图片副本
  （可右键删除、清空时随暂存删除），预览里不会出现两条一模一样的条目。
- **预览内单项删除**：右键点击条目后隐去该项并显示 5 秒撤销条；删除只会在
  提交后清理本程序 `stash` 目录中的副本。普通拖入的文件只从暂存列表移除，
  原始文件始终不动；删空时会立即关闭面板并提交删除。
- 清空只会删除本程序保存的副本（剪贴板图片，以及从压缩软件等拖入时复制到
  `stash` 文件夹的文件），**绝不会动你的原始文件**（普通拖入的文件只记录路径，
  清空只是"忘记"它们）。
- 同时只能运行一个实例；已在运行时再次启动会自动静默退出。

## 开机自启（可选）

右键菜单勾选 **「开机自启」** 即可：写入当前用户的注册表 Run 键
（`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值名 `SideShelf`），
无需管理员权限；取消勾选即关闭。每次打开菜单都会实时读取注册表状态；
若启用后移动了程序目录，下次启动会自动把注册路径更新为新位置。

## 从源码构建

### 依赖

- Visual Studio 2022 Build Tools，含「使用 C++ 的桌面开发」工作负载
  （提供 MSVC v143、CMake 与 Ninja）
- Qt 6.9+ 的 MSVC 2022 64 位预编译包，例如通过
  [`aqtinstall`](https://github.com/miurahr/aqtinstall) 安装到 `C:\Qt`

### 构建

**必须先进入 MSVC 环境**。若 PATH 上存在 MinGW（如 `C:\mingw64`），CMake 会选中
`g++`，随后去链接 MSVC 构建的 Qt，以 ABI 不匹配（链接期 `undefined reference`）
告终。所以这里既建立 MSVC 环境，也显式指定编译器：

```bat
rem 进入 MSVC 环境：开始菜单搜索「x64 Native Tools Command Prompt for VS 2022」，
rem 或在普通 cmd 中先执行下面这行：
rem   "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

cd cpp
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.9.3/msvc2022_64
cmake --build build
```

> ⚠️ 本项目构建目录的头文件依赖跟踪不可靠：**改动任何 `.h` 后必须干净重建**，
> 否则会出现新旧头文件混编（对象 `new`/`delete` 尺寸不匹配 → 预览窗口打开即
> 堆损坏崩溃）。干净重建：
> ```bat
> cmake --build build --target clean
> cmake --build build
> ```

### 自检与界面预览

```bat
rem 自检：输出 SELFTEST OK 即通过。校验透明窗口属性、位置钳制、暂存/去重/清空、
rem 透明度钳制、滑入滑出几何、自动监测、剪贴板文件+图片、退出即清空。
rem 需要真实的交互式桌面会话，因此不纳入 CI。
build\side_shelf.exe --selftest

rem 离屏渲染当前界面，供外观调整时自查（浅色桌面渐变底、按当前默认透明度合成）
build\side_shelf.exe --render-preview out.png
build\side_shelf.exe --render-preview out.png --preview-count 0     rem 空态
build\side_shelf.exe --render-preview out.png --preview-count 120   rem 99+
```

### 打包发布包

```powershell
pwsh -File scripts/package-release.ps1 -Exe build\side_shelf.exe -Version v1.0.0
```

脚本用 `windeployqt` 收集 Qt 运行库、压缩为 zip 并生成 `SHA256SUMS.txt`。
CI 与本地使用同一份脚本，产物一致。

## 项目结构

```
.
├── cpp/                          主程序（C++20 + Qt 6）
│   ├── CMakeLists.txt
│   └── src/                      字体收口、把手窗口、预览面板、GL 画布、开机自启
├── legacy_python/                旧版 Python/PySide6 实现（已停止维护，仅作参考）
├── docs/                         README 截图
├── scripts/
│   ├── capture-screenshots.ps1   生成 docs/ 截图（纯离屏渲染，可复现）
│   └── package-release.ps1       生成 Windows 发布包
├── .github/workflows/            CI 构建与打标签自动发布
├── CHANGELOG.md
├── CONTRIBUTING.md
└── LICENSE
```

## 实现要点

- 界面渲染优先使用 GPU（OpenGL 4× MSAA 离屏渲染 + 把手 3× 超采样），轮廓外
  叠加柔和中性阴影；无可用 GPU（远程桌面 / 无驱动虚拟机）时自动回退到软件光栅
  3× 超采样，观感基本一致。主体填充色自带透明度（滚轮可调），描边 / 图标 /
  数字保持不透明，任何壁纸上都不发灰。
- 预览窗口的"点击窗口外关闭"由全局低级鼠标钩子（`WH_MOUSE_LL`）实现：钩子
  回调只做 `PostMessage`（绝无超时风险），关闭判断在 GUI 线程完成。
- 自动监测通过每 0.4 秒检查一次 Windows 剪贴板序号来感知变化，开销极小；
  重复复制同一文件 / 图片不会重复暂存。开启时会把检测到的每次剪贴板变化的
  **格式**（不含内容）记入 `clip_monitor.log`，便于排查问题。
- 界面文字统一使用**微软雅黑 Light**，只有两处刻意例外：把手上的**数量徽标用
  同字族的 Bold**（Light 在 11~13px 的灰蓝把手上偏细），**右键菜单（含「透明度」
  子菜单）用 Regular**。字族与字重集中定义在 `cpp/src/uifont.h`
  （`applyStyle` / `make` / `withWeight`）：**`Microsoft YaHei Light` 并不是独立
  字族**（按该名字请求会静默回退到 Tahoma），Light / Regular / Bold 都是
  `Microsoft YaHei` 的字面，必须用 `QFont::Light(300)` / `Normal(400)` /
  `Bold(700)` 命中；字体缺失时依次回退至 微软雅黑 / Microsoft YaHei UI /
  Segoe UI。

## 参与贡献

欢迎提交 issue 与 PR。开始之前请先阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，
其中记录了构建方式、验证手段，以及几处容易踩坑的约束（尤其是头文件依赖跟踪
与字体收口）。

## 许可

本项目以 [MIT 协议](LICENSE) 授权。
