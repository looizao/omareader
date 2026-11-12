# The container CI's jobs run their commands in (see run, next to this
# file): Arch Linux, which is what Omarchy runs, with what building, testing
# and packaging omareader need already installed, so a job starts compiling
# instead of downloading packages. The packages are listed in the file
# packages. .github/workflows/ci-image.yml rebuilds the image when anything
# here changes, and every week to follow Arch.
#
# Every line of packages becomes a layer of its own. Layers are downloaded
# side by side, so a few middling ones are ready in half the time one large
# one takes, and fetching the image is most of what a CI job costs once the
# compiler cache is warm. A fifth line and any after it join the fourth's
# layer.
#
# pacman's downloads and its package lists sit in build caches, outside the
# image, which keeps them out of every layer.

# The base image is up to a week old. Upgrading it in a stage of its own and
# copying the result leaves one layer without the files the upgrade replaced.
FROM archlinux:base AS base
RUN --mount=type=cache,target=/var/cache/pacman/pkg \
    --mount=type=cache,target=/var/lib/pacman/sync \
    pacman -Syu --noconfirm

FROM scratch
COPY --from=base / /
ENV LANG=C.UTF-8
CMD ["/usr/bin/bash"]

RUN --mount=type=cache,target=/var/cache/pacman/pkg \
    --mount=type=cache,target=/var/lib/pacman/sync \
    --mount=type=bind,source=packages,target=/tmp/packages \
    pacman -S --noconfirm --needed $(sed -n 1p /tmp/packages)

RUN --mount=type=cache,target=/var/cache/pacman/pkg \
    --mount=type=cache,target=/var/lib/pacman/sync \
    --mount=type=bind,source=packages,target=/tmp/packages \
    pacman -S --noconfirm --needed $(sed -n 2p /tmp/packages)

RUN --mount=type=cache,target=/var/cache/pacman/pkg \
    --mount=type=cache,target=/var/lib/pacman/sync \
    --mount=type=bind,source=packages,target=/tmp/packages \
    pacman -S --noconfirm --needed $(sed -n 3p /tmp/packages)

# makepkg refuses to run as root, hence the builder user.
RUN --mount=type=cache,target=/var/cache/pacman/pkg \
    --mount=type=cache,target=/var/lib/pacman/sync \
    --mount=type=bind,source=packages,target=/tmp/packages \
    pacman -S --noconfirm --needed $(sed -n '4,$p' /tmp/packages) \
 && useradd -m builder
