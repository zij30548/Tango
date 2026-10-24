from .errors import ApiError
from .compat import now_ms
from .store import validateCredentials
from .invite_store import inviteCodeHash, InviteStore


class RegistrationService:
    def __init__(
        self,
        options=None,
        *,
        store=None,
        registrationMode="invite_only",
        passwordMin=15,
        now=now_ms,
    ):
        options = options or {}
        self.store = options.get("store", store)
        self.registrationMode = options.get("registrationMode", registrationMode)
        self.passwordMin = options.get("passwordMin", passwordMin)
        self.now = options.get("now", now)
        if (
            self.registrationMode not in ["invite_only", "open", "closed"]
            or type(self.passwordMin) is not int
            or not 8 <= self.passwordMin <= 128
        ):
            raise TypeError("Invalid registration policy")

    def publicConfig(self):
        return {
            "version": 1,
            "registrationMode": self.registrationMode,
            "inviteRequired": self.registrationMode == "invite_only",
            "passwordMin": self.passwordMin,
            "passwordMax": 128,
            "usernameMin": 2,
            "usernameMax": 32,
        }

    def validate(self, body):
        if self.registrationMode == "closed":
            raise ApiError(403, "REGISTRATION_CLOSED", "当前暂不开放注册。")
        if (
            not isinstance(body, dict)
            or set(body) - {"username", "password", "inviteCode"}
            or not {"username", "password"} <= set(body)
            or "inviteCode" in body
            and not isinstance(body["inviteCode"], str)
        ):
            raise ApiError(
                400,
                "INVALID_REGISTRATION_FORMAT",
                "注册请求格式无效，请填写用户名、密码和所需内测码。",
            )
        credentials = validateCredentials(
            {"username": body["username"], "password": body["password"]}
        )
        if (
            len(credentials["password"].encode("utf-16-le", "surrogatepass")) // 2
            < self.passwordMin
        ):
            raise ApiError(
                400,
                "INVALID_PASSWORD",
                f"新账户密码须为 {self.passwordMin} 至 128 个字符。",
            )
        if self.registrationMode == "invite_only":
            inviteCodeHash(body.get("inviteCode"))
        return {
            "credentials": credentials,
            "options": {
                "registrationMode": self.registrationMode,
                "inviteCode": body.get("inviteCode", "").strip(),
            },
        }

    async def register(self, body):
        input = self.validate(body)
        if self.registrationMode == "invite_only":
            await InviteStore(self.store).assertUsable(
                input["options"]["inviteCode"], self.now()
            )
        return await self.store.register(
            input["credentials"], self.now(), input["options"]
        )
