clean::
		$(RM) $(CLEANSUFFIXES:%=libavcodec/aac/%)

OBJS-$(CONFIG_AAC_DECODER)          +=  aac/aacdec.o aac/aacdec_tab.o \
                                        aac/aacdec_float.o \
                                        aac/aacdec_ac.o aac/aacdec_lpd.o \
                                        $(if $(!CONFIG_LIBFDK_AAC_DECODER), aac/aacdec_usac.o)
OBJS-$(CONFIG_AAC_FIXED_DECODER)    +=  aac/aacdec.o aac/aacdec_tab.o \
                                        aac/aacdec_fixed.o
