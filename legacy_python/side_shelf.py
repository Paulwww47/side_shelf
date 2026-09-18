# -*- coding: utf-8 -*-
"""
Side Shelf（侧边暂存栏）
========================

常驻屏幕右侧的半透明小半圆：
  · 鼠标移到屏幕最右边缘时，半圆从边缘滑出一点，上下两个小圆跟随显现
  · 点击半圆：剪贴板中有非文字内容（文件 / 图片）时，暂存到侧边栏
  · 把文件拖到半圆上：暂存这些文件（也可拖入图片）
  · 按住半圆上下拖动：调整垂直位置（位置会被记住）
  · 按住上方小圆拖动：把全部暂存内容作为文件拖出去（复制，拖出后暂存保留）
  · 点击下方小圆：清空全部暂存
  · 右键菜单：自动监测剪贴板（开启后自动暂存此后复制的文件 / 图片，全屏时暂停，重启默认关闭）
  · 滚轮 / 右键菜单：调节透明度（默认 50%）
  · 有程序全屏（游戏 / 视频）时自动隐藏禁用，退出全屏后恢复
  · 退出程序即清空全部暂存
"""

import ctypes
import hashlib
import json
import os
import sys
import tempfile
import time

from PySide6.QtCore import (
    Property,
    QByteArray,
    QBuffer,
    QEasingCurve,
    QEvent,
    QIODevice,
    QLineF,
    QMimeData,
    QPoint,
    QPointF,
    QPropertyAnimation,
    QRect,
    QRectF,
    Qt,
    QTimer,
    QUrl,
)
from PySide6.QtGui import (
    QColor,
    QCursor,
    QDrag,
    QFont,
    QGuiApplication,
    QImage,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
    QRegion,
)
from PySide6.QtWidgets import (
    QApplication,
    QMenu,
    QToolTip,
    QWidget,
)

APP_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_STASH_DIR = os.path.join(APP_DIR, "stash")
DEFAULT_STATE_FILE = os.path.join(APP_DIR, "side_shelf_state.json")

# ---- 几何参数 ----
R = 52            # 半圆半径（窗口宽度 = R）
R_SMALL = 11      # 小圆绘制半径
R_HIT = 14        # 小圆点击 / 遮罩半径（比绘制略大，方便抓取）
GAP = 12          # 半圆与小圆之间的空隙
HIDE = 10         # 收起时留在屏幕内的宽度（像素）
W = R
H = 2 * R + 2 * GAP + 4 * (R_SMALL + 4)   # 上下留一点余量

# ---- 颜色 ----
COLOR_BASE = QColor(88, 140, 240)
COLOR_HOVER = QColor(118, 164, 248)
COLOR_GREEN = QColor(96, 202, 124)
COLOR_RED = QColor(232, 106, 90)
COLOR_NEUTRAL = QColor(152, 160, 172)

# ---- 字体：全界面统一使用「微软雅黑 Light」。注意 Light 不是独立字族，而是
# Microsoft YaHei 的一个字面，必须用 QFont.Weight.Light(300) 命中（按
# "Microsoft YaHei Light" 请求会静默回退到别的字体）；与 C++/Qt6 主程序
# cpp/src/uifont.cpp 的字族链 + 字重保持一致 ----
UI_FONT_FAMILIES = ["Microsoft YaHei", "微软雅黑", "Microsoft YaHei UI", "Segoe UI"]


def ui_font(point_size, weight=QFont.Weight.Light):
    """按统一字族链 + Light 字重构造 QFont（QFont 构造参数不接受字族列表，须用 setFamilies）。"""
    font = QFont()
    font.setFamilies(UI_FONT_FAMILIES)
    font.setPointSize(point_size)
    font.setWeight(weight)
    return font


_MUTEX_HANDLE = None


def _acquire_single_instance(name):
    """用命名互斥量保证单实例；进程退出后互斥量自动释放，无残留锁问题。"""
    global _MUTEX_HANDLE
    try:
        k32 = ctypes.windll.kernel32
        k32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p]
        k32.CreateMutexW.restype = ctypes.c_void_p
        h = k32.CreateMutexW(None, False, name)
        _MUTEX_HANDLE = h
        return not (h and ctypes.get_last_error() == 183)  # ERROR_ALREADY_EXISTS
    except Exception:
        return True


