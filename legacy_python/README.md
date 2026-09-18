# legacy_python —— 旧版 Python / PySide6 实现

本目录是 2026-08 之前的主程序实现，**已不再维护**，仅作历史参考保留。

现行主程序为 `cpp/` 下的 C++20 + Qt 6 实现，功能与行为以它为准；
发布包也由它构建。两者在若干细节上已经不同（例如字体处理、预览面板的
放大动画、临时文件寄存策略），请勿以本目录的行为推断当前程序的行为。

如果只是想运行这个工具，请直接下载
[最新发布版](https://github.com/Paulwww47/side_shelf/releases/latest)。

## 运行旧版（不推荐）

```bat
pip install -r requirements.txt
python side_shelf.py
```

依赖 PySide6 6.9 或更高版本。
