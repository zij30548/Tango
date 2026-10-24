#!/usr/bin/env python3
"""补充身份澄清诊断入口；plan/prepare均不调用模型或曲库。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))

from evaluation.music_identity_followup import main

if __name__ == "__main__":
    raise SystemExit(main())
