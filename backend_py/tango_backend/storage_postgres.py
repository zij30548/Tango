import re
from .errors import ApiError


async def pgTransaction(database, operation):
    async with database.transaction():
        return await operation(database)


def pgTaskConflict():
    return ApiError(409, "TASK_WRITE_CONFLICT", "任务已更新，请刷新后重试。")


def pgLeaseConflict():
    return ApiError(409, "TASK_CLAIMED", "此任务正在其他后台执行，请稍后重试。")


def positiveInteger(value, label="value"):
    if type(value) is not int or value < 1 or value > 9007199254740991:
        raise TypeError("Invalid " + label)
    return value


def safeCount(value):
    number = int(value or 0)
    if number < 0 or number > 9007199254740991 or number != float(value or 0):
        raise ValueError("Stored count is outside the supported range")
    return number


def callDiagnostic(diagnostics, status):
    if not diagnostics:
        return None
    return {
        "stage": str(diagnostics.get("stage", "model_result"))[:40],
        "outcome": str(diagnostics.get("outcome", status))[:40],
        "reasonCounts": {
            k: v
            for k, v in diagnostics.get("reasonCounts", {}).items()
            if re.fullmatch("[A-Za-z][A-Za-z0-9_]{0,39}", k)
            and type(v) is int
            and 0 <= v <= 1000000
        },
    }
