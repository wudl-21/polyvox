import os
import numpy as np
import shutil
from scipy.spatial.transform import Rotation as R
# --- 新增：导入 cKDTree 用于顶点焊接，以及翻译函数 ---
from scipy.spatial import cKDTree
from localization import t

SURFACE_OFFSET_MULTIPLIER = 0.25

def weld_vertices(vertices, faces, tolerance=1e-4):
    """
    顶点焊接：合并距离非常近的顶点，以消除浮点误差。
    返回新的顶点数组、新的面列表和旧索引到新索引的映射。
    """
    tree = cKDTree(vertices)
    # 查找所有距离小于容差的点对
    pairs = tree.query_pairs(r=tolerance)
    
    # 使用并查集（Disjoint Set Union）来有效地对顶点进行分组
    parent = list(range(len(vertices)))
    def find(i):
        if parent[i] == i:
            return i
        parent[i] = find(parent[i])
        return parent[i]
    def union(i, j):
        root_i = find(i)
        root_j = find(j)
        if root_i != root_j:
            parent[root_j] = root_i

    for i, j in pairs:
        union(i, j)

    # 为每个旧顶点找到其最终的代表顶点
    for i in range(len(vertices)):
        find(i)

    # 创建从旧索引到新索引的映射，并计算新顶点的位置（取平均值）
    new_vertices_map = {}
    new_vertices_list = []
    old_to_new_map = {}

    for i, p in enumerate(parent):
        if p not in new_vertices_map:
            new_vertices_map[p] = len(new_vertices_list)
            new_vertices_list.append([])
        new_vertices_list[new_vertices_map[p]].append(vertices[i])
        old_to_new_map[i] = new_vertices_map[p]

    # 计算每个焊接后顶点的新坐标（平均值）
    welded_vertices = np.array([np.mean(v_group, axis=0) for v_group in new_vertices_list])

    # 更新面索引
    new_faces = []
    for face in faces:
        new_face = []
        for v_idx, vt_idx, vn_idx in face:
            new_v_idx = old_to_new_map[v_idx]
            new_face.append((new_v_idx, vt_idx, vn_idx))
        
        # 检查焊接后是否产生退化面（例如，两个顶点合并了）
        if len(set(v[0] for v in new_face)) >= 3:
            new_faces.append(new_face)
            
    return welded_vertices, new_faces

def filter_duplicate_faces(faces, face_materials):
    """
    在顶点焊接后，根据面的顶点索引过滤掉完全重复的面。
    这是确保几何数据干净的最后一步。
    """
    unique_faces_set = set()
    new_faces = []
    new_face_materials = []
    
    for i, face in enumerate(faces):
        # 使用焊接后的顶点索引创建签名
        face_signature = tuple(sorted(v[0] for v in face))
        
        if face_signature in unique_faces_set:
            continue
            
        unique_faces_set.add(face_signature)
        new_faces.append(face)
        new_face_materials.append(face_materials[i])
        
    return new_faces, new_face_materials

