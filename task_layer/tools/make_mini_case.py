# -*- coding: utf-8 -*-
"""生成 Compact 准静态 PEEC 的最小算例（平行板 + 两端端口）。

用途：任务层联调 / 冒烟测试。真实样例（10916 个 PEC 面）提取一次要 ~3 分钟、
产物 ~2.9GB，不适合反复跑；本脚本生成 16 个三角形的小模型，全链路秒级完成。

输出目录结构（符合 Compact 输入契约 V1）：
  <out>/DATA/Model/mini.msh     主网格（唯一）
  <out>/DATA/Model/matdb.json   材料库 schemaVersion=3
  <out>/DATA/Model/port.json    端口绑定（含网格 SHA-256）
  <out>/DATA/Setting/set.txt    计算设置

用法：
  python make_mini_case.py --out <caseDir> [--nx 8] [--gap-um 500]
"""
import argparse
import hashlib
import json
import os


def build_mesh(nx, ny, length_mm, width_mm, gap_mm):
    """两块平行板，各 nx*ny 个四边形（每个拆 2 个三角形），返回 (msh 文本, 每板三角形 id 列表)。"""
    dx = length_mm / nx
    dy = width_mm / ny
    z_top = gap_mm          # 上板
    z_bot = 0.0             # 下板

    nodes = []          # (x, y, z)

    def grid_nodes(z):
        base = len(nodes) + 1
        idx = {}
        for j in range(ny + 1):
            for i in range(nx + 1):
                nodes.append((i * dx, j * dy, z))
                idx[(i, j)] = base + j * (nx + 1) + i
        return idx

    top = grid_nodes(z_top)
    bot = grid_nodes(z_bot)

    elements = []       # (n1, n2, n3)
    for idx in (top, bot):
        for j in range(ny):
            for i in range(nx):
                p00 = idx[(i, j)]
                p10 = idx[(i + 1, j)]
                p11 = idx[(i + 1, j + 1)]
                p01 = idx[(i, j + 1)]
                elements.append((p00, p10, p11))
                elements.append((p00, p11, p01))

    n_top = 2 * nx * ny                 # 上板三角形数量
    top_tris = list(range(1, n_top + 1))
    bot_tris = list(range(n_top + 1, 2 * n_top + 1))

    lines = ["$MeshFormat", "2.2 0 8", "$EndMeshFormat",
             "$PhysicalNames", "1", '2 1 "PEC"', "$EndPhysicalNames",
             "$Nodes", str(len(nodes))]
    for k, (x, y, z) in enumerate(nodes, start=1):
        lines.append("%d %.17g %.17g %.17g" % (k, x, y, z))
    lines += ["$EndNodes", "$Elements", str(len(elements))]
    for k, (n1, n2, n3) in enumerate(elements, start=1):
        # elm-number elm-type num-tags physical-tag elementary-tag nodes...
        lines.append("%d 2 2 1 1 %d %d %d" % (k, n1, n2, n3))
    lines.append("$EndElements")
    text = "\n".join(lines) + "\n"
    return text, top_tris, bot_tris


def face_center(elem_nodes, nodes):
    xs = [nodes[n - 1][0] for n in elem_nodes]
    ys = [nodes[n - 1][1] for n in elem_nodes]
    zs = [nodes[n - 1][2] for n in elem_nodes]
    return (sum(xs) / 3.0, sum(ys) / 3.0, sum(zs) / 3.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="算例目录（将创建 DATA/）")
    ap.add_argument("--nx", type=int, default=8, help="沿 x 的分段数")
    ap.add_argument("--ny", type=int, default=1, help="沿 y 的分段数")
    ap.add_argument("--length", type=float, default=10.0, help="板长 mm")
    ap.add_argument("--width", type=float, default=1.0, help="板宽 mm")
    ap.add_argument("--gap-um", type=float, default=500.0, help="板间距 um")
    ap.add_argument("--f-start", default="1e9")
    ap.add_argument("--f-end", default="1e10")
    ap.add_argument("--n-fp", type=int, default=11)
    args = ap.parse_args()

    gap_mm = args.gap_um / 1000.0
    msh, top_tris, bot_tris = build_mesh(args.nx, args.ny, args.length, args.width, gap_mm)

    data = os.path.join(args.out, "DATA")
    for sub in ("Model", "Setting", "Result"):
        os.makedirs(os.path.join(data, sub), exist_ok=True)

    msh_path = os.path.join(data, "Model", "mini.msh")
    with open(msh_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(msh)
    sha = hashlib.sha256(open(msh_path, "rb").read()).hexdigest()

    matdb = {
        "schemaVersion": 3,
        "nextId": 2,
        "materials": [{
            "id": 1,
            "name": "PEC_Surface_35um",
            "dimension": "surface",
            "type": "C",
            "properties": {
                "conductivity": {"model": "constant", "unit": "S/m", "value": 5.8e7},
            },
            "thickness": 35.0,
            "thicknessUnit": "um",
        }],
    }
    with open(os.path.join(data, "Model", "matdb.json"), "w", encoding="utf-8") as f:
        json.dump(matdb, f, indent=2, ensure_ascii=False)

    # 端口：两端各取一对上下三角形，方向按“第一面心 -> 第二面心”给出
    n_per_row = 2 * args.ny
    p1_top, p1_bot = top_tris[0], bot_tris[0]
    p2_top, p2_bot = top_tris[-n_per_row], bot_tris[-n_per_row]

    def direction(a, b):
        # 上板 z=gap，下板 z=0，故方向恒为 (0,0,-1)
        return [0, 0, -1] if a <= len(top_tris) else [0, 0, 1]

    ports = []
    for a, b in ((p1_top, p1_bot), (p2_top, p2_bot)):
        ports.append({
            "impedanceOhm": 120,
            "direction": direction(a, b),
            "triangleIds": [a, b],
        })
    port_json = {"meshFile": os.path.basename(msh_path), "meshSha256": sha, "ports": ports}
    with open(os.path.join(data, "Model", "port.json"), "w", encoding="utf-8") as f:
        json.dump(port_json, f, indent=2)

    set_txt = "\n".join([
        "Solver_SET 0",
        "MNA_WEIGHT 0.8",
        "FS %s" % args.f_start,
        "FE %s" % args.f_end,
        "N_FP %d" % args.n_fp,
        "DIM 1000",
        "PRECISION 0",
        "Taylor 0",
        "$End_Def",
        "",
    ])
    with open(os.path.join(data, "Setting", "set.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(set_txt)

    print("case written:", os.path.abspath(args.out))
    print("  triangles:", len(top_tris) + len(bot_tris), " nodes:", msh.count("\n"))
    print("  ports:", ports)
    print("  meshSha256:", sha)


if __name__ == "__main__":
    main()
