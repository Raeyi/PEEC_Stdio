# -*- coding: utf-8 -*-
"""task_layer serve 模式的 JSON-RPC 冒烟测试。

验证链路：UI(QProcess) -> task_layer.exe serve -> 计算层 exe -> 状态/结果查询。
用法：
  python rpc_smoke.py --exe <task_layer.exe 路径> --data-root <算例 DATA 路径>
"""
import argparse
import json
import os
import subprocess
import sys
import time


class Rpc:
    def __init__(self, exe, cwd):
        self.proc = subprocess.Popen(
            [exe, "serve"],
            cwd=cwd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        self.next_id = 0

    def send(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method,
               "params": params or {}}
        self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        return self.next_id

    def wait_response(self, req_id, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("task_layer 提前退出（stdout 关闭）")
            line = line.strip()
            if not line:
                continue
            msg = json.loads(line)
            if msg.get("id") == req_id:
                if "error" in msg:
                    raise RuntimeError("RPC error: %s" % msg["error"])
                return msg.get("result")
            # 其他是通知，忽略（真实 UI 应在此更新状态）
        raise TimeoutError("等待响应超时: id=%s" % req_id)

    def call(self, method, params=None, timeout=30):
        return self.wait_response(self.send(method, params), timeout)

    def wait_notification(self, method, timeout=180):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("task_layer 提前退出（stdout 关闭）")
            line = line.strip()
            if not line:
                continue
            msg = json.loads(line)
            if msg.get("method") == method:
                return msg.get("params")
        raise TimeoutError("等待通知超时: %s" % method)

    def close(self):
        try:
            self.send("shutdown")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--data-root", required=True)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    data_root = os.path.abspath(args.data_root)   # 任务层进程的工作目录不是本脚本目录，必须绝对化
    rpc = Rpc(exe, cwd=os.path.dirname(exe))
    ok = True
    try:
        r = rpc.call("ping")
        print("[1] ping            ->", r)
        ok &= r.get("pong") is True

        r = rpc.call("modules.list")
        print("[2] modules.list    -> %d 个模块" % len(r.get("modules", [])))
        ok &= len(r.get("modules", [])) > 0

        r = rpc.call("pipelines.list")
        pids = [p["id"] for p in r.get("pipelines", [])]
        print("[3] pipelines.list  ->", pids)
        ok &= "peec_basic" in pids

        r = rpc.call("inputs.validate",
                     {"dataRoot": data_root, "pipeline": "peec_basic"})
        print("[4] inputs.validate -> valid=%s issues=%d"
              % (r.get("valid"), len(r.get("issues", []))))
        ok &= r.get("valid") is True

        r = rpc.call("task.run", {"pipeline": "peec_basic", "dataRoot": data_root})
        task_id = r["taskId"]
        print("[5] task.run        ->", task_id)

        fin = rpc.wait_notification("task.finished")
        print("[6] task.finished   -> status=%s elapsedMs=%s summary=%s"
              % (fin["status"], fin["elapsedMs"], fin["summary"]))
        ok &= fin["status"] == "done"

        # 任务结束后仍可查询（原实现会 -32001）
        st = rpc.call("task.status", {"taskId": task_id})
        print("[7] task.status     -> %d stages, status=%s"
              % (len(st.get("stages", [])), st.get("status")))
        ok &= st.get("status") == "done"

        res = rpc.call("task.result", {"taskId": task_id})
        print("[8] task.result     -> %d 个结果文件" % len(res.get("resultFiles", [])))
        ok &= len(res.get("resultFiles", [])) > 0
        for f in res.get("resultFiles", []):
            if not os.path.exists(f):
                print("    ! 结果文件不存在:", f)
                ok = False
    finally:
        rpc.close()

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
