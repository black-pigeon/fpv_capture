# The historical package name builds the shared app for either board.
E200_RADIO_PROBE_CONF_OPTS += -DFPV_BOARD=$(call qstrip,$(BR2_FPV_BOARD))
