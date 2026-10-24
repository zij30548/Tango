import assert from 'node:assert/strict';
import test from 'node:test';
import { serviceHealthStatus } from '../launch.mjs';

test('launcher only reuses the current catalog protocol with account isolation', () => {
  const current = { app:'quiet-netease', version:1, pid:1234, profileIsolation:1, catalogVersion:2 };
  assert.equal(serviceHealthStatus(current,200),'ready');
  for (const patch of [{catalogVersion:undefined},{catalogVersion:1},{catalogVersion:3},
    {catalogVersion:'2'},{profileIsolation:undefined},{profileIsolation:0}])
    assert.equal(serviceHealthStatus({...current,...patch},200),'outdated');
  for (const patch of [{app:'foreign'},{version:2},{pid:0},{pid:'1234'}])
    assert.equal(serviceHealthStatus({...current,...patch},200),'occupied');
  assert.equal(serviceHealthStatus(current,500),'occupied');
  assert.equal(serviceHealthStatus(null,200),'occupied');
});
