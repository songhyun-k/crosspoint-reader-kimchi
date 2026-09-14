# Canonical version constants for the .cpfont binary format and font manifest.
#
# These are the single source of truth for the build tooling. The CI workflow
# (release-fonts.yml) and both Python scripts (fontconvert_sdcard.py,
# generate-font-manifest.py) read from here.
#
# The firmware C++ headers (SdCardFont.h, FontDownloadActivity.h) carry their
# own copies — those must be bumped manually when the firmware is updated to
# support a new version.

# .cpfont binary format version. Bump when the on-disk struct layout changes.
CPFONT_VERSION = 4

# JSON manifest schema version. Bump when the manifest shape changes.
FONTS_MANIFEST_VERSION = 1

# kimchi uses one repository for firmware and font assets, but font releases
# must not replace the firmware's GitHub "latest" release (deployment handoff).
FONT_RELEASE_REPOSITORY = "songhyun-k/crosspoint-reader-kimchi"


def font_release_tag() -> str:
    return f"sd-fonts-m{FONTS_MANIFEST_VERSION}-b{CPFONT_VERSION}"


def font_release_base_url() -> str:
    return f"https://github.com/{FONT_RELEASE_REPOSITORY}/releases/download/{font_release_tag()}/"
