const assert = require('node:assert/strict');
const shell = require('../tools/command-builder/command-shell.js');

assert.equal(shell.defaultForPlatform('Win32'), 'powershell');
assert.equal(shell.defaultForPlatform('macOS'), 'zsh');
assert.equal(shell.defaultForPlatform('Darwin'), 'zsh');
assert.equal(shell.defaultForPlatform('Linux'), 'bash');
assert.equal(shell.supported('toString'), false);
assert.equal(shell.render('mosaicraft', ['create', "a'b $x.jpg"], 'powershell'), "& 'mosaicraft' create 'a''b $x.jpg'");
assert.equal(shell.render('mosaicraft', ['create', 'a & b.jpg'], 'cmd'), 'mosaicraft create "a & b.jpg"');
assert.equal(shell.render('mosaicraft', ['C:\\a b\\'], 'cmd'), 'mosaicraft "C:\\a b\\\\"');
assert.equal(shell.render('mosaicraft', ['C:\\a b\\'], 'powershell'), "& 'mosaicraft' 'C:\\a b\\\\'");
assert.equal(shell.render('mosaicraft', ['a\u2019b'], 'powershell'), "& 'mosaicraft' 'a\u2019\u2019b'");
assert.match(shell.render('a b/mosaicraft', ['"quoted"', ''], 'pwsh'), /PSNativeCommandArgumentPassing = 'Standard'/);
for (const name of ['bash', 'zsh', 'sh']) {
  assert.equal(shell.render('./mosaicraft', ['create', "a'b $x.jpg"], name), "./mosaicraft create 'a'\\''b $x.jpg'");
  assert.equal(shell.render('./mosaicraft', ['', '=value', '*.jpg'], name), "./mosaicraft '' '=value' '*.jpg'");
}
for (const name of Object.keys(shell.names)) {
  for (const value of ['line\nbreak', 'line\rbreak', 'nul\0byte', '\u2028']) {
    assert.throws(() => shell.render('mosaicraft', [value], name), /control-character/);
  }
}
for (const value of ['%PATH%', '!VAR!', 'a"b']) {
  assert.throws(() => shell.render('mosaicraft', [value], 'cmd'), /cmd-argument/);
}
assert.throws(() => shell.render('mosaicraft', ['a"b'], 'powershell'), /powershell-legacy-argument/);
assert.throws(() => shell.render('mosaicraft', [''], 'powershell'), /powershell-legacy-argument/);
assert.throws(() => shell.render('mosaicraft', [], 'unknown'), /unknown-shell/);
assert.throws(() => shell.render('', [], 'bash'), /missing-executable/);
console.log('Command shell quoting and platform defaults passed.');
