import logging
from localization import t, load_translations
import geometry_processor as geo
import external_tools as tools
import os
import argparse
import shutil
from logger_config import setup_logger
# --- 新增导入 ---
import tempfile
from contextlib import contextmanager

# --- 修改：从新的核心枚举文件导入，打破循环依赖 ---
from core_enums import ProcessingStage

# --- 新增：一个上下文管理器来处理可回滚的文件操作 ---
@contextmanager
def atomic_commit(target_dir, stop_checker=None):
    """
    一个上下文管理器，用于安全地将文件从临时位置移动到最终目标。
    如果目标文件/目录已存在，会先将其备份。
    如果过程中断，会自动回滚所有操作。
    """
    backup_dir = tempfile.mkdtemp(prefix="polyvox_backup_")
    moved_items = []  # (type, src, dest) 'type' can be 'move' or 'backup'
    
    def commit(source_path, dest_name):
        if stop_checker and stop_checker(): raise RuntimeError(t("GUI_USER_STOPPED"))
        
        source_item = os.path.join(source_path, dest_name)
        target_item = os.path.join(target_dir, dest_name)
        
        if not os.path.exists(source_item): return

        # 1. 如果目标已存在，备份它
        if os.path.exists(target_item):
            backup_path = os.path.join(backup_dir, dest_name)
            shutil.move(target_item, backup_path)
            moved_items.append(('backup', backup_path, target_item))
            msg = t("PY_BACKUP_EXISTING", target=target_item, backup=backup_path)
            logging.info(msg)

        # 2. 移动新文件到目标位置
        shutil.move(source_item, target_item)
        moved_items.append(('commit', source_item, target_item))
        msg = t("PY_COMMIT_SUCCESS", target=target_item)
        logging.info(msg)

    try:
        yield commit
    except Exception:
        # --- 回滚逻辑 ---
        msg = t("PY_COMMIT_ERROR")
        logging.warning(msg)
        for op_type, src, dest in reversed(moved_items):
            try:
                if op_type == 'commit': # 撤销提交：删除新移入的文件
                    if os.path.isdir(dest): shutil.rmtree(dest)
                    else: os.remove(dest)
                    msg = t("PY_ROLLBACK_REMOVED", dest=dest)
                    logging.info(msg)
                elif op_type == 'backup': # 恢复备份
                    shutil.move(src, dest)
                    msg = t("PY_ROLLBACK_RESTORED", dest=dest)
                    logging.info(msg)
            except Exception as e:
                msg = t("PY_ROLLBACK_ERROR", error=str(e), target_dir=target_dir.replace("\\", "/"), backup_dir=backup_dir.replace("\\", "/"))
                logging.error(msg)
        raise # 重新抛出异常，让上层知道出错了
    finally:
        # --- 清理备份目录 ---
        shutil.rmtree(backup_dir)
        msg = t("PY_COMMIT_CLEANUP")
        logging.info(msg)


