#!/usr/bin/env python3
"""用固定上游执行核心演示回归；不调用真实模型、曲库或云服务。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))

from evaluation.demo_stability import main

if __name__ == "__main__":
    raise SystemExit(main())
