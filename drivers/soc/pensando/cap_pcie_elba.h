/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021, Pensando Systems Inc.
 */

#ifndef __CAP_PCIE_ELBA_H__
#define __CAP_PCIE_ELBA_H__

#define PCIEPORT_NPORTS		8

#define ELB_ADDR_BASE_PP_PXC_0_OFFSET 0x20100000
#define ELB_ADDR_BASE_PP_PXC_0_SIZE 0x40000
#define ELB_ADDR_BASE_PP_PP_0_OFFSET 0x20300000
#define ELB_ADDR_BASE_PP_PP_0_SIZE 0x40000

#define ELB_PXC_CSR_CFG_C_PORT_MAC_BYTE_ADDRESS 0x20f8
#define ELB_PXC_CSR_CFG_C_PORT_MAC_CFG_C_PORT_MAC_0_2_LTSSM_EN_FIELD_MASK 0x00000002
#define ELB_PXC_CSR_CFG_C_PORT_MAC_CFG_C_PORT_MAC_0_2_CFG_RETRY_EN_FIELD_MASK 0x00000008

#define ELB_PXC_CSR_INT_C_MAC_INTREG_BYTE_ADDRESS 0x2220
#define ELB_PXC_CSR_INT_C_MAC_INTREG_RST_DN2UP_INTERRUPT_FIELD_MASK 0x00000010

#define _PP_BASE(pn) \
	(ELB_ADDR_BASE_PP_PP_0_OFFSET + \
	(((pn) >> 2) * ELB_ADDR_BASE_PP_PP_0_SIZE))

#define PP_(REG, pn) \
	(_PP_BASE(pn) + ELB_PP_CSR_ ##REG## _BYTE_ADDRESS)

#define _PXC_BASE(pn) \
	(ELB_ADDR_BASE_PP_PXC_0_OFFSET + \
	((pn) * ELB_ADDR_BASE_PP_PXC_0_SIZE))

#define PXC_(REG, pn) \
	(_PXC_BASE(pn) + ELB_PXC_CSR_ ##REG## _BYTE_ADDRESS)

#define CFG_MACF_(REG) \
	(ELB_PXC_CSR_CFG_C_PORT_MAC_CFG_C_PORT_MAC_ ##REG## _FIELD_MASK)
#define MAC_INTREGF_(REG) \
	(ELB_PXC_CSR_INT_C_MAC_INTREG_ ##REG## _INTERRUPT_FIELD_MASK)

#define ELB_SOC_CSR_CFG_WDT_RST_EN_LSB 0
#define CFG_WDT_RST_EN  ELB_SOC_CSR_CFG_WDT_RST_EN_LSB

#define WDT_CR          0x00
#define WDT_TORR        0x01
#define WDT_CRR         0x03

#define WDT_CR_ENABLE   0x1
#define WDT_CR_PCLK_256 (0x7 << 2)

#define WDT_KICK_VAL    0x76

#endif /* __CAP_PCIE_ELBA_H__ */
