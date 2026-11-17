E200_RADIO_PROBE_VERSION = 1.0
E200_RADIO_PROBE_SITE = $(abspath $(BR2_EXTERNAL_MPSOC_IMAGE_BUILDER_PATH)/../../app)
E200_RADIO_PROBE_SITE_METHOD = local
# Individual vendored drivers retain the license headers in radio/lib/.
E200_RADIO_PROBE_LICENSE = ADI-specific BSD
E200_RADIO_PROBE_LICENSE_FILES = radio/lib/no_os/no_os_spi.c
$(eval $(cmake-package))
