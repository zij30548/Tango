"""启用标记是一次明确切换；MySQL 故障后不能回退旧账号库。"""

import json
import pytest
from tango_backend.storage_profile import selectedStorageProfile
from tango_backend.service_config import storageConfiguration
from tango_backend import ensure_storage


def marker(root, value=None):
    directory = root / '.local/mysql-v1'
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'storage.env').write_text('fixture-placeholder\n')
    path = directory / 'active.json'
    path.write_text(json.dumps(value or {'driver':'mysql','version':1}))
    path.chmod(0o600)
    return path


def test_new_install_defaults_to_mysql_and_requires_shared_configuration(tmp_path):
    assert selectedStorageProfile(tmp_path)['driver'] == 'mysql'
    with pytest.raises(ValueError): storageConfiguration({})
    config = storageConfiguration({'QUIET_DATABASE_URL':'mysql://fixture:placeholder@127.0.0.1:3306/test',
                                   'QUIET_REDIS_URL':'redis://127.0.0.1:6379/0', 'QUIET_REDIS_NAMESPACE':'tango:fixture'})
    assert config['driver'] == 'mysql'


def test_legacy_profile_only_applies_before_explicit_cutover(tmp_path):
    old = tmp_path / '.local/storage-v1/storage.env'
    old.parent.mkdir(parents=True);old.write_text('legacy-placeholder\n')
    assert selectedStorageProfile(tmp_path) == {'driver':'postgres','envFile':old}
    path = marker(tmp_path)
    assert selectedStorageProfile(tmp_path)['driver'] == 'mysql'
    (path.parent / 'storage.env').unlink()
    with pytest.raises(ValueError): selectedStorageProfile(tmp_path)


@pytest.mark.parametrize('case', ['permissions','symlink','version','size','boolean_version'])
def test_invalid_mysql_marker_fails_closed(tmp_path, case):
    path = marker(tmp_path)
    if case == 'permissions': path.chmod(0o644)
    elif case == 'symlink':
        saved=path.with_suffix('.saved');path.rename(saved);path.symlink_to(saved)
    elif case == 'version': path.write_text('{"driver":"mysql","version":2}')
    elif case == 'boolean_version': path.write_text('{"driver":"mysql","version":true}')
    else: path.write_text(' ' * 4097)
    with pytest.raises((ValueError, OSError)): selectedStorageProfile(tmp_path)


@pytest.mark.parametrize('relative', ['backend/data/quiet.sqlite', 'backend_py/.data/quiet.sqlite'])
def test_existing_sqlite_accounts_require_explicit_migration(tmp_path, monkeypatch, relative):
    monkeypatch.delenv('QUIET_STORAGE_DRIVER', raising=False)
    old = tmp_path / relative
    old.parent.mkdir(parents=True);old.write_bytes(b'existing-database')
    def forbidden(*args, **kwargs): pytest.fail('不能启动空库替代已有账号库')
    monkeypatch.setattr(ensure_storage, 'start', forbidden)
    with pytest.raises(ValueError, match='旧本机账号库'): ensure_storage.ensure(tmp_path)


def test_legacy_postgres_requires_explicit_migration_and_never_starts_retired_node(tmp_path, monkeypatch):
    import subprocess
    monkeypatch.delenv('QUIET_STORAGE_DRIVER', raising=False)
    old = tmp_path / '.local/storage-v1'
    old.mkdir(parents=True)
    (old / 'storage.env').write_text('legacy-placeholder\n')
    (old / 'infra.json').write_text('{}\n')
    def forbidden(*args, **kwargs): pytest.fail('旧 PG 配置不能启动已退役 Node 或替代空库')
    monkeypatch.setattr(subprocess, 'run', forbidden)
    monkeypatch.setattr(ensure_storage, 'start', forbidden)
    with pytest.raises(ValueError, match='PostgreSQL|迁移|旧'):
        ensure_storage.ensure(tmp_path)


@pytest.mark.parametrize('driver', ['mysql', 'postgres', 'sqlite'])
def test_explicit_external_storage_does_not_start_local_instances(tmp_path, monkeypatch, driver):
    monkeypatch.setenv('QUIET_STORAGE_DRIVER', driver)
    def forbidden(*args, **kwargs): pytest.fail('显式部署不应启动本机基础设施')
    monkeypatch.setattr(ensure_storage, 'start', forbidden)
    ensure_storage.ensure(tmp_path)
