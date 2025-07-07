from enum import Enum, auto

class ProcessingStage(Enum):
    """定义处理流程的各个阶段"""
    PREPARING = auto()            # 准备阶段 (不确定进度)
    PROCESSING_SURFACES = auto()  # 处理表面 (确定进度)
    MERGING = auto()              # 合并文件 (不确定进度)
    CLEANUP = auto()              # 清理文件 (不确定进度)
    COMPLETE = auto()             # 完成
    ERROR = auto()                # 错误
    STOPPED = auto()              # 用户中止

class SortMode(Enum):
    """定义材质列表的排序模式"""
    ALPHA_ASC = 0
    ALPHA_DESC = 1
    TYPE_ASC_ALPHA_ASC = 2
    TYPE_DESC_ALPHA_DESC = 3