# SPDX-License-Identifier: GPL-2.0
ifeq ($(CONFIG_SOC_S5E8825_OVERCLOCK),y)
dtb-$(CONFIG_ARCH_EXYNOS) += s5e8825_oc.dtb
else
dtb-$(CONFIG_ARCH_EXYNOS) += s5e8825.dtb
endif

ifeq ($(CONFIG_ARCH_EXYNOS),y)
    dtbo-y += samsung/a53x/a53x_eur_open_w00_r00.dtbo
    dtbo-y += samsung/a53x/a53x_eur_open_w00_r01.dtbo
    dtbo-y += samsung/a53x/a53x_eur_open_w00_r02.dtbo
    dtbo-y += samsung/a53x/a53x_eur_open_w00_r05.dtbo
    dtbo-y += samsung/a53x/a53x_eur_open_w00_r06.dtbo
endif

targets += dtbs
DTB_LIST  := $(dtb-y) $(dtbo-y)
always-y    := $(DTB_LIST)

dtbs: $(addprefix $(obj)/, $(DTB_LIST))

clean-files := *.dtb*
