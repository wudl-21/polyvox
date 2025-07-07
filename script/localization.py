import json
import os
import logging
import sys

_translations = {}
_current_lang = "en"

# --- 新增：在 localization.py 中独立定义 resource_path 函数 ---
# 这样它就不再依赖 main_gui.py
def resource_path(relative_path):
    """ 获取资源的绝对路径，适用于开发环境和PyInstaller打包环境 """
    try:
        # PyInstaller 创建一个临时文件夹，并将路径存储在 _MEIPASS 中
        base_path = sys._MEIPASS
    except Exception:
        # 在开发环境中，基础路径是项目根目录
        base_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    return os.path.join(base_path, relative_path)

def load_translations(lang_code):
    global _translations, _current_lang
    _current_lang = lang_code
    # --- 修改：使用本文件中定义的 resource_path ---
    path = resource_path(f"locale/{lang_code}.json")
    try:
        with open(path, 'r', encoding='utf-8') as f:
            _translations = json.load(f)
    except FileNotFoundError:
        logging.error(f"Translation file not found for language '{lang_code}' at {path}")
        _translations = {}
    except json.JSONDecodeError:
        logging.error(f"Error decoding JSON from translation file: {path}")
        _translations = {}

def get_available_languages():
    """
    扫描 locale 目录，返回一个包含所有可用语言的字典。
    键是语言代码（如 'en'），值是其母语名称（如 'English'）。
    """
    available_langs = {}
    try:
        locale_dir = resource_path('locale')
        if not os.path.isdir(locale_dir):
            logging.error(f"Locale directory not found at {locale_dir}")
            return {'en': 'English'} # Fallback

        for filename in os.listdir(locale_dir):
            if filename.endswith(".json"):
                lang_code = os.path.splitext(filename)[0]
                try:
                    with open(os.path.join(locale_dir, filename), 'r', encoding='utf-8') as f:
                        data = json.load(f)
                        # 使用特殊键获取母语名称，如果找不到则回退到语言代码
                        native_name = data.get("_LANG_NAME_NATIVE_", lang_code)
                        available_langs[lang_code] = native_name
                except (json.JSONDecodeError, IOError) as e:
                    logging.warning(f"Could not read or parse language file {filename}: {e}")
    except Exception as e:
        logging.error(f"Failed to scan for available languages: {e}")
        return {'en': 'English'} # Fallback
    
    return available_langs

def get_message(message_id, **kwargs):
    """
    根据ID获取翻译后的消息，并替换占位符。
    占位符格式为 {key}。
    """
    message = _translations.get(message_id, message_id)
    try:
        return message.format(**kwargs)
    except KeyError as e:
        logging.warning(f"Missing placeholder {e} for message ID '{message_id}' in language '{_current_lang}'")
        return message

# 快捷方式
t = get_message