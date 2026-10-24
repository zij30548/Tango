#!/usr/bin/env python3
"""运行已暴露失败的原生工程回归；固定替身、隔离曲库，真实模型请求恒为零。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))

from evaluation.repair_regression import main

if __name__ == "__main__":
    raise SystemExit(main())
