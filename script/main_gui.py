import sys
import os
import logging
import traceback
import time
import winreg as winreg_module # 避免与变量名冲突
import locale # <-- 1. 导入 locale 模块
import numpy as np

# --- 修复：在程序启动时强制设置数字格式区域性 ---
# 这可以防止因操作系统区域设置（例如，使用逗号作为小数点）而导致的数值转换错误。
# 我们只设置LC_NUMERIC，以避免影响其他本地化功能（如日期、货币等）。
try:
    locale.setlocale(locale.LC_NUMERIC, 'C')
except locale.Error:
    logging.warning("Could not set the C locale for numeric formatting. Using system default.")

# --- 修复：将 winreg 的导入放在 try-except 块中 ---
try:
    import winreg
except ImportError:
    winreg = None

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLineEdit, QPushButton, QFileDialog, QLabel, QTextEdit,
    QProgressBar, QDoubleSpinBox, QComboBox, QFrame, QMessageBox,
    QSplashScreen, QListWidget, QStackedWidget, QDialog, QDialogButtonBox, QCheckBox,
    QAbstractItemView, QScrollArea, QGridLayout, QFormLayout, QTabWidget, QFontComboBox, QStyle,
    QSlider, QAbstractSpinBox
)
from PySide6.QtCore import Qt, QThread, QObject, Signal, Slot, QSettings, QTimer
from PySide6.QtGui import (
    QPixmap, QAction, QTextCursor, QBrush, QColor, QPainter, QPainterPath, QIcon, QFont, 
    QFontInfo, QFontDatabase, QHelpEvent
)

# --- 新增修复：显式导入SVG模块以确保插件被加载 ---
# 必须在 QApplication 实例化之前导入
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtSvgWidgets import QSvgWidget

# 导入我们项目中的模块
from logger_config import setup_logger, QtLogHandler
from localization import load_translations, t, get_available_languages
from main_workflow import process_model

# --- 修改：从新的核心枚举文件导入 ---
from core_enums import ProcessingStage, SortMode

# --- 删除以下已移动到 core_enums.py 的代码 ---
# from enum import Enum, auto
# class ProcessingStage(Enum):
#     ...
# class SortMode(Enum):
#     ...

# --- NEW: Helper function for PyInstaller paths ---
def resource_path(relative_path):
    """ Get absolute path to resource, works for dev and for PyInstaller """
    try:
        # PyInstaller creates a temp folder and stores path in _MEIPASS
        base_path = sys._MEIPASS
    except Exception:
        # --- 修复：使用 __file__ 来可靠地定位项目根目录 ---
        # In development, the base path is the project root
        base_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

    return os.path.join(base_path, relative_path)

# --- Fix: Use the helper function for all resource paths ---
ORGANIZATION_NAME = "wu_dl"
APPLICATION_NAME = "Polyvox"

# --- 新增：程序信息常量 ---
APP_VERSION = "1.0.0"
APP_AUTHOR = "wu_dl"
APP_LICENSE = "MIT License"
APP_GITHUB_URL = "https://github.com/wudl-21/polyvox.git" # 请替换为您的项目链接

# --- 修复：将缓存改为 set 以提高查找性能 ---
MONOSPACE_FONT_CACHE = set()

def populate_monospace_font_cache():
    """
    遍历系统字体一次，填充等宽字体缓存。
    这是一个耗时操作，只应在程序启动时调用一次。
    """
    global MONOSPACE_FONT_CACHE
    if MONOSPACE_FONT_CACHE: # 如果缓存已填充，则直接返回
        return
    
    msg = t("SPLASH_FONT_CACHE")
    logging.info(msg)
    # --- 修复：直接实例化 QFontDatabase 获取字体列表 ---
    font_database = QFontDatabase()
    
    monospace_fonts = set()
    for family in font_database.families():
        if font_database.isFixedPitch(family):
            monospace_fonts.add(family)
    MONOSPACE_FONT_CACHE = monospace_fonts
    msg = t("SPLASH_MONO_FONT_FOUND", count=len(MONOSPACE_FONT_CACHE))
    logging.info(msg)

# --- 新增：一个响应拖动和滚轮的自定义滑块 ---
class ValuePreviewSlider(QSlider):
    """
    一个自定义的QSlider，它能处理浮点数，响应鼠标滚轮。
    其数值预览和编辑功能由外部控件实现。
    """
    # 定义一个信号，当浮点值改变时发射
    floatValueChanged = Signal(float)

    def __init__(self, orientation, parent=None):
        super().__init__(orientation, parent)
        self._min_val = 0.0
        self._max_val = 1.0
        self._log_min = 0.0
        self._log_max = 0.0
        self.setRange(0, 1000) # 内部使用0-1000的整数范围
        
        # --- 核心修改：移除与内部工具提示相关的代码 ---
        # self.setToolTip("") 

        self.valueChanged.connect(self._emit_float_value)

    def setFloatRange(self, min_val, max_val):
        """设置滑块代表的真实浮点数范围。"""
        self._min_val = min_val
        self._max_val = max_val
        self._log_min = np.log10(min_val)
        self._log_max = np.log10(max_val)

    def floatValue(self):
        """获取当前滑块位置对应的浮点值。"""
        log_val = self._log_min + (self.value() / 1000.0) * (self._log_max - self._log_min)
        return 10**log_val

    def setFloatValue(self, value):
        """根据浮点值设置滑块的位置。"""
        log_val = np.log10(max(self._min_val, value))
        slider_val = int(((log_val - self._log_min) / (self._log_max - self._log_min)) * 1000.0)
        self.setValue(slider_val)

    def wheelEvent(self, event):
        """重写滚轮事件，以小步长调整滑块。"""
        if event.angleDelta().y() > 0:
            self.setValue(self.value() + 1)
        else:
            self.setValue(self.value() - 1)
        event.accept()

    # --- 核心修改：移除所有与工具提示预览相关的事件处理器 ---
    # def mouseMoveEvent(self, event): ...
    # def enterEvent(self, event): ...
    # def _update_tooltip(self): ...

    def _emit_float_value(self):
        """当内部整数值改变时，发射包含浮点值的信号。"""
        self.floatValueChanged.emit(self.floatValue())


class MaterialPropertyManager:
    """
    A central class to define and manage properties for different material types.
    This is the single source of truth for material properties.
    """
    # --- 修复：将PROPERTIES结构化，包含翻译ID和属性列表 ---
    PROPERTIES = {
        "metal": {
            "label_id": "VOX_MAT_METAL",
            "properties": [
                {"name": "rough", "label_id": "PROP_ROUGHNESS", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 0.2},
                {"name": "spec", "label_id": "PROP_SPECULAR", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 0.8},
                {"name": "ior", "label_id": "PROP_IOR", "type": "float", "min": 1.0, "max": 3.0, "step": 0.01, "default": 1.4},
                {"name": "metal", "label_id": "PROP_METALLIC", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 1.0},
            ]
        },
        "glass": {
            "label_id": "VOX_MAT_GLASS",
            "properties": [
                {"name": "rough", "label_id": "PROP_ROUGHNESS", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 0.05},
                {"name": "ior", "label_id": "PROP_IOR", "type": "float", "min": 1.0, "max": 3.0, "step": 0.01, "default": 1.5},
                {"name": "trans", "label_id": "PROP_TRANSPARENCY", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 0.9},
            ]
        },
        "emit": {
            "label_id": "VOX_MAT_EMIT",
            "properties": [
                {"name": "emission", "label_id": "PROP_EMISSION", "type": "float", "min": 0.0, "max": 16.0, "step": 0.1, "default": 1.0},
                {"name": "power", "label_id": "PROP_POWER", "type": "float", "min": 0.0, "max": 64.0, "step": 0.1, "default": 4.0},
                {"name": "ldr", "label_id": "PROP_LDR", "type": "float", "min": 0.0, "max": 1.0, "step": 0.01, "default": 0.0},
            ]
        },
        "diffuse": {
            "label_id": "VOX_MAT_DIFFUSE",
            "properties": []
        },
    }

    @staticmethod
    def get_properties_for_type(tech_name):
        return MaterialPropertyManager.PROPERTIES.get(tech_name, {}).get("properties", [])

    @staticmethod
    def create_property_widgets(prop_definitions, parent=None):
        widgets = {}
        for prop in prop_definitions:
            if prop["type"] == "float":
                widget = QDoubleSpinBox(parent)
                widget.setRange(prop["min"], prop["max"])
                widget.setSingleStep(prop["step"])
                widget.setValue(prop["default"])
                widget.setDecimals(3)
                widgets[prop["name"]] = widget
        return widgets

# --- 新增：一个辅助函数，用于检测非ASCII字符 ---
def contains_non_ascii(text):
    """如果字符串包含任何非ASCII字符，则返回True。"""
    return not text.isascii()

def pre_check_dependencies(obj_path):
    """
    预检查OBJ文件的依赖项（MTL和纹理）以及文件名。
    返回一个元组 (status_message, is_ok)，is_ok表示所有硬性依赖是否都找到。
    消息内容支持HTML格式。
    """
    if not obj_path or not os.path.exists(obj_path):
        return t("GUI_PRECHECK_NO_FILE"), False
    if not obj_path.lower().endswith('.obj'):
        return t("GUI_PRECHECK_NOT_OBJ"), False

    obj_dir = os.path.dirname(obj_path)
    obj_basename = os.path.splitext(os.path.basename(obj_path))[0]
    
    # --- 核心修改：将所有检查结果收集到一个列表中 ---
    results = []
    hard_fail = False

    # 1. 检查文件名中的非ASCII字符 (警告)
    if contains_non_ascii(obj_basename):
        results.append(f"<span style='color: #d9a300;'>{t('GUI_PRECHECK_OBJ_NAME_NON_ASCII')}</span>")

    # 2. 查找MTL文件引用 (错误)
    mtl_filename = None
    try:
        with open(obj_path, 'r', encoding='utf-8') as f:
            for line in f:
                if line.strip().startswith('mtllib'):
                    mtl_filename = line.strip().split(maxsplit=1)[1]
                    break
    except Exception as e:
        results.append(f"<span style='color: red;'>{t('GUI_PRECHECK_OBJ_READ_ERROR', error=e)}</span>")
        return "<br>".join(results), False

    if not mtl_filename:
        results.append(f"<span style='color: green;'>{t('GUI_PRECHECK_NO_MTL_REF')}</span>")
        return "<br>".join(results), True

    mtl_path = os.path.join(obj_dir, mtl_filename)
    if not os.path.exists(mtl_path):
        results.append(f"<span style='color: red;'>{t('GUI_PRECHECK_MTL_NOT_FOUND', filename=mtl_filename)}</span>")
        return "<br>".join(results), False

    # 3. 查找纹理文件引用 (错误)
    texture_files = []
    try:
        with open(mtl_path, 'r', encoding='utf-8') as f:
            for line in f:
                if line.strip().startswith('map_Kd'):
                    texture_files.append(line.strip().split(maxsplit=1)[1])
    except Exception as e:
        results.append(f"<span style='color: red;'>{t('GUI_PRECHECK_MTL_READ_ERROR', error=e)}</span>")
        return "<br>".join(results), False

    missing_textures = [tex for tex in texture_files if not os.path.exists(os.path.join(obj_dir, tex))]

    if missing_textures:
        hard_fail = True
        files_html = "<ul>" + "".join(f"<li>{tex}</li>" for tex in missing_textures[:3]) + "</ul>"
        if len(missing_textures) > 3:
            files_html += f"{t('GUI_PRECHECK_TEX_MISSING_OMITTED', num=len(missing_textures)-3)}"
        results.append(f"<span style='color: red;'>{t('GUI_PRECHECK_TEX_MISSING')}</span>{files_html}")
    
    # 4. 最终总结
    if not hard_fail:
        results.append(f"<span style='color: green;'>{t('GUI_PRECHECK_ALL_FOUND')}</span>")

    return "<br>".join(results), not hard_fail

def get_system_theme():
    """检测Windows系统的应用主题，返回 'dark' 或 'light'"""
    if winreg:
        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
            value, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
            return "light" if value > 0 else "dark"
        except FileNotFoundError:
            # 注册表项不存在，通常是较旧的Windows版本
            return "light"
    return "light" # 非Windows系统或发生错误时，默认为亮色

def apply_application_styles(app, config):
    """
    加载并应用完整的应用程序样式，包括基础主题和动态生成的日志字体规则。
    这是应用样式的唯一入口点。
    """
    # 1. 确定基础主题
    theme_name = get_system_theme() if config.get("follow_system", True) else config.get("theme", "light")
    
    # 2. 加载基础主题QSS
    base_qss = ""
    qss_path = resource_path(f"script/themes/{theme_name}.qss")
    try:
        with open(qss_path, "r", encoding="utf-8") as f:
            base_qss = f.read()
    except Exception as e:
        msg = t("PREF_THEME_LOAD_ERROR", path=qss_path, e=str(e))
        logging.warning(msg)

    # 3. 生成日志字体QSS规则
    font_family = config.get("log_font_family", "Consolas")
    font_size = config.get("log_font_size", 10)
    log_font_qss = f"""
    QTextEdit#LogOutput {{
        font-family: "{font_family}";
        font-size: {font_size}pt;
    }}
    """

    # 4. 合并并应用样式表
    final_qss = base_qss + log_font_qss
    app.setStyleSheet(final_qss)

# --- 废弃：旧的 apply_theme 函数，其逻辑已被 apply_application_styles 取代 ---
# def apply_theme(app, theme_name):
#     ...