def parse_obj(obj_path, stop_check_callback=None):
    """
    解析OBJ文件，返回顶点、UV、法线、面、材质等信息。
    """
    vertices = []
    uvs = []
    # normals 列表不再需要，因为我们完全忽略文件中的法线
    faces = []
    face_materials = []
    mtllib = None
    current_mtl = None
    
    # --- 核心修改：移除此处的 unique_faces_set，因为它必须在顶点焊接后执行 ---
    # unique_faces_set = set()

    with open(obj_path, 'r', encoding='utf-8') as f:
        # --- 新增：在循环中检查中止信号 ---
        for i, line in enumerate(f):
            if i % 4096 == 0: # 每处理4096行检查一次，避免性能开销过大
                if stop_check_callback and stop_check_callback():
                    raise RuntimeError(t("GUI_USER_STOPPED"))
            
            # --- 修复：使用 maxsplit=1 来正确处理带空格的文件名 ---
            parts = line.strip().split(maxsplit=1)
            if not parts:
                continue
            
            command = parts[0]
            if len(parts) > 1:
                value = parts[1]
            else:
                value = ""

            if command == 'mtllib':
                mtllib = value
            elif command == 'usemtl':
                current_mtl = value
            elif command == 'v':
                vertices.append([float(x) for x in value.split()])
            elif command == 'vt':
                uvs.append([float(x) for x in value.split()])
            elif command == 'vn':
                # 我们将忽略文件中的法线，因为它们可能包含硬边信息，
                # 这会干扰我们的共面分组。我们将自己重新计算。
                pass # 直接跳过
            elif command == 'f':
                face_v_indices = []
                face_tuples = []
                
                # --- 核心修复：只取面定义的前3个顶点，以处理非标准OBJ文件 ---
                # 有些导出器可能会在f行中产生多余的顶点引用。
                # 通过只取前3个顶点，我们强制将每个f行都视为一个单独的三角形。
                face_vertices_str = value.split()[:3]

                # 如果顶点数少于3，则为退化面，直接跳过
                if len(face_vertices_str) < 3:
                    continue

                for vert in face_vertices_str:
                    v = vert.split('/')
                    v_idx = int(v[0]) - 1
                    vt_idx = int(v[1]) - 1 if len(v) > 1 and v[1] else None
                    vn_idx = None # 强制忽略文件中的法线索引
                    
                    face_v_indices.append(v_idx)
                    face_tuples.append((v_idx, vt_idx, vn_idx))

                # --- 核心修改：直接添加面，不再进行重复检查 ---
                faces.append(face_tuples)
                face_materials.append(current_mtl)

    # 在返回时，返回一个空的法线数组，因为我们没有使用文件中的法线
    return np.array(vertices), np.array(uvs), np.array([]), faces, face_materials, mtllib

def get_face_normals(vertices, faces, stop_check_callback=None):
    """
    计算每个面的法线。
    """
    normals = []
    valid_indices = []
    for i, face in enumerate(faces):
        # --- 新增：在循环中检查中止信号 ---
        if i % 1024 == 0: # 每处理1024个面检查一次
            if stop_check_callback and stop_check_callback():
                raise RuntimeError(t("GUI_USER_STOPPED"))

        # 如果面顶点数少于3，则为退化面，跳过
        if len(face) < 3:
            continue

        # --- 核心修复：由于面已被强制三角化，改用标准叉乘计算法线 ---
        # 纽维尔方法适用于多边形，但对于已确定的三角形，标准叉乘更直接且精确。
        # 此前的纽维尔方法在处理大量狭长三角形时，浮点误差累积导致相邻面的法线偏差超出容差。
        p0 = vertices[face[0][0]]
        p1 = vertices[face[1][0]]
        p2 = vertices[face[2][0]]
        
        v1 = np.array(p1) - np.array(p0)
        v2 = np.array(p2) - np.array(p0)
        
        normal = np.cross(v1, v2)

        n_norm = np.linalg.norm(normal)
        if n_norm < 1e-10: # 避免除以零，处理退化三角形
            continue
            
        normal = normal / n_norm
        normals.append(normal)
        valid_indices.append(i)
        
    return np.array(normals), valid_indices

def group_coplanar_faces(vertices, faces, normals, stop_check_callback=None, angle_tol=1e-2, dist_tol=1e-3):
    """
    根据共面性和连通性将面分组。
    """
    visited = np.zeros(len(faces), dtype=bool)
    groups = []
    
    # --- 核心修改：构建边到面的映射以优化邻接查找 ---
    edge_to_faces = {}
    for i, face in enumerate(faces):
        for j in range(len(face)):
            v1_idx = face[j][0]
            v2_idx = face[(j + 1) % len(face)][0]
            edge = tuple(sorted((v1_idx, v2_idx)))
            if edge not in edge_to_faces:
                edge_to_faces[edge] = []
            edge_to_faces[edge].append(i)

    for i in range(len(faces)):
        if i % 256 == 0:
            if stop_check_callback and stop_check_callback():
                raise RuntimeError(t("GUI_USER_STOPPED"))

        if visited[i]:
            continue
        
        group = []
        stack = [i]
        visited[i] = True
        
        # --- 核心修改：实现递归共面扩展 ---
        # 使用分组中第一个“种子”面的法线和参考点作为整个分组的判断基准。
        # 这可以防止因法线微小误差累积导致的分组中断。
        ref_normal = normals[i]
        ref_point = vertices[faces[i][0][0]]
        
        while stack:
            current_face_idx = stack.pop()
            group.append(current_face_idx)
            
            # --- 核心修改：通过边映射查找邻接面，而不是遍历所有面 ---
            face_edges = set()
            current_face_verts = faces[current_face_idx]
            for j in range(len(current_face_verts)):
                v1_idx = current_face_verts[j][0]
                v2_idx = current_face_verts[(j + 1) % len(current_face_verts)][0]
                face_edges.add(tuple(sorted((v1_idx, v2_idx))))

            neighbor_candidates = set()
            for edge in face_edges:
                for neighbor_idx in edge_to_faces.get(edge, []):
                    if neighbor_idx != current_face_idx:
                        neighbor_candidates.add(neighbor_idx)

            for n in neighbor_candidates:
                if visited[n]:
                    continue
                
                # 所有邻居都与最初的 ref_normal 和 ref_point 比较
                angle = np.arccos(np.clip(np.dot(ref_normal, normals[n]), -1, 1))
                if angle > angle_tol:
                    continue
                
                face_verts = [vertices[v[0]] for v in faces[n]]
                dists = np.abs(np.dot(np.array(face_verts) - ref_point, ref_normal))
                if np.any(dists > dist_tol):
                    continue
                
                visited[n] = True
                stack.append(n)
                
        groups.append(group)
    return groups

