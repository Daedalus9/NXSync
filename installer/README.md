# NXSync Installer

The installer is a standalone NRO with no network dependency. A release build embeds
the common NXSync components and one patched `dmnt` content override for every exact
supported Atmosphère identity.

It refuses unknown `package3` files, unknown `stratosphere.romfs` files and unknown
`dmnt` overrides. It never rewrites the active `package3` or ROMFS. Installation
requires `ZL + ZR + A`; removal requires `ZL + ZR + X`.

Payload files are generated and ignored by Git. Run the staging script before
building this directory.