class ShelfWidget(QWidget):
    def __init__(self, state_file=DEFAULT_STATE_FILE, stash_dir=DEFAULT_STASH_DIR,
                 log_file=None):
        super().__init__(
            None,
            Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint,
        )
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_AlwaysShowToolTips, True)
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setWindowTitle("Side Shelf")
        self.setFixedSize(W, H)

        self._state_file = state_file
        self._stash_dir = stash_dir
        self._log_file = log_file or os.path.join(APP_DIR, "clip_monitor.log")

        self._items = []            # [{"kind": "file"|"image", "path": ..., "sha256": ...?}]
        self._path_set = set()
        self._image_hashes = set()
        self._opacity = 0.5
        self._saved_y = None
        self._slide = 0.0           # 0 = 收起，1 = 滑出
        self._shown = False
        self._fs_hidden = False     # 全屏禁用中
        self._hover = None          # "main" / "top" / "bottom"
        self._press = None
        self._press_pos = QPointF()
        self._press_global = QPoint()
        self._start_y = 0
        self._dragging = False      # 正在上下拖动半圆
        self._pulse = 0.0           # 存储/清空时的颜色闪烁
        self._flash_color = COLOR_NEUTRAL
        self._anim = None
        self._pulse_anim = None
        self._screen_geo = QRect()

        # 暂存内容不跨进程保留：启动时清掉上次遗留的剪贴板图片副本
        os.makedirs(self._stash_dir, exist_ok=True)
        for name in os.listdir(self._stash_dir):
            try:
                p = os.path.join(self._stash_dir, name)
                if os.path.isfile(p):
                    os.remove(p)
            except OSError:
                pass

        self._load_state()

        g = QGuiApplication.primaryScreen().geometry()
        self._screen_geo = g
        self._screen_right = g.right()
        if isinstance(self._saved_y, (int, float)):
            self._y = int(max(g.top(), min(g.bottom() - H + 1, round(self._saved_y))))
        else:
            self._y = g.top() + (g.height() - H) // 2
        self._x_shown = self._screen_right - W + 1
        self._x_hidden = self._screen_right - HIDE + 1
        self.setGeometry(self._x_hidden, self._y, W, H)
        self.setWindowOpacity(self._opacity)
        self._apply_mask()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(40)

        self._auto_monitor = False      # 自动监测剪贴板（每次启动默认关闭）
        self._last_clip_seq = None
        self._clip_timer = QTimer(self)
        self._clip_timer.timeout.connect(self._poll_clipboard)
        self._clip_timer.start(400)

    # ---------- 状态持久化（只存透明度与位置，不存暂存内容）----------

    def _load_state(self):
        try:
            with open(self._state_file, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            data = {}
        self._opacity = float(data.get("opacity", 0.5))
        self._opacity = max(0.15, min(1.0, self._opacity))
        self._saved_y = data.get("y")

    def _save_state(self):
        data = {"opacity": self._opacity, "y": int(self._y)}
        tmp = self._state_file + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            os.replace(tmp, self._state_file)
        except Exception:
            pass

    # ---------- 暂存逻辑 ----------

    def add_files(self, paths):
        """暂存本地文件，返回新增数量。"""
        added = 0
        for p in paths:
            p = os.path.normcase(os.path.abspath(p))
            if os.path.isfile(p) and p not in self._path_set:
                self._items.append({"kind": "file", "path": p})
                self._path_set.add(p)
                added += 1
        if added:
            self.update()
        return added

    def add_image(self, image):
        """暂存一张剪贴板 / 拖入的图片（按内容去重），返回新增数量。"""
        if image is None or image.isNull():
            return 0
        buf = QBuffer()
        buf.open(QIODevice.WriteOnly)
        image.save(buf, "PNG")
        data = bytes(buf.data())
        h = hashlib.sha256(data).hexdigest()
        if h in self._image_hashes:
            return 0
        name = "clip_%s_%s.png" % (time.strftime("%Y%m%d_%H%M%S"), h[:6])
        path = os.path.join(self._stash_dir, name)
        try:
            with open(path, "wb") as f:
                f.write(data)
        except Exception:
            return 0
        self._image_hashes.add(h)
        self._items.append({"kind": "image", "path": path, "sha256": h})
        self._path_set.add(path)
        self.update()
        return 1

    def clear_items(self, flash=True):
        """清空暂存。只删除本程序保存的剪贴板图片副本，不会动用户的原始文件。"""
        stash_real = os.path.realpath(self._stash_dir)
        for it in self._items:
            p = it.get("path", "")
            try:
                if (
                    os.path.isfile(p)
                    and os.path.commonpath([os.path.realpath(p), stash_real]) == stash_real
                ):
                    os.remove(p)
            except Exception:
                pass
        self._items = []
        self._path_set = set()
        self._image_hashes = set()
        if flash:
            self._flash(COLOR_RED)
        self.update()

    def store_from_clipboard(self):
        """点击半圆：剪贴板中有文件 / 图片等非文字内容时暂存。"""
        md = QGuiApplication.clipboard().mimeData()
        added = self._process_clipboard_content(md)
        self._flash(COLOR_GREEN if added else COLOR_NEUTRAL)

    # ---------- 剪贴板内容处理 ----------

    def _has_image_formats(self, md):
        # Qt 内部用 application/x-qt-image 表示图片，外部程序常用 image/*（如 image/png）
        if md.hasImage():
            return True
        return any(fmt.startswith("image/") for fmt in md.formats())

    def _extract_image(self, md):
        img = md.imageData()
        if isinstance(img, QImage) and not img.isNull():
            return img
        img = QGuiApplication.clipboard().image()
        if isinstance(img, QImage) and not img.isNull():
            return img
        return None

    def _process_clipboard_content(self, md):
        """处理剪贴板 / 拖入内容：文件与图片独立判断（互不排斥），返回新增数量。"""
        added = 0
        if md.hasUrls():
            added += self.add_files(
                [u.toLocalFile() for u in md.urls() if u.isLocalFile()]
            )
        if self._has_image_formats(md):
            img = self._extract_image(md)
            if img is not None:
                added += self.add_image(img)
        return added

    # ---------- 自动监测剪贴板 ----------

    def set_auto_monitor(self, on):
        self._auto_monitor = bool(on)
        if self._auto_monitor:
            # 从开启这一刻起只收集之后的新变化，不回溯处理当前剪贴板
            self._last_clip_seq = self._clip_sequence()
            self._log_clip(None, -1, "monitor ON, seq=%s" % self._last_clip_seq)
        else:
            self._log_clip(None, -1, "monitor OFF")

    def _clip_sequence(self):
        try:
            user32 = ctypes.windll.user32
            user32.GetClipboardSequenceNumber.restype = ctypes.c_uint32
            return int(user32.GetClipboardSequenceNumber())
        except Exception:
            return None

    def _poll_clipboard(self):
        if not self._auto_monitor:
            return
        seq = self._clip_sequence()
        if seq is None:
            return
        if self._fs_hidden:
            # 全屏期间暂停：持续同步序号，退出全屏后只收集新变化
            self._last_clip_seq = seq
            return
        if seq == self._last_clip_seq:
            return
        self._last_clip_seq = seq
        md = QGuiApplication.clipboard().mimeData()
        formats = list(md.formats())
        added = self._process_clipboard_content(md)
        self._log_clip(formats, added)
        if added:
            self._flash(COLOR_GREEN)

    def _log_clip(self, formats, added, extra=None):
        """把检测到的剪贴板变化记入日志（只记格式与结果，不记内容）。"""
        try:
            if extra:
                line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), extra)
            else:
                line = "%s seq=%s formats=[%s] added=%d\n" % (
                    time.strftime("%Y-%m-%d %H:%M:%S"),
                    self._last_clip_seq,
                    ",".join(formats) if formats else "-",
                    added,
                )
            with open(self._log_file, "a", encoding="utf-8") as f:
                f.write(line)
            # 防止日志无限增长：超过 200KB 只保留最近约一半
            try:
                if os.path.getsize(self._log_file) > 200_000:
                    with open(self._log_file, "r", encoding="utf-8") as f:
                        tail = f.read()[-100_000:]
                    with open(self._log_file, "w", encoding="utf-8") as f:
                        f.write(tail)
            except Exception:
                pass
        except Exception:
            pass

    # ---------- 拖出 ----------

    def start_drag_out(self):
        """按住上方小圆拖动：把全部暂存内容作为文件拖出去（复制）。"""
        paths = [it["path"] for it in self._items if os.path.isfile(it["path"])]
        if not paths:
            self._flash(COLOR_RED)
            QToolTip.showText(QCursor.pos(), "暂存区是空的", self)
            return
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(p) for p in paths])
        # 默认动作为「复制」而不是「移动」
        mime.setData(
            'application/x-qt-windows-mime;value="Preferred DropEffect"',
            QByteArray(bytes([1, 0, 0, 0])),
        )
        drag = QDrag(self)
        drag.setMimeData(mime)
        pm = self._drag_pixmap(len(paths))
        drag.setPixmap(pm)
        drag.setHotSpot(QPoint(pm.width() // 2, pm.height() // 2))
        drag.exec(Qt.DropAction.CopyAction | Qt.DropAction.MoveAction,
                  Qt.DropAction.CopyAction)

    def _drag_pixmap(self, count):
        pm = QPixmap(64, 64)
        pm.fill(Qt.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(88, 140, 240, 230))
        p.drawRoundedRect(QRectF(6, 6, 52, 52), 12, 12)
        p.setPen(QColor(255, 255, 255))
        p.setFont(ui_font(14, QFont.Weight.Bold))  # 徽标：唯一刻意加粗处
        p.drawText(pm.rect(), Qt.AlignCenter, str(count))
        p.end()
        return pm

    # ---------- 边缘感应 / 全屏检测 / 滑入滑出 ----------

    def _is_fullscreen_active(self):
        """前景窗口是否覆盖主显示器且处于全屏状态（非普通最大化）。"""
        try:
            user32 = ctypes.windll.user32
            user32.GetForegroundWindow.restype = ctypes.c_void_p
            user32.GetShellWindow.restype = ctypes.c_void_p
            user32.GetDesktopWindow.restype = ctypes.c_void_p
            hwnd = user32.GetForegroundWindow()
            if not hwnd:
                return False
            wid = int(self.winId())
            if hwnd in (wid, user32.GetShellWindow(), user32.GetDesktopWindow()):
                return False

            class _Rect(ctypes.Structure):
                _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                            ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

            user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Rect)]
            r = _Rect()
            if not user32.GetWindowRect(hwnd, ctypes.byref(r)):
                return False

            # 排除桌面 / 资源管理器桌面（Win+D 后焦点在桌面上）
            user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
            buf = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, buf, 256)
            if buf.value in ("Progman", "WorkerW"):
                return False

            user32.GetWindowLongW.restype = ctypes.c_long
            user32.GetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int]
            WS_MAXIMIZE = 0x01000000
            WS_EX_TOPMOST = 0x00000008
            style = user32.GetWindowLongW(hwnd, -16)      # GWL_STYLE
            exstyle = user32.GetWindowLongW(hwnd, -20)    # GWL_EXSTYLE

            g = QGuiApplication.primaryScreen().geometry()
            covers = (r.left <= g.left() + 2 and r.top <= g.top() + 2
                      and r.right >= g.right() - 2 and r.bottom >= g.bottom() - 2)
            # 覆盖全屏，且（不是普通最大化窗口 或 窗口置顶）：游戏 / 视频全屏
            return covers and (
                not bool(style & WS_MAXIMIZE) or bool(exstyle & WS_EX_TOPMOST)
            )
        except Exception:
            return False

    def _poll(self):
        fs = self._is_fullscreen_active()

        g = QGuiApplication.primaryScreen().geometry()
        if g != self._screen_geo:            # 分辨率 / 显示器变化
            self._screen_geo = g
            self._screen_right = g.right()
            self._y = max(g.top(), min(g.bottom() - H + 1, self._y))
            self._x_shown = self._screen_right - W + 1
            self._x_hidden = self._screen_right - HIDE + 1
            self._set_slide(self._slide)

        if fs and not self._fs_hidden:
            self._fs_hidden = True
            self._shown = False
            if self._anim:
                self._anim.stop()
                self._anim = None
            self._set_slide(0.0)
            self.move(self._screen_right + 1, self._y)   # 完全移出屏幕
            self._log_clip(None, -1, "fullscreen ON, monitor paused")
        elif not fs and self._fs_hidden:
            self._fs_hidden = False
            self._set_slide(0.0)                         # 回到收起位置
            self._log_clip(None, -1, "fullscreen OFF, monitor resumed")

        if self._fs_hidden:
            return

        x = QCursor.pos().x()
        if not self._shown and x >= self._screen_right - 1:
            self._set_shown(True)
        elif self._shown and not self._dragging and x < self._x_shown - 80:
            self._set_shown(False)

    def _set_shown(self, shown):
        if self._shown == shown:
            return
        self._shown = shown
        if self._anim:
            self._anim.stop()
        anim = QPropertyAnimation(self, b"slide", self)
        anim.setDuration(190)
        anim.setEasingCurve(QEasingCurve.OutCubic)
        anim.setStartValue(self._slide)
        anim.setEndValue(1.0 if shown else 0.0)
        anim.start()
        self._anim = anim

    def _get_slide(self):
        return self._slide

    def _set_slide(self, v):
        self._slide = v
        x = self._x_hidden + (self._x_shown - self._x_hidden) * v
        self.move(round(x), self._y)

    slide = Property(float, _get_slide, _set_slide)

    # ---------- 透明度 ----------

    def set_opacity(self, value):
        self._opacity = max(0.15, min(1.0, value))
        self.setWindowOpacity(self._opacity)
        self._save_state()

    def wheelEvent(self, e):
        step = 0.05 if e.angleDelta().y() > 0 else -0.05
        self.set_opacity(self._opacity + step)
        QToolTip.showText(
            e.globalPosition().toPoint(),
            "透明度 %d%%" % round(self._opacity * 100),
            self,
        )

    # ---------- 外观 ----------

    def _centers(self):
        cy = self.height() / 2.0
        return {
            "main": QPointF(R, cy),
            "top": QPointF(R - R_SMALL, cy - R - GAP - R_SMALL),
            "bottom": QPointF(R - R_SMALL, cy + R + GAP + R_SMALL),
        }

    def _hit(self, pos):
        p = QPointF(pos)
        c = self._centers()
        if QLineF(p, c["main"]).length() <= R:
            return "main"
        if QLineF(p, c["top"]).length() <= R_HIT:
            return "top"
        if QLineF(p, c["bottom"]).length() <= R_HIT:
            return "bottom"
        return None

    def _shape_path(self):
        cy = self.height() / 2.0
        path = QPainterPath()
        path.addEllipse(QRectF(0, cy - R, 2 * R, 2 * R))
        path.addEllipse(QRectF(R - 2 * R_HIT, cy - R - GAP - 2 * R_HIT,
                               2 * R_HIT, 2 * R_HIT))
        path.addEllipse(QRectF(R - 2 * R_HIT, cy + R + GAP, 2 * R_HIT, 2 * R_HIT))
        return path

    def _apply_mask(self):
        self.setMask(QRegion(self._shape_path().toFillPolygon().toPolygon()))

    def _color(self, c):
        if self._pulse <= 0.01:
            return c
        t = min(1.0, self._pulse)
        f = self._flash_color
        return QColor(
            round(c.red() + (f.red() - c.red()) * t),
            round(c.green() + (f.green() - c.green()) * t),
            round(c.blue() + (f.blue() - c.blue()) * t),
        )

    def _flash(self, color):
        self._flash_color = color
        self._pulse = 1.0
        if self._pulse_anim:
            self._pulse_anim.stop()
        anim = QPropertyAnimation(self, b"pulse", self)
        anim.setDuration(450)
        anim.setEasingCurve(QEasingCurve.OutCubic)
        anim.setStartValue(1.0)
        anim.setEndValue(0.0)
        anim.start()
        self._pulse_anim = anim
        self.update()

    def _get_pulse(self):
        return self._pulse

    def _set_pulse(self, v):
        self._pulse = v
        self.update()

    pulse = Property(float, _get_pulse, _set_pulse)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        cy = self.height() / 2.0

        semi = QPainterPath()
        semi.moveTo(R, cy - R)
        semi.arcTo(QRectF(0, cy - R, 2 * R, 2 * R), 90, 180)
        semi.closeSubpath()

        c = self._centers()
        top_rect = QRectF(R - 2 * R_SMALL, c["top"].y() - R_SMALL,
                          2 * R_SMALL, 2 * R_SMALL)
        bot_rect = QRectF(R - 2 * R_SMALL, c["bottom"].y() - R_SMALL,
                          2 * R_SMALL, 2 * R_SMALL)

        base = self._color(COLOR_BASE)
        hover = self._color(COLOR_HOVER)

        p.setPen(QPen(QColor(255, 255, 255, 70), 1.4))
        p.setBrush(hover if self._hover == "main" else base)
        p.drawPath(semi)
        p.setBrush(hover if self._hover == "top" else base)
        p.drawEllipse(top_rect)
        p.setBrush(hover if self._hover == "bottom" else base)
        p.drawEllipse(bot_rect)

        # 小圆图标：上=向上箭头（拖出），下=叉（清空）
        icon_pen = QPen(QColor(255, 255, 255, 210), 2.2,
                        Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
        p.setPen(icon_pen)
        p.setBrush(Qt.NoBrush)
        tx, ty = c["top"].x(), c["top"].y()
        bx, by = c["bottom"].x(), c["bottom"].y()
        p.drawLine(QPointF(tx - 5, ty + 2.5), QPointF(tx, ty - 3.5))
        p.drawLine(QPointF(tx, ty - 3.5), QPointF(tx + 5, ty + 2.5))
        p.drawLine(QPointF(bx - 4, by - 4), QPointF(bx + 4, by + 4))
        p.drawLine(QPointF(bx - 4, by + 4), QPointF(bx + 4, by - 4))

        # 暂存数量徽标
        n = len(self._items)
        if n:
            p.setPen(QColor(255, 255, 255, 235))
            p.setFont(ui_font(13, QFont.Weight.Bold))  # 徽标：唯一刻意加粗处
            p.drawText(QRectF(0, cy - 15, R, 30), Qt.AlignCenter, str(n))
        p.end()

    # ---------- 鼠标交互 ----------

    def mousePressEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        hit = self._hit(e.position())
        if hit is None:
            return
        self._press = hit
        self._press_pos = e.position()
        self._press_global = e.globalPosition().toPoint()
        self._start_y = self._y
        self._dragging = False

    def mouseMoveEvent(self, e):
        hit = self._hit(e.position())
        if hit != self._hover:
            self._hover = hit
            self.update()
        if self._press == "top":
            if ((e.position() - self._press_pos).manhattanLength()
                    >= QApplication.startDragDistance()):
                self._press = None
                self.start_drag_out()
        elif self._press == "main":
            d = e.globalPosition().toPoint() - self._press_global
            if (not self._dragging
                    and d.manhattanLength() >= QApplication.startDragDistance()):
                self._dragging = True
            if self._dragging:
                g = QGuiApplication.primaryScreen().geometry()
                y = max(g.top(), min(g.bottom() - H + 1, self._start_y + d.y()))
                if y != self._y:
                    self._y = y
                    self._set_slide(self._slide)
                    self._save_state()

    def mouseReleaseEvent(self, e):
        if e.button() != Qt.LeftButton or not self._press:
            return
        hit = self._hit(e.position())
        if hit == self._press and not self._dragging:
            if hit == "main":
                self.store_from_clipboard()
            elif hit == "bottom":
                self.clear_items()
        self._press = None
        self._dragging = False

    def leaveEvent(self, e):
        self._hover = None
        self.update()

    # ---------- 拖入 ----------

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls() or self._has_image_formats(e.mimeData()):
            e.acceptProposedAction()

    def dragMoveEvent(self, e):
        if e.mimeData().hasUrls() or self._has_image_formats(e.mimeData()):
            e.acceptProposedAction()

    def dropEvent(self, e):
        added = self._process_clipboard_content(e.mimeData())
        self._flash(COLOR_GREEN if added else COLOR_NEUTRAL)

    # ---------- 右键菜单 / 提示 / 关闭 ----------

    def contextMenuEvent(self, e):
        menu = QMenu(self)
        sub = menu.addMenu("透明度")
        current = round(self._opacity * 100)
        for v in (15, 25, 35, 50, 65, 80, 100):
            act = sub.addAction("%d%%" % v)
            act.setCheckable(True)
            act.setChecked(v == current)
            act.triggered.connect(
                lambda _=False, val=v: self.set_opacity(val / 100.0)
            )
        menu.addSeparator()
        act_mon = menu.addAction("自动监测剪贴板（文件 / 图片）")
        act_mon.setCheckable(True)
        act_mon.setChecked(self._auto_monitor)
        act_mon.toggled.connect(self.set_auto_monitor)
        menu.addAction("清空暂存（%d 项）" % len(self._items), self.clear_items)
        menu.addSeparator()
        menu.addAction("退出", QApplication.quit)
        menu.exec(e.globalPos())

    def event(self, e):
        if e.type() == QEvent.ToolTip:
            hit = self._hit(QPointF(e.pos()))
            tips = {
                "main": "点击：暂存剪贴板里的文件 / 图片\n"
                        "按住拖动：上下移动位置\n"
                        "把文件拖到这里：暂存",
                "top": "按住并拖动：把全部暂存内容拖出去（复制）",
                "bottom": "点击：清空暂存",
            }
            if hit:
                QToolTip.showText(e.globalPos(), tips[hit], self)
            else:
                QToolTip.hideText()
            return True
        return super().event(e)

    def closeEvent(self, e):
        self.clear_items(flash=False)   # 退出即清空
        self._save_state()
        super().closeEvent(e)


def main(argv):
    app = QApplication(argv)
    app.setApplicationName("SideShelf")

    selftest = "--selftest" in argv
    if selftest:
        tmp = tempfile.gettempdir()
        state_file = os.path.join(tmp, "side_shelf_selftest_state.json")
        stash_dir = os.path.join(tmp, "side_shelf_selftest_stash")
        log_file = os.path.join(tmp, "side_shelf_selftest_clip.log")
    else:
        state_file = DEFAULT_STATE_FILE
        stash_dir = DEFAULT_STASH_DIR
        log_file = None

    name = "Local\\SideShelf-SingleInstance" + ("-Selftest" if selftest else "")
    if not _acquire_single_instance(name):
        if selftest:
            print("lock failed", file=sys.stderr)
        else:
            return 0    # 已在运行：静默退出，避免"已经在运行了"弹窗滞留

    w = ShelfWidget(state_file=state_file, stash_dir=stash_dir, log_file=log_file)
    w.show()

    if selftest:
        ok = True
        ok = ok and (w.mask() is not None and not w.mask().isEmpty())
        g = QGuiApplication.primaryScreen().geometry()
        ok = ok and g.top() <= w._y <= g.bottom() - H + 1
        t1 = os.path.join(stash_dir, "selftest_a.txt")
        t2 = os.path.join(stash_dir, "selftest_b.txt")
        with open(t1, "w", encoding="utf-8") as f:
            f.write("a")
        with open(t2, "w", encoding="utf-8") as f:
            f.write("b")
        ok = ok and w.add_files([t1, t2]) == 2
        ok = ok and w.add_files([t1]) == 0          # 去重
        ok = ok and len(w._items) == 2
        w.clear_items()
        ok = ok and len(w._items) == 0
        ok = ok and not os.path.exists(t1) and not os.path.exists(t2)
        w.set_opacity(5.0)
        ok = ok and abs(w._opacity - 1.0) < 1e-6
        w.set_opacity(0.0)
        ok = ok and abs(w._opacity - 0.15) < 1e-6
        # 滑入滑出几何位置
        ok = ok and w._x_shown == w._screen_right - W + 1
        ok = ok and w._x_hidden == w._screen_right - HIDE + 1
        # 自动监测：开启后同步当前序号，无新变化则不新增
        w.set_auto_monitor(True)
        ok = ok and w._auto_monitor
        w._poll_clipboard()
        ok = ok and len(w._items) == 0
        w.set_auto_monitor(False)
        ok = ok and not w._auto_monitor
        # 文件与图片同时存在时应都暂存（修复浏览器复制图片漏检）
        t4 = os.path.join(stash_dir, "selftest_d.txt")
        with open(t4, "w", encoding="utf-8") as f:
            f.write("d")
        md = QMimeData()
        md.setUrls([QUrl.fromLocalFile(t4)])
        img = QImage(8, 8, QImage.Format_ARGB32)
        img.fill(QColor(10, 20, 30))
        md.setImageData(img)
        ok = ok and w._process_clipboard_content(md) == 2
        ok = ok and len(w._items) == 2
        w.clear_items()
        ok = ok and len(w._items) == 0
        # 退出即清空：closeEvent 应清掉暂存
        t3 = os.path.join(stash_dir, "selftest_c.txt")
        with open(t3, "w", encoding="utf-8") as f:
            f.write("c")
        w.add_files([t3])
        ok = ok and len(w._items) == 1
        w.close()
        ok = ok and len(w._items) == 0 and not os.path.exists(t3)
        try:
            os.remove(state_file)
        except OSError:
            pass
        print("SELFTEST " + ("OK" if ok else "FAILED"))
        return 0 if ok else 2

    return app.exec()


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Exception:
        import traceback
        try:
            with open(os.path.join(APP_DIR, "side_shelf_error.log"), "w",
                      encoding="utf-8") as f:
                traceback.print_exc(file=f)
        except Exception:
            pass
        print("启动失败，详情见 side_shelf_error.log", file=sys.stderr)
        sys.exit(1)
