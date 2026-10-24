from .store import Store, PostgresStore, MySQLStore


async def openStorage(options=None, **kwargs):
    value = {**(options or {}), **kwargs}
    driver = value.pop("driver", "sqlite")
    if driver == "sqlite":
        store = Store(value.pop("sqlitePath", ":memory:"), **value)
    elif driver == "postgres":
        store = PostgresStore(**value)
    elif driver == "mysql":
        store = MySQLStore(**value)
    else:
        raise TypeError("Unsupported storage driver")
    await store.initialize()
    return store
