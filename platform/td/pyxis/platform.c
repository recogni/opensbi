// SPDX-License-Identifier: BSD-2-Clause
/*
 *
 * Copyright 2024 Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors:
 *   Xiang Xu <xuxiang@eswincomputing.com>
 *   Min Lin <linmin@eswincomputing.com>
 *   Yu Ning <ningyu@eswincomputing.com>
 *   Yifeng Huang <huangyifeng@eswincomputing.com>
 *
 */

#include <libfdt.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_const.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>
#include <sbi/riscv_io.h>
#include <sbi_utils/irqchip/plic.h>
#include <sbi_utils/serial/uart8250.h>
#include <sbi_utils/timer/aclint_mtimer.h>
#include <sbi_utils/ipi/aclint_mswi.h>
#include <sbi_utils/fdt/fdt_pmu.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi/riscv_asm.h>
#include <sbi_utils/fdt/fdt_fixup.h>
#include <sbi/sbi_hart.h>
#include "eic770x_uart.h"

/* clang-format off */
#define EIC770X_HART_COUNT				4
#define DIE_REG_OFFSET					0

#define EIC770X_ACLINT_MSWI_ADDR			0x2000000
#define EIC770X_ACLINT_MTIMER_ADDR			0x2000000
#define EIC770X_ACLINT_MTIMER_FREQ			1000000

#define EIC770X_PLIC_ADDR				0xc000000
#define EIC770X_PLIC_SIZE				(0x200000 + \
		(EIC770X_HART_COUNT * 0x1000))
#define EIC770X_PLIC_NUM_SOURCES			520
#define EIC770X_PLIC_NUM_PRIORITIES			7

#define EIC770X_UART_BAUDRATE				115200
#define EIC770X_UART0_ADDR				(0x50900000UL + DIE_REG_OFFSET)
#define EIC770X_UART2_ADDR				(0x50920000UL + DIE_REG_OFFSET)
#define EIC770X_UART_RESET_ADDR			(0x51828434UL + DIE_REG_OFFSET)

#define EIC770X_UART_CLK				(200000000UL)

/* Full tlb flush always */
#define EIC770X_TLB_RANGE_FLUSH_LIMIT			0

/* system reset register */
#define EIC770X_SYS_RESET_ADDR				0x51828300UL
#define EIC770X_SYS_RESET_VALUE			0x1ac0ffe6

/* clang-format on */

#define UART_DLF_OFFSET				48	/* I/O: Divisor Latch Fraction Register */

/*
 * Divide positive or negative dividend by positive or negative divisor
 * and round to closest integer. Result is undefined for negative
 * divisors if the dividend variable type is unsigned and for negative
 * dividends if the divisor variable type is unsigned.
 */
#define DIV_ROUND_CLOSEST(x, divisor)(  \
{   \
    typeof(x) __x = x;  \
    typeof(divisor) __d = divisor;  \
    (((typeof(x))-1) > 0 || \
     ((typeof(divisor))-1) > 0 ||   \
     (((__x) > 0) == ((__d) > 0))) ?    \
     (((__x) + ((__d) / 2)) / (__d)) :  \
     (((__x) - ((__d) / 2)) / (__d));   \
}   \
)

static struct plic_data plic = {
	.addr = EIC770X_PLIC_ADDR,
	.size =	EIC770X_PLIC_SIZE,
	.num_src = EIC770X_PLIC_NUM_SOURCES,
};

static struct aclint_mswi_data mswi = {
	.addr = EIC770X_ACLINT_MSWI_ADDR,
	.size = ACLINT_MSWI_SIZE,
	.first_hartid = 0,
	.hart_count = EIC770X_HART_COUNT,
};

static struct aclint_mtimer_data mtimer = {
	.mtime_freq = EIC770X_ACLINT_MTIMER_FREQ,
	.mtime_addr = EIC770X_ACLINT_MTIMER_ADDR +
		      0xbff8,
	.mtime_size = ACLINT_DEFAULT_MTIME_SIZE,
	.mtimecmp_addr = EIC770X_ACLINT_MTIMER_ADDR +
			 0x4000,
	.mtimecmp_size = ACLINT_DEFAULT_MTIMECMP_SIZE,
	.first_hartid = 0,
	.hart_count = EIC770X_HART_COUNT,
	.has_64bit_mmio = false,
};

static u32 eic770x_hart_index2id[EIC770X_HART_COUNT] = {
	[0] = 0,
	[1] = 1,
	[2] = 2,
	[3] = 3,
};

static void eic770x_modify_dt(void *fdt)
{
	fdt_cpu_fixup(fdt);

	fdt_fixups(fdt);
}

static int eic770x_system_reset_check(u32 type, u32 reason)
{
	switch (type) {
	case SBI_SRST_RESET_TYPE_SHUTDOWN:
	case SBI_SRST_RESET_TYPE_COLD_REBOOT:
	case SBI_SRST_RESET_TYPE_WARM_REBOOT:
	        return 1;
	}

	return 0;
}

/* tell stm32 on the carrier to shut down the power */
static int eic770x_core_shutdown(void)
{
	Message shutdown_reply = {
		.header = FRAME_HEADER,
		.msg_type = MSG_NOTIFY,
		.cmd_type = CMD_POWER_OFF,
		.data_len = 0x0,
		.tail = FRAME_TAIL,
	};
	sbi_printf("%s\n", __func__);
	transmit_message(&shutdown_reply);
	return 0;
}

static int eic770x_cold_reset(void)
{
	Message shutdown_reply = {
		.header = FRAME_HEADER,
		.msg_type = MSG_NOTIFY,
		.cmd_type = CMD_RESTART,
		.data_len = 0x0,
		.tail = FRAME_TAIL,
	};
	sbi_printf("%s\n", __func__);
	transmit_message(&shutdown_reply);
	sbi_timer_mdelay(3000);
	/*When it is not a DVB board, reboot can still be done, but there is no real power off/power on action at that time.*/
	writel(EIC770X_SYS_RESET_VALUE, (volatile void *)EIC770X_SYS_RESET_ADDR);
	return 0;
}

static int eic770x_core_reset(void)
{
	sbi_printf("%s\n", __func__);
	writel(EIC770X_SYS_RESET_VALUE, (volatile void *)EIC770X_SYS_RESET_ADDR);
	return 0;
}

static void eic770x_system_reset(u32 type, u32 reason)
{
	switch (type) {
		case SBI_SRST_RESET_TYPE_SHUTDOWN:
			eic770x_core_shutdown();
			break;
		case SBI_SRST_RESET_TYPE_COLD_REBOOT:
			eic770x_cold_reset();
			break;
		case SBI_SRST_RESET_TYPE_WARM_REBOOT:
			eic770x_core_reset();
			break;
	}

	sbi_hart_hang();
}

static struct sbi_system_reset_device eic770x_reset = {
	.name = "eswin_eic770x_reset",
	.system_reset_check = eic770x_system_reset_check,
	.system_reset = eic770x_system_reset
};

/* UART2 is used for communication with stm32 on the carrier board of DVB */
int eic770x_uart2_init()
{
	/*reset uart2*/
	writeb(0x1B, (volatile void *)EIC770X_UART_RESET_ADDR);
	writeb(0x1F, (volatile void *)EIC770X_UART_RESET_ADDR);
	return eic770x_uart8250_init(EIC770X_UART2_ADDR,
			EIC770X_UART_CLK,
			EIC770X_UART_BAUDRATE,
			0x2,
			0x2);

}

static int eic770x_early_init(bool cold_boot)
{
	int ret;
	u32 bdiv_f, base_baud;

	if (cold_boot)
		sbi_system_reset_add_device(&eic770x_reset);

	ret =  uart8250_init(EIC770X_UART0_ADDR,
			EIC770X_UART_CLK,
			EIC770X_UART_BAUDRATE,
			0x2,
			0x2,0);
	if (ret<0)
		return ret;

	base_baud = EIC770X_UART_BAUDRATE * 16;
	bdiv_f = EIC770X_UART_CLK % base_baud;
	bdiv_f = DIV_ROUND_CLOSEST(bdiv_f << 0x4, base_baud);

	writew(bdiv_f, ((volatile char *)EIC770X_UART0_ADDR + (UART_DLF_OFFSET << 2)));

	return 0;
}

static int eic770x_final_init(bool cold_boot)
{
	void *fdt;

	if (!cold_boot)
		return 0;

	fdt = sbi_scratch_thishart_arg1_ptr();
	eic770x_modify_dt(fdt);

	return 0;
}

static int eic770x_irqchip_init(void)
{
	return plic_cold_irqchip_init(&plic);
}

static int eic770x_ipi_init(void)
{
	return aclint_mswi_cold_init(&mswi);
}

static u64 eic770x_get_tlbr_flush_limit(void)
{
	return EIC770X_TLB_RANGE_FLUSH_LIMIT;
}

static int eic770x_timer_init(void)
{
	return aclint_mtimer_cold_init(&mtimer, NULL);
}

static int generic_pmu_init(void)
{
	return fdt_pmu_setup(fdt_get_address());
}

static uint64_t generic_pmu_xlate_to_mhpmevent(uint32_t event_idx,
					       uint64_t data)
{
	uint64_t evt_val = 0;

	/* data is valid only for raw events and is equal to event selector */
	if (event_idx == SBI_PMU_EVENT_RAW_IDX)
		evt_val = data;
	else {
		/**
		 * Generic platform follows the SBI specification recommendation
		 * i.e. zero extended event_idx is used as mhpmevent value for
		 * hardware general/cache events if platform does't define one.
		 */
		evt_val = fdt_pmu_get_select_value(event_idx);
		if (!evt_val)
			evt_val = (uint64_t)event_idx;
	}

	return evt_val;
}

const struct sbi_platform_operations platform_ops = {
	.early_init		= eic770x_early_init,
	.final_init		= eic770x_final_init,
	.irqchip_init		= eic770x_irqchip_init,
	.ipi_init		= eic770x_ipi_init,
	.get_tlbr_flush_limit	= eic770x_get_tlbr_flush_limit,
	.timer_init		= eic770x_timer_init,
	.pmu_init		= generic_pmu_init,
	.pmu_xlate_to_mhpmevent = generic_pmu_xlate_to_mhpmevent,
};

const struct sbi_platform platform = {
	.opensbi_version	= OPENSBI_VERSION,
	.platform_version	=
		SBI_PLATFORM_VERSION(CONFIG_PLATFORM_GENERIC_MAJOR_VER,
				CONFIG_PLATFORM_GENERIC_MINOR_VER),
	.name			= "TENSORDYNE PYXIS",
	.features		= 0,
	.hart_count		= EIC770X_HART_COUNT,
	.hart_index2id		= eic770x_hart_index2id,
	.hart_stack_size	= SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
	.heap_size =
		SBI_PLATFORM_DEFAULT_HEAP_SIZE(EIC770X_HART_COUNT),
	.platform_ops_addr	= (unsigned long)&platform_ops
};