def get_plane_reference(normal):
    """
    根据法线，返回一个在与之垂直的平面内的、稳定的参考向量。
    """
    if not np.allclose(np.abs(normal), [0, 1, 0]):
        ref_dir = np.cross(normal, [0, 1, 0])
    else:
        ref_dir = np.cross(normal, [0, 0, 1])
    
    ref_dir = ref_dir / np.linalg.norm(ref_dir)
    ref_dir_editor = np.array([ref_dir[0], ref_dir[2], -ref_dir[1]])
    return ref_dir, ref_dir_editor

def calculate_surface_transforms(vertices, faces, normals_arr, groups, voxel_size, stop_check_callback=None):
    """
    为每个表面分组计算其中心点、法线和最终的编辑器变换。
    """
    surfaces_info = []
    for i, group in enumerate(groups):
        # --- 新增：在循环中检查中止信号 ---
        if i % 256 == 0: # 每处理256个分组检查一次
            if stop_check_callback and stop_check_callback():
                raise RuntimeError(t("GUI_USER_STOPPED"))

        if not group:
            continue
        
        all_face_vertices = [vertices[v] for idx in group for v, _, _ in faces[idx]]
        center = np.mean(all_face_vertices, axis=0)

        group_normals = [normals_arr[idx] for idx in group]
        normal = np.mean(group_normals, axis=0)
        normal = normal / np.linalg.norm(normal)

        # === 新增：应用半个体素厚度的偏移 ===
        # 为了让体素化后的外表面与原始模型对齐，
        # 需要将中心点沿着法线反方向移动。
        
        center_offset = -normal * (voxel_size * SURFACE_OFFSET_MULTIPLIER)
        center += center_offset
        
        center_editor = [center[0], center[2], -center[1]]
        normal_editor = np.array([normal[0], normal[2], -normal[1]])

        _, ref_dir_editor = get_plane_reference(normal)
        target_normal = np.array([0, 1, 0])
        target_ref_dir = np.array([1, 0, 0])
        rot, _ = R.align_vectors([normal_editor, ref_dir_editor], [target_normal, target_ref_dir])
        euler = rot.as_euler('xyz', degrees=True)

        surfaces_info.append({
            "name": f"surface_{i+1}",
            "index": i+1,
            "center": center_editor,
            "normal_euler_deg": euler.tolist(),
            "face_indices": group,
        })
    return surfaces_info

