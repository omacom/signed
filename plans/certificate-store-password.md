# Password-protected certificate store

Status: accepted.

The certificate store's private keys can be protected with a password. The
feature is off by default, so existing installs keep today's behaviour until
the user turns it on.

## Turning it on

Add this to `~/.config/signed/signed.conf` and restart Signed:

```ini
[certificateStore]
passwordProtected=true
```

The setting is read once at startup. Turning it off later does not make an
already-protected store readable.

## Behaviour

- **New store, setting on:** making the first certificate asks for a password
  and a confirmation. The password protects the whole store.
- **Every sealed save:** signing asks for the store password once. One entry
  covers every certificate used by that save.
- **Wrong password:** the save does not happen. The user can try again or
  cancel.
- **Existing protected store:** it keeps asking for its password even if the
  setting is later turned off.
- **Existing unprotected store:** it stays unprotected. The setting only
  applies to stores created with it.
- **Store listing:** saved certificates can be seen and selected while the
  store is locked. Only an operation that uses a private key asks for the
  password.
- **No password store:** with the setting off, stores are created without a
  password and sealing never prompts, as before.

## Importing a .p12

The file's own password is separate from the store password. Importing into a
new protected store asks for the new store password and confirmation; importing
into an existing protected store asks for that store password once. The `.p12`
password is still asked only for the import and is never stored.

## Security

The store password encrypts the private keys at rest. It is not saved in
settings or passed on a command line, and the store is locked again after each
signing pass. Public certificate names and the rest of the certificate database
are not encrypted.

There is no recovery. Losing the password means the certificates in that store
can no longer sign anything. PDFs that were already sealed remain valid.
