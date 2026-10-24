import os
import re
from .account_quota import DEFAULT_QUOTA_POLICY


def count(env, name, fallback, minimum=1, maximum=1000000):
    raw = env.get(name)
    if raw is None or raw == "":
        return fallback
    if (
        not isinstance(raw, str)
        or not re.fullmatch(r"\d+", raw)
        or not minimum <= int(raw) <= maximum
    ):
        raise ValueError("Invalid " + name + " configuration.")
    return int(raw)


def securityConfiguration(env=None):
    env = os.environ if env is None else env
    mode = env.get("QUIET_REGISTRATION_MODE") or "invite_only"
    if mode not in ["invite_only", "open", "closed"]:
        raise ValueError("Invalid registration mode.")
    return {
        "registrationMode": mode,
        "requestPerMinute": count(env, "QUIET_REQUESTS_PER_MINUTE", 240, 1, 100000),
        "userRequestPerMinute": count(
            env, "QUIET_USER_REQUESTS_PER_MINUTE", 180, 1, 100000
        ),
        "conversationCreatesPerDay": count(
            env, "QUIET_CONVERSATION_CREATES_PER_DAY", 20, 1, 1000
        ),
        "defaultQuotaPolicy": {
            "dailyLimit": count(
                env,
                "QUIET_ACCOUNT_DEFAULT_DAILY_LIMIT",
                DEFAULT_QUOTA_POLICY["dailyLimit"],
                0,
            ),
            "totalLimit": count(
                env,
                "QUIET_ACCOUNT_DEFAULT_TOTAL_LIMIT",
                DEFAULT_QUOTA_POLICY["totalLimit"],
                0,
                1000000000,
            ),
            "enabled": True,
        },
    }
