GCOV_PROFILE := y
EXTRA_CFLAGS += -Idrivers/staging/tegra/video/host
EXTRA_CFLAGS += -Idrivers/staging/tegra/include

# subdir-ccflags-y := -Werror
EXTRA_CFLAGS += -Idrivers/video/tegra/host
obj-$(CONFIG_TEGRA_GRHOST) += host/
obj-$(CONFIG_TEGRA_DC) += dc/
obj-$(CONFIG_FB_TEGRA) += fb.o
obj-$(CONFIG_TEGRA_NVMAP) += nvmap/
obj-$(CONFIG_FB_TEGRA_CMC623) += cmc623.o
