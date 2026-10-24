import { constants } from 'node:fs';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { randomBytes } from 'node:crypto';

export function dataDirectory() {
  return process.env.MUSICPLAYER_DATA_DIR ||
    path.join(os.homedir(), 'Library', 'Application Support', 'Zijing', 'MusicPlayer2');
}

async function privateDirectory(directory) {
  await fs.mkdir(directory, { recursive: true, mode: 0o700 });
  const stat = await fs.lstat(directory);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('unsafe_data_directory');
  await fs.chmod(directory, 0o700);
}

async function privateRead(filename) {
  const handle = await fs.open(filename, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const stat = await handle.stat();
    if (!stat.isFile() || stat.size > 32768) throw new Error('invalid_credentials');
    await handle.chmod(0o600);
    return await handle.readFile('utf8');
  } finally {
    await handle.close();
  }
}

async function atomicWrite(filename, text) {
  const temporary = `${filename}.${randomBytes(8).toString('hex')}.tmp`;
  try {
    const handle = await fs.open(temporary, 'wx', 0o600);
    try {
      await handle.writeFile(text, 'utf8');
      await handle.sync();
    } finally {
      await handle.close();
    }
    await fs.rename(temporary, filename);
  } finally {
    await fs.rm(temporary, { force: true });
  }
}

export async function openCredentials(baseDirectory = dataDirectory()) {
  const directory = path.join(baseDirectory, 'netease');
  await privateDirectory(directory);
  const tokenPath = path.join(directory, 'token');
  let token;
  try {
    token = (await privateRead(tokenPath)).trim();
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    const generated = randomBytes(32).toString('hex');
    try {
      await fs.writeFile(tokenPath, `${generated}\n`, { flag: 'wx', mode: 0o600 });
      token = generated;
    } catch (createError) {
      if (createError.code !== 'EEXIST') throw createError;
      token = (await privateRead(tokenPath)).trim();
    }
  }
  if (!/^[a-f0-9]{64}$/.test(token)) throw new Error('invalid_token_file');

  // 原 Cookie 只属于访客配置，不得仅因为用户在本机
  // 登录 Tango 账号，就将其迁入该账号。
  const slots = new Map();
  async function openSlot(slotDirectory) {
    const cookiePath = path.join(slotDirectory, 'cookie.json');
    let cookie = '';
    try {
      const stored = JSON.parse(await privateRead(cookiePath));
      if (typeof stored.cookie !== 'string' || stored.cookie.length > 16384 || /[\r\n]/.test(stored.cookie)) {
        throw new Error('invalid_cookie_file');
      }
      cookie = stored.cookie;
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
    let revision = 0;
    let writes = Promise.resolve();
    const enqueue = (operation) => {
      const next = writes.then(operation);
      writes = next.catch(() => {});
      return next;
    };
    return {
      get cookie() { return cookie; },
      async saveCookie(value) {
        if (typeof value !== 'string' || value.length > 16384 || /[\r\n]/.test(value)) {
          throw new Error('invalid_cookie');
        }
        const current = ++revision;
        await enqueue(async () => {
          await atomicWrite(cookiePath, JSON.stringify({ cookie: value }));
          if (revision === current) cookie = value;
        });
      },
      async clearCookie() {
        // 即使正在进行磁盘 I/O，本机退出登录也优先于此前排队的保存。
        ++revision;
        cookie = '';
        await enqueue(() => fs.rm(cookiePath, { force: true }));
      },
    };
  }
  const guest = await openSlot(directory);
  return {
    directory, token,
    // 兼容 API 有意始终访问旧版访客存储槽。
    get cookie() { return guest.cookie; },
    saveCookie: (value) => guest.saveCookie(value),
    clearCookie: () => guest.clearCookie(),
    async forProfile(profile = 'guest') {
      if (profile === 'guest') return guest;
      if (typeof profile !== 'string' || !/^[a-f0-9]{64}$/.test(profile)) throw new Error('invalid_profile');
      if (!slots.has(profile)) {
        // 原生账号配置离开 Qt 前先经过哈希；原始账号 ID
        // 和后端令牌不会成为目录名或音乐 API 请求头。
        if (slots.size >= 128) throw new Error('too_many_profiles');
        const loading = (async () => {
          const profilesDirectory = path.join(directory, 'profiles');
          await privateDirectory(profilesDirectory);
          const profileDirectory = path.join(profilesDirectory, profile);
          await privateDirectory(profileDirectory);
          return openSlot(profileDirectory);
        })();
        slots.set(profile, loading);
        loading.catch(() => { if (slots.get(profile) === loading) slots.delete(profile); });
      }
      return slots.get(profile);
    },
  };
}

export async function privateTemporaryDirectory(directory) {
  const temporary = path.join(directory, 'tmp');
  await privateDirectory(temporary);
  // 上游请求模块在加载时读取此值；它是匿名令牌，
  // 不是账号 Cookie，并始终留在同一个私人数据目录内。
  await atomicWrite(path.join(temporary, 'anonymous_token'), '');
  return temporary;
}
