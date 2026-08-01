# Security Policy

## Secrets

Never publish real API keys or user configuration. The real `config/api.ini`
and `config/launcher.ini` are local-only files and are ignored by `.gitignore`.

If a key was ever committed, uploaded, pasted into an issue, or shared in a
screenshot, revoke it and create a new key. Removing it from a later commit is
not enough.

## Reporting Issues

When reporting a bug:

- Redact API keys, access tokens, local usernames, and full local paths.
- Do not attach game files, extracted scripts, generated translation caches,
  or copyrighted game dialogue.
- Use synthetic text that reproduces the shape of the issue.

## Local Server

The translation server is designed for local use. Bind it only to localhost
unless you have reviewed the network exposure and authentication model for your
own deployment.

Browser requests are restricted to file/null or explicit loopback origins.
Browser-origin requests cannot call shutdown or cache import/export/dump
routes. Native launcher and engine clients omit the `Origin` header and retain
the existing local API contract.

Remote API endpoints must use HTTPS. Plaintext HTTP is accepted only for exact
loopback hosts (`localhost`, `127.0.0.1`, or `[::1]`) so local compatible
providers remain usable without sending bearer credentials or game text over
the network in cleartext.
