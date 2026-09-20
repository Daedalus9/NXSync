# Security policy

NXSync handles save data and a Nextcloud app password. Treat both as sensitive.

## Reporting

Once the repository is public, report vulnerabilities through a private GitHub
Security Advisory rather than a public issue. Do not attach a real `config.ini`,
save archive, `_index` directory, profile UID, device ID or crash dump without first
removing personal data.

## Credential handling

- Use a dedicated Nextcloud app password, not the account's primary password.
- Revoke the app password immediately if it is exposed.
- Never include runtime configuration in release packages.
- HTTPS certificate and hostname verification must remain enabled.

## Restore safety

Archive and metadata validation is a security boundary. Changes that relax path,
size, CRC, title, manifest or SHA-256 checks require explicit review and adversarial
tests.

This development project does not currently promise a fixed security-support window.