# --- 修改函数签名，增加 stop_check_callback 和新的容差参数 ---
def process_model(obj_path, out_dir, polyvox_exe, voxel_size, lang, 
                  progress_callback=None, stage_callback=None, stop_check_callback=None, 
                  material_maps=None, material_properties=None, temp_dir_path=None,
                  angle_tol=1e-5, dist_tol=1e-4):
    """
    主处理流程，编排所有步骤。
    """
    # --- 修改：如果提供了自定义路径，则在该路径下创建临时目录 ---
    work_dir = tempfile.mkdtemp(prefix="polyvox_work_", dir=temp_dir_path if temp_dir_path and os.path.isdir(temp_dir_path) else None)
    msg = t("PY_CREATED_TEMP_DIR", dir=work_dir.replace("\\", "/"))
    logging.info(msg)

    try:
        def report_stage(stage, text_id, **kwargs):
            if stop_check_callback and stop_check_callback(): raise RuntimeError(t("GUI_USER_STOPPED"))
            if stage_callback:
                stage_callback(stage, t(text_id, **kwargs))

        report_stage(ProcessingStage.PREPARING, "GUI_STATUS_BUSY")
        logging.info(t("PY_WF_STEP1_PARSE"))
        
        # --- 新增：获取OBJ文件名作为子目录名 ---
        obj_basename = os.path.splitext(os.path.basename(obj_path))[0]

        # --- 所有操作都在临时工作目录中进行 ---
        # --- 核心修改：保持临时目录结构扁平化 ---
        vox_dir = os.path.join(work_dir, "vox")
        xml_dir = os.path.join(work_dir, "prefab")
        temp_obj_dir = os.path.join(work_dir, "temp_obj")
        os.makedirs(vox_dir, exist_ok=True)
        os.makedirs(xml_dir, exist_ok=True)
        os.makedirs(temp_obj_dir, exist_ok=True)

        # 1. 解析OBJ
        report_stage(ProcessingStage.PREPARING, "PY_WF_STEP1_PARSE")
        vertices, uvs, normals_from_file, faces, face_materials, mtllib = geo.parse_obj(obj_path, stop_check_callback)
        
        # --- 核心修改：调整处理顺序 ---
        # 2. 顶点焊接
        report_stage(ProcessingStage.PREPARING, "PY_WF_STEP1_WELD")
        logging.info(t("PY_WF_WELDING_VERTICES", count=len(vertices)))
        vertices, faces = geo.weld_vertices(vertices, faces)
        logging.info(t("PY_WF_WELDING_COMPLETE", count=len(vertices)))

        # 3. 过滤重复面（在焊接后！）
        report_stage(ProcessingStage.PREPARING, "PY_WF_STEP1_FILTER")
        logging.info(t("PY_WF_FILTERING_FACES", count=len(faces)))
        faces, face_materials = geo.filter_duplicate_faces(faces, face_materials)
        logging.info(t("PY_WF_FILTERING_COMPLETE", count=len(faces)))

        # 4. 计算法线并分组
        report_stage(ProcessingStage.PREPARING, "PY_WF_STEP1_GROUP")
        normals_arr, valid_indices = geo.get_face_normals(vertices, faces, stop_check_callback)
        
        # 更新列表以匹配有效法线
        faces = [faces[i] for i in valid_indices]
        face_materials = [face_materials[i] for i in valid_indices]
        
        # --- 核心修复：将可配置的容差传递给分组函数 ---
        groups = geo.group_coplanar_faces(vertices, faces, normals_arr, stop_check_callback, angle_tol=angle_tol, dist_tol=dist_tol)

        # 5. 计算所有表面的变换信息
        report_stage(ProcessingStage.PREPARING, "PY_WF_STEP2")
        surfaces_info = geo.calculate_surface_transforms(vertices, faces, normals_arr, groups, voxel_size, stop_check_callback)
        logging.info(t("PY_WF_FOUND_SURFACES", count=len(surfaces_info)))

        # 6. 循环处理每个表面
        report_stage(ProcessingStage.PROCESSING_SURFACES, "PY_WF_STEP3")
        xml_paths = []
        total_surfaces = len(surfaces_info)
        obj_src_dir = os.path.dirname(os.path.abspath(obj_path))

        for i, surf in enumerate(surfaces_info):
            if stop_check_callback and stop_check_callback(): raise RuntimeError(t("GUI_USER_STOPPED"))
            logging.info(t("PY_WF_PROCESS_SURFACE", current=i+1, total=len(surfaces_info), name=surf['name']))
            if progress_callback:
                progress_callback(i + 1, total_surfaces)
            
            out_obj = os.path.join(temp_obj_dir, f"{surf['name']}.obj")
            geo.export_single_surface_obj(
                vertices, uvs, normals_from_file, faces, face_materials, 
                surf['face_indices'], out_obj, mtllib, 
                obj_src_dir, obj_path, normals_arr
            )

            # --- 核心修改：所有 .vox 文件都直接生成在扁平的 vox_dir 中 ---
            out_vox = os.path.join(vox_dir, f"{surf['name']}.vox")
            tools.run_polyvox(
                polyvox_exe, out_obj, out_vox, voxel_size, lang, 
                material_maps=material_maps, 
                material_properties=material_properties,
                stop_checker=stop_check_callback
            )

            # --- 核心修改：临时XML也从扁平的 vox_dir 中移动 ---
            temp_xml_path = os.path.splitext(out_vox)[0] + ".xml"
            final_xml_path = os.path.join(xml_dir, f"{surf['name']}.xml")
            
            if os.path.exists(temp_xml_path):
                shutil.move(temp_xml_path, final_xml_path)
            else:
                logging.error(t("PY_TOOL_XML_NOT_FOUND", path=temp_xml_path))
                continue

            tools.update_group_transform(final_xml_path, surf["center"], surf["normal_euler_deg"])
            xml_paths.append(final_xml_path)
        
        # 4. 合并XML
        report_stage(ProcessingStage.MERGING, "PY_WF_STEP4")
        merged_xml_name = f"{obj_basename}.xml" # <-- 使用 obj_basename 命名
        merged_xml_path = os.path.join(xml_dir, merged_xml_name)
        # --- 修改：将OBJ文件名传递给合并函数，以构建正确的路径 ---
        tools.merge_xmls(xml_paths, merged_xml_path, obj_basename, global_rotation="90 0 0", global_prop="tags=nocull")

        # --- 新增：在提交前，清理掉用于合并的中间XML文件 ---
        logging.info(t("PY_WF_CLEANUP_INTERMEDIATE"))
        for p in xml_paths:
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError as e:
                logging.warning(f"Could not remove intermediate file {p}: {e}")

        # 5. 提交结果到最终目录
        report_stage(ProcessingStage.MERGING, "GUI_STATUS_MERGING") # 使用“合并中”状态
        with atomic_commit(out_dir, stop_check_callback) as committer:
            # --- 核心修改：将扁平的临时目录内容提交到结构化的最终目录 ---
            # 1. 创建目标子目录
            final_vox_dir = os.path.join(out_dir, "vox", obj_basename)
            os.makedirs(final_vox_dir, exist_ok=True)
            final_prefab_dir = os.path.join(out_dir, "prefab")
            os.makedirs(final_prefab_dir, exist_ok=True)
            
            # 2. 移动所有 .vox 文件
            for item in os.listdir(vox_dir):
                shutil.move(os.path.join(vox_dir, item), os.path.join(final_vox_dir, item))
            
            # 3. 移动最终的 .xml 文件
            shutil.move(merged_xml_path, os.path.join(final_prefab_dir, merged_xml_name))

        logging.info(t("PY_WF_COMPLETE", path=out_dir.replace("\\", "/")))

    finally:
        # --- 无论成功、失败还是中止，都清理临时工作目录 ---
        shutil.rmtree(work_dir)
        msg = t("PY_CLEANUP_TEMP_DIR", dir=work_dir.replace("\\", "/"))
        logging.info(msg)


if __name__ == "__main__":
    # 在程序开始时，首先设置日志系统
    setup_logger()

    parser = argparse.ArgumentParser(description="Unified Polyvox Workflow")
    parser.add_argument("--obj", "-o", required=True, help="Input OBJ model path")
    parser.add_argument("--polyvox", "-p", required=True, help="polyvox.exe path")
    parser.add_argument("--outdir", "-d", required=True, help="Output directory")
    parser.add_argument("--voxel-size", "-s", type=float, default=0.1, help="Voxel size for processing")
    parser.add_argument("--lang", "-l", default="en", choices=['en', 'zh'], help="Language for log messages (en/zh)")
    args = parser.parse_args()

    # 初始化多语言环境
    load_translations(args.lang)

    process_model(args.obj, args.outdir, args.polyvox, args.voxel_size, args.lang)
