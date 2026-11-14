# certum-sign

The release workflow signs the Windows binaries with the maintainer's Certum code-signing
certificate (SimplySign cloud key) through this action. It is a copy of
[IvanHanloth/super-simply-sign](https://github.com/IvanHanloth/super-simply-sign) (MIT, `LICENSE`)
at commit `5cc1417f169e01640ae1627d5d3d9d687bb237c6`, kept in this repository so that the code that
receives the SimplySign seed changes only through a pull request here, never through a change of
the upstream repository.

What it does, from its source (`src/`):

- It logs in to Certum SimplySign (`https://cloudsign.webnotarius.pl`) with the account e-mail and
  a one-time code made from the TOTP seed, and asks the cloud key to sign each file's hash; the
  files themselves never leave the runner.
- Its HTTP client talks only to the Certum origins it is given and to the timestamp authority
  (`http://time.certum.pl/`); a redirect anywhere else is an error, never followed.
- It masks the e-mail, the seed, every one-time code and the session tokens in the log
  (`core.setSecret`).
- It refuses a file that already carries a signature (`replace-existing-signature` stays off).

The runner executes `dist/index.cjs`, the esbuild bundle of `src/`. To check that the bundle is
the source and nothing else:

    cd .github/actions/certum-sign
    npm ci --ignore-scripts
    cp dist/index.cjs /tmp/index.committed.cjs
    npm run build
    cmp dist/index.cjs /tmp/index.committed.cjs

An update is a new copy of an upstream commit whose source was read the same way, with this file's
commit and the bundle check renewed.
