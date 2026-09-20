# Application icons

| Application | Original | Embedded icon |
| --- | --- | --- |
| NXSync | `nxsync.png` (cloud with upload arrow) | `nxsync.jpg` |
| NXSync Installer | `installer.png` (download arrow) | `installer.jpg` |

The PNGs are the original 500 x 500 images supplied by the project maintainer.
The JPEGs are 256 x 256 RGB baseline exports at quality 95 for the homebrew menu.
They preserve the original composition and background colors.

Both Makefiles embed the corresponding JPEG directly and track it as a build
dependency. To update an icon, retain its PNG source and replace its JPEG export.