class Worker(QObject):
    """
    工作线程，用于执行耗时任务。
    """
    # --- 修改：添加一个专门用于表示成功的信号 ---
    success = Signal()
    finished = Signal()
    error = Signal(str)
    progress = Signal(int, int)
    # --- 修改：使用新的 stage_changed 信号替换 status 信号 ---
    stage_changed = Signal(ProcessingStage, str)
    stop_signal = Signal()

    # --- 修复：在构造函数中接收 material_properties 和 temp_dir_path ---
    def __init__(self, obj_path, out_dir, polyvox_exe, voxel_size, lang, material_maps=None, material_properties=None, temp_dir_path=None, angle_tol=1e-5, dist_tol=1e-4):
        super().__init__()
        self.obj_path = obj_path
        self.out_dir = out_dir
        self.polyvox_exe = polyvox_exe
        self.voxel_size = voxel_size
        self.lang = lang
        self.material_maps = material_maps
        self.material_properties = material_properties
        self.temp_dir_path = temp_dir_path # <-- 新增
        self._should_stop = False
        # --- 新增：存储容差值 ---
        self.angle_tol = angle_tol
        self.dist_tol = dist_tol

    @Slot()
    def run(self):
        try:
            def progress_callback(current, total):
                if self._should_stop:
                    raise RuntimeError(t("GUI_USER_STOPPED"))
                self.progress.emit(current, total)

            def stage_callback(stage, status_text):
                if self._should_stop:
                    raise RuntimeError(t("GUI_USER_STOPPED"))
                self.stage_changed.emit(stage, status_text)

            def stop_check_callback():
                return self._should_stop

            process_model(
                self.obj_path, self.out_dir, self.polyvox_exe,
                self.voxel_size, self.lang, 
                progress_callback=progress_callback,
                stage_callback=stage_callback,
                stop_check_callback=stop_check_callback,
                material_maps=self.material_maps,
                material_properties=self.material_properties,
                temp_dir_path=self.temp_dir_path, # <-- 新增
                # --- 新增：传递容差参数 ---
                angle_tol=self.angle_tol,
                dist_tol=self.dist_tol
            )
            
            if not self._should_stop:
                self.success.emit()

        except RuntimeError as e:
            # 捕获由中止操作引发的 RuntimeError
            if str(e) == t("GUI_USER_STOPPED"):
                logging.info(t("GUI_USER_STOPPED"))
            else:
                # 其他未预料到的 RuntimeError
                if not self._should_stop:
                    self.error.emit(str(e))
        except Exception as e:
            # 只有在真正发生错误时才发射 error 信号
            if not self._should_stop: # 避免将用户中止也报告为错误
                self.error.emit(str(e))
        finally:
            self.finished.emit()

    def stop(self):
        self._should_stop = True

