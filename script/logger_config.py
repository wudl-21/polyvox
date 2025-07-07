import logging
import sys
from PySide6.QtCore import QObject, Signal

class ColorFormatter(logging.Formatter):
    # ANSI escape codes for console
    GREY = "\x1b[38;20m"
    GREEN_BOLD = "\x1b[1;32m"
    YELLOW_BOLD = "\x1b[1;33m"
    RED_BOLD = "\x1b[1;31m"
    BOLD_RED = "\x1b[1;41m"
    RESET = "\x1b[0m"
    YELLOW = "\x1b[33;20m"
    RED = "\x1b[31;20m"
    
    # HTML colors for GUI
    HTML_COLORS = {
        logging.DEBUG: "grey",
        logging.INFO: "green",
        logging.WARNING: "orange",
        logging.ERROR: "red",
        logging.CRITICAL: "red",
    }

    def __init__(self, fmt):
        # --- FIX: Call the parent class's __init__ method ---
        super().__init__(fmt)
        
        self.LEVEL_STYLES = {
            logging.DEBUG: (self.GREEN_BOLD, "[{levelname}]"),
            logging.INFO: (self.GREEN_BOLD, "[{levelname}]"),
            logging.WARNING: (self.YELLOW_BOLD, "[{levelname}]"),
            logging.ERROR: (self.RED_BOLD, "[{levelname}]"),
            logging.CRITICAL: (self.RED_BOLD, "[{levelname}]"),
        }

    def format(self, record):
        # 检查记录中是否有 'is_gui' 标志
        is_gui = getattr(record, 'is_gui', False)

        if is_gui:
            # --- 生成 HTML 格式 ---
            log_level_color = self.HTML_COLORS.get(record.levelno, "black")
            levelname_html = f'<font color="{log_level_color}"><b>[{record.levelname}]</b></font>'
            # 使用 html.escape 避免消息内容中的特殊字符（如 < >）破坏HTML结构
            import html
            message_html = html.escape(record.getMessage())
            return f"{levelname_html} - {message_html}"
        else:
            # --- 生成 ANSI 格式 (保持原有逻辑) ---
            color, fmt = self.LEVEL_STYLES.get(record.levelno, (self.GREEN_BOLD, "[{levelname}]"))
            levelname_colored = f"{color}{fmt.format(levelname=record.levelname)}{self.RESET}"
            
            # 创建一个记录的副本以避免修改原始记录
            record_copy = logging.makeLogRecord(record.__dict__)
            record_copy.levelname = levelname_colored
            
            # 使用基础的 Formatter 来完成最终的格式化
            formatter = logging.Formatter("%(levelname)s - %(message)s")
            return formatter.format(record_copy)

class QtLogHandler(logging.Handler, QObject):
    """
    一个将日志记录发送到Qt信号的处理器。
    现在它会生成HTML格式的日志。
    """
    new_record = Signal(str)

    def __init__(self):
        super().__init__()
        QObject.__init__(self)
        # 为这个处理器设置我们自定义的格式化器
        # --- FIX: Provide the 'fmt' argument ---
        self.setFormatter(ColorFormatter("[%(levelname)s] - %(message)s"))

    def emit(self, record):
        # 添加一个标志，让 Formatter 知道这是给GUI的
        record.is_gui = True
        msg = self.format(record)
        self.new_record.emit(msg)

def setup_logger():
    """设置全局日志记录器"""
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)

    # 清除任何可能存在的旧处理器
    if logger.hasHandlers():
        logger.handlers.clear()

    # 创建一个处理器，用于将日志输出到控制台
    console_handler = logging.StreamHandler(sys.stdout)
    # --- FIX: Provide the 'fmt' argument ---
    console_handler.setFormatter(ColorFormatter("[%(levelname)s] - %(message)s")) # 控制台也使用我们的格式化器
    logger.addHandler(console_handler)

    # 返回一个Qt处理器，由GUI自己决定何时添加
    return QtLogHandler()