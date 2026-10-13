/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 StarFive Technology Co., Ltd.
 *
 * Author: Jun Liang Tan <junliang.tan@linux.starfivetech.com>
 */

#include <sbi/sbi_domain.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi_utils/serial/cadence-uart.h>

/* clang-format off */

#define UART_REG_CTRL		0x00
#define UART_REG_MODE		0x04
#define UART_REG_IDR		0x0C
#define UART_REG_BRGR		0x18
#define UART_REG_CSR		0x2C
#define UART_REG_RFIFO_TFIFO	0x30
#define UART_REG_BDIVR		0x34

#define UART_CTRL_RXRES		0x00000001
#define UART_CTRL_TXRES		0x00000002
#define UART_CTRL_RXEN		0x00000004
#define UART_CTRL_RXDIS		0x00000008
#define UART_CTRL_TXEN		0x00000010
#define UART_CTRL_TXDIS		0x00000020

#define UART_MODE_CLKSEL	0x00000001	/* sel_clk = uart_clk / 8 */
#define UART_MODE_PAR_NONE	0x00000020	/* No parity set */

#define UART_BDIV_MIN		4
#define UART_BDIV_MAX		255
#define UART_CD_MAX		65535

#define	UART_CSR_REMPTY		0x00000002
#define	UART_CSR_TFUL		0x00000010

/* clang-format on */

static volatile void *uart_base;
static u32 uart_in_freq;
static u32 uart_baudrate;

/*
 * Find the CD (BRGR) / BDIV (BDIVR) pair giving the baud rate closest to
 * the requested one: baud = sel_clk / (CD * (BDIV + 1)).
 * Based on cdns_uart_calc_baud_divs() from Linux xilinx_uartps.c.
 * Returns the achieved baud rate, or 0 if no valid divisors exist.
 */
static u32 cadence_uart_calc_baud_divs(u32 clk, u32 baud, u32 *rbdiv,
				       u32 *rcd, bool *div8)
{
	u32 cd, bdiv, den, calc_baud, err;
	u32 best_baud = 0, best_err = ~0U;

	*div8 = false;
	if (baud < clk / ((UART_BDIV_MAX + 1) * UART_CD_MAX)) {
		*div8 = true;
		clk /= 8;
	}

	for (bdiv = UART_BDIV_MIN; bdiv <= UART_BDIV_MAX; bdiv++) {
		den = baud * (bdiv + 1);
		cd = (clk + den / 2) / den;
		if (cd < 1 || cd > UART_CD_MAX)
			continue;

		calc_baud = clk / (cd * (bdiv + 1));
		err = calc_baud > baud ? calc_baud - baud : baud - calc_baud;
		if (err < best_err) {
			*rbdiv = bdiv;
			*rcd = cd;
			best_baud = calc_baud;
			best_err = err;
		}
	}

	return best_baud;
}

static u32 get_reg(u32 offset)
{
	return readl(uart_base + offset);
}

static void set_reg(u32 offset, u32 val)
{
	writel(val, uart_base + offset);
}

static void cadence_uart_putc(char ch)
{
	while (get_reg(UART_REG_CSR) & UART_CSR_TFUL)
		;

	set_reg(UART_REG_RFIFO_TFIFO, ch);
}

static int cadence_uart_getc(void)
{
	u32 ret = get_reg(UART_REG_CSR);

	if (!(ret & UART_CSR_REMPTY))
		return get_reg(UART_REG_RFIFO_TFIFO);

	return -1;
}

static struct sbi_console_device cadence_console = {
	.name = "cadence_uart",
	.console_putc = cadence_uart_putc,
	.console_getc = cadence_uart_getc
};

int cadence_uart_init(unsigned long base, u32 in_freq, u32 baudrate)
{
	u32 mode = UART_MODE_PAR_NONE;
	u32 cd, bdiv;
	bool div8;

	uart_base     = (volatile void *)base;
	uart_in_freq  = in_freq;
	uart_baudrate = baudrate;

	/* Disable interrupts */
	set_reg(UART_REG_IDR, 0xFFFFFFFF);

	/* Disable TX RX */
	set_reg(UART_REG_CTRL, UART_CTRL_TXDIS | UART_CTRL_RXDIS);

	/* Configure baudrate */
	if (in_freq && baudrate &&
	    cadence_uart_calc_baud_divs(in_freq, baudrate, &bdiv, &cd, &div8)) {
		if (div8)
			mode |= UART_MODE_CLKSEL;
		set_reg(UART_REG_MODE, mode);
		set_reg(UART_REG_BRGR, cd);
		set_reg(UART_REG_BDIVR, bdiv);
	}

	/* Software reset TX RX data path and enable TX RX */
	set_reg(UART_REG_CTRL, UART_CTRL_TXRES | UART_CTRL_RXRES
		| UART_CTRL_TXEN | UART_CTRL_RXEN);

	/*
	 * Set:
	 * 1 stop bit, bits[07:06] = 0x00,
	 * no parity set, bits[05:03] = 0x100,
	 * 8 bits character length, bits[02:01] = 0x00,
	 * sel_clk = uart_clk or uart_clk / 8, bit[0] = CLKSEL
	 */
	set_reg(UART_REG_MODE, mode);

	sbi_console_set_device(&cadence_console);

	return sbi_domain_root_add_memrange(base, PAGE_SIZE, PAGE_SIZE,
					    (SBI_DOMAIN_MEMREGION_MMIO |
					    SBI_DOMAIN_MEMREGION_SHARED_SURW_MRW));
}