class PreviewWidget(QWidget):
    """一个专门用于显示材质预览（纹理或颜色）并叠加一个彩色角标的控件。"""
    # --- 新增：定义一个带字符串参数的信号 ---
    clicked = Signal(str)

    def __init__(self, material_name="", parent=None):
        super().__init__(parent)
        self._pixmap = None
        self._background_color = None
        self._badge_color = None
        # --- 新增：用于存储占位文本 ---
        self._placeholder_text = ""
        # --- 新增：存储自身的材质名称 ---
        self.material_name = material_name
        self.setToolTip("")

    # --- 新增：处理鼠标点击事件 ---
    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton:
            self.clicked.emit(self.material_name)
        super().mousePressEvent(event)

    def set_pixmap(self, pixmap):
        self._pixmap = pixmap
        # --- 新增：设置纹理时清除占位文本 ---
        if pixmap:
            self._placeholder_text = ""
        self.update() # 请求重绘

    def set_background_color(self, color):
        self._background_color = color
        # --- 新增：设置背景色时清除占位文本 ---
        if color:
            self._placeholder_text = ""
        self.update()

    def set_badge_color(self, color):
        self._badge_color = color
        self.update()

    # --- 新增：设置占位文本的方法 ---
    def set_placeholder_text(self, text):
        self._placeholder_text = text
        self.update()

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        # --- 修复：创建一个带圆角的剪切路径，确保所有绘制都在边框内 ---
        border_radius = 4
        border_path = QPainterPath()
        # 调整矩形，为1像素的边框留出空间
        border_path.addRoundedRect(self.rect().adjusted(0, 0, -1, -1), border_radius, border_radius)
        
        painter.save()
        painter.setClipPath(border_path) # 应用剪切路径

        # 1. 绘制背景 (纹理或纯色)
        if self._pixmap and not self._pixmap.isNull():
            # --- 修复：保持宽高比绘制纹理，并将其居中 ---
            target_rect = self.rect()
            # 将 pixmap 缩放到适合 target_rect 的尺寸，同时保持宽高比
            scaled_pixmap = self._pixmap.scaled(target_rect.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
            # 计算居中后的左上角坐标
            pixmap_rect = scaled_pixmap.rect()
            pixmap_rect.moveCenter(target_rect.center())
            # 在计算好的位置绘制缩放后的 pixmap
            painter.drawPixmap(pixmap_rect, scaled_pixmap)
        elif self._background_color:
            painter.fillRect(self.rect(), self._background_color)
        # --- 新增：如果以上都没有，则绘制占位文本 ---
        elif self._placeholder_text:
            painter.setPen(QColor("grey"))
            # 使用 TextWordWrap 以便长文本可以自动换行
            painter.drawText(self.rect(), Qt.AlignCenter | Qt.TextWordWrap, self._placeholder_text)

        painter.restore() # 恢复绘制状态，以便边框和角标可以画在剪切区域之外

        # 2. 绘制边框
        painter.setPen(QColor("grey"))
        painter.drawPath(border_path)

        # 3. 绘制角标
        if self._badge_color and self._badge_color.alpha() > 0:
            # --- 修复：创建不透明的角标颜色 ---
            opaque_badge_color = QColor(self._badge_color)
            opaque_badge_color.setAlpha(255)

            # --- 新增：根据控件大小动态调整角标大小 ---
            badge_size = max(8, int(self.width() * 0.15)) # 角标大小为宽度的15%，但最小为8px
            path = QPainterPath()
            path.moveTo(self.width(), self.height() - badge_size)
            path.lineTo(self.width(), self.height())
            path.lineTo(self.width() - badge_size, self.height())
            path.closeSubpath()
            painter.fillPath(path, QBrush(opaque_badge_color))

# --- 新增：一个自定义对话框基类，用于修复回车键自动关闭的问题 ---
class NonClosingDialog(QDialog):
    """
    一个自定义的QDialog子类，它重写了键盘事件处理逻辑。
    当焦点在任何子控件（而不是按钮）上时，按下回车键不会触发对话框的accept()方法。
    这可以防止在SpinBox或LineEdit中输入后按回车导致对话框意外关闭。
    """
    def keyPressEvent(self, event):
        # 检查按下的键是否是回车键
        if event.key() == Qt.Key_Return or event.key() == Qt.Key_Enter:
            # 检查当前拥有焦点的控件是否是一个按钮
            focused_widget = QApplication.focusWidget()
            if not isinstance(focused_widget, QPushButton):
                # 如果焦点不在按钮上，则忽略此回车事件，不让它关闭对话框。
                # 我们可以选择性地让它表现得像按下了Tab键，将焦点移到下一个控件。
                self.focusNextPrevChild(True)
                return # 阻止事件继续传播
        
        # 对于所有其他按键，执行默认行为
        super().keyPressEvent(event)


# --- 修改：让首选项对话框继承自我们新的 NonClosingDialog ---
class PreferencesDialog(NonClosingDialog):
    def __init__(self, current_config, parent=None):
        super().__init__(parent)
        
        # --- 修改：self.config 现在只是一个初始值 ---
        self.config = {} 
        self.app = QApplication.instance()
        self.resize(500, 400)

        self.available_languages = get_available_languages()
        self.MATERIAL_PROFILES = MaterialMappingDialog.get_material_profiles()
        self.property_widgets = {}

        main_layout = QHBoxLayout(self)
        
        self.category_list = QListWidget()
        self.category_list.setFixedWidth(120)
        main_layout.addWidget(self.category_list)

        self.pages_stack = QStackedWidget()
        main_layout.addWidget(self.pages_stack)

        self.create_general_page()
        self.create_appearance_page()
        self.create_materials_page()
        self.create_advanced_page()
        
        self.category_list.currentRowChanged.connect(self.pages_stack.setCurrentIndex)
        
        self.button_box = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        self.button_box.accepted.connect(self.accept)
        self.button_box.rejected.connect(self.reject)
        
        right_layout = QVBoxLayout()
        right_layout.addWidget(self.pages_stack)
        right_layout.addWidget(self.button_box)
        main_layout.addLayout(right_layout)

        self.category_list.setCurrentRow(0)
        
        # --- 修改：使用新的加载方法来完成初始化 ---
        self.load_config(current_config)
        self.retranslate_ui()

    def load_config(self, config_data):
        """
        一个公共方法，用于从外部加载配置并更新所有UI控件的状态。
        这是实现对话框复用的关键。
        """
        self.config = config_data.copy() # 创建一个副本以进行隔离操作

        # 更新通用页面
        current_lang_code = self.config.get("lang", "en")
        index = self.lang_combo.findData(current_lang_code)
        if index != -1:
            self.lang_combo.setCurrentIndex(index)
        self.temp_dir_edit.setText(self.config.get("temp_dir_path", ""))

        # 更新外观页面
        self.theme_combo.setCurrentText(self.config.get("theme", "Light").capitalize())
        self.follow_system_checkbox.setChecked(self.config.get("follow_system", True))
        current_font_family = self.config.get("log_font_family", "Consolas")
        current_font_size = self.config.get("log_font_size", 10)
        self.log_font_combo.setCurrentFont(QFont(current_font_family))
        self.log_font_size_combo.setCurrentText(str(current_font_size))
        
        # 更新材质预设页面
        self._load_all_presets()

        # --- 核心修复：使用新的自定义滑块加载高级设置 ---
        self.angle_tol_slider.setFloatValue(self.config.get("angle_tol", 1e-5))
        self.dist_tol_slider.setFloatValue(self.config.get("dist_tol", 1e-4))

    def retranslate_ui(self):
        """更新此对话框中的所有UI文本"""
        self.setWindowTitle(t("PREF_TITLE"))
        self.category_list.item(0).setText(t("PREF_CAT_GENERAL"))
        self.category_list.item(1).setText(t("PREF_CAT_APPEARANCE"))
        self.category_list.item(2).setText(t("PREF_CAT_MATERIALS"))
        self.category_list.item(3).setText(t("PREF_CAT_ADVANCED"))
        # 通用页面
        self.general_page_lang_label.setText(t("GUI_LANGUAGE"))
        self.temp_dir_label.setText(t("PREF_GENERAL_TEMP_DIR"))
        # 外观页面
        self.appearance_page_theme_label.setText(t("PREF_APPEARANCE_THEME"))
        self.follow_system_checkbox.setText(t("PREF_APPEARANCE_FOLLOW_SYSTEM"))
        self.log_font_label.setText(t("PREF_APPEARANCE_LOG_FONT"))
        self.log_font_size_label.setText(t("PREF_APPEARANCE_LOG_FONT_SIZE"))
        # 重置按钮
        self.reset_settings_button.setText(t("PREF_RESET_BUTTON"))
        # 临时目录及浏览按钮
        self.temp_dir_edit.setPlaceholderText(t("PREF_GENERAL_TEMP_DIR_PLACEHOLDER"))
        self.browse_temp_dir_button.setText(t("GUI_BROWSE_BUTTON"))

        # --- 修复：简化并修正材质页面的文本更新逻辑 ---
        # 1. 更新材质选项卡的标题
        updated_profiles = MaterialMappingDialog.get_material_profiles()
        for i in range(self.material_tabs.count()):
            # 从布局中获取 tech_name
            # 注意：这里的查找逻辑依赖于布局结构，如果布局改变，这里可能需要调整
            tech_name = self.material_tabs.widget(i).findChild(QComboBox).property("tech_name")
            if tech_name in updated_profiles:
                self.material_tabs.setTabText(i, updated_profiles[tech_name]["display"])

        # 2. 更新每个选项卡内部的控件文本
        for tech_name, controls in self.property_widgets.items():
            # 更新 "渲染材质类型:" 标签
            controls["vox_type_label"].setText(f"<b>{t('MAT_MAP_VOX_TYPE')}:</b>")

            # 更新 VOX 类型下拉框中的项目文本
            combo = controls["vox_type_combo"]
            combo.blockSignals(True)
            for i in range(combo.count()):
                vox_tech_name = combo.itemData(i)
                if vox_tech_name in MaterialPropertyManager.PROPERTIES:
                    label_id = MaterialPropertyManager.PROPERTIES[vox_tech_name]['label_id']
                    combo.setItemText(i, t(label_id))
            combo.blockSignals(False)
            
            # 重新构建属性编辑器以更新其内部的标签
            self._rebuild_preset_property_editor(tech_name)

        # --- 新增：更新高级页面文本 ---
        self.grouping_tolerance_label.setText(f"<b>{t('PREF_ADVANCED_GROUPING_TOLERANCE')}</b>")
        self.angle_tol_label.setText(t("PREF_ADVANCED_ANGLE_TOL"))
        self.dist_tol_label.setText(t("PREF_ADVANCED_DIST_TOL"))
        self.angle_tol_help.setToolTip(t("PREF_ADVANCED_ANGLE_TOL_TOOLTIP_SIMPLE"))
        self.dist_tol_help.setToolTip(t("PREF_ADVANCED_DIST_TOL_TOOLTIP_SIMPLE"))

    def create_general_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        
        form_layout = QFormLayout()
        form_layout.setRowWrapPolicy(QFormLayout.WrapAllRows)

        self.lang_combo = QComboBox()
        self.lang_combo.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        
        current_lang_code = self.config.get("lang", "en")
        current_index = 0
        i = 0
        for code, name in self.available_languages.items():
            self.lang_combo.addItem(name, code)
            if code == current_lang_code:
                current_index = i
            i += 1
        self.lang_combo.setCurrentIndex(current_index)

        self.general_page_lang_label = QLabel()
        form_layout.addRow(self.general_page_lang_label, self.lang_combo)

        self.temp_dir_label = QLabel()
        temp_dir_layout = QHBoxLayout()
        # --- 修复：在这里也使用我们增强后的 PathLineEdit ---
        self.temp_dir_edit = PathLineEdit()
        self.temp_dir_edit.setPlaceholderText(t("PREF_GENERAL_TEMP_DIR_PLACEHOLDER"))
        self.temp_dir_edit.setText(self.config.get("temp_dir_path", ""))
        temp_dir_layout.addWidget(self.temp_dir_edit)
        self.browse_temp_dir_button = QPushButton(t("GUI_BROWSE_BUTTON"))
        self.browse_temp_dir_button.setFixedSize(25, 25)
        self.browse_temp_dir_button.clicked.connect(self._browse_temp_dir)
        temp_dir_layout.addWidget(self.browse_temp_dir_button)
        form_layout.addRow(self.temp_dir_label, temp_dir_layout)

        layout.addLayout(form_layout)
        layout.addStretch()

        self.reset_settings_button = QPushButton()
        self.reset_settings_button.clicked.connect(self._reset_settings)
        reset_layout = QHBoxLayout()
        reset_layout.addStretch()
        reset_layout.addWidget(self.reset_settings_button)
        layout.addLayout(reset_layout)

        self.category_list.addItem(t("PREF_CAT_GENERAL"))
        self.pages_stack.addWidget(page)

    def _browse_temp_dir(self):
        path = QFileDialog.getExistingDirectory(
            self,
            t("PREF_GENERAL_TEMP_DIR_TITLE"),
            self.temp_dir_edit.text()
        )
        if path:
            self.temp_dir_edit.setText(path)

    def create_appearance_page(self):
        """创建外观设置页面"""
        page = QWidget()
        layout = QVBoxLayout(page)
        
        # 主题选择
        theme_layout = QHBoxLayout()
        self.appearance_page_theme_label = QLabel()
        theme_layout.addWidget(self.appearance_page_theme_label)
        self.theme_combo = QComboBox()
        self.theme_combo.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        self.theme_combo.addItems(["Light", "Dark"]) # 预设主题列表
        theme_layout.addWidget(self.theme_combo)
        theme_layout.addStretch()
        layout.addLayout(theme_layout)

        # 跟随系统
        self.follow_system_checkbox = QCheckBox()
        layout.addWidget(self.follow_system_checkbox)
        
        layout.addSpacing(10)

        # --- 修复：将日志字体设置放入带边框的容器中，并使用并列布局 ---
        log_font_group = QFrame()
        log_font_group.setProperty("frame-style", "bordered") # For QSS styling
        log_font_group.setFrameShape(QFrame.StyledPanel)
        
        grid_layout = QGridLayout(log_font_group)
        grid_layout.setContentsMargins(10, 10, 10, 10)

        self.log_font_label = QLabel()
        self.log_font_combo = QFontComboBox()
        
        self.log_font_size_label = QLabel()
        self.log_font_size_combo = QComboBox()
        self.log_font_size_combo.addItems([str(s) for s in range(8, 26, 2)])

        grid_layout.addWidget(self.log_font_label, 0, 0)
        grid_layout.addWidget(self.log_font_combo, 0, 1)
        grid_layout.addWidget(self.log_font_size_label, 0, 2)
        grid_layout.addWidget(self.log_font_size_combo, 0, 3)
        grid_layout.setColumnStretch(1, 1) # 让字体下拉框优先伸展

        layout.addWidget(log_font_group)

        layout.addStretch()

        self.category_list.addItem(t("PREF_CAT_APPEARANCE"))
        self.pages_stack.addWidget(page)

        self.follow_system_checkbox.stateChanged.connect(self._on_follow_system_changed)
        self.theme_combo.currentTextChanged.connect(self._on_theme_preview)

        self.theme_combo.setCurrentText(self.config.get("theme", "Light").capitalize())
        self.follow_system_checkbox.setChecked(self.config.get("follow_system", True))
        self._on_follow_system_changed()

        current_font_family = self.config.get("log_font_family", "Consolas")
        current_font_size = self.config.get("log_font_size", 10)
        self.log_font_combo.setCurrentFont(QFont(current_font_family))
        self.log_font_size_combo.setCurrentText(str(current_font_size))

        self.log_font_combo.setCurrentFont(QFont(current_font_family))
        self.log_font_size_combo.setCurrentText(str(current_font_size))
                    
    def _on_follow_system_changed(self):
        is_following = self.follow_system_checkbox.isChecked()
        self.theme_combo.setEnabled(not is_following)
        
        self.config["follow_system"] = is_following
        if is_following:
            system_theme = get_system_theme()
            self.theme_combo.setCurrentText(system_theme.capitalize())
        else:
            # --- 修复：当取消勾选时，确保下拉框与当前配置的非系统主题同步 ---
            current_theme = self.config.get("theme", "light")
            self.theme_combo.setCurrentText(current_theme.capitalize())
        
        apply_application_styles(self.app, self.config)

    def _on_theme_preview(self, theme_name):
        if not self.follow_system_checkbox.isChecked():
            # --- 修复：调用新的全局样式函数 ---
            # 临时更新配置以反映下拉框的选择
            self.config["theme"] = theme_name.lower()
            apply_application_styles(self.app, self.config)

    def _reset_settings(self):
        reply = QMessageBox.question(
            self,
            t("PREF_RESET_CONFIRM_TITLE"),
            t("PREF_RESET_CONFIRM_MSG"),
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No
        )
        if reply == QMessageBox.Yes:
            settings = QSettings(ORGANIZATION_NAME, APPLICATION_NAME)
            settings.setValue("reset_on_next_run", True)
            settings.sync()
            QMessageBox.information(
                self,
                t("PREF_RESET_COMPLETE_TITLE"),
                t("PREF_RESET_COMPLETE_MSG")
            )
            self.app.quit()

    def reject(self):
        """重写 reject 方法，以便在取消时恢复主题预览。"""
        # 在对话框关闭前，将主题恢复到打开时的状态
        # 这是通过重新应用主窗口传递过来的原始配置实现的
        apply_application_styles(self.app, self.parent().config)
        super().reject()

    # --- 修复：完全重构材质页面以匹配新逻辑 ---
    def create_materials_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        self.material_tabs = QTabWidget()
        
        for tech_name, profile in self.MATERIAL_PROFILES.items():
            if not tech_name or tech_name == "$TD_auto": continue

            tab_widget = QWidget()
            tab_layout = QVBoxLayout(tab_widget)
            
            vox_type_layout = QHBoxLayout()
            
            # --- 修复：创建控件，但将它们添加到统一的字典中 ---
            vox_type_label = QLabel() # 文本将在 retranslate_ui 中设置
            vox_type_combo = QComboBox()
            vox_type_combo.setProperty("tech_name", tech_name)
            vox_type_combo.setSizeAdjustPolicy(QComboBox.AdjustToContents)
            
            for vox_tech_name, vox_profile in MaterialPropertyManager.PROPERTIES.items():
                vox_type_combo.addItem(t(vox_profile['label_id']), vox_tech_name)
            
            vox_type_layout.addWidget(vox_type_label)
            vox_type_layout.addWidget(vox_type_combo)
            vox_type_layout.addStretch()
            tab_layout.addLayout(vox_type_layout)

            prop_editor_frame = QFrame()
            prop_editor_frame.setProperty("frame-style", "bordered")
            prop_editor_frame.setFrameShape(QFrame.StyledPanel)
            prop_editor_layout = QFormLayout(prop_editor_frame)
            prop_editor_layout.setContentsMargins(10, 10, 10, 10)
            prop_editor_layout.setSpacing(8)
            tab_layout.addWidget(prop_editor_frame)
            tab_layout.addStretch()

            # --- 修复：使用统一的数据结构来存储所有相关控件 ---
            self.property_widgets[tech_name] = {
                "vox_type_label": vox_type_label, # 将标签也包含进来
                "vox_type_combo": vox_type_combo,
                "prop_editor_layout": prop_editor_layout,
                "prop_widgets": {}
            }

            vox_type_combo.currentTextChanged.connect(
                lambda text, tn=tech_name: self._rebuild_preset_property_editor(tn)
            )

            self.material_tabs.addTab(tab_widget, profile["display"])

        layout.addWidget(self.material_tabs)
        self.category_list.addItem(t("PREF_CAT_MATERIALS"))
        self.pages_stack.addWidget(page)
        
        self._load_all_presets()

    def _load_preset_widgets(self, tech_name):
        """加载一个 Teardown 材质类型 (如 $TD_metal) 的完整预设值。"""
        # 1. 获取此 TD 材质对应的控件组
        controls = self.property_widgets.get(tech_name)
        if not controls: return

        # 2. 从配置中确定此 TD 材质应使用的 vox_type (渲染材质类型)
        #    例如，对于 tech_name='$TD_metal'，其 vox_type 可能是 'metal' 或 'emit'
        config_vox_type_key = f"preset/{tech_name}/vox_type"
        vox_type = self.config.get(config_vox_type_key, "diffuse")

        # 3. 将 vox_type 设置到下拉框中
        combo = controls["vox_type_combo"]
        index = combo.findData(vox_type)
        if index != -1:
            combo.setCurrentIndex(index)
        
        # 4. 手动触发属性编辑器重建，以确保控件与所选 vox_type 匹配
        #    这一步至关重要，因为它会创建正确的属性控件 (spinbox)
        self._rebuild_preset_property_editor(tech_name)

        # 5. 现在，控件已正确创建，可以加载每个属性的值
        for prop_name, widget in controls["prop_widgets"].items():
            config_prop_key = f"preset/{tech_name}/prop_{prop_name}"
            
            # 从 MaterialPropertyManager 中为当前 vox_type 查找正确的默认值
            prop_definitions = MaterialPropertyManager.get_properties_for_type(vox_type)
            default_value = next((p['default'] for p in prop_definitions if p['name'] == prop_name), 0.0)

            value = self.config.get(config_prop_key, default_value)
            widget.setValue(float(value))

    def _rebuild_preset_property_editor(self, tech_name):
        tab_controls = self.property_widgets[tech_name]
        layout = tab_controls["prop_editor_layout"]
        
        # --- 修复：使用 QFormLayout.removeRow() 来保证完全清理 ---
        # 这是清理 QFormLayout 最健壮的方法，它会同时删除标签和字段控件，
        # 从而从根本上解决孤儿控件和内存泄漏问题。
        while layout.rowCount() > 0:
            layout.removeRow(0)

        tab_controls["prop_widgets"].clear()

        vox_type = tab_controls["vox_type_combo"].currentData()
        prop_list = MaterialPropertyManager.PROPERTIES.get(vox_type, {}).get("properties", [])

        if prop_list:
            widgets = MaterialPropertyManager.create_property_widgets(prop_list, self)
            tab_controls["prop_widgets"] = widgets
            for prop in prop_list:
                layout.addRow(f"{t(prop['label_id'])}:", widgets[prop['name']])
                config_key = f"preset/{tech_name}/prop_{prop['name']}"
                default_value = prop['default']
                value = self.config.get(config_key, default_value)
                widgets[prop['name']].setValue(float(value))
        else:
            placeholder_label = QLabel(t("MAT_MAP_NO_PROPS"))
            placeholder_label.setStyleSheet("font-style: italic; color: grey;")
            layout.addRow(placeholder_label)

    def _load_all_presets(self):
        for tech_name, controls in self.property_widgets.items():
            self._load_preset_widgets(tech_name)

    # --- 核心重构：创建高级设置页面 ---
    def create_advanced_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        
        self.grouping_tolerance_label = QLabel()
        layout.addWidget(self.grouping_tolerance_label)
        
        tolerance_group = QFrame()
        tolerance_group.setProperty("frame-style", "bordered")
        tolerance_group.setFrameShape(QFrame.StyledPanel)
        group_layout = QVBoxLayout(tolerance_group)
        group_layout.setContentsMargins(10, 15, 10, 15)
        group_layout.setSpacing(20)

        # --- 核心修复：接收并存储由辅助函数创建的标签和帮助图标 ---
        # 1. 创建角度容差控件
        self.angle_tol_slider, self.angle_tol_spinbox, self.angle_tol_label, self.angle_tol_help = self._create_tolerance_control(
            group_layout,
            label_text=t("PREF_ADVANCED_ANGLE_TOL"),
            tooltip_text=t("PREF_ADVANCED_ANGLE_TOL_TOOLTIP_SIMPLE"),
            min_val=1e-9, max_val=0.5
        )

        # 2. 创建距离容差控件
        self.dist_tol_slider, self.dist_tol_spinbox, self.dist_tol_label, self.dist_tol_help = self._create_tolerance_control(
            group_layout,
            label_text=t("PREF_ADVANCED_DIST_TOL"),
            tooltip_text=t("PREF_ADVANCED_DIST_TOL_TOOLTIP_SIMPLE"),
            min_val=1e-9, max_val=0.1
        )

        layout.addWidget(tolerance_group)
        layout.addStretch()

        self.category_list.addItem(t("PREF_CAT_ADVANCED"))
        self.pages_stack.addWidget(page)

    # --- 核心重构：创建复合控件的辅助函数 ---
    def _create_tolerance_control(self, parent_layout, label_text, tooltip_text, min_val, max_val):
        """在一个给定的布局中，创建一个完整的、带标签、输入框、滑块和刻度的容差控件组。"""
        # 1. 创建顶部布局 (标签 + 输入框 + 帮助图标)
        top_layout = QHBoxLayout()
        label = QLabel(label_text)
        spinbox = QDoubleSpinBox()
        help_icon = self._create_help_icon()
        help_icon.setToolTip(tooltip_text)
        
        spinbox.setDecimals(8)
        spinbox.setSingleStep(1e-6)
        spinbox.setRange(min_val, max_val)
        spinbox.setButtonSymbols(QAbstractSpinBox.NoButtons) # 隐藏上下箭头

        top_layout.addWidget(label)
        top_layout.addStretch()
        top_layout.addWidget(spinbox)
        top_layout.addWidget(help_icon)
        
        # 2. 创建滑块
        slider = ValuePreviewSlider(Qt.Horizontal)
        slider.setFloatRange(min_val, max_val)

        # 3. 创建底部刻度布局
        ticks_layout = QHBoxLayout()
        ticks_layout.setContentsMargins(10, 0, 10, 0)
        num_ticks = 5 # 总共显示5个刻度
        for i in range(num_ticks):
            pos = i / (num_ticks - 1)
            log_val = slider._log_min + pos * (slider._log_max - slider._log_min)
            tick_val = 10**log_val
            tick_label = QLabel(f"{tick_val:.1e}") # 使用科学计数法显示
            tick_label.setAlignment(Qt.AlignCenter)
            ticks_layout.addWidget(tick_label)
            if i < num_ticks - 1:
                ticks_layout.addStretch()
        
        # 4. 连接信号与槽，实现双向绑定
        slider.floatValueChanged.connect(lambda v, s=spinbox: (s.blockSignals(True), s.setValue(v), s.blockSignals(False)))
        spinbox.valueChanged.connect(slider.setFloatValue)

        # 5. 将所有控件添加到父布局中
        parent_layout.addLayout(top_layout)
        parent_layout.addWidget(slider)
        parent_layout.addLayout(ticks_layout)

        # --- 核心修复：返回所有需要被外部引用的控件 ---
        return slider, spinbox, label, help_icon

    # --- 新增：创建帮助图标的辅助函数 ---
    def _create_help_icon(self):
        """创建一个标准的帮助图标标签。"""
        help_icon = QLabel()
        # 使用Qt内置的标准信息图标，确保在不同主题下都能良好显示
        icon = self.style().standardIcon(QStyle.SP_MessageBoxQuestion)
        pixmap = icon.pixmap(16, 16)
        if not pixmap.isNull():
            help_icon.setPixmap(pixmap)
        else:
            help_icon.setText("❓") # 回退方案
        return help_icon

    def accept(self):
        self.config["lang"] = self.lang_combo.currentData()
        self.config["theme"] = self.theme_combo.currentText().lower()
        self.config["follow_system"] = self.follow_system_checkbox.isChecked()
        self.config["temp_dir_path"] = self.temp_dir_edit.text()
        self.config["log_font_family"] = self.log_font_combo.currentFont().family()
        self.config["log_font_size"] = int(self.log_font_size_combo.currentText())

        for tech_name, controls in self.property_widgets.items():
            vox_type = controls["vox_type_combo"].currentData()
            self.config[f"preset/{tech_name}/vox_type"] = vox_type
            for prop_name, widget in controls["prop_widgets"].items():
                self.config[f"preset/{tech_name}/prop_{prop_name}"] = widget.value()
        
        # --- 核心修复：从新的自定义滑块保存高级设置 ---
        self.config["angle_tol"] = self.angle_tol_slider.floatValue()
        self.config["dist_tol"] = self.dist_tol_slider.floatValue()

        super().accept()

    def get_config(self):
        return self.config
    
    # --- 新增：一个辅助函数，用于递归地清空和删除布局中的所有控件 ---
    def _clear_layout(self, layout):
        """一个健壮的辅助函数，用于清空任何布局并删除其中的所有子控件。"""
        if layout is None:
            return
        while layout.count():
            item = layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()
            else:
                # 如果item是另一个布局，则递归清理
                sub_layout = item.layout()
                if sub_layout is not None:
                    self._clear_layout(sub_layout)

# --- 修改：让材质映射对话框也继承自我们新的 NonClosingDialog ---
class MaterialMappingDialog(NonClosingDialog):
    """A dialog for manually mapping materials to TD Notes with advanced features."""
    
    @staticmethod
    def get_material_profiles():
        return {
            "$TD_auto":         {"display": t("MAT_MAP_NOTE_AUTO"),         "color": QColor(0, 0, 0, 0)},
            "":                 {"display": t("MAT_MAP_NOTE_NONE"),         "color": QColor(0, 0, 0, 0)},
            "$TD_metal":        {"display": t("MAT_MAP_NOTE_METAL"),        "color": QColor(180, 180, 255, 200)},   # 更亮的银蓝色
            "$TD_wood":         {"display": t("MAT_MAP_NOTE_WOOD"),         "color": QColor(160, 82, 45, 200)},    # 更深的棕色
            "$TD_glass":        {"display": t("MAT_MAP_NOTE_GLASS"),        "color": QColor(0, 180, 255, 200)},    # 鲜亮蓝色
            "$TD_masonry":      {"display": t("MAT_MAP_NOTE_MASONRY"),      "color": QColor(100, 100, 100, 200)},  # 更深灰色
            "$TD_foliage":      {"display": t("MAT_MAP_NOTE_FOLIAGE"),      "color": QColor(0, 180, 0, 200)},     # 鲜亮绿色
            "$TD_plastic":      {"display": t("MAT_MAP_NOTE_PLASTIC"),      "color": QColor(255, 0, 128, 200)},   # 鲜亮粉色
            "$TD_plaster":      {"display": t("MAT_MAP_NOTE_PLASTER"),      "color": QColor(255, 255, 200, 200)}, # 更亮米白色
            "$TD_heavymetal":   {"display": t("MAT_MAP_NOTE_HEAVYMETAL"),   "color": QColor(60, 60, 60, 200)},    # 更深金属灰
            "$TD_rock":         {"display": t("MAT_MAP_NOTE_ROCK"),         "color": QColor(70, 130, 180, 200)},  # 更深石板蓝
            "$TD_dirt":         {"display": t("MAT_MAP_NOTE_DIRT"),         "color": QColor(120, 72, 0, 200)},    # 更深棕色
            "$TD_hardmetal":    {"display": t("MAT_MAP_NOTE_HARDMETAL"),    "color": QColor(220, 220, 220, 200)}, # 更亮银色
            "$TD_hardmasonry":  {"display": t("MAT_MAP_NOTE_HARDMASONRY"),  "color": QColor(80, 80, 80, 200)},    # 更深中灰色
            "$TD_ice":          {"display": t("MAT_MAP_NOTE_ICE"),          "color": QColor(0, 220, 255, 200)},   # 鲜亮冰蓝色
            "$TD_unphysical":   {"display": t("MAT_MAP_NOTE_UNPHYSICAL"),   "color": QColor(255, 255, 0, 200)},   # 更亮黄色
        }

    def __init__(self, material_data, obj_dir, initial_mappings=None, default_mapping_mode="$TD_auto", parent=None):
        super().__init__(parent)
        self.setWindowTitle(t("MAT_MAP_TITLE"))
        self.setMinimumSize(800, 550) # 稍微增加高度以容纳新控件

        # --- 1. Core Data Initialization ---
        self.MATERIAL_PROFILES = self.get_material_profiles()
        self.material_data = material_data
        self.obj_dir = obj_dir
        self.config = parent.config if parent else {}
        self.property_editor_widgets = {}
        self.default_mapping_mode = default_mapping_mode
        
        # --- 修改：数据模型分离 ---
        # current_mappings 存储 Teardown Note
        self.current_mappings = {mat: default_mapping_mode for mat in self.material_data.keys()}
        if initial_mappings:
            self.current_mappings.update(initial_mappings)
        
        # material_data 将额外存储 vox_type 和属性
        for mat_name in self.material_data.keys():
            if 'vox_type' not in self.material_data[mat_name]:
                self.material_data[mat_name]['vox_type'] = 'diffuse' # 默认为 diffuse

        self.display_to_tech_name = {v["display"]: k for k, v in self.MATERIAL_PROFILES.items()}
        # --- 修改：使用新的排序模式枚举 ---
        self.sort_mode = SortMode.ALPHA_ASC
        self.last_grid_columns = 0

        # --- 2. Main Layout ---
        main_layout = QHBoxLayout(self)

        # --- 3. Create Left Panel Widgets ---
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_panel.setFixedWidth(250)
        
        list_controls_layout = QHBoxLayout()
        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText(t("MAT_MAP_SEARCH_PLACEHOLDER"))
        list_controls_layout.addWidget(self.search_edit)
        # --- 修复：将按钮设置为正方形 ---
        self.sort_button = QPushButton("↑↓")
        self.sort_button.setFixedSize(25, 25) # 设置一个固定的正方形尺寸
        self.sort_button.setToolTip(t("MAT_MAP_SORT_TOOLTIP"))
        # --- 修改：更新排序按钮的初始图标和提示 ---
        self.sort_button.setText("A-Z")
        self.sort_button.setToolTip(t("MAT_MAP_SORT_ALPHA_ASC"))
        list_controls_layout.addWidget(self.sort_button)
        
        self.material_list = QListWidget()
        self.material_list.setSelectionMode(QAbstractItemView.ExtendedSelection)
        
        left_layout.addLayout(list_controls_layout)
        left_layout.addWidget(self.material_list)
        main_layout.addWidget(left_panel)

        # --- 4. Create Right Panel Widgets ---
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)

        self.preview_stack = QStackedWidget()
        self._create_single_preview_page()
        self._create_multi_preview_page()

        # --- 修复：重构右侧面板布局 ---

        # 1. 物理材质下拉框 (单独放置，无边框)
        td_note_layout = QFormLayout()
        self.td_note_combo = QComboBox()
        self.td_note_combo.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        self.td_note_combo.addItems([p["display"] for p in self.MATERIAL_PROFILES.values()])
        td_note_layout.addRow(f"<b>{t('MAT_MAP_TD_NOTE_MAPPING')}</b>", self.td_note_combo)

        # 2. 渲染材质和属性编辑器 (放置在同一个带边框的容器内)
        # --- 修复：将 render_group 提升为成员变量 ---
        self.render_group = QFrame()
        self.render_group.setProperty("frame-style", "bordered")
        self.render_group.setFrameShape(QFrame.StyledPanel)
        # 将属性编辑器的布局直接作为 render_group 的布局
        self.property_editor_layout = QFormLayout(self.render_group)
        self.property_editor_layout.setContentsMargins(10, 10, 10, 10)
        self.property_editor_layout.setSpacing(8)

        # 将 VOX 类型下拉框作为第一行添加到属性布局中
        self.vox_type_combo = QComboBox()
        self.vox_type_combo.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        for tech_name, profile in MaterialPropertyManager.PROPERTIES.items():
            self.vox_type_combo.addItem(t(profile['label_id']), tech_name)
        self.property_editor_layout.addRow(f"<b>{t('MAT_MAP_VOX_TYPE')}:</b>", self.vox_type_combo)

        # 3. 按钮
        button_box = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        self.clear_button = button_box.addButton(t("MAT_MAP_CLEAR_SELECTED_BUTTON"), QDialogButtonBox.ResetRole)
        self.load_preset_button = QPushButton(t("MAT_MAP_LOAD_PRESET_BUTTON"))
        button_box.addButton(self.load_preset_button, QDialogButtonBox.ActionRole)

        # --- 5. Add Widgets to Right Panel Layout ---
        right_layout.addWidget(self.preview_stack)
        right_layout.addLayout(td_note_layout) # 添加独立的物理材质布局
        right_layout.addWidget(self.render_group)  # 添加新的渲染材质+属性容器
        right_layout.addStretch()
        right_layout.addWidget(button_box)
        main_layout.addWidget(right_panel)

        # --- 6. Debounced Search Timer ---
        self.search_timer = QTimer(self)
        self.search_timer.setSingleShot(True)
        self.search_timer.setInterval(300)

        # --- 7. Signal Connections ---
        self.search_edit.textChanged.connect(self.search_timer.start)
        self.search_timer.timeout.connect(self._filter_and_sort_list)
        self.sort_button.clicked.connect(self._toggle_sort_order)
        self.material_list.itemSelectionChanged.connect(self._on_selection_changed)
        self.td_note_combo.currentTextChanged.connect(self._save_current_mapping)
        # --- 新增：连接新下拉框的信号 ---
        self.vox_type_combo.currentTextChanged.connect(self._on_vox_type_changed)
        self.clear_button.clicked.connect(self._clear_selected_mappings)
        self.load_preset_button.clicked.connect(self._load_preset_for_selection)
        button_box.accepted.connect(self.accept)
        button_box.rejected.connect(self.reject)
        
        # --- 8. Initial Population ---
        self._filter_and_sort_list()
        if self.material_list.count() > 0:
            self.material_list.setCurrentRow(0)
        else:
            self._on_selection_changed()

    def _create_single_preview_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setAlignment(Qt.AlignCenter)
        preview_title = QLabel(f"<b>{t('MAT_MAP_PREVIEW')}</b>")
        preview_title.setAlignment(Qt.AlignCenter)
        layout.addWidget(preview_title)
        # --- 修改：使用新的 PreviewWidget ---
        self.preview_widget = PreviewWidget()
        self.preview_widget.setFixedSize(150, 150)
        layout.addWidget(self.preview_widget)
        self.preview_stack.addWidget(page)
    
    def resizeEvent(self, event):
        """重写窗口大小调整事件，以更新网格布局。"""
        super().resizeEvent(event)
        # 仅当多选预览视图可见时才触发重绘
        if self.preview_stack.currentIndex() == 1:
            self._populate_multi_preview_grid(self.material_list.selectedItems())

    def _create_multi_preview_page(self):
        scroll_area = QScrollArea()
        scroll_area.setWidgetResizable(True)
        # --- 修改：允许在需要时显示水平滚动条 ---
        scroll_area.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.multi_preview_widget = QWidget()
        self.multi_preview_layout = QGridLayout(self.multi_preview_widget)
        self.multi_preview_layout.setAlignment(Qt.AlignTop)
        scroll_area.setWidget(self.multi_preview_widget)
        self.preview_stack.addWidget(scroll_area)

    def _filter_and_sort_list(self):
        search_text = self.search_edit.text().lower()
        current_selection_names = {item.text() for item in self.material_list.selectedItems()}
        self.material_list.blockSignals(True)
        self.material_list.clear()
        
        filtered_items = [name for name in self.material_data.keys() if search_text in name.lower()]
        
        # --- 修复：实现新的多模式排序逻辑 ---
        if self.sort_mode == SortMode.ALPHA_ASC:
            filtered_items.sort()
        elif self.sort_mode == SortMode.ALPHA_DESC:
            filtered_items.sort(reverse=True)
        elif self.sort_mode == SortMode.TYPE_ASC_ALPHA_ASC:
            filtered_items.sort(key=lambda name: (self.current_mappings.get(name, ""), name))
        elif self.sort_mode == SortMode.TYPE_DESC_ALPHA_DESC:
            filtered_items.sort(key=lambda name: (self.current_mappings.get(name, ""), name), reverse=True)

        self.material_list.addItems(filtered_items)
        for i in range(self.material_list.count()):
            item = self.material_list.item(i)
            self._update_item_highlight(item)
            if item.text() in current_selection_names:
                item.setSelected(True)
        self.material_list.blockSignals(False)
        self._on_selection_changed()

    def _toggle_sort_order(self):
        # --- 修复：在四种模式之间循环切换 ---
        current_mode_index = self.sort_mode.value
        next_mode_index = (current_mode_index + 1) % 4
        self.sort_mode = SortMode(next_mode_index)

        # --- 新增：根据新模式更新按钮文本和提示 ---
        if self.sort_mode == SortMode.ALPHA_ASC:
            self.sort_button.setText("A-Z")
            self.sort_button.setToolTip(t("MAT_MAP_SORT_ALPHA_ASC"))
        elif self.sort_mode == SortMode.ALPHA_DESC:
            self.sort_button.setText("Z-A")
            self.sort_button.setToolTip(t("MAT_MAP_SORT_ALPHA_DESC"))
        elif self.sort_mode == SortMode.TYPE_ASC_ALPHA_ASC:
            self.sort_button.setText("T↓ A-Z")
            self.sort_button.setToolTip(t("MAT_MAP_SORT_TYPE_ASC"))
        elif self.sort_mode == SortMode.TYPE_DESC_ALPHA_DESC:
            self.sort_button.setText("T↑ Z-A")
            self.sort_button.setToolTip(t("MAT_MAP_SORT_TYPE_DESC"))

        self._filter_and_sort_list()

    def _on_selection_changed(self):
        selected_items = self.material_list.selectedItems()
        num_selected = len(selected_items)
        
        # --- 修复：同时禁用两个下拉框和整个渲染属性组 ---
        is_enabled = num_selected > 0
        self.td_note_combo.setEnabled(is_enabled)
        self.render_group.setEnabled(is_enabled) # 控制整个容器的可用状态
        self.clear_button.setEnabled(is_enabled)
        self.load_preset_button.setEnabled(is_enabled)

        if num_selected > 1:
            self.preview_stack.setCurrentIndex(1)
            self._populate_multi_preview_grid(selected_items)
        else:
            self.preview_stack.setCurrentIndex(0)
            if num_selected == 1:
                self.update_details(selected_items[0])
            else:
                # --- 修改：清除自定义控件的内容并设置占位文本 ---
                self.preview_widget.set_pixmap(None)
                self.preview_widget.set_background_color(None)
                self.preview_widget.set_badge_color(None)
                self.preview_widget.setToolTip("") # 清空悬浮提示
                self.preview_widget.set_placeholder_text(t('MAT_MAP_SELECT_ITEM_PROMPT'))
                # --- 修复：不再隐藏，由 setEnabled 控制 ---
                # self.render_group.setVisible(False) # 此行被移除

        # --- 修改：属性编辑器重建现在由 vox_type_combo 驱动，但选择变化时仍需触发 ---
        if num_selected > 0:
            self._rebuild_property_editor()
            if num_selected == 1:
                self._load_material_properties(selected_items[0])
        else:
            self._clear_property_editor()
    
    def _on_vox_type_changed(self):
        """当VOX材质类型下拉框变化时，重建属性编辑器并保存。"""
        # --- 修复：在保存前先重建，确保 self.property_editor_widgets 是最新的 ---
        self._rebuild_property_editor()
        self._save_current_mapping() # 保存所有更改

    def _clear_property_editor(self):
        """清空并隐藏属性编辑器。"""
        # --- 修复：使用新的辅助函数来清理，确保控件被删除 ---
        # 我们需要一个临时列表来存储要保留的控件
        widgets_to_keep = []
        if self.property_editor_layout.rowCount() > 0:
            # 保留第0行的标签和下拉框
            label_item = self.property_editor_layout.itemAt(0, QFormLayout.LabelRole)
            widget_item = self.property_editor_layout.itemAt(0, QFormLayout.FieldRole)
            if label_item and label_item.widget():
                widgets_to_keep.append(label_item.widget())
            if widget_item and widget_item.widget():
                widgets_to_keep.append(widget_item.widget())

        # 清理整个布局
        self._clear_layout(self.property_editor_layout)
        
        # 将需要保留的控件重新添加回去
        for widget in widgets_to_keep:
            if isinstance(widget, QLabel):
                self.property_editor_layout.setWidget(0, QFormLayout.LabelRole, widget)
            elif isinstance(widget, QComboBox):
                self.property_editor_layout.setWidget(0, QFormLayout.FieldRole, widget)

        self.property_editor_widgets.clear()

    def _rebuild_property_editor(self):
        """根据VOX材质类型下拉框重建属性编辑器。"""
        # 1. 清理旧的属性控件（保留第0行的VOX类型下拉框）
        # --- 修复：使用新的辅助函数来清理，确保控件被删除 ---
        while self.property_editor_layout.rowCount() > 1:
            # removeRow 会同时删除标签和字段控件
            self.property_editor_layout.removeRow(1)
        
        self.property_editor_widgets.clear()

        # 2. 获取新属性并创建新控件
        vox_type = self.vox_type_combo.currentData()
        prop_list = MaterialPropertyManager.PROPERTIES.get(vox_type, {}).get("properties", [])

        if not prop_list:
            # --- 新增：显示占位提示 ---
            placeholder_label = QLabel(t("MAT_MAP_NO_PROPS"))
            placeholder_label.setStyleSheet("font-style: italic; color: grey;")
            self.property_editor_layout.addRow(placeholder_label)
            return
        
        widgets = MaterialPropertyManager.create_property_widgets(prop_list, self)
        self.property_editor_widgets = widgets

        for prop in prop_list:
            # --- 修复：使用翻译函数 t() 获取标签文本 ---
            self.property_editor_layout.addRow(f"{t(prop['label_id'])}:", widgets[prop['name']])
            widgets[prop['name']].valueChanged.connect(self._save_current_mapping)

    def _load_mtl_properties(self, item):
        """Loads initial property values from parsed MTL data for a single item."""
        mat_name = item.text()
        mtl_info = self.material_data.get(mat_name, {})
        
        for prop_name, widget in self.property_editor_widgets.items():
            if prop_name in mtl_info:
                widget.setValue(mtl_info[prop_name])

    def _load_preset_for_selection(self):
        """根据当前选中的TD Note，加载并应用其完整的预设（VOX类型+属性）。"""
        selected_items = self.material_list.selectedItems()
        if not selected_items: return

        # 1. 获取当前选中的 Teardown Note 的技术名称
        td_display_name = self.td_note_combo.currentText()
        tech_name = self.display_to_tech_name.get(td_display_name, "")
        
        if not tech_name or tech_name == "$TD_auto":
            QMessageBox.information(self, t("MAT_MAP_LOAD_PRESET_TITLE"), t("MAT_MAP_LOAD_PRESET_NO_PRESET"))
            return

        # 2. 从配置中读取该TD Note的预设VOX类型
        preset_vox_type_key = f"preset/{tech_name}/vox_type"
        preset_vox_type = self.config.get(preset_vox_type_key, "diffuse")

        # --- 修复：应用预设的VOX类型到下拉框 ---
        # 通过 userData (技术名称) 查找索引，而不是通过显示的文本
        index = self.vox_type_combo.findData(preset_vox_type)
        if index != -1:
            # 阻止信号，以避免触发不必要的保存操作，我们将在最后手动保存
            self.vox_type_combo.blockSignals(True)
            self.vox_type_combo.setCurrentIndex(index)
            self.vox_type_combo.blockSignals(False)
        
        # 3. 手动重建属性编辑器以匹配新的VOX类型
        self._rebuild_property_editor()

        # 4. 加载并应用该VOX类型下的所有预设属性值
        for prop_name, widget in self.property_editor_widgets.items():
            prop_key = f"preset/{tech_name}/prop_{prop_name}"
            # 检查配置中是否存在该预设值
            if prop_key in self.config:
                widget.setValue(float(self.config[prop_key]))
        
        # 5. 确保所有更改（新的VOX类型和属性）都已保存到当前选中的项目中
        self._save_current_mapping()

    def _populate_multi_preview_grid(self, items):
        # --- 新增：动态计算列数 ---
        item_width_with_spacing = 90  # 80px 宽度 + 10px 间距
        # 使用滚动区域的视口宽度进行计算，更为准确
        viewport_width = self.preview_stack.widget(1).viewport().width()
        num_columns = max(4, viewport_width // item_width_with_spacing)

        # 如果列数和项目数都未改变，则无需重绘，提高性能
        # --- 修复：移除过于激进的优化，以确保内容更改时能够重绘 ---
        # if num_columns == self.last_grid_columns and self.multi_preview_layout.count() == len(items):
        #     return
        # self.last_grid_columns = num_columns

        # --- 修复：在添加新控件前，使用新的辅助函数来清空布局 ---
        self._clear_layout(self.multi_preview_layout)

        for i, item in enumerate(items):
            mat_name = item.text()
            mat_info = self.material_data[mat_name]
            tech_name = self.current_mappings.get(mat_name, "")
            badge_color = self.MATERIAL_PROFILES.get(tech_name, {}).get("color")

            # --- 修改：创建时传入材质名称，并连接信号 ---
            preview = PreviewWidget(material_name=mat_name)
            preview.setFixedSize(80, 80)
            preview.setToolTip(mat_name)
            preview.set_badge_color(badge_color)
            preview.clicked.connect(self._on_preview_clicked) # 连接信号

            if mat_info.get('texture'):
                pixmap = QPixmap(os.path.join(self.obj_dir, mat_info['texture']))
                preview.set_pixmap(pixmap)
            elif mat_info.get('color'):
                r, g, b = mat_info['color']
                preview.set_background_color(QColor(r, g, b))
            
            row, col = divmod(i, num_columns)
            self.multi_preview_layout.addWidget(preview, row, col)

    # --- 新增：处理小预览图点击事件的槽函数 ---
    def _on_preview_clicked(self, material_name):
        """当多格预览中的一个小部件被点击时调用。"""
        # 查找左侧列表中对应的项目
        items = self.material_list.findItems(material_name, Qt.MatchExactly)
        if items:
            # 切换到单选模式，并选中该项
            # 这会自动触发 material_list 的 itemSelectionChanged 信号，
            # 进而调用 _on_selection_changed 来更新整个右侧面板。
            self.material_list.setCurrentItem(items[0])

    def _clear_selected_mappings(self):
        # --- 修改：作用域从所有可见项变为所有选中项 ---
        items_to_clear = self.material_list.selectedItems()
        if not items_to_clear:
            return

        # Create confirmation dialog
        msg_box = QMessageBox(self)
        msg_box.setIcon(QMessageBox.Warning)
        msg_box.setWindowTitle(t("MAT_MAP_CLEAR_CONFIRM_TITLE"))
        # --- 修改：使用新的翻译文本 ---
        msg_box.setText(t("MAT_MAP_CLEAR_SELECTED_CONFIRM_MSG", count=len(items_to_clear)))
        # Display list of materials to be cleared
        detailed_text = "\n".join([item.text() for item in items_to_clear[:15]]) # Show up to 15
        if len(items_to_clear) > 15:
            msg = t("MAT_MAP_CLEAR_ITEM_OMITTED", num=len(items_to_clear) - 15)
            detailed_text += msg
        msg_box.setDetailedText(detailed_text)
        msg_box.setStandardButtons(QMessageBox.Ok | QMessageBox.Cancel)
        msg_box.setDefaultButton(QMessageBox.Cancel)
        
        if msg_box.exec() == QMessageBox.Ok:
            # --- 修复：使用保存在 self 中的默认模式，而不是访问不存在的 self.config ---
            for item in items_to_clear:
                self.current_mappings[item.text()] = self.default_mapping_mode
                self._update_item_highlight(item)
            # --- 修复：手动触发UI更新 ---
            self._on_selection_changed()

    def _update_item_highlight(self, item):
        mat_name = item.text()
        tech_name = self.current_mappings.get(mat_name, "")
        color = self.MATERIAL_PROFILES.get(tech_name, {}).get("color", QColor(0,0,0,0))
        item.setBackground(QBrush(color) if color.alpha() > 0 else Qt.NoBrush)

    def _load_material_properties(self, item):
        """加载一个项目的所有属性（包括VOX类型和具体参数）。"""
        mat_name = item.text()
        mat_data = self.material_data.get(mat_name, {})
        
        # 1. 设置VOX类型下拉框
        vox_type = mat_data.get('vox_type', 'diffuse')
        self.vox_type_combo.blockSignals(True)
        # --- 修复：通过技术名称查找并设置下拉框的索引 ---
        index = self.vox_type_combo.findData(vox_type)
        if index != -1:
            self.vox_type_combo.setCurrentIndex(index)
        self.vox_type_combo.blockSignals(False)
        
        # 2. 重建编辑器以匹配该类型
        self._rebuild_property_editor()

        # 3. 加载具体参数值
        for prop_name, widget in self.property_editor_widgets.items():
            if prop_name in mat_data:
                widget.setValue(mat_data[prop_name])

    def update_details(self, item):
        mat_name = item.text()
        mat_info = self.material_data[mat_name]
        tech_name = self.current_mappings.get(mat_name, "")
        badge_color = self.MATERIAL_PROFILES.get(tech_name, {}).get("color")

        # --- 修改：更新自定义控件 ---
        self.preview_widget.set_badge_color(badge_color)
        self.preview_widget.setToolTip(mat_name)

        if mat_info.get('texture'):
            pixmap = QPixmap(os.path.join(self.obj_dir, mat_info['texture']))
            self.preview_widget.set_pixmap(pixmap)
            if pixmap.isNull():
                 self.preview_widget.setToolTip(f"{mat_name}\n({t('MAT_MAP_TEX_NOT_FOUND')})")
        elif mat_info.get('color'):
            r, g, b = mat_info['color']
            self.preview_widget.set_background_color(QColor(r, g, b))
            self.preview_widget.set_pixmap(None) # 确保清除旧纹理
        
        # --- 修改：加载两个独立的下拉框和属性编辑器 ---
        mat_name = item.text()
        
        # 1. 加载 Teardown Note
        tech_name = self.current_mappings.get(mat_name, "")
        display_name = self.MATERIAL_PROFILES.get(tech_name, {}).get("display", t("MAT_MAP_NOTE_NONE"))
        self.td_note_combo.blockSignals(True)
        self.td_note_combo.setCurrentText(display_name)
        self.td_note_combo.blockSignals(False)

        # 2. 加载 VOX 类型和属性
        self._load_material_properties(item)

    def _save_current_mapping(self):
        """保存所有选中项目的所有设置（TD Note, VOX Type, Properties）。"""
        selected_items = self.material_list.selectedItems()
        if not selected_items: return

        # 1. 获取 Teardown Note
        td_display_name = self.td_note_combo.currentText()
        td_tech_name = self.display_to_tech_name.get(td_display_name, "")
        
        # 2. 获取 VOX Type
        # --- 修复：通过 currentData 获取技术名称 ---
        vox_type = self.vox_type_combo.currentData()

        prop_values = {name: widget.value() for name, widget in self.property_editor_widgets.items()}

        for item in selected_items:
            mat_name = item.text()
            # 保存 Teardown Note
            self.current_mappings[mat_name] = td_tech_name
            
            # 保存 VOX Type 和属性
            if mat_name not in self.material_data: self.material_data[mat_name] = {}
            self.material_data[mat_name]['vox_type'] = vox_type
            self.material_data[mat_name].update(prop_values)
            
            self._update_item_highlight(item)
        
        # --- 修复：恢复对单格预览的轻量级刷新 ---
        # 检查单格预览是否可见，以及是否只有一个项目被选中
        if self.preview_stack.currentIndex() == 0 and len(selected_items) == 1:
            # 只更新角标颜色，不调用重量级的 update_details
            mat_name = selected_items[0].text()
            tech_name = self.current_mappings.get(mat_name, "")
            badge_color = self.MATERIAL_PROFILES.get(tech_name, {}).get("color")
            self.preview_widget.set_badge_color(badge_color)
        # 多格预览的刷新逻辑保持不变
        elif self.preview_stack.currentIndex() == 1:
            self._populate_multi_preview_grid(selected_items)

    def get_mappings(self):
        """返回 Teardown Note 的映射关系。"""
        return {k: v for k, v in self.current_mappings.items() if v and v != "$TD_auto"}

    # --- 修复：重命名并修正方法，以避免混淆 ---
    def get_full_properties(self):
        """
        返回一个包含所有配置的完整字典，包括 vox_type 和属性。
        这是为了在Python端（MainWindow）存储状态。
        """
        # 直接返回内部的、包含所有信息的 material_data 即可
        return self.material_data

    def get_numeric_properties_for_backend(self):
        """
        返回一个只包含纯数值属性的“干净”字典。
        这是为了传递给C++后端。
        """
        clean_properties = {}
        for mat_name, all_data in self.material_data.items():
            numeric_props = {}
            for key, value in all_data.items():
                if isinstance(value, (int, float)):
                    numeric_props[key] = value
            
            if numeric_props:
                clean_properties[mat_name] = numeric_props
                
        return clean_properties

# --- 新增：一个辅助函数，用于递归地清空和删除布局中的所有控件 ---
    def _clear_layout(self, layout):
        """一个健壮的辅助函数，用于清空任何布局并删除其中的所有子控件。"""
        if layout is None:
            return
        while layout.count():
            item = layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()
            else:
                # 如果item是另一个布局，则递归清理
                sub_layout = item.layout()
                if sub_layout is not None:
                    self._clear_layout(sub_layout)

class PathLineEdit(QLineEdit):
    """
    一个支持文件拖放的QLineEdit，并能自动显示完整内容的工具提示。
    """
    fileSelected = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAcceptDrops(True)
        # --- 新增：连接自身的 textChanged 信号到内部的 tooltip 更新器 ---
        self.textChanged.connect(self._update_tooltip)

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            super().dragEnterEvent(event)

    def dropEvent(self, event):
        if event.mimeData().hasUrls():
            url = event.mimeData().urls()[0]
            if url.isLocalFile():
                path = url.toLocalFile()
                self.setText(path)
            event.acceptProposedAction()
        else:
            super().dropEvent(event)
    
    def setText(self, text):
        current_text = self.text()
        if current_text != text:
            super().setText(text)
        self.fileSelected.emit(text)

    # --- 新增：重写 setPlaceholderText 以便在占位符改变时也能更新工具提示 ---
    def setPlaceholderText(self, text):
        super().setPlaceholderText(text)
        # 如果当前没有文本，立即更新工具提示以显示新的占位符
        if not self.text():
            self.setToolTip(text)

    # --- 新增：用于自动更新工具提示的内部槽函数 ---
    @Slot(str)
    def _update_tooltip(self, text):
        """根据当前文本或占位符文本，更新工具提示。"""
        tooltip_text = text if text else self.placeholderText()
        self.setToolTip(tooltip_text)

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        
        self.config = {} 
        self.dependencies_ok = False
        self.material_mappings = {}
        self.material_properties = {} 
        self.stopped_by_user = False
        # --- 新增：用于缓存首选项对话框的成员变量 ---
        self._preferences_dialog = None

        self.setWindowIcon(QIcon(resource_path("img/icon.ico")))
        self.resize(800, 600)
        screen_geometry = self.screen().geometry()
        x = (screen_geometry.width() - self.width()) / 2
        y = (screen_geometry.height() - self.height()) / 2
        self.move(int(x), int(y))

        self._create_controls()
        self._create_layout()
        self._create_menu_bar()
        
        self._connect_signals()
        self._load_settings()
        self._apply_log_font_from_config() # 应用字体
        
        self.retranslate_ui()
        
        self._find_polyvox_exe()
        
        self._update_start_button_state()
        self._update_material_controls_state()

    # --- 新增：加载设置的函数 ---
    def _load_settings(self):
        settings = QSettings(ORGANIZATION_NAME, APPLICATION_NAME)
        
        lang = settings.value("lang", "en")
        self.config["lang"] = lang
        load_translations(lang)

        self.config["theme"] = settings.value("theme", "light")
        self.config["follow_system"] = settings.value("follow_system", True, type=bool)
        self.config["log_font_family"] = settings.value("log_font_family", "Consolas")
        self.config["log_font_size"] = settings.value("log_font_size", 10, type=int)

        # --- 修复：启动时调用新的全局样式函数 ---
        apply_application_styles(QApplication.instance(), self.config)

        self.config["manual_mapping_default"] = settings.value("manual_mapping_default", "$TD_auto")
        self.config["temp_dir_path"] = settings.value("temp_dir_path", "")

        self.polyvox_path_edit.setText(settings.value("polyvox_exe_path", resource_path("bin/polyvox.exe")))
        self.outdir_path_edit.setText(settings.value("output_dir", ""))
        self.voxel_size_spinbox.setValue(float(settings.value("voxel_size", 0.1)))
        self.auto_material_checkbox.setChecked(settings.value("auto_material", True, type=bool))
        
        # 加载所有预设
        for key in settings.allKeys():
            if key.startswith("preset/"):
                self.config[key] = settings.value(key)

    # --- 新增：保存设置的函数（通过重写closeEvent） ---
    def closeEvent(self, event):
        settings = QSettings(ORGANIZATION_NAME, APPLICATION_NAME)
        for key, value in self.config.items():
            settings.setValue(key, value)
        
        settings.setValue("polyvox_exe_path", self.polyvox_path_edit.text())
        settings.setValue("output_dir", self.outdir_path_edit.text())
        settings.setValue("last_obj_path", self.obj_path_edit.text())
        settings.setValue("voxel_size", self.voxel_size_spinbox.value())
        settings.setValue("auto_material", self.auto_material_checkbox.isChecked())

        super().closeEvent(event)

    def _create_controls(self):
        self.obj_path_edit = PathLineEdit() 
        # --- 修复：将所有路径框都替换为我们可复用的 PathLineEdit ---
        self.polyvox_path_edit = PathLineEdit()
        self.polyvox_path_edit.setReadOnly(True)
        self.outdir_path_edit = PathLineEdit()
        self.outdir_path_edit.setReadOnly(True)
        self.browse_obj_button = QPushButton(t("GUI_BROWSE_BUTTON"))
        self.browse_polyvox_button = QPushButton(t("GUI_BROWSE_BUTTON"))
        self.browse_outdir_button = QPushButton(t("GUI_BROWSE_BUTTON"))
        self.voxel_size_spinbox = QDoubleSpinBox()
        self.voxel_size_spinbox.setDecimals(3)
        self.voxel_size_spinbox.setSingleStep(0.01)
        # --- （如果启用以下这行）禁止更改体素大小，强制使用默认值0.1 ---
        # self.voxel_size_spinbox.setEnabled(False)
        self.log_edit = QTextEdit()
        # --- 新增：为日志窗口设置一个唯一的对象名称，以便QSS选择器可以定位它 ---
        self.log_edit.setObjectName("LogOutput")
        self.log_edit.setReadOnly(True)
        self.progress_bar = QProgressBar()
        self.progress_bar.setValue(0)
        self.progress_bar.setTextVisible(True)
        self.status_label = QLabel()
        self.start_button = QPushButton()
        self.start_button.setObjectName("StartButton")
        self.start_button.setFixedHeight(40)
        self.stop_button = QPushButton()
        self.stop_button.setEnabled(False)
        self.stop_button.setFixedHeight(40)
        
        self.obj_label = QLabel()
        self.polyvox_label = QLabel()
        self.outdir_label = QLabel()
        self.voxel_size_label = QLabel()
        self.voxel_size_metric_label = QLabel()
        self.dependency_status_label = QLabel()
        self.auto_material_checkbox = QCheckBox()
        self.configure_material_button = QPushButton()

    def _create_layout(self):
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QVBoxLayout(main_widget)

        obj_group = QFrame()
        obj_group.setProperty("frame-style", "bordered")
        obj_group.setFrameShape(QFrame.StyledPanel)
        obj_layout = QVBoxLayout(obj_group)
        obj_layout.setContentsMargins(15, 15, 15, 15)
        
        obj_layout.addWidget(self.obj_label)
        
        obj_input_layout = QHBoxLayout()
        obj_input_layout.addWidget(self.obj_path_edit)
        obj_input_layout.addWidget(self.browse_obj_button)
        obj_layout.addLayout(obj_input_layout)

        self.drop_label = QLabel()
        self.drop_label.setAlignment(Qt.AlignCenter)
        self.drop_label.setStyleSheet("color: grey; font-style: italic;")
        obj_layout.addWidget(self.drop_label)
        obj_layout.addWidget(self.dependency_status_label)
        main_layout.addWidget(obj_group)

        other_input_group = QFrame()
        other_input_group.setProperty("frame-style", "bordered")
        other_input_group.setFrameShape(QFrame.StyledPanel)
        other_input_layout = QVBoxLayout(other_input_group)
        other_input_layout.setContentsMargins(15, 15, 15, 15)
        other_input_layout.addLayout(self._create_path_selector(self.polyvox_label, self.polyvox_path_edit, self.browse_polyvox_button))
        other_input_layout.addLayout(self._create_path_selector(self.outdir_label, self.outdir_path_edit, self.browse_outdir_button))
        
        params_layout = QHBoxLayout()
        params_layout.addWidget(self.voxel_size_label)
        params_layout.addWidget(self.voxel_size_spinbox)
        params_layout.addWidget(self.voxel_size_metric_label)
        params_layout.addStretch()
        other_input_layout.addLayout(params_layout)
        
        material_layout = QHBoxLayout()
        material_layout.addWidget(self.auto_material_checkbox)
        material_layout.addWidget(self.configure_material_button)
        material_layout.addStretch()
        other_input_layout.addLayout(material_layout)
        
        main_layout.addWidget(other_input_group)
        
        main_layout.addWidget(self.log_edit, 1)
        main_layout.addWidget(self.progress_bar)
        main_layout.addWidget(self.status_label)
        
        btn_layout = QHBoxLayout()
        btn_layout.addWidget(self.start_button)
        btn_layout.addWidget(self.stop_button)
        main_layout.addLayout(btn_layout)

    def _create_path_selector(self, label, line_edit, button):
        layout = QHBoxLayout()
        label.setFixedWidth(100)
        layout.addWidget(label)
        layout.addWidget(line_edit)
        layout.addWidget(button)
        return layout

    def _connect_signals(self):
        self.browse_obj_button.clicked.connect(self._browse_obj_file)
        self.browse_polyvox_button.clicked.connect(self._browse_polyvox_exe)
        self.browse_outdir_button.clicked.connect(self._browse_outdir)
        self.start_button.clicked.connect(self.start_processing)
        self.stop_button.clicked.connect(self.stop_processing)
        
        self.obj_path_edit.fileSelected.connect(self._on_obj_path_changed)
        self.obj_path_edit.textChanged.connect(self._update_start_button_state)
        self.polyvox_path_edit.textChanged.connect(self._update_start_button_state)
        self.outdir_path_edit.textChanged.connect(self._update_start_button_state)
        self.obj_path_edit.textChanged.connect(lambda: self._on_obj_path_changed(self.obj_path_edit.text()))

        # --- 移除：以下信号连接已不再需要，因为逻辑已移入 PathLineEdit 内部 ---
        # self.obj_path_edit.textChanged.connect(self._update_line_edit_tooltip)
        # self.polyvox_path_edit.textChanged.connect(self._update_line_edit_tooltip)
        # self.outdir_path_edit.textChanged.connect(self._update_line_edit_tooltip)

        self.auto_material_checkbox.stateChanged.connect(self._update_material_controls_state)
        self.configure_material_button.clicked.connect(self.open_material_mapper)
        self.obj_path_edit.fileSelected.connect(self._update_material_controls_state)
        
        self.log_handler = setup_logger()
        self.log_handler.new_record.connect(self.append_log)
        logging.getLogger().addHandler(self.log_handler)
    
    # --- 废弃：此方法的功能已完全被 apply_application_styles 取代 ---
    # def _apply_styles(self):
    #     ...

    def _apply_log_font_from_config(self):
        """从配置中读取字体设置并应用到日志窗口。"""
        # --- 修复：此方法现在只调用新的全局样式函数 ---
        apply_application_styles(QApplication.instance(), self.config)

    @Slot(ProcessingStage, str)
    def on_stage_changed(self, stage, status_text):
        """根据处理阶段更新UI"""
        self.status_label.setText(status_text)

        # 根据阶段枚举值来控制进度条模式
        is_indeterminate = stage in [
            ProcessingStage.PREPARING,
            ProcessingStage.MERGING,
            ProcessingStage.CLEANUP
        ]

        if is_indeterminate:
            if self.progress_bar.maximum() != 0:
                self.progress_bar.setRange(0, 0)
                self.progress_bar.setTextVisible(False)
        else: # 对于 PROCESSING_SURFACES, COMPLETE, ERROR, STOPPED 等阶段
            if self.progress_bar.maximum() == 0:
                self.progress_bar.setRange(0, 100)
                self.progress_bar.setValue(0)
                self.progress_bar.setTextVisible(True)

    def _browse_obj_file(self):
        path, _ = QFileDialog.getOpenFileName(self, t("GUI_SELECT_OBJ_TITLE"), "", "OBJ Files (*.obj)")
        if path:
            # 调用我们重写的 setText，它会自动发射 fileSelected 信号
            self.obj_path_edit.setText(path)

    def _browse_polyvox_exe(self):
        # --- 新增：在打开文件对话框前显示确认窗口 ---
        reply = QMessageBox.question(
            self,
            t("GUI_CONFIRM_MANUAL_POLYVOX_TITLE"),
            t("GUI_CONFIRM_MANUAL_POLYVOX_MSG"),
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No
        )

        if reply == QMessageBox.Yes:
            # 只有当用户确认时，才打开文件浏览器
            path, _ = QFileDialog.getOpenFileName(self, t("GUI_SELECT_POLYVOX_TITLE"), "", "Executable Files (*.exe)")
            if path:
                self.polyvox_path_edit.setText(path)

    def _browse_outdir(self):
        path = QFileDialog.getExistingDirectory(self, t("GUI_SELECT_OUTDIR_TITLE"))
        if path:
            self.outdir_path_edit.setText(path)
            
    def _find_polyvox_exe(self):
        # 尝试在打包后的环境中或开发环境中找到polyvox.exe
        if getattr(sys, 'frozen', False):
            # 如果是打包后的程序
            base_path = sys._MEIPASS
        else:
            # 开发环境
            base_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
        
        exe_path = os.path.join(base_path, 'bin', 'polyvox.exe')
        if os.path.exists(exe_path):
            # 设置路径并将反斜杠替换为斜杠以匹配windows路径习惯
            self.polyvox_path_edit.setText(os.path.normpath(exe_path).replace("\\", "/"))
    
    def _update_start_button_state(self):
        """
        检查所有输入条件，并根据结果启用或禁用“开始”按钮。
        """
        # 1. 检查所有路径是否都已填写
        all_paths_filled = all([
            self.obj_path_edit.text(),
            self.polyvox_path_edit.text(),
            self.outdir_path_edit.text()
        ])
        
        # 2. 结合路径填写情况和依赖检查结果
        is_ready = all_paths_filled and self.dependencies_ok
        
        self.start_button.setEnabled(is_ready)

    @Slot(str)
    def append_log(self, message):
        # QTextEdit 没有 appendHtml 方法，正确的方式是移动光标到末尾再插入
        self.log_edit.moveCursor(QTextCursor.End)
        self.log_edit.insertHtml(message)
        # insertHtml 不会自动换行，所以我们手动追加一个换行符
        self.log_edit.append("") 
        self.log_edit.ensureCursorVisible() # 确保日志框自动滚动到底部

    def retranslate_ui(self):
        """更新主窗口中的所有UI文本"""
        # --- 修复：使用翻译ID设置窗口标题 ---
        self.setWindowTitle(t("APP_TITLE"))
        # 菜单
        self.options_menu.setTitle(t("MENU_OPTIONS"))
        self.preferences_action.setText(t("MENU_PREFERENCES"))
        # --- 新增：更新“关于”菜单项的文本 ---
        self.about_action.setText(t("MENU_ABOUT"))
        # 路径选择
        self.obj_label.setText(t("GUI_OBJ_FILE"))
        self.polyvox_label.setText(t("GUI_POLYVOX_EXE"))
        self.outdir_label.setText(t("GUI_OUTPUT_DIR"))
        self.browse_obj_button.setText(t("GUI_BROWSE_BUTTON"))
        self.browse_polyvox_button.setText(t("GUI_BROWSE_BUTTON"))
        self.browse_outdir_button.setText(t("GUI_BROWSE_BUTTON"))
        # 占位符
        self.obj_path_edit.setPlaceholderText(t("GUI_SELECT_OR_DROP_OBJ_PLACEHOLDER"))
        self.polyvox_path_edit.setPlaceholderText(t("GUI_SELECT_POLYVOX_PLACEHOLDER"))
        self.outdir_path_edit.setPlaceholderText(t("GUI_SELECT_OUTDIR_PLACEHOLDER"))

        # --- 移除：此函数调用已不再需要，PathLineEdit 会自动处理 ---
        # self._update_all_line_edit_tooltips()

        # 拖放提示
        self.drop_label.setText(t("GUI_DROP_TIP"))
        # 其他
        self.voxel_size_label.setText(t("GUI_VOXEL_SIZE"))
        self.voxel_size_metric_label.setText(t("GUI_VOXEL_SIZE_METRIC"))
        self.status_label.setText(t("GUI_STATUS_READY"))
        self.start_button.setText(t("GUI_START_BUTTON"))
        self.stop_button.setText(t("GUI_STOP_BUTTON"))
        # --- 新增文本 ---
        self.auto_material_checkbox.setText(t("GUI_AUTO_MATERIAL_CHECK"))
        self.configure_material_button.setText(t("GUI_CONFIGURE_MATERIAL_BUTTON"))
        
        # --- 修改：语言切换时，重新执行检查以更新提示文本 ---
        current_obj_path = self.obj_path_edit.text()
        if current_obj_path:
            self._on_obj_path_changed(current_obj_path)
        else:
            # 如果没有路径，确保标签是隐藏的
            self.dependency_status_label.hide()

    def _create_menu_bar(self):
        menu_bar = self.menuBar()
        self.options_menu = menu_bar.addMenu("Options") # 文本会被 retranslate_ui 覆盖
        
        # 首选项
        self.preferences_action = QAction("Preferences...", self)
        self.preferences_action.triggered.connect(self.open_preferences)
        self.options_menu.addAction(self.preferences_action)

        # --- 新增：添加分割线和“关于”菜单项 ---
        self.options_menu.addSeparator()
        
        self.about_action = QAction("About...", self)
        self.about_action.triggered.connect(self._show_about_dialog)
        self.options_menu.addAction(self.about_action)
    
    # --- 新增：显示“关于”对话框的槽函数 ---
    def _show_about_dialog(self):
        """创建并显示“关于”对话框（带滚动框显示LICENSE内容）"""

        icon_path = resource_path("img/icon.ico")
        # 读取LICENSE文件内容
        license_path = resource_path("LICENSE")
        license_text = ""
        try:
            with open(license_path, "r", encoding="utf-8") as f:
                license_text = f.read()
        except Exception:
            # --- 修复：使用翻译ID ---
            license_text = t("ABOUT_LICENSE_NOT_FOUND")

        dialog = QDialog(self)
        # --- 修改：使用 resource_path ---
        dialog.setWindowIcon(QIcon(resource_path("img/icon.ico")))
        dialog.setWindowTitle(t("ABOUT_TITLE"))
        dialog.setMinimumWidth(480)
        dialog.setMinimumHeight(420)

        layout = QVBoxLayout(dialog)

        # 顶部图标和标题
        top_widget = QWidget()
        top_layout = QVBoxLayout(top_widget)
        top_layout.setAlignment(Qt.AlignCenter)
        icon_label = QLabel()
        icon_pixmap = QPixmap(icon_path).scaled(64, 64, Qt.KeepAspectRatio, Qt.SmoothTransformation)
        icon_label.setPixmap(icon_pixmap)
        icon_label.setAlignment(Qt.AlignCenter)
        title_label = QLabel(f"<span style='font-size:22px; font-weight:bold;'>{APPLICATION_NAME}</span>")
        title_label.setAlignment(Qt.AlignCenter)
        top_layout.addWidget(icon_label)
        top_layout.addWidget(title_label)
        layout.addWidget(top_widget)

        # 信息表格整体居中
        info_html = f"""
            <div style="text-align:center;">
            <table style="margin:auto; border:0;">
                <tr>
                <td align="right" style="padding-right:10px;"><b>{t('ABOUT_VERSION')}:</b></td>
                <td align="left">{APP_VERSION}</td>
                </tr>
                <tr>
                <td align="right" style="padding-right:10px;"><b>{t('ABOUT_AUTHOR')}:</b></td>
                <td align="left">{APP_AUTHOR}</td>
                </tr>
                <tr>
                <td align="right" style="padding-right:10px;"><b>{t('ABOUT_GITHUB')}:</b></td>
                <td align="left"><a href='{APP_GITHUB_URL}' style='color:#4aa3ff;text-decoration:underline'>{APP_GITHUB_URL}</a></td>
                </tr>
            </table>
            </div>
        """
        info_label = QLabel(info_html)
        info_label.setTextFormat(Qt.RichText)
        info_label.setOpenExternalLinks(True)
        info_label.setAlignment(Qt.AlignCenter)
        layout.addWidget(info_label)

        # 分割线
        line = QLabel("<hr>")
        line.setTextFormat(Qt.RichText)
        layout.addWidget(line)

        # 许可证标题
        license_title = QLabel(f"<b>{t('ABOUT_LICENSE')}:</b>")
        license_title.setAlignment(Qt.AlignLeft)
        layout.addWidget(license_title)

        # 滚动框显示LICENSE内容
        license_edit = QTextEdit()
        license_edit.setReadOnly(True)
        license_edit.setPlainText(license_text)
        
        # --- 修复：设置尺寸策略以强制垂直扩展 ---
        from PySide6.QtWidgets import QSizePolicy
        size_policy = QSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        license_edit.setSizePolicy(size_policy)
        
        layout.addWidget(license_edit)

        # 按钮
        button_box = QDialogButtonBox(QDialogButtonBox.Ok)
        button_box.accepted.connect(dialog.accept)
        layout.addWidget(button_box)

        dialog.exec()

    def open_preferences(self):
        if self._preferences_dialog is None:
            self._preferences_dialog = PreferencesDialog(self.config, self)

        self._preferences_dialog.load_config(self.config)
        
        if self._preferences_dialog.exec():
            new_config = self._preferences_dialog.get_config()
            
            lang_has_changed = self.config.get("lang") != new_config.get("lang")
            
            self.config = new_config
            
            apply_application_styles(QApplication.instance(), self.config)
            
            if lang_has_changed:
                load_translations(self.config["lang"])
                self.retranslate_ui()
                self._preferences_dialog.retranslate_ui() 

        else:
            # --- 修复：如果用户点击“取消”，也需要恢复主题 ---
            # 这是因为在对话框中更改主题会立即生效以供预览
            apply_application_styles(QApplication.instance(), self.config)

        # --- 核心修复：在对话框关闭后，无条件地将焦点设置到开始按钮上 ---
        # 即使按钮被禁用，它仍然可以接收焦点。
        # 这可以最可靠地防止焦点自动回到日志窗口，从而彻底避免不必要的滚动。
        self.start_button.setFocus(Qt.FocusReason.OtherFocusReason)

    # --- 移除：以下两个方法已不再需要，其功能已封装到 PathLineEdit 中 ---
    # @Slot(str)
    # def _update_line_edit_tooltip(self, text):
    #     ...
    #
    # def _update_all_line_edit_tooltips(self):
    #     ...

    def _update_material_controls_state(self, _=None):
        """更新材质配置相关控件的状态"""
        is_obj_valid = self.dependencies_ok and bool(self.obj_path_edit.text())

        # 1. 根据OBJ文件的有效性，启用或禁用“自动检测”复选框
        self.auto_material_checkbox.setEnabled(is_obj_valid)

        # 2. “配置材质”按钮的状态现在依赖于两个条件：
        #    a) OBJ文件必须有效
        #    b) “自动检测”必须未被勾选
        is_auto_checked = self.auto_material_checkbox.isChecked()
        self.configure_material_button.setEnabled(is_obj_valid and not is_auto_checked)
        
        if is_auto_checked:
            self.material_mappings = {} # 启用自动时，清空自定义映射

    def _parse_mtl_for_gui(self, mtl_path):
        """在Python端解析MTL，为GUI提供数据"""
        materials = {}
        current_mat = None
        try:
            with open(mtl_path, 'r', encoding='utf-8') as f:
                for line in f:
                    # --- 核心修复：使用 maxsplit=1 来正确处理带空格的材质名和纹理路径 ---
                    parts = line.strip().split(maxsplit=1)
                    if not parts: continue
                    
                    command = parts[0]
                    value = parts[1] if len(parts) > 1 else ""

                    if command == 'newmtl':
                        current_mat = value
                        materials[current_mat] = {}
                    elif command == 'map_Kd' and current_mat:
                        materials[current_mat]['texture'] = value
                    elif command == 'Kd' and current_mat:
                        # Kd 值通常不含空格，但为了安全也用 parts
                        color_parts = value.split()
                        if len(color_parts) >= 3:
                            r, g, b = [int(float(c) * 255) for c in color_parts[:3]]
                            materials[current_mat]['color'] = (r, g, b)
        except Exception as e:
            msg = t("GUI_MTL_PARSE_ERROR", e=str(e))
            logging.error(msg)
        return materials

    def open_material_mapper(self):
        obj_path = self.obj_path_edit.text()
        if not obj_path: return

        # 1. 解析MTL文件以获取基础材质列表
        mtl_filename = None
        try:
            with open(obj_path, 'r', encoding='utf-8') as f:
                for line in f:
                    if line.strip().startswith('mtllib'):
                        mtl_filename = line.strip().split(None, 1)[1]
                        break
        except Exception as e:
            msg = t("GUI_OBJ_MTL_REF_ERROR", e=str(e))
            logging.error(msg)
            # --- 修复：使弹窗内容可选 ---
            msg_box = QMessageBox(self)
            msg_box.setIcon(QMessageBox.Warning)
            msg_box.setWindowTitle(t("MAT_MAP_ERROR_TITLE"))
            msg_box.setText(t("GUI_PRECHECK_OBJ_READ_ERROR", error=str(e)))
            msg_box.setTextInteractionFlags(Qt.TextSelectableByMouse)
            msg_box.exec()
            return
        
        if not mtl_filename:
            # 这个是信息提示，可以不改，但为了统一也改一下
            msg_box = QMessageBox(self)
            msg_box.setIcon(QMessageBox.Information)
            msg_box.setWindowTitle(t("MAT_MAP_NO_MTL_TITLE"))
            msg_box.setText(t("GUI_PRECHECK_NO_MTL_REF"))
            msg_box.setTextInteractionFlags(Qt.TextSelectableByMouse)
            msg_box.exec()
            return

        obj_dir = os.path.dirname(obj_path)
        mtl_path = os.path.join(obj_dir, mtl_filename)
        
        # `parsed_data` 是当前OBJ文件材质的“源数据”
        parsed_data = self._parse_mtl_for_gui(mtl_path)
        if not parsed_data:
            QMessageBox.warning(self, t("MAT_MAP_ERROR_TITLE"), t("GUI_PRECHECK_MTL_READ_ERROR", error=""))
            return

        # --- 修复：合并解析数据和已存的用户配置 ---
        # 1. 创建一个最终要传递给对话框的数据字典
        import copy
        dialog_data = copy.deepcopy(parsed_data)

        # 2. 用 self.material_properties 中已有的用户设置来更新它
        #    这会保留用户对 vox_type 和其他属性的修改
        for mat_name, saved_props in self.material_properties.items():
            if mat_name in dialog_data:
                dialog_data[mat_name].update(saved_props)
        
        # 3. 将合并后的完整数据传递给对话框
        default_mode = self.config.get("manual_mapping_default", "$TD_auto")
        # --- 修复：移除多余的 self.config 参数 ---
        dialog = MaterialMappingDialog(
            dialog_data, 
            obj_dir, 
            self.material_mappings, 
            default_mode, 
            self # 最后一个参数是 parent
        )

        if dialog.exec():
            # 对话框返回后，更新主窗口的数据
            self.material_mappings = dialog.get_mappings()
            # --- 修复：调用 get_full_properties 来获取完整数据 ---
            self.material_properties = dialog.get_full_properties()
            
            # --- 修复：恢复并增强详细的日志记录 ---
            # --- 修复：现在 props 字典中会包含 vox_type ---
            for mat_name, props in self.material_properties.items():
                td_note = self.material_mappings.get(mat_name, "$TD_auto")
                vox_type = props.get('vox_type', 'diffuse')
                # 使用 get_material_profiles 获取翻译后的显示名称
                td_display = MaterialMappingDialog.get_material_profiles().get(td_note, {}).get('display', td_note)
                
                # 将 vox_type 转为对应语言的名称译文
                vox_type_display = t(MaterialPropertyManager.PROPERTIES.get(vox_type, {}).get('label_id', vox_type))
                log_msg = t("MAT_MAP_UPDATED_LOG", mat_name=mat_name, td_note=td_display, vox_type=vox_type_display)
                logging.info(log_msg)

    def start_processing(self):
        """开始处理模型。"""
        # --- 新增：在开始时清空日志 ---
        self.log_edit.clear()

        # 1. 验证输入
        if not all([self.obj_path_edit.text(), self.polyvox_path_edit.text(), self.outdir_path_edit.text()]):
            # --- 修复：使弹窗内容可选 ---
            msg_box = QMessageBox(self)
            msg_box.setIcon(QMessageBox.Warning)
            msg_box.setWindowTitle(t("GUI_INPUT_ERROR_TITLE"))
            msg_box.setText(t("GUI_INPUT_ERROR"))
            msg_box.setTextInteractionFlags(Qt.TextSelectableByMouse)
            msg_box.exec()
            return

        # 2. 禁用UI
        self.set_ui_enabled(False)
        self.status_label.setText(t("GUI_STATUS_BUSY"))
        # --- 新增：在开始时重置进度条为确定模式 ---
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setTextVisible(True)

        # --- 修改：移除文件预计算逻辑，因为现在由 workflow 管理 ---
        self.generated_files = []

        # 中止转换
        self.stop_button.setEnabled(True)
        self.stopped_by_user = False

        # 3. 创建线程和Worker
        self.thread = QThread()
        
        # --- 修复：在这里组合 material_maps 和 material_properties ---
        # 新的格式: "mat_name:$TD_note:vox_type"
        final_material_maps = []
        for mat_name, td_note in self.material_mappings.items():
            # 从 properties 字典中获取该材质的 vox_type，如果没有则默认为 'diffuse'
            vox_type = self.material_properties.get(mat_name, {}).get('vox_type', 'diffuse')
            final_material_maps.append(f"{mat_name}:{td_note}:{vox_type}")

        # --- 修复：只传递数值属性给 material_properties ---
        numeric_properties = {}
        for mat_name, props in self.material_properties.items():
            num_props = {k: v for k, v in props.items() if isinstance(v, (int, float))}
            if num_props:
                numeric_properties[mat_name] = num_props

        # --- 新增：从配置中读取容差值 ---
        angle_tol = self.config.get("angle_tol", 0.1)
        dist_tol = self.config.get("dist_tol", 0.01)

        self.worker = Worker(
            self.obj_path_edit.text(),
            self.outdir_path_edit.text(),
            self.polyvox_path_edit.text(),
            self.voxel_size_spinbox.value(),
            self.config.get("lang", "en"),
            material_maps=final_material_maps,
            material_properties=numeric_properties,
            temp_dir_path=self.config.get("temp_dir_path"), # <-- 新增
            # --- 新增：传递容差参数 ---
            angle_tol=angle_tol,
            dist_tol=dist_tol
        )
        self.worker.moveToThread(self.thread)

        # 4. 连接信号和槽
        self.thread.started.connect(self.worker.run)
        self.worker.finished.connect(self.on_finished) # 连接到新的 on_finished
        self.worker.error.connect(self.on_error)
        # --- 新增：连接到新的 success 信号 ---
        self.worker.success.connect(self.on_success)
        self.worker.progress.connect(self.update_progress)
        # --- 修改：连接到新的 stage_changed 信号 ---
        self.worker.stage_changed.connect(self.on_stage_changed)
        
        # --- 修改：将线程清理连接移到 on_finished 中 ---
        # self.worker.finished.connect(self.thread.quit)
        # self.worker.finished.connect(self.worker.deleteLater)
        # self.thread.finished.connect(self.thread.deleteLater)

        # 5. 启动线程
        self.thread.start()

    def stop_processing(self):
        if hasattr(self, 'worker') and self.worker is not None:
            self.stopped_by_user = True
            self.worker.stop()
            self.status_label.setText(t("GUI_STATUS_STOPPING"))
            self.stop_button.setEnabled(False)

    def clean_output_dirs(self):
        """此函数现在不再需要，因为清理逻辑已移至 workflow。保留为空或移除。"""
        pass

    def set_ui_enabled(self, enabled):
        """启用或禁用所有输入相关的UI控件。"""
        # --- 新增：禁用/启用整个菜单栏 ---
        self.menuBar().setEnabled(enabled)

        # 路径输入框
        self.obj_path_edit.setEnabled(enabled)
        self.polyvox_path_edit.setEnabled(enabled)
        self.outdir_path_edit.setEnabled(enabled)
        
        # 浏览按钮
        self.browse_obj_button.setEnabled(enabled)
        self.browse_polyvox_button.setEnabled(enabled)
        self.browse_outdir_button.setEnabled(enabled)
        
        # 参数和配置控件
        # --- （如果注释掉以下这行）禁止更改体素大小，强制使用默认值0.1 ---
        self.voxel_size_spinbox.setEnabled(enabled)
        self.auto_material_checkbox.setEnabled(enabled)
        # 配置按钮的状态由 _update_material_controls_state 控制，
        # 但在禁用时需要强制禁用
        if enabled:
            self._update_material_controls_state()
        else:
            self.configure_material_button.setEnabled(False)

        # 主操作按钮
        # 只有在UI启用时，才根据依赖状态决定“开始”按钮是否可用
        if enabled:
            self._update_start_button_state()
        else:
            self.start_button.setEnabled(False)
        
        # 停止按钮的状态与启用状态相反
        self.stop_button.setEnabled(not enabled)

    def on_error(self, error_message):
        """处理来自工作线程的错误信号"""
        self.status_label.setText(t("GUI_STATUS_ERROR"))
        # --- 修复：使弹窗内容可选 ---
        msg_box = QMessageBox(self)
        msg_box.setIcon(QMessageBox.Critical)
        msg_box.setWindowTitle(t("GUI_STATUS_ERROR_TITLE"))
        msg_box.setText(error_message)
        msg_box.setTextInteractionFlags(Qt.TextSelectableByMouse)
        msg_box.exec()
        
        self.clean_output_dirs()
        # UI的恢复将由 on_finished 统一处理，这里不再调用 set_ui_enabled

    # --- 新增：专门处理成功完成的槽函数 ---
    def on_success(self):
        """仅在任务成功完成时调用"""
        self.status_label.setText(t("GUI_STATUS_COMPLETE"))
        self.progress_bar.setValue(100)
        
        reply = QMessageBox.information(
            self,
            t("GUI_COMPLETE_TITLE"),
            t("GUI_COMPLETE_MESSAGE", path=self.outdir_path_edit.text()),
            QMessageBox.Open | QMessageBox.Cancel,
            QMessageBox.Open
        )
        if reply == QMessageBox.Open:
            os.startfile(self.outdir_path_edit.text())

    def on_finished(self):
        """
        无论成功、失败还是中止，工作线程结束时都会调用此函数。
        负责最终的清理和UI状态恢复。
        """
        # --- 修复：在清理线程前，先断开所有信号连接，以避免时序问题 ---
        if hasattr(self, 'worker') and self.worker:
            try:
                self.thread.started.disconnect(self.worker.run)
                self.worker.finished.disconnect(self.on_finished)
                self.worker.error.disconnect(self.on_error)
                self.worker.success.disconnect(self.on_success)
                self.worker.progress.disconnect(self.update_progress)
                self.worker.stage_changed.disconnect(self.on_stage_changed)
            except (TypeError, RuntimeError) as e:
                # 如果信号已经断开，disconnect会抛出异常，可以安全地忽略
                logging.debug(f"Error disconnecting worker signals, likely already disconnected: {e}")

        # --- (以下代码保持不变) ---
        if self.progress_bar.maximum() == 0:
            self.progress_bar.setRange(0, 100)
            self.progress_bar.setTextVisible(True)
        
        if self.stopped_by_user:
            self.status_label.setText(t("GUI_STATUS_STOPPED"))
            self.progress_bar.setValue(0)
        else:
            QTimer.singleShot(500, lambda: self.progress_bar.setValue(0))

        self.set_ui_enabled(True)
        self.stop_button.setEnabled(False)

        if hasattr(self, 'thread') and self.thread is not None:
            self.thread.quit()
            self.thread.wait()
            self.worker.deleteLater()
            self.thread.deleteLater()
            self.worker = None
            self.thread = None

        # --- 彻底修复：延迟一帧再同步日志窗口的滚动条 ---
        def sync_log_scroll():
            scrollbar = self.log_edit.verticalScrollBar()
            scrollbar.setValue(scrollbar.maximum())
        QTimer.singleShot(0, sync_log_scroll)

    @Slot(int, int)
    def update_progress(self, current, total):
        if total > 0:
            progress_percent = int((current / total) * 100)
            self.progress_bar.setValue(progress_percent)
            self.progress_bar.setFormat(f"%p%")
            self.status_label.setText(t("GUI_PROCESSING_SURFACE", current=current, total=total))

    @Slot(str)
    def _on_obj_path_changed(self, path):
        """当OBJ路径被设置时，执行预检查并更新UI"""
        message, is_ok = pre_check_dependencies(path)
        
        # --- 修改：存储依赖检查结果 ---
        self.dependencies_ok = is_ok
        
        if message:
            self.dependency_status_label.setText(message)
            if is_ok:
                self.dependency_status_label.setStyleSheet("color: green;")
            else:
                self.dependency_status_label.setStyleSheet("color: red;") # 错误用红色更醒目
            self.dependency_status_label.show()
        else:
            self.dependency_status_label.hide()
            
        # --- 修复：在检查后同时更新所有相关控件的状态 ---
        self._update_start_button_state()
        self._update_material_controls_state()

if __name__ == "__main__":
    app = QApplication(sys.argv)

    app.setStyle("Fusion")
    
    app.setWindowIcon(QIcon(resource_path("img/icon.ico")))

    settings = QSettings(ORGANIZATION_NAME, APPLICATION_NAME)
    if settings.value("reset_on_next_run", False, type=bool):
        settings.clear()
        settings.setValue("reset_on_next_run", False) 
        settings.sync()

    initial_lang = settings.value("lang")
    if initial_lang is None:
        initial_lang = "en"
    load_translations(initial_lang)

    splash_pix = QPixmap(resource_path("img/splash.png"))
    splash = QSplashScreen(splash_pix, Qt.WindowStaysOnTopHint)
    splash.setMask(splash_pix.mask())
    splash.showMessage(t("SPLASH_INIT"), Qt.AlignBottom | Qt.AlignLeft, Qt.white)
    splash.show()
    time.sleep(0.45)
    app.processEvents()

    splash.showMessage(t("SPLASH_LOGGER"), Qt.AlignBottom | Qt.AlignLeft, Qt.white)
    setup_logger()
    time.sleep(0.45)
    app.processEvents()

    # --- 新增：在加载UI前，预先填充字体缓存 ---
    splash.showMessage(t("SPLASH_FONTS"), Qt.AlignBottom | Qt.AlignLeft, Qt.white)
    populate_monospace_font_cache()
    time.sleep(0.45)
    app.processEvents()

    splash.showMessage(t("SPLASH_UI"), Qt.AlignBottom | Qt.AlignLeft, Qt.white)
    window = MainWindow()
    time.sleep(0.45)
    app.processEvents()

    window.show()
    splash.finish(window)
    
    sys.exit(app.exec())