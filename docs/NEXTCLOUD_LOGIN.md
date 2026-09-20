# Connect Nextcloud with a QR code

1. Open **Settings > Nextcloud account > Connect with QR** in NXSync.
2. Enter the HTTPS address of your Nextcloud instance, including its subdirectory
   if needed. Leave **Remote folder** set to **`NXSync`** for the default setup.
   This is the folder in your Nextcloud account where cloud backups will be stored.
   NXSync creates it during the first upload and organizes backups inside it by
   console, profile and game. Use the same folder on your other NXSync consoles
   so they can discover each other's backups.
3. Scan the on-screen code with your phone's camera. Check the server address,
   sign in to Nextcloud and grant NXSync access.
4. Leave NXSync open until the connection is saved. It tests the connection next.

Your account password is entered only on the Nextcloud website. Browser-based
two-factor authentication and SSO follow the server's normal login process.
NXSync receives a revocable application password and encrypts it with the same
console-bound storage used by manual setup. An OCS lookup obtains the actual
WebDAV user ID, which may differ from an email address used to sign in.

Press **B** to cancel or **+** to exit. An error or cancellation keeps the previous
configuration. Sessions expire after 20 minutes; **A** creates a new QR code after
an error or expiry. Temporary network/server errors during polling are retried.
If saving fails after authorization, **A** retries the local save without consuming
the single-use authorization response again.

Requirements: a Nextcloud server with Login Flow v2, a certificate trusted by the
console, the correct console date/time, and network access from both devices.
Use the final HTTPS address; redirects and cross-origin login/poll/credential
endpoints are rejected. Custom reverse proxies must expose a consistent origin.
Servers with unusually long login URLs (over 1024 bytes) require manual setup.

**Manual setup** remains in the same account menu for servers whose login flow or
OCS user endpoint is unavailable. Never disable TLS verification as a workaround.
The QR is generated locally from the short-lived login URL; no external QR service
receives it. The polling token and application password are not displayed or logged.
Do not share a live login QR. Remove unused grants in Nextcloud's **Personal
settings > Security > Devices & sessions**, including grants approved on the phone
after you cancelled NXSync or if the local save failed.

Implementation reference: [Nextcloud Login Flow v2 and user ID lookup](https://docs.nextcloud.com/server/stable/developer_manual/client_apis/LoginFlow/index.html).
QR generation: [Project Nayuki, MIT license](https://www.nayuki.io/page/qr-code-generator-library).

Host tests cover synthetic HTTPS authorization, pending responses, retries, expiry,
cancellation, malformed replies, TLS rejection and endpoint validation. Console
and real-account testing remain required before promoting this candidate.
