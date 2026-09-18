## 变更内容

<!-- 说明这个 PR 做了什么，以及为什么 -->

## 关联 issue

<!-- 例如：Closes #12 -->

## 自检清单

- [ ] 已在本地用 MSVC + Qt 6 干净重建
      （**改动任何 `.h` 后必须** `cmake --build build --target clean` 再构建，
      否则会新旧头文件混编，导致预览窗口打开即堆损坏崩溃）
- [ ] `side_shelf.exe --selftest` 输出 `SELFTEST OK`
- [ ] 界面改动已用 `--render-preview` 复核
- [ ] 用户可见的变更已写入 `CHANGELOG.md`

## 截图 / 录屏

<!-- 界面变更请附上 --render-preview 产出的对照图 -->
