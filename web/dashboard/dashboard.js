/* Dashboard: a registry of panels rendered into a grid. Add a mode/panel by pushing onto PANELS. */
(function () {
  'use strict';
  var W = window.Webauthn;
  var me = null;       // {user, passkeys, email_enabled}
  var grid = document.getElementById('grid');

  // ---- tiny DOM helpers (textContent only: no HTML injection) ----
  function h(tag, attrs) {
    var el = document.createElement(tag), k, i;
    attrs = attrs || {};
    for (k in attrs) {
      if (k === 'class') el.className = attrs[k];
      else if (k.slice(0, 2) === 'on') el[k] = attrs[k];
      else if (attrs[k] !== false && attrs[k] != null) el.setAttribute(k, attrs[k] === true ? '' : attrs[k]);
    }
    function add(c) {
      if (c == null || c === false) return;
      if (Array.isArray(c)) c.forEach(add);
      else el.appendChild(typeof c === 'object' ? c : document.createTextNode(String(c)));
    }
    for (i = 2; i < arguments.length; i++) add(arguments[i]);
    return el;
  }
  function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }
  function fmt(iso) { if (!iso) return '\u2013'; var d = new Date(iso); return isNaN(d) ? '\u2013' : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }); }

  function toast(msg, isErr) {
    var t = h('div', { class: 'toast' + (isErr ? ' err' : '') }, msg);
    document.getElementById('toasts').appendChild(t);
    setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, isErr ? 6000 : 3500);
  }

  function fail(e) {
    if (e && e.status === 401) { location.assign('/auth/?next=' + encodeURIComponent(location.pathname + location.search)); return; }
    toast((e && e.message) || 'Something went wrong.', true);
  }

  function confirmDialog(text, label) {
    return new Promise(function (resolve) {
      var bg = h('div', { class: 'modal-bg', role: 'dialog', 'aria-modal': 'true' });
      function done(v) { document.removeEventListener('keydown', onKey); bg.parentNode.removeChild(bg); resolve(v); }
      function onKey(e) { if (e.key === 'Escape') done(false); }
      var cancel = h('button', { class: 'btn ghost', type: 'button', onclick: function () { done(false); } }, 'Cancel');
      var ok = h('button', { class: 'btn danger', type: 'button', onclick: function () { done(true); } }, label || 'Confirm');
      bg.appendChild(h('div', { class: 'modal' }, h('p', null, text), h('div', { class: 'row' }, cancel, ok)));
      bg.onclick = function (e) { if (e.target === bg) done(false); };
      document.addEventListener('keydown', onKey);
      document.body.appendChild(bg);
      cancel.focus();
    });
  }

  // text prompt modelled on confirmDialog: resolves the trimmed text, or null when cancelled
  function promptDialog(text, value, label) {
    return new Promise(function (resolve) {
      var bg = h('div', { class: 'modal-bg', role: 'dialog', 'aria-modal': 'true' });
      var input = h('input', { type: 'text', maxlength: '40', 'aria-label': text });
      input.value = value || '';
      function done(v) { document.removeEventListener('keydown', onKey); bg.parentNode.removeChild(bg); resolve(v); }
      function onKey(e) { if (e.key === 'Escape') done(null); }
      input.onkeydown = function (e) { if (e.key === 'Enter') done(input.value.trim()); };
      var cancel = h('button', { class: 'btn ghost', type: 'button', onclick: function () { done(null); } }, 'Cancel');
      var ok = h('button', { class: 'btn', type: 'button', onclick: function () { done(input.value.trim()); } }, label || 'OK');
      bg.appendChild(h('div', { class: 'modal' }, h('p', null, text), input, h('div', { class: 'row', style: 'margin-top:12px' }, cancel, ok)));
      bg.onclick = function (e) { if (e.target === bg) done(null); };
      document.addEventListener('keydown', onKey);
      document.body.appendChild(bg);
      input.focus();
      input.select();
    });
  }

  // ---- save game summaries (tools/saves.py SUMMARY_KEYS, uploaded by the game) ----
  function fmtPlay(sec) {
    var m = Math.floor(sec / 60);
    return Math.floor(m / 60) + 'h ' + String(m % 60).padStart(2, '0') + 'm';
  }
  function statsText(s) {
    return s ? s.percent + '% \u00b7 ' + s.goldenBananas + ' GB \u00b7 ' + s.keys + '/8 keys \u00b7 ' + fmtPlay(s.playSeconds) : 'Not started yet';
  }
  function playUrl(save) { return '/play?save=' + save.id; }

  function copy(text) {
    function fallback() {
      var ta = h('textarea', { style: 'position:fixed;opacity:0' });
      ta.value = text; document.body.appendChild(ta); ta.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (e) { /* ignore */ }
      document.body.removeChild(ta);
      toast(ok ? 'Link copied.' : 'Copy failed. Select the link and copy it manually.', !ok);
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { toast('Link copied.'); }, fallback);
    } else fallback();
  }

  // ---- panels ----
  function panel(title, cls) {
    var body = h('div');
    return { el: h('section', { class: 'panel ' + (cls || '') }, h('h2', null, title), body), body: body };
  }

  function savesPanel(body) {
    var top = h('div'), grid = h('div', { class: 'panel-grid', style: 'margin-top:10px' });
    var nameIn = h('input', { type: 'text', class: 'grow', maxlength: '40', placeholder: 'New save name (optional)', 'aria-label': 'New save name' });
    var create = h('button', { class: 'btn', type: 'submit' }, 'New save');
    var form = h('form', { class: 'row', style: 'margin-top:14px', onsubmit: function (e) {
      e.preventDefault();
      create.disabled = true;
      W.api('POST', '/api/saves', { name: nameIn.value.trim() }).then(function () {
        nameIn.value = ''; toast('Save created.'); load();
      }).catch(fail).then(function () { create.disabled = false; });
    } }, nameIn, create);
    // Import save…: a new save game whose first version is the file's EEPROM (web/savefile.js parses .eep / .srm)
    var fileIn = h('input', { type: 'file', accept: '.eep,.srm', style: 'display:none', onchange: function () {
      var f = fileIn.files[0];
      fileIn.value = '';
      if (!f) return;
      importBtn.disabled = true;
      f.arrayBuffer().then(function (buf) {
        var bytes = window.dk64savefile.parse(buf);
        return W.api('POST', '/api/saves', {
          name: nameIn.value.trim() || f.name.replace(/\.(eep|srm)$/i, '').trim().slice(0, 40),
          eeprom: btoa(String.fromCharCode.apply(null, bytes)),
        });
      }).then(function (r) {
        nameIn.value = ''; toast('Imported as \u201c' + r.save.name + '\u201d.'); load();
      }).catch(fail).then(function () { importBtn.disabled = false; });
    } });
    var importBtn = h('button', { class: 'btn ghost', type: 'button', onclick: function () { fileIn.click(); } }, 'Import save\u2026');
    form.appendChild(importBtn);
    form.appendChild(fileIn);
    body.appendChild(top);
    body.appendChild(form);
    body.appendChild(grid);
    // Future modes (co-op, level select, ...) append tiles here.
    body.appendChild(h('div', { class: 'panel-grid', id: 'modes' }));

    function tile(s) {
      return h('div', { class: 'tile' },
        h('h3', null, s.name),
        h('p', { class: 'stats' }, statsText(s.summary)),
        h('p', { class: 'muted' }, 'Last played ' + fmt(s.played_at)),
        h('div', { class: 'acts' },
          h('button', { class: 'btn', type: 'button', onclick: function () { location.assign(playUrl(s)); } }, 'Play'),
          h('button', { class: 'btn small', type: 'button', onclick: function () {
            promptDialog('Rename \u201c' + s.name + '\u201d', s.name, 'Rename').then(function (name) {
              if (name) W.api('PATCH', '/api/saves/' + s.id, { name: name }).then(function () { toast('Save renamed.'); load(); }).catch(fail);
            });
          } }, 'Rename'),
          h('button', { class: 'btn small', type: 'button', onclick: function () { exportSave(s); } }, 'Export'),
          h('button', { class: 'btn small danger', type: 'button', onclick: function () {
            confirmDialog('Delete ' + s.name + '? Its whole save history is removed. This can\u2019t be undone.', 'Delete').then(function (y) {
              if (y) W.api('DELETE', '/api/saves/' + s.id).then(function () { toast('Save deleted.'); load(); }).catch(fail);
            });
          } }, 'Delete')));
    }

    // the save's latest EEPROM version as an .eep file (web/savefile.js)
    function exportSave(s) {
      fetch('/api/saves/' + s.id + '/eeprom', { credentials: 'same-origin', cache: 'no-store' }).then(function (r) {
        if (r.ok) {
          return r.arrayBuffer().then(function (buf) {
            var name = window.dk64savefile.fileName(s.name);
            window.dk64savefile.download(new Uint8Array(buf), name);
            toast('Exported ' + name + '.');
          });
        }
        return r.json().catch(function () { return null; }).then(function (b) {
          if (b && b.error === 'no_save_data') { toast('Nothing to export yet: this save has no progress.', true); return; }
          var e = new Error((b && b.message) || 'Export failed (' + r.status + ')');
          e.status = r.status;
          throw e;
        });
      }).catch(fail);
    }

    function load() {
      W.api('GET', '/api/saves').then(function (r) {
        clear(top); clear(grid);
        if (r.saves.length) {
          top.appendChild(h('button', { class: 'play', type: 'button', onclick: function () { location.assign(playUrl(r.saves[0])); } }, 'Continue ' + r.saves[0].name));
        } else {
          top.appendChild(h('button', { class: 'play', type: 'button', onclick: function (e) {
            e.target.disabled = true;
            W.api('POST', '/api/saves', {}).then(function (c) { location.assign(playUrl(c.save)); }, function (er) { e.target.disabled = false; fail(er); });
          } }, 'Start a new game'));
        }
        r.saves.forEach(function (s) { grid.appendChild(tile(s)); });
      }).catch(fail);
    }
    load();
  }

  function playersPanel(body) {
    var box = h('div');
    body.appendChild(box);
    W.api('GET', '/api/players').then(function (r) {
      var rows = [];
      r.players.forEach(function (u) {
        var who = [u.username, ' ',
          u.owner ? h('span', { class: 'badge owner' }, 'owner') : null, ' ',
          u.id === me.user.id ? h('span', { class: 'badge' }, 'you') : null, ' ',
          u.disabled ? h('span', { class: 'badge bad' }, 'disabled') : null];
        if (!u.saves.length) {
          rows.push(h('tr', null, h('td', null, who), h('td', { class: 'muted' }, 'No saves yet'), ['\u2013', '\u2013', '\u2013', '\u2013', '\u2013'].map(function (t) { return h('td', null, t); })));
          return;
        }
        u.saves.forEach(function (s) {
          var m = s.summary;
          var cells = m ? [m.percent + '%', m.goldenBananas, m.keys + '/8', fmtPlay(m.playSeconds)] : ['\u2013', '\u2013', '\u2013', '\u2013'];
          rows.push(h('tr', null, h('td', null, who.map(function (c) { return c && c.cloneNode ? c.cloneNode(true) : c; })), h('td', null, s.name),
            cells.map(function (t) { return h('td', null, t); }), h('td', null, fmt(s.played_at))));
        });
      });
      box.appendChild(h('div', { class: 'tbl-wrap' }, h('table', { class: 'tbl' },
        h('thead', null, h('tr', null, ['Player', 'Save', 'Progress', 'Golden bananas', 'Keys', 'Play time', 'Last played'].map(function (t) { return h('th', null, t); }))),
        h('tbody', null, rows))));
    }).catch(fail);
  }

  function invitesPanel(body) {
    var list = h('div'), created = h('div');
    var maxSel = h('select', { id: 'inv-type' }, h('option', { value: 'single' }, 'Single-use'), h('option', { value: 'multi' }, 'Reusable'));
    var maxIn = h('input', { type: 'number', min: '2', placeholder: 'Max uses (blank = unlimited)', 'aria-label': 'Max uses' });
    var expSel = h('select', { 'aria-label': 'Expires in' }, h('option', { value: '' }, 'Never expires'),
      h('option', { value: '1' }, '1 day'), h('option', { value: '7' }, '7 days'), h('option', { value: '30' }, '30 days'));
    var note = h('input', { type: 'text', maxlength: '120', placeholder: 'Note (optional)', 'aria-label': 'Note' });
    var mail = h('input', { type: 'email', placeholder: 'Email the link (optional)', 'aria-label': 'Email recipient',
      disabled: !me.email_enabled,
      title: me.email_enabled ? 'Send the link once it is created' : 'Email is not configured on this server.' });
    var go = h('button', { class: 'btn', type: 'submit' }, 'Create link');
    maxIn.style.display = 'none';
    maxSel.onchange = function () { maxIn.style.display = maxSel.value === 'multi' ? '' : 'none'; };
    var form = h('form', { onsubmit: function (e) {
      e.preventDefault();
      var max = 1;
      if (maxSel.value === 'multi') max = maxIn.value ? parseInt(maxIn.value, 10) : null;
      go.disabled = true;
      W.api('POST', '/api/invites', { max_uses: max, expires_in_days: expSel.value ? parseInt(expSel.value, 10) : null,
          note: note.value.trim() || null, email: mail.value.trim() || null })
        .then(function (r) {
          clear(created);
          created.appendChild(h('div', { class: 'link' }, h('input', { readonly: true, value: r.url, 'aria-label': 'Invite link', onfocus: function (e) { e.target.select(); } }),
            h('button', { class: 'btn', type: 'button', onclick: function () { copy(r.url); } }, 'Copy')));
          created.appendChild(h('p', { class: 'muted' }, 'This is the only time the link is shown \u2014 copy it now.'));
          if (r.email_sent_to) created.appendChild(h('p', { class: 'muted' }, 'Emailed to ' + r.email_sent_to + '.'));
          note.value = ''; mail.value = ''; load();
        }).catch(fail).then(function () { go.disabled = false; });
    } }, h('div', { class: 'row' }, h('div', { class: 'grow' }, maxSel), h('div', { class: 'grow' }, maxIn)),
      h('div', { class: 'row' }, h('div', { class: 'grow' }, expSel), h('div', { class: 'grow' }, note)),
      h('div', { class: 'row' }, h('div', { class: 'grow' }, mail), go));
    body.appendChild(form);
    if (!me.email_enabled) body.appendChild(h('p', { class: 'muted' }, 'Email sending is not configured on this server: copy the link and send it yourself.'));
    body.appendChild(created); body.appendChild(h('div', { style: 'height:12px' })); body.appendChild(list);

    function row(inv) {
      var exp = inv.expires_at && new Date(inv.expires_at) < new Date();
      var exhausted = inv.max_uses != null && inv.uses >= inv.max_uses;
      var state = inv.revoked ? ['revoked', 'bad'] : exp ? ['expired', 'bad'] : exhausted ? ['used up', 'bad'] : ['active', 'ok'];
      var acts = h('div', { class: 'acts' });
      if (state[1] === 'ok') {
        acts.appendChild(h('button', { class: 'btn small danger', type: 'button', onclick: function (e) {
          e.target.disabled = true;
          W.api('DELETE', '/api/invites/' + inv.id).then(function () { toast('Invite revoked.'); load(); }, function (er) { e.target.disabled = false; fail(er); });
        } }, 'Revoke'));
      }
      var cells = [h('td', null, inv.note || '\u2013', inv.last_email_to ? h('div', { class: 'muted' }, 'emailed ' + inv.last_email_to) : null)];
      if (me.user.owner) cells.push(h('td', null, inv.created_by || ''));
      cells.push(h('td', null, inv.uses + '/' + (inv.max_uses == null ? '\u221e' : inv.max_uses)));
      cells.push(h('td', null, inv.expires_at ? fmt(inv.expires_at) : 'never'));
      cells.push(h('td', null, h('span', { class: 'badge ' + state[1] }, state[0])));
      cells.push(h('td', null, acts));
      return h('tr', null, cells);
    }

    function load() {
      W.api('GET', '/api/invites').then(function (r) {
        clear(list);
        if (!r.invites.length) { list.appendChild(h('p', { class: 'muted' }, 'No invites yet.')); return; }
        var head = [h('th', null, 'Note')];
        if (me.user.owner) head.push(h('th', null, 'By'));
        head.push(h('th', null, 'Uses'), h('th', null, 'Expires'), h('th', null, 'State'), h('th', null, ''));
        list.appendChild(h('div', { class: 'tbl-wrap' }, h('table', { class: 'tbl' }, h('thead', null, h('tr', null, head)),
          h('tbody', null, r.invites.map(row)))));
      }).catch(fail);
    }
    load();
  }

  function accountPanel(body) {
    var pk = h('div');
    body.appendChild(h('p', null, 'Signed in as ', h('b', null, me.user.username), ' ', me.user.owner ? h('span', { class: 'badge owner' }, 'owner') : null));
    body.appendChild(pk);

    function render() {
      clear(pk);
      var only = me.passkeys.length <= 1;
      if (only) pk.appendChild(h('div', { class: 'note' }, 'Add a second passkey so a lost device doesn\u2019t lock you out.'));
      me.passkeys.forEach(function (p) {
        var name = h('b', null, p.name);
        var line = h('div', { class: 'tile', style: 'margin-bottom:8px' }, name,
          h('div', { class: 'muted' }, 'Added ' + fmt(p.created_at) + ' \u00b7 Last used ' + (p.last_used_at ? fmt(p.last_used_at) : 'never')));
        var acts = h('div', { class: 'acts', style: 'margin-top:8px' });
        acts.appendChild(h('button', { class: 'btn small', type: 'button', onclick: function () { editName(p, line, name, acts); } }, 'Rename'));
        acts.appendChild(h('button', { class: 'btn small danger', type: 'button', disabled: only,
          title: only ? 'You can\u2019t remove your only passkey.' : 'Remove this passkey',
          onclick: function () {
            confirmDialog('Remove passkey \u201c' + p.name + '\u201d? Devices using it will no longer be able to sign in.', 'Remove').then(function (y) {
              if (!y) return;
              W.api('DELETE', '/api/me/passkeys/' + encodeURIComponent(p.id)).then(function () { toast('Passkey removed.'); return refreshMe(); }).then(render).catch(fail);
            });
          } }, 'Remove'));
        line.appendChild(acts);
        pk.appendChild(line);
      });
    }

    function editName(p, line, nameEl, acts) {
      var input = h('input', { type: 'text', value: p.name, maxlength: '40', 'aria-label': 'Passkey name' });
      var save = h('button', { class: 'btn small', type: 'button' }, 'Save');
      var cancel = h('button', { class: 'btn small ghost', type: 'button', onclick: render }, 'Cancel');
      function doSave() {
        var v = input.value.trim();
        if (!v) return;
        save.disabled = true;
        W.api('PATCH', '/api/me/passkeys/' + encodeURIComponent(p.id), { name: v }).then(function () { return refreshMe(); }).then(render)
          .catch(function (e) { save.disabled = false; fail(e); });
      }
      save.onclick = doSave;
      input.onkeydown = function (e) { if (e.key === 'Enter') doSave(); else if (e.key === 'Escape') render(); };
      line.replaceChild(h('div', { class: 'row' }, h('div', { class: 'grow' }, input), save, cancel), nameEl);
      acts.style.display = 'none';
      input.focus(); input.select();
    }

    var add = h('button', { class: 'btn', type: 'button' }, 'Add passkey');
    var nameIn = h('input', { type: 'text', maxlength: '40', placeholder: 'Name, e.g. iPhone (optional)', 'aria-label': 'New passkey name' });
    if (!W.supported()) { add.disabled = true; body.appendChild(h('div', { class: 'note' }, 'Passkeys need HTTPS (or localhost).')); }
    add.onclick = function () {
      add.disabled = true;
      W.api('POST', '/api/me/passkeys/options', {}).then(function (o) {
        return W.register(o.publicKey).then(function (cred) {
          var body = { challenge_id: o.challenge_id, credential: cred };
          if (nameIn.value.trim()) body.name = nameIn.value.trim();
          return W.api('POST', '/api/me/passkeys/complete', body);
        });
      }).then(function () { toast('Passkey added.'); nameIn.value = ''; return refreshMe(); }).then(render).catch(fail).then(function () { add.disabled = !W.supported(); });
    };
    body.appendChild(h('div', { class: 'row' }, h('div', { class: 'grow' }, nameIn), add));
    body.appendChild(h('div', { class: 'row' }, h('button', { class: 'btn ghost', type: 'button', onclick: signOut }, 'Sign out')));
    render();
  }

  function usersPanel(body) {
    var box = h('div');
    body.appendChild(box);
    function load() {
      W.api('GET', '/api/users').then(function (r) {
        clear(box);
        var rows = r.users.map(function (u) {
          var locked = u.owner || u.id === me.user.id;
          var why = u.owner ? 'The owner can\u2019t be changed.' : 'You can\u2019t change your own account.';
          return h('tr', null,
            h('td', null, u.username, ' ', u.owner ? h('span', { class: 'badge owner' }, 'owner') : null, u.id === me.user.id && !u.owner ? h('span', { class: 'badge' }, 'you') : null),
            h('td', null, h('span', { class: 'badge ' + (u.disabled ? 'bad' : 'ok') }, u.disabled ? 'disabled' : 'enabled')),
            h('td', null, fmt(u.created_at)), h('td', null, fmt(u.last_login_at)), h('td', null, u.passkey_count),
            h('td', null, h('div', { class: 'acts' },
              h('button', { class: 'btn small', type: 'button', disabled: locked, title: locked ? why : '', onclick: function () {
                W.api('PATCH', '/api/users/' + u.id, { disabled: !u.disabled }).then(function () { toast(u.disabled ? 'User enabled.' : 'User disabled.'); load(); }).catch(fail);
              } }, u.disabled ? 'Enable' : 'Disable'),
              h('button', { class: 'btn small danger', type: 'button', disabled: locked, title: locked ? why : '', onclick: function () {
                confirmDialog('Delete ' + u.username + '? Their passkeys and sessions are removed. This can\u2019t be undone.', 'Delete').then(function (y) {
                  if (y) W.api('DELETE', '/api/users/' + u.id).then(function () { toast('User deleted.'); load(); }).catch(fail);
                });
              } }, 'Delete'))));
        });
        box.appendChild(h('div', { class: 'tbl-wrap' }, h('table', { class: 'tbl' },
          h('thead', null, h('tr', null, ['User', 'Status', 'Created', 'Last login', 'Passkeys', ''].map(function (t) { return h('th', null, t); }))),
          h('tbody', null, rows))));
      }).catch(fail);
    }
    load();
  }

  function settingsPanel(body) {
    var cb = h('input', { type: 'checkbox', disabled: true });
    body.appendChild(h('label', { class: 'switch' }, cb, h('span', null, 'Open registration (anyone can create an account)')));
    body.appendChild(h('p', { class: 'muted' }, 'Invite links keep working when registration is closed.'));
    W.api('GET', '/api/settings').then(function (s) { cb.checked = !!s.registration_open; cb.disabled = false; }).catch(fail);
    cb.onchange = function () {
      var want = cb.checked;
      cb.disabled = true;
      W.api('PATCH', '/api/settings', { registration_open: want }).then(function (s) { cb.checked = !!s.registration_open; toast('Registration ' + (s.registration_open ? 'opened' : 'closed') + '.'); })
        .catch(function (e) { cb.checked = !want; fail(e); }).then(function () { cb.disabled = false; });
    };
  }

  var PANELS = [
    { id: 'saves', title: 'Your saves', cls: 'wide', render: savesPanel },
    { id: 'players', title: 'Players', cls: 'span2', render: playersPanel },
    { id: 'account', title: 'Account', render: accountPanel },
    { id: 'invites', title: 'Invites', cls: 'span2', render: invitesPanel },
    { id: 'users', title: 'Users', cls: 'span2', owner: true, render: usersPanel },
    { id: 'settings', title: 'Settings', owner: true, render: settingsPanel }
  ];

  function refreshMe() {
    return W.api('GET', '/api/me').then(function (r) { me = r; });
  }

  function signOut() {
    W.api('POST', '/auth/api/logout', {}).catch(function () { /* cookie cleared or already gone */ }).then(function () { location.assign('/auth/'); });
  }

  function boot() {
    if (window.Banana) window.Banana.mount(document.getElementById('banana'));
    document.getElementById('signout').onclick = signOut;
    refreshMe().then(function () {
      var who = document.getElementById('who');
      who.appendChild(document.createTextNode('signed in as '));
      who.appendChild(h('b', null, me.user.username));
      PANELS.forEach(function (p) {
        if (p.owner && !me.user.owner) return;
        var pn = panel(p.title, p.cls);
        pn.el.id = 'panel-' + p.id;
        grid.appendChild(pn.el);
        p.render(pn.body);
      });
    }).catch(fail);
  }

  boot();
})();