def export_single_surface_obj(vertices, uvs, normals, faces, face_materials, group_indices, out_obj, mtllib_path, obj_src_dir, input_obj_path, normals_arr, stop_check_callback=None):
    """
    导出一个独立的、旋转到XY平面的表面OBJ文件，供PolyVox处理。
    --- 核心修复：将原始MTL文件复制并重命名为不含空格/特殊字符的安全名称，以供C++核心程序使用。---
    """
    if not group_indices:
        return

    used_v, used_vt, used_vn = set(), set(), set()
    for idx in group_indices:
        for v, vt, vn in faces[idx]:
            used_v.add(v)
            if vt is not None: used_vt.add(vt)
            if vn is not None: used_vn.add(vn)

    v_map = {old: new for new, old in enumerate(sorted(used_v))}
    vt_map = {old: new for new, old in enumerate(sorted(used_vt))}
    vn_map = {old: new for new, old in enumerate(sorted(used_vn))}

    all_face_vertices = [vertices[v] for idx in group_indices for v, _, _ in faces[idx]]
    center = np.mean(all_face_vertices, axis=0)

    group_face_normals = [normals_arr[idx] for idx in group_indices]
    avg_normal = np.mean(group_face_normals, axis=0)
    avg_normal = avg_normal / np.linalg.norm(avg_normal)

    ref_dir_obj, _ = get_plane_reference(avg_normal)
    target_normal = np.array([0, 0, 1])
    target_ref_dir = np.array([1, 0, 0])
    rot, _ = R.align_vectors([target_normal, target_ref_dir], [avg_normal, ref_dir_obj])

    transformed_vertices = []
    for v_idx in sorted(used_v):
        vtx = vertices[v_idx] - center
        vtx = rot.apply(vtx)
        transformed_vertices.append(vtx)

    # --- 1. 准备安全的文件名 ---
    surface_basename = os.path.splitext(os.path.basename(out_obj))[0]
    safe_mtl_name = f"{surface_basename}.mtl"

    with open(out_obj, 'w', encoding='utf-8') as obj:
        if mtllib_path:
            # --- 2. 在OBJ中写入安全、无空格的MTL文件名 ---
            obj.write(f"mtllib {safe_mtl_name}\n")
        
        for vtx in transformed_vertices:
            obj.write(f"v {' '.join(map(str, vtx))}\n")
        for vt_idx in sorted(used_vt):
            obj.write(f"vt {' '.join(map(str, uvs[vt_idx]))}\n")
        for vn_idx in sorted(used_vn):
            obj.write(f"vn {' '.join(map(str, normals[vn_idx]))}\n")
        
        last_mtl = None
        for i, idx in enumerate(group_indices):
            if i % 1024 == 0:
                if stop_check_callback and stop_check_callback():
                    raise RuntimeError(t("GUI_USER_STOPPED"))

            mtl = face_materials[idx]
            if mtl != last_mtl:
                obj.write(f"usemtl {mtl}\n")
                last_mtl = mtl
            face = faces[idx]
            f_str = []
            for v, vt, vn in face:
                s = str(v_map[v]+1)
                s += f"/{vt_map[vt]+1}" if vt is not None else "/"
                if vn is not None: s += f"/{vn_map[vn]+1}"
                f_str.append(s)
            obj.write("f " + " ".join(f_str) + "\n")

    # --- 3. 复制并处理MTL文件和纹理 ---
    if mtllib_path:
        # 原始MTL文件的完整路径
        mtl_src = os.path.join(obj_src_dir, os.path.basename(mtllib_path))
        # 目标MTL文件的完整路径（使用安全名称）
        mtl_dst = os.path.join(os.path.dirname(out_obj), safe_mtl_name)
        
        # 纹理子目录，现在以原始OBJ文件名命名，以避免冲突
        input_obj_base = os.path.splitext(os.path.basename(input_obj_path))[0]
        tex_subdir = os.path.join(os.path.dirname(out_obj), input_obj_base)
        os.makedirs(tex_subdir, exist_ok=True)
        
        if os.path.exists(mtl_src):
            new_lines = []
            with open(mtl_src, 'r', encoding='utf-8') as mtl_file:
                for line in mtl_file:
                    parts = line.strip().split(maxsplit=1)
                    if len(parts) > 1 and parts[0] == 'map_Kd':
                        tex_path = parts[1]
                        tex_name = os.path.basename(tex_path)
                        src_img = os.path.join(obj_src_dir, tex_path)
                        dst_img = os.path.join(tex_subdir, tex_name)
                        new_tex_path = os.path.join(os.path.basename(tex_subdir), tex_name).replace("\\", "/")
                        line = f"map_Kd {new_tex_path}\n"
                        if os.path.exists(src_img) and not os.path.exists(dst_img):
                            shutil.copy(src_img, dst_img)
                    new_lines.append(line)
            # 将处理过的内容写入到新的、安全的MTL文件中
            with open(mtl_dst, 'w', encoding='utf-8') as mtl_file:
                mtl_file.writelines(new_lines)
