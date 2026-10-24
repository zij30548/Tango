#!/usr/bin/env python3
"""准备并单次执行音乐需求真实验收；prepare不读密钥、不调用模型或曲库。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend_py"))

from evaluation.music_requirements_live import main

if __name__ == "__main__":
    raise SystemExit(main())
