# 贡献指南

感谢你愿意改进 Side Shelf。本文档说明本地开发、验证方式与几处容易踩坑的约束。

## 环境要求

| 组件 | 版本 | 说明 |
| --- | --- | --- |
| Visual Studio 2022 Build Tools | 含「使用 C++ 的桌面开发」工作负载 | 提供 MSVC v143、CMake 与 Ninja |
| Qt | 6.9+（MSVC 2022 64 位预编译包） | 例如用 `aqtinstall` 装到 `C:\Qt` |
| PowerShell | 7+（`pwsh`） | 运行 `scripts/` 下的脚本 |

## 构建

```bat
cd cpp
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.9.3/msvc2022_64
cmake --build build
```

## 验证

```bat
rem 自检：校验透明窗口属性、位置钳制、暂存/去重/清空、透明度钳制、
rem 滑入滑出几何、自动监测、剪贴板文件+图片、退出即清空
build\side_shelf.exe --selftest

rem 界面预览：离屏渲染当前界面，供外观调整时自查
build\side_shelf.exe --render-preview out.png
build\side_shelf.exe --render-preview out.png --preview-count 0     rem 空态
build\side_shelf.exe --render-preview out.png --preview-count 120   rem 99+
```

`--selftest` 必须输出 `SELFTEST OK`。注意它需要真实的交互式桌面会话
（要校验透明窗口与屏幕边缘几何），因此不纳入 CI，请在本机运行。

`docs/` 下的 README 截图由脚本统一产出，界面改动后请重新生成：

```powershell
pwsh -File scripts/capture-screenshots.ps1
```

## 关键约束

以下几点是踩过坑之后固化下来的，改动相关代码时请一并遵守：

1. **改动任何 `.h` 后必须干净重建**。本项目的构建目录头文件依赖跟踪不可靠，
   增量构建会造成新旧头文件混编（对象 `new`/`delete` 尺寸不匹配），
   表现为预览窗口一打开就堆损坏崩溃。
   ```bat
   cmake --build build --target clean
   cmake --build build
   ```

2. **字体必须经由 `cpp/src/uifont.h` 收口**。`Microsoft YaHei Light` 并不是
   独立字族，按该名字请求会静默回退到 Tahoma；Light 是 `Microsoft YaHei`
   字族的一个字面，必须用 `QFont::Light(300)` 才能命中。任何需要指定字体
   样式的地方都走 `UiFont::applyStyle` / `make` / `withWeight`，不要直接
   写字体名。

3. **绝不触碰用户的原始文件**。清空与删除只允许清理本程序在 `stash/` 目录
   中的副本；普通位置的文件只记录路径，清空只是"忘记"它们。

4. **退出即清空是本程序的定位**，不要引入把暂存内容持久化到磁盘的行为。

5. 界面文案为中文。新增文案请与既有风格保持一致。

## 提交信息

采用 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/)：

```
feat: 预览面板支持在放大态下用方向键切换条目
fix: 修复多显示器不同 DPI 下把手位置偏移
docs: 补充自动监测剪贴板的行为说明
```

## 发布流程

发布由 CI 完成，不要在本地手工上传资产：

1. 更新 `CHANGELOG.md`
2. 推送标签，工作流自动构建并创建 Release
   ```bat
   git tag v1.0.0
   git push origin v1.0.0
   ```

工作流会调用 `scripts/package-release.ps1`：用 `windeployqt` 收集 Qt 运行库、
压缩为 zip、生成 `SHA256SUMS.txt`，并作为 Release 资产上传。本地需要临时
出包时可以直接运行同一份脚本，保证人工与自动产出一致：

```powershell
pwsh -File scripts/package-release.ps1 -Exe side_shelf.exe -Version v1.0.0
```

## 许可

本项目以 [MIT 协议](LICENSE) 授权。提交贡献即表示你同意以同一协议分发你的改动。
