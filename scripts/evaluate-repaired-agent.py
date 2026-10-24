#!/usr/bin/env python3
"""准备修后真实验收协议；付费运行须显式新增授权和共享硬预算。"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))
from evaluation.repaired_quality import main

if __name__ == "__main__":
    raise SystemExit(main())
