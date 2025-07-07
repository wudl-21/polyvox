import subprocess
import xml.etree.ElementTree as ET
import sys
import logging
from localization import t
import os
import time # <-- 新增导入

# --- 修改函数签名，增加 stop_checker 回调 ---
def run_polyvox(polyvox_exe, obj_path, out_vox, voxel_size, lang, material_maps=None, material_properties=None, stop_checker=None):
    """
    调用 polyvox.exe，并允许在执行过程中中止。
    """
    command = [polyvox_exe, "-i", obj_path, "-o", out_vox, "-s", str(voxel_size), "-l", lang, "-v"]
    
    if material_maps:
        for map_string in material_maps:
            command.extend(["-m", map_string])
    
    if material_properties:
        for mat_name, props in material_properties.items():
            for prop_name, prop_value in props.items():
                command.extend(["-p", f"{mat_name}:{prop_name}:{prop_value}"])

    logging.info(t("PY_EXECUTING_COMMAND", cmd=(' '.join(command).replace('\\','/'))))
    
    try:
        startupinfo = None
        if os.name == 'nt':
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = subprocess.SW_HIDE

        # --- 修改：使用 Popen 实现非阻塞调用 ---
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding='utf-8',
            startupinfo=startupinfo
        )

        # --- 新增：在等待进程结束时，周期性检查中止信号 ---
        while process.poll() is None:
            if stop_checker and stop_checker():
                logging.warning(t("GUI_STOPPING_SUBPROCESS"))
                process.terminate() # 发送终止信号
                try:
                    # 等待一小段时间确保进程已终止
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill() # 如果无法终止，则强制杀死
                # 抛出异常，让上层知道是用户中止的
                raise RuntimeError(t("GUI_USER_STOPPED"))
            time.sleep(0.1) # 避免CPU空转

        # 进程结束后，获取输出和返回码
        stdout, stderr = process.communicate()
        if stdout:
            for line in stdout.strip().split('\n'):
                logging.info(line)
        if stderr:
            for line in stderr.strip().split('\n'):
                logging.warning(line)
        
        # 如果进程返回非零代码，则视为失败
        if process.returncode != 0:
            raise subprocess.CalledProcessError(process.returncode, command, output=stdout, stderr=stderr)

    except subprocess.CalledProcessError as e:
        error_output = e.stderr if e.stderr else t("PY_TOOL_POLYVOX_NO_OUTPUT")
        logging.error(t("PY_TOOL_POLYVOX_ERROR", error=error_output))
        raise
    except FileNotFoundError:
        logging.error(t("PY_TOOL_POLYVOX_NOT_FOUND", path=polyvox_exe))
        raise

def update_group_transform(xml_path, pos, rot):
    """
    更新由 PolyVox 生成的 XML 文件中的 group 节点的 pos 和 rot 属性。
    """
    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()
        if root.tag == "group":
            root.attrib["pos"] = f"{pos[0]} {pos[1]} {pos[2]}"
            root.attrib["rot"] = f"{rot[0]} {rot[1]} {rot[2]}"
            tree.write(xml_path, encoding="utf-8", xml_declaration=True)
        else:
            logging.warning(t("PY_TOOL_XML_NO_GROUP", path=xml_path))
    except ET.ParseError as e:
        logging.error(t("PY_TOOL_XML_PARSE_ERROR", path=xml_path, error=e))
    except FileNotFoundError:
        logging.error(t("PY_TOOL_XML_NOT_FOUND", path=xml_path))

# --- 修改：函数签名增加 obj_basename 参数 ---
def merge_xmls(xml_paths, output_xml, obj_basename, global_rotation="90 0 0", global_prop="tags=nocull"):
    """
    将多个 XML 文件合并到一个符合Teardown规范的prefab文件中。
    """
    # 1. 创建包含所有表面的核心 <group>
    merged_group = ET.Element("group", {"name": "merged", "pos": "0 0 0", "rot": global_rotation, "prop0": global_prop})

    for xml_path in xml_paths:
        try:
            tree = ET.parse(xml_path)
            group = tree.getroot()
            
            for vox_tag in group.findall(".//vox"):
                if 'file' in vox_tag.attrib:
                    vox_filename = os.path.basename(vox_tag.attrib['file'])
                    # --- 修改：使用 obj_basename 构建正确的相对路径 ---
                    # Teardown 使用 "MOD/" 前缀来表示mod根目录
                    new_path = os.path.join("MOD", "vox", obj_basename, vox_filename).replace("\\", "/")
                    vox_tag.attrib['file'] = new_path

            merged_group.append(group)
        except ET.ParseError as e:
            logging.warning(t("PY_TOOL_XML_SKIP_INVALID", path=xml_path.replace("\\", "/"), error=e))
        except FileNotFoundError:
            logging.warning(t("PY_TOOL_XML_SKIP_MISSING", path=xml_path.replace("\\", "/")))

    # 2. 创建符合Teardown规范的顶层结构
    prefab_root = ET.Element("prefab", {"version": "1.7.0"})
    
    # 创建instance group
    instance_name = f"instance=MOD/prefab/{os.path.basename(output_xml)}".replace("\\", "/")
    instance_group = ET.Element("group", {
        "name": instance_name,
        "pos": "0 0 0", 
        "rot": "0 0 0"
    })
    
    # 将合并好的内容放入instance group
    instance_group.append(merged_group)
    
    # 将instance group放入prefab根节点
    prefab_root.append(instance_group)

    # 3. 写入文件，不带XML声明
    # ElementTree.tostring() 可以更好地控制输出
    xml_string = ET.tostring(prefab_root, encoding='unicode')
    
    with open(output_xml, 'w', encoding='utf-8') as f:
        f.write(xml_string)

    logging.info(t("PY_TOOL_XML_MERGE_SUCCESS", count=len(xml_paths), path=output_xml.replace("\\", "/")))
