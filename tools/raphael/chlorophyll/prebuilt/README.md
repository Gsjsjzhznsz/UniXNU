Prebuilt Chlorophyll dylibs for the raphael A640, from the upstream Bergamota release
`bgmtl-20261025` (`https://github.com/ry-diffusion/Bergamota/releases/download/bgmtl-20261025/bgmtl-dylibs-20261025.tar.gz`):

- `bgmtl-inject.dylib`, `libbgair.dylib`, `libdrm.2.dylib` (arm64e, MIT/Mesa)

`id_ed25519.pub` is this project's dropbear key (public half only); the private half stays off the
repository. Renegade's `bgmtl()` stages the dylibs into `/usr/local/lib/bgmtl` and links the Metal
device into MercuryPosterExtension; `ssh()` puts this key into `/var/root/.ssh/authorized_keys`.
