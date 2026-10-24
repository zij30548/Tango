#!/usr/bin/env python3
"""启动探歌对话开发校准；默认离线，真实请求需要新的明确预算授权。"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))
from evaluation.calibration import main

if __name__ == "__main__":
    raise SystemExit(main())
