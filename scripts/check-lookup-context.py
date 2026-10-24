#!/usr/bin/env python3
"""验收多轮候选上下文；固定上游、隔离账号和曲库，真实模型请求恒为零。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))

from evaluation.lookup_regression import main

if __name__ == "__main__":
    raise SystemExit(main())
