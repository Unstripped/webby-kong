/* Shared WebAuthn helpers + JSON fetch wrapper. Exposes window.Webauthn. */
(function () {
  'use strict';

  function b64uToBuf(s) {
    s = String(s).replace(/-/g, '+').replace(/_/g, '/');
    while (s.length % 4) s += '=';
    var bin = atob(s), u = new Uint8Array(bin.length), i;
    for (i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
    return u.buffer;
  }

  function bufToB64u(buf) {
    var u = new Uint8Array(buf), s = '', i;
    for (i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
    return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }

  function mapIds(list) {
    return (list || []).map(function (d) {
      var c = Object.assign({}, d);
      c.id = b64uToBuf(d.id);
      return c;
    });
  }

  function prepareCreationOptions(pk) {
    var o = Object.assign({}, pk);
    o.challenge = b64uToBuf(pk.challenge);
    o.user = Object.assign({}, pk.user, { id: b64uToBuf(pk.user.id) });
    if (pk.excludeCredentials) o.excludeCredentials = mapIds(pk.excludeCredentials);
    return o;
  }

  function prepareRequestOptions(pk) {
    var o = Object.assign({}, pk);
    o.challenge = b64uToBuf(pk.challenge);
    if (pk.allowCredentials) o.allowCredentials = mapIds(pk.allowCredentials);
    return o;
  }

  function supported() {
    return !!(window.PublicKeyCredential && window.isSecureContext && navigator.credentials);
  }

  function friendly(err) {
    var n = err && err.name;
    if (n === 'NotAllowedError') return 'The passkey prompt was cancelled or timed out.';
    if (n === 'InvalidStateError') return 'This device already has a passkey for this account.';
    if (n === 'SecurityError') return 'Passkeys are not allowed on this address. Use HTTPS (or localhost).';
    if (n === 'NotSupportedError') return 'This browser or device does not support the required passkey type.';
    return null;
  }

  // Resolves to the credential JSON the server expects; rejects with an Error (message is user-safe).
  function register(publicKey) {
    return navigator.credentials.create({ publicKey: prepareCreationOptions(publicKey) }).then(function (c) {
      var r = c.response;
      return {
        id: c.id,
        rawId: bufToB64u(c.rawId),
        type: c.type,
        response: {
          clientDataJSON: bufToB64u(r.clientDataJSON),
          attestationObject: bufToB64u(r.attestationObject),
          transports: typeof r.getTransports === 'function' ? r.getTransports() : []
        },
        clientExtensionResults: {}
      };
    }).catch(wrap);
  }

  function signIn(publicKey) {
    return navigator.credentials.get({ publicKey: prepareRequestOptions(publicKey) }).then(function (c) {
      var r = c.response;
      return {
        id: c.id,
        rawId: bufToB64u(c.rawId),
        type: c.type,
        response: {
          clientDataJSON: bufToB64u(r.clientDataJSON),
          authenticatorData: bufToB64u(r.authenticatorData),
          signature: bufToB64u(r.signature),
          userHandle: r.userHandle ? bufToB64u(r.userHandle) : null
        },
        clientExtensionResults: {}
      };
    }).catch(wrap);
  }

  function wrap(err) {
    var m = friendly(err);
    if (m) { var e = new Error(m); e.code = 'webauthn_' + (err.name || 'error'); throw e; }
    throw err;
  }

  // Same-origin JSON call. Non-GET always sends a JSON body. Rejects with Error{code,status,message}.
  function api(method, url, body) {
    var init = { method: method, credentials: 'same-origin', headers: { Accept: 'application/json' }, cache: 'no-store' };
    if (method !== 'GET' && method !== 'HEAD') {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body === undefined ? {} : body);
    }
    return fetch(url, init).then(function (res) {
      if (res.status === 204) return null;
      return res.text().then(function (txt) {
        var data = null;
        try { data = txt ? JSON.parse(txt) : null; } catch (e) { /* non-JSON body */ }
        if (res.ok) return data;
        var err = new Error((data && data.message) || ('Request failed (' + res.status + ')'));
        err.code = (data && data.error) || 'http_' + res.status;
        err.status = res.status;
        throw err;
      });
    }, function () {
      var err = new Error('Could not reach the server. Check your connection.');
      err.code = 'network';
      throw err;
    });
  }

  window.Webauthn = {
    b64uToBuf: b64uToBuf,
    bufToB64u: bufToB64u,
    supported: supported,
    register: register,
    signIn: signIn,
    api: api
  };
})();
