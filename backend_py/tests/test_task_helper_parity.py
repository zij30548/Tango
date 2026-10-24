"""与冻结的 JS 同题结果逐项比较；这是迁移兼容性，不是模型效果评测。"""
import importlib
import json
from pathlib import Path
import pytest
from tango_backend.errors import ApiError

FIXTURE=json.loads((Path(__file__).parent/'fixtures/task_helpers_js_v1.json').read_text())
MODULES={'schema':'task_schema','patch':'task_brief_patch','lang':'task_language','pending':'task_pending_request','style':'task_style_evidence',
         'duration':'task_duration','query':'task_query_coverage','tool':'task_tool_evidence','release':'task_release_intent'}

@pytest.mark.parametrize('case',FIXTURE['cases'],ids=lambda c:c['label'])
def test_frozen_js_helper_equivalence(case):
    function=getattr(importlib.import_module('tango_backend.'+MODULES[case['module']]),case['fn'])
    try:
        output={'value':function(*case['args'])}
    except ApiError as error:
        output={'error':{'status':error.status,'code':error.code,'message':error.message}}
    expected={key:case[key] for key in ('value','error') if key in case}
    assert output==expected
