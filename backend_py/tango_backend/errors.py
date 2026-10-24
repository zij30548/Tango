"""对外只返回经过筛选的业务错误，不泄漏上游响应和凭据。"""


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
