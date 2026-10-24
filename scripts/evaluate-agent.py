#!/usr/bin/env python3
"""启动原生新题评测；真实调用须显式指定本轮授权与共享硬预算。"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))
from evaluation.independent import main

if __name__ == "__main__":
    raise SystemExit(main())
