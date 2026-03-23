#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright 2024 Beijing ESWIN Computing Technology Co., Ltd.
#
# Authors:
#	Xiang Xu <xuxiang@eswincomputing.com>
#	Yifeng Huang <huangyifeng@eswincomputing.com>
#

platform-objs-$(CONFIG_PLATFORM_TENSORDYNE) += platform.o
#platform-objs-y += eic770x_uart.o

# Compiler flags
platform-cppflags-y =
platform-cflags-y =
platform-asflags-y =
platform-ldflags-y = -fno-stack-protector

# Command for platform specific "make run"

# Blobs to build
FW_DYNAMIC=y
FW_JUMP=y
FW_PAYLOAD=y

FW_TEXT_START=0x80000000
# This needs to be 2MB aligned for 64-bit system
FW_JUMP_ADDR=0x80200000
FW_JUMP_FDT_ADDR=0x00000000
FW_PAYLOAD_OFFSET=0x200000
FW_PAYLOAD_FDT_ADDR=0x00000000
