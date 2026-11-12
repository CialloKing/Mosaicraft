// Keep argument construction separate from shell syntax so every CLI page shares it.
const commandShell = (() => {
  const names = {
    powershell: 'Windows PowerShell 5.1', pwsh: 'PowerShell 7.3+',
    cmd: 'CMD (cmd.exe)', bash: 'Bash', zsh: 'Zsh', sh: 'POSIX sh'
  };
  const supported = shell => Object.hasOwn(names, shell);
  const defaultForPlatform = platform => /^(win|windows)/i.test(platform) ? 'powershell' : /mac|darwin/i.test(platform) ? 'zsh' : 'bash';
  const singleQuoted = value => "'" + value.replace(/'/g, "'\\''") + "'";
  const powershellQuoted = value => "'" + value.replace(/'/g, "''") + "'";
  const windowsQuoted = value => '"' + value.replace(/(\\*)"/g, '$1$1\\"').replace(/(\\+)$/g, '$1$1') + '"';

  function render(executable, args, shell) {
    if (!supported(shell)) throw new Error('unknown-shell');
    const values = [executable, ...args].map(String);
    if (!values[0].trim()) throw new Error('missing-executable');
    // One pasted command must never contain a second physical command line.
    if (values.some(value => /[\x00-\x1f\x7f\u2028\u2029]/.test(value))) {
      throw new Error('control-character');
    }
    const safe = value => /^[A-Za-z0-9_./:+-][A-Za-z0-9_./:=+-]*$/.test(value);
    if (shell === 'cmd') {
      // Expansion also happens inside double quotes. Refuse ambiguous paste syntax.
      if (values.some(value => /[%!\"]/.test(value))) {
        throw new Error('cmd-argument');
      }
      return values.map(value => safe(value) ? value : windowsQuoted(value)).join(' ');
    }
    if (shell === 'powershell' || shell === 'pwsh') {
      if (shell === 'powershell' && values.some(value => value === '' || value.includes('"'))) {
        throw new Error('powershell-legacy-argument');
      }
      const quote = value => {
        // Legacy native binding adds quotes around whitespace but leaves trailing slashes unescaped.
        const native = shell === 'powershell' && /\s/.test(value) ? value.replace(/(\\+)$/g, '$1$1') : value;
        return safe(native) ? native : powershellQuoted(native);
      };
      const command = '& ' + powershellQuoted(values[0]) + values.slice(1).map(value => ' ' + quote(value)).join('');
      // Scope the native argument preference to this invocation, not the user's session.
      return shell === 'pwsh' ? "& { $PSNativeCommandArgumentPassing = 'Standard'; " + command + ' }' : command;
    }
    return values.map(value => safe(value) ? value : singleQuoted(value)).join(' ');
  }
  return {names, supported, defaultForPlatform, render};
})();
if (typeof module !== 'undefined' && module.exports) module.exports = commandShell;
