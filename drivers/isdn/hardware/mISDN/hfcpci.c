/*
 *
 * hfcpci.c     low level driver for CCD's hfc-pci based cards
 *
 * Author     Werner Cornelius (werner@isdn4linux.de)
 *            based on existing driver for CCD hfc ISA cards
 *            type approval valid for HFC-S PCI A based card
 *
 * Copyright 1999  by Werner Cornelius (werner@isdn-development.de)
 * Copyright 2008  by Karsten Keil <kkeil@novell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Module options:
 *
 * debug:
 *	NOTE: only one poll value must be given for all cards
 *	See hfc_pci.h for debug flags.
 *
 * poll:
 *	NOTE: only one poll value must be given for all cards
 *	Give the number of samples for each fifo process.
 *	By default 128 is used. Decrease to reduce delay, increase to
 *	reduce cpu load. If unsure, don't mess with it!
 *	A value of 128 will use controller's interrupt. Other values will
 *	use kernel timer, because the controller will not allow lower values
 *	than 128.
 *	Also note that the value depends on the kernel timer frequency.
 *	If kernel uses a frequency of 1000 Hz, steps of 8 samples are possible.
 *	If the kernel uses 100 Hz, steps of 80 samples are possible.
 *	If the kernel uses 300 Hz, steps of about 26 samples are possible.
 *
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mISDNhw.h>
#include <linux/slab.h>

#include "hfc_pci.h"

static const char *hfcpci_revision = "2.0";

static int HFC_cnt;
static uint debug;
static uint poll, tics;
static struct timer_list hfc_tl;
static unsigned long hfc_jiffies;

MODULE_AUTHOR("Karsten Keil");
MODULE_LICENSE("GPL");
module_param(debug, uint, S_IRUGO | S_IWUSR);
module_param(poll, uint, S_IRUGO | S_IWUSR);

enum {
	HFC_CCD_2BD0,
	HFC_CCD_B000,
	HFC_CCD_B006,
	HFC_CCD_B007,
	HFC_CCD_B008,
	HFC_CCD_B009,
	HFC_CCD_B00A,
	HFC_CCD_B00B,
	HFC_CCD_B00C,
	HFC_CCD_B100,
	HFC_CCD_B700,
	HFC_CCD_B701,
	HFC_ASUS_0675,
	HFC_BERKOM_A1T,
	HFC_BERKOM_TCONCEPT,
	HFC_ANIGMA_MC145575,
	HFC_ZOLTRIX_2BD0,
	HFC_DIGI_DF_M_IOM2_E,
	HFC_DIGI_DF_M_E,
	HFC_DIGI_DF_M_IOM2_A,
	HFC_DIGI_DF_M_A,
	HFC_ABOCOM_2BD1,
	HFC_SITECOM_DC105V2,
};

struct hfcPCI_hw {
	unsigned char		cirm;
	unsigned char		ctmt;
	unsigned char		clkdel;
	unsigned char		states;
	unsigned char		conn;
	unsigned char		mst_m;
	unsigned char		int_m1;
	unsigned char		int_m2;
	unsigned char		sctrl;
	unsigned char		sctrl_r;
	unsigned char		sctrl_e;
	unsigned char		trm;
	unsigned char		fifo_en;
	unsigned char		bswapped;
	unsigned char		protocol;
	int			nt_timer;
	unsigned char __iomem	*pci_io; 
	dma_addr_t		dmahandle;
	void			*fifos; 
	int			last_bfifo_cnt[2];
	
	struct timer_list	timer;
};

#define	HFC_CFG_MASTER		1
#define HFC_CFG_SLAVE		2
#define	HFC_CFG_PCM		3
#define HFC_CFG_2HFC		4
#define HFC_CFG_SLAVEHFC	5
#define HFC_CFG_NEG_F0		6
#define HFC_CFG_SW_DD_DU	7

#define FLG_HFC_TIMER_T1	16
#define FLG_HFC_TIMER_T3	17

#define NT_T1_COUNT	1120	
#define NT_T3_COUNT	31	
#define CLKDEL_TE	0x0e	
#define CLKDEL_NT	0x6c	


struct hfc_pci {
	u_char			subtype;
	u_char			chanlimit;
	u_char			initdone;
	u_long			cfg;
	u_int			irq;
	u_int			irqcnt;
	struct pci_dev		*pdev;
	struct hfcPCI_hw	hw;
	spinlock_t		lock;	
	struct dchannel		dch;
	struct bchannel		bch[2];
};

static void
enable_hwirq(struct hfc_pci *hc)
{
	hc->hw.int_m2 |= HFCPCI_IRQ_ENABLE;
	Write_hfc(hc, HFCPCI_INT_M2, hc->hw.int_m2);
}

static void
disable_hwirq(struct hfc_pci *hc)
{
	hc->hw.int_m2 &= ~((u_char)HFCPCI_IRQ_ENABLE);
	Write_hfc(hc, HFCPCI_INT_M2, hc->hw.int_m2);
}

static void
release_io_hfcpci(struct hfc_pci *hc)
{
	
	pci_write_config_word(hc->pdev, PCI_COMMAND, 0);
	del_timer(&hc->hw.timer);
	pci_free_consistent(hc->pdev, 0x8000, hc->hw.fifos, hc->hw.dmahandle);
	iounmap(hc->hw.pci_io);
}

static void
hfcpci_setmode(struct hfc_pci *hc)
{
	if (hc->hw.protocol == ISDN_P_NT_S0) {
		hc->hw.clkdel = CLKDEL_NT;	
		hc->hw.sctrl |= SCTRL_MODE_NT;	
		hc->hw.states = 1;		
	} else {
		hc->hw.clkdel = CLKDEL_TE;	
		hc->hw.sctrl &= ~SCTRL_MODE_NT;	
		hc->hw.states = 2;		
	}
	Write_hfc(hc, HFCPCI_CLKDEL, hc->hw.clkdel);
	Write_hfc(hc, HFCPCI_STATES, HFCPCI_LOAD_STATE | hc->hw.states);
	udelay(10);
	Write_hfc(hc, HFCPCI_STATES, hc->hw.states | 0x40); 
	Write_hfc(hc, HFCPCI_SCTRL, hc->hw.sctrl);
}

static void
reset_hfcpci(struct hfc_pci *hc)
{
	u_char	val;
	int	cnt = 0;

	printk(KERN_DEBUG "reset_hfcpci: entered\n");
	val = Read_hfc(hc, HFCPCI_CHIP_ID);
	printk(KERN_INFO "HFC_PCI: resetting HFC ChipId(%x)\n", val);
	
	pci_write_config_word(hc->pdev, PCI_COMMAND, PCI_ENA_MEMIO);
	disable_hwirq(hc);
	
	pci_write_config_word(hc->pdev, PCI_COMMAND,
			      PCI_ENA_MEMIO + PCI_ENA_MASTER);
	val = Read_hfc(hc, HFCPCI_STATUS);
	printk(KERN_DEBUG "HFC-PCI status(%x) before reset\n", val);
	hc->hw.cirm = HFCPCI_RESET;	
	Write_hfc(hc, HFCPCI_CIRM, hc->hw.cirm);
	set_current_state(TASK_UNINTERRUPTIBLE);
	mdelay(10);			
	hc->hw.cirm = 0;		
	Write_hfc(hc, HFCPCI_CIRM, hc->hw.cirm);
	val = Read_hfc(hc, HFCPCI_STATUS);
	printk(KERN_DEBUG "HFC-PCI status(%x) after reset\n", val);
	while (cnt < 50000) { 
		udelay(5);
		cnt += 5;
		val = Read_hfc(hc, HFCPCI_STATUS);
		if (!(val & 2))
			break;
	}
	printk(KERN_DEBUG "HFC-PCI status(%x) after %dus\n", val, cnt);

	hc->hw.fifo_en = 0x30;	

	hc->hw.bswapped = 0;	
	hc->hw.ctmt = HFCPCI_TIM3_125 | HFCPCI_AUTO_TIMER;
	hc->hw.trm = HFCPCI_BTRANS_THRESMASK; 
	hc->hw.sctrl = 0x40;	
	hc->hw.sctrl_r = 0;
	hc->hw.sctrl_e = HFCPCI_AUTO_AWAKE;	
	hc->hw.mst_m = 0;
	if (test_bit(HFC_CFG_MASTER, &hc->cfg))
		hc->hw.mst_m |= HFCPCI_MASTER;	
	if (test_bit(HFC_CFG_NEG_F0, &hc->cfg))
		hc->hw.mst_m |= HFCPCI_F0_NEGATIV;
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	Write_hfc(hc, HFCPCI_TRM, hc->hw.trm);
	Write_hfc(hc, HFCPCI_SCTRL_E, hc->hw.sctrl_e);
	Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt);

	hc->hw.int_m1 = HFCPCI_INTS_DTRANS | HFCPCI_INTS_DREC |
		HFCPCI_INTS_L1STATE | HFCPCI_INTS_TIMER;
	Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);

	
	val = Read_hfc(hc, HFCPCI_INT_S1);

	
	hfcpci_setmode(hc);

	Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
	Write_hfc(hc, HFCPCI_SCTRL_R, hc->hw.sctrl_r);

	if (test_bit(HFC_CFG_PCM, &hc->cfg)) {
		
		hc->hw.conn = 0x09;
	} else {
		hc->hw.conn = 0x36;	
		if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg)) {
			Write_hfc(hc, HFCPCI_B1_SSL, 0xC0);
			Write_hfc(hc, HFCPCI_B2_SSL, 0xC1);
			Write_hfc(hc, HFCPCI_B1_RSL, 0xC0);
			Write_hfc(hc, HFCPCI_B2_RSL, 0xC1);
		} else {
			Write_hfc(hc, HFCPCI_B1_SSL, 0x80);
			Write_hfc(hc, HFCPCI_B2_SSL, 0x81);
			Write_hfc(hc, HFCPCI_B1_RSL, 0x80);
			Write_hfc(hc, HFCPCI_B2_RSL, 0x81);
		}
	}
	Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
	val = Read_hfc(hc, HFCPCI_INT_S2);
}

static void
hfcpci_Timer(struct hfc_pci *hc)
{
	hc->hw.timer.expires = jiffies + 75;
	
}


static struct bchannel *
Sel_BCS(struct hfc_pci *hc, int channel)
{
	if (test_bit(FLG_ACTIVE, &hc->bch[0].Flags) &&
	    (hc->bch[0].nr & channel))
		return &hc->bch[0];
	else if (test_bit(FLG_ACTIVE, &hc->bch[1].Flags) &&
		 (hc->bch[1].nr & channel))
		return &hc->bch[1];
	else
		return NULL;
}

static void
hfcpci_clear_fifo_rx(struct hfc_pci *hc, int fifo)
{
	u_char		fifo_state;
	struct bzfifo	*bzr;

	if (fifo) {
		bzr = &((union fifo_area *)(hc->hw.fifos))->b_chans.rxbz_b2;
		fifo_state = hc->hw.fifo_en & HFCPCI_FIFOEN_B2RX;
	} else {
		bzr = &((union fifo_area *)(hc->hw.fifos))->b_chans.rxbz_b1;
		fifo_state = hc->hw.fifo_en & HFCPCI_FIFOEN_B1RX;
	}
	if (fifo_state)
		hc->hw.fifo_en ^= fifo_state;
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	hc->hw.last_bfifo_cnt[fifo] = 0;
	bzr->f1 = MAX_B_FRAMES;
	bzr->f2 = bzr->f1;	
	bzr->za[MAX_B_FRAMES].z1 = cpu_to_le16(B_FIFO_SIZE + B_SUB_VAL - 1);
	bzr->za[MAX_B_FRAMES].z2 = cpu_to_le16(
		le16_to_cpu(bzr->za[MAX_B_FRAMES].z1));
	if (fifo_state)
		hc->hw.fifo_en |= fifo_state;
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
}

static void hfcpci_clear_fifo_tx(struct hfc_pci *hc, int fifo)
{
	u_char		fifo_state;
	struct bzfifo	*bzt;

	if (fifo) {
		bzt = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b2;
		fifo_state = hc->hw.fifo_en & HFCPCI_FIFOEN_B2TX;
	} else {
		bzt = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b1;
		fifo_state = hc->hw.fifo_en & HFCPCI_FIFOEN_B1TX;
	}
	if (fifo_state)
		hc->hw.fifo_en ^= fifo_state;
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	if (hc->bch[fifo].debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG "hfcpci_clear_fifo_tx%d f1(%x) f2(%x) "
		       "z1(%x) z2(%x) state(%x)\n",
		       fifo, bzt->f1, bzt->f2,
		       le16_to_cpu(bzt->za[MAX_B_FRAMES].z1),
		       le16_to_cpu(bzt->za[MAX_B_FRAMES].z2),
		       fifo_state);
	bzt->f2 = MAX_B_FRAMES;
	bzt->f1 = bzt->f2;	
	bzt->za[MAX_B_FRAMES].z1 = cpu_to_le16(B_FIFO_SIZE + B_SUB_VAL - 1);
	bzt->za[MAX_B_FRAMES].z2 = cpu_to_le16(B_FIFO_SIZE + B_SUB_VAL - 2);
	if (fifo_state)
		hc->hw.fifo_en |= fifo_state;
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	if (hc->bch[fifo].debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG
		       "hfcpci_clear_fifo_tx%d f1(%x) f2(%x) z1(%x) z2(%x)\n",
		       fifo, bzt->f1, bzt->f2,
		       le16_to_cpu(bzt->za[MAX_B_FRAMES].z1),
		       le16_to_cpu(bzt->za[MAX_B_FRAMES].z2));
}

static void
hfcpci_empty_bfifo(struct bchannel *bch, struct bzfifo *bz,
		   u_char *bdata, int count)
{
	u_char		*ptr, *ptr1, new_f2;
	int		maxlen, new_z2;
	struct zt	*zp;

	if ((bch->debug & DEBUG_HW_BCHANNEL) && !(bch->debug & DEBUG_HW_BFIFO))
		printk(KERN_DEBUG "hfcpci_empty_fifo\n");
	zp = &bz->za[bz->f2];	
	new_z2 = le16_to_cpu(zp->z2) + count;	
	if (new_z2 >= (B_FIFO_SIZE + B_SUB_VAL))
		new_z2 -= B_FIFO_SIZE;	
	new_f2 = (bz->f2 + 1) & MAX_B_FRAMES;
	if ((count > MAX_DATA_SIZE + 3) || (count < 4) ||
	    (*(bdata + (le16_to_cpu(zp->z1) - B_SUB_VAL)))) {
		if (bch->debug & DEBUG_HW)
			printk(KERN_DEBUG "hfcpci_empty_fifo: incoming packet "
			       "invalid length %d or crc\n", count);
#ifdef ERROR_STATISTIC
		bch->err_inv++;
#endif
		bz->za[new_f2].z2 = cpu_to_le16(new_z2);
		bz->f2 = new_f2;	
	} else {
		bch->rx_skb = mI_alloc_skb(count - 3, GFP_ATOMIC);
		if (!bch->rx_skb) {
			printk(KERN_WARNING "HFCPCI: receive out of memory\n");
			return;
		}
		count -= 3;
		ptr = skb_put(bch->rx_skb, count);

		if (le16_to_cpu(zp->z2) + count <= B_FIFO_SIZE + B_SUB_VAL)
			maxlen = count;		
		else
			maxlen = B_FIFO_SIZE + B_SUB_VAL -
				le16_to_cpu(zp->z2);	

		ptr1 = bdata + (le16_to_cpu(zp->z2) - B_SUB_VAL);
		
		memcpy(ptr, ptr1, maxlen);	
		count -= maxlen;

		if (count) {	
			ptr += maxlen;
			ptr1 = bdata;	
			memcpy(ptr, ptr1, count);	
		}
		bz->za[new_f2].z2 = cpu_to_le16(new_z2);
		bz->f2 = new_f2;	
		recv_Bchannel(bch, MISDN_ID_ANY);
	}
}

static int
receive_dmsg(struct hfc_pci *hc)
{
	struct dchannel	*dch = &hc->dch;
	int		maxlen;
	int		rcnt, total;
	int		count = 5;
	u_char		*ptr, *ptr1;
	struct dfifo	*df;
	struct zt	*zp;

	df = &((union fifo_area *)(hc->hw.fifos))->d_chan.d_rx;
	while (((df->f1 & D_FREG_MASK) != (df->f2 & D_FREG_MASK)) && count--) {
		zp = &df->za[df->f2 & D_FREG_MASK];
		rcnt = le16_to_cpu(zp->z1) - le16_to_cpu(zp->z2);
		if (rcnt < 0)
			rcnt += D_FIFO_SIZE;
		rcnt++;
		if (dch->debug & DEBUG_HW_DCHANNEL)
			printk(KERN_DEBUG
			       "hfcpci recd f1(%d) f2(%d) z1(%x) z2(%x) cnt(%d)\n",
			       df->f1, df->f2,
			       le16_to_cpu(zp->z1),
			       le16_to_cpu(zp->z2),
			       rcnt);

		if ((rcnt > MAX_DFRAME_LEN + 3) || (rcnt < 4) ||
		    (df->data[le16_to_cpu(zp->z1)])) {
			if (dch->debug & DEBUG_HW)
				printk(KERN_DEBUG
				       "empty_fifo hfcpci paket inv. len "
				       "%d or crc %d\n",
				       rcnt,
				       df->data[le16_to_cpu(zp->z1)]);
#ifdef ERROR_STATISTIC
			cs->err_rx++;
#endif
			df->f2 = ((df->f2 + 1) & MAX_D_FRAMES) |
				(MAX_D_FRAMES + 1);	
			df->za[df->f2 & D_FREG_MASK].z2 =
				cpu_to_le16((le16_to_cpu(zp->z2) + rcnt) &
					    (D_FIFO_SIZE - 1));
		} else {
			dch->rx_skb = mI_alloc_skb(rcnt - 3, GFP_ATOMIC);
			if (!dch->rx_skb) {
				printk(KERN_WARNING
				       "HFC-PCI: D receive out of memory\n");
				break;
			}
			total = rcnt;
			rcnt -= 3;
			ptr = skb_put(dch->rx_skb, rcnt);

			if (le16_to_cpu(zp->z2) + rcnt <= D_FIFO_SIZE)
				maxlen = rcnt;	
			else
				maxlen = D_FIFO_SIZE - le16_to_cpu(zp->z2);
			

			ptr1 = df->data + le16_to_cpu(zp->z2);
			
			memcpy(ptr, ptr1, maxlen);	
			rcnt -= maxlen;

			if (rcnt) {	
				ptr += maxlen;
				ptr1 = df->data;	
				memcpy(ptr, ptr1, rcnt);	
			}
			df->f2 = ((df->f2 + 1) & MAX_D_FRAMES) |
				(MAX_D_FRAMES + 1);	
			df->za[df->f2 & D_FREG_MASK].z2 = cpu_to_le16((
									      le16_to_cpu(zp->z2) + total) & (D_FIFO_SIZE - 1));
			recv_Dchannel(dch);
		}
	}
	return 1;
}

static void
hfcpci_empty_fifo_trans(struct bchannel *bch, struct bzfifo *rxbz,
			struct bzfifo *txbz, u_char *bdata)
{
	__le16	*z1r, *z2r, *z1t, *z2t;
	int	new_z2, fcnt_rx, fcnt_tx, maxlen;
	u_char	*ptr, *ptr1;

	z1r = &rxbz->za[MAX_B_FRAMES].z1;	
	z2r = z1r + 1;
	z1t = &txbz->za[MAX_B_FRAMES].z1;
	z2t = z1t + 1;

	fcnt_rx = le16_to_cpu(*z1r) - le16_to_cpu(*z2r);
	if (!fcnt_rx)
		return;	

	if (fcnt_rx <= 0)
		fcnt_rx += B_FIFO_SIZE;	
	new_z2 = le16_to_cpu(*z2r) + fcnt_rx;	
	if (new_z2 >= (B_FIFO_SIZE + B_SUB_VAL))
		new_z2 -= B_FIFO_SIZE;	

	if (fcnt_rx > MAX_DATA_SIZE) {	
		*z2r = cpu_to_le16(new_z2);		
		return;
	}

	fcnt_tx = le16_to_cpu(*z2t) - le16_to_cpu(*z1t);
	if (fcnt_tx <= 0)
		fcnt_tx += B_FIFO_SIZE;
	
	fcnt_tx = B_FIFO_SIZE - fcnt_tx;
	

	bch->rx_skb = mI_alloc_skb(fcnt_rx, GFP_ATOMIC);
	if (bch->rx_skb) {
		ptr = skb_put(bch->rx_skb, fcnt_rx);
		if (le16_to_cpu(*z2r) + fcnt_rx <= B_FIFO_SIZE + B_SUB_VAL)
			maxlen = fcnt_rx;	
		else
			maxlen = B_FIFO_SIZE + B_SUB_VAL - le16_to_cpu(*z2r);
		

		ptr1 = bdata + (le16_to_cpu(*z2r) - B_SUB_VAL);
		
		memcpy(ptr, ptr1, maxlen);	
		fcnt_rx -= maxlen;

		if (fcnt_rx) {	
			ptr += maxlen;
			ptr1 = bdata;	
			memcpy(ptr, ptr1, fcnt_rx);	
		}
		recv_Bchannel(bch, fcnt_tx); 
	} else
		printk(KERN_WARNING "HFCPCI: receive out of memory\n");

	*z2r = cpu_to_le16(new_z2);		
}

static void
main_rec_hfcpci(struct bchannel *bch)
{
	struct hfc_pci	*hc = bch->hw;
	int		rcnt, real_fifo;
	int		receive = 0, count = 5;
	struct bzfifo	*txbz, *rxbz;
	u_char		*bdata;
	struct zt	*zp;

	if ((bch->nr & 2) && (!hc->hw.bswapped)) {
		rxbz = &((union fifo_area *)(hc->hw.fifos))->b_chans.rxbz_b2;
		txbz = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b2;
		bdata = ((union fifo_area *)(hc->hw.fifos))->b_chans.rxdat_b2;
		real_fifo = 1;
	} else {
		rxbz = &((union fifo_area *)(hc->hw.fifos))->b_chans.rxbz_b1;
		txbz = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b1;
		bdata = ((union fifo_area *)(hc->hw.fifos))->b_chans.rxdat_b1;
		real_fifo = 0;
	}
Begin:
	count--;
	if (rxbz->f1 != rxbz->f2) {
		if (bch->debug & DEBUG_HW_BCHANNEL)
			printk(KERN_DEBUG "hfcpci rec ch(%x) f1(%d) f2(%d)\n",
			       bch->nr, rxbz->f1, rxbz->f2);
		zp = &rxbz->za[rxbz->f2];

		rcnt = le16_to_cpu(zp->z1) - le16_to_cpu(zp->z2);
		if (rcnt < 0)
			rcnt += B_FIFO_SIZE;
		rcnt++;
		if (bch->debug & DEBUG_HW_BCHANNEL)
			printk(KERN_DEBUG
			       "hfcpci rec ch(%x) z1(%x) z2(%x) cnt(%d)\n",
			       bch->nr, le16_to_cpu(zp->z1),
			       le16_to_cpu(zp->z2), rcnt);
		hfcpci_empty_bfifo(bch, rxbz, bdata, rcnt);
		rcnt = rxbz->f1 - rxbz->f2;
		if (rcnt < 0)
			rcnt += MAX_B_FRAMES + 1;
		if (hc->hw.last_bfifo_cnt[real_fifo] > rcnt + 1) {
			rcnt = 0;
			hfcpci_clear_fifo_rx(hc, real_fifo);
		}
		hc->hw.last_bfifo_cnt[real_fifo] = rcnt;
		if (rcnt > 1)
			receive = 1;
		else
			receive = 0;
	} else if (test_bit(FLG_TRANSPARENT, &bch->Flags)) {
		hfcpci_empty_fifo_trans(bch, rxbz, txbz, bdata);
		return;
	} else
		receive = 0;
	if (count && receive)
		goto Begin;

}

static void
hfcpci_fill_dfifo(struct hfc_pci *hc)
{
	struct dchannel	*dch = &hc->dch;
	int		fcnt;
	int		count, new_z1, maxlen;
	struct dfifo	*df;
	u_char		*src, *dst, new_f1;

	if ((dch->debug & DEBUG_HW_DCHANNEL) && !(dch->debug & DEBUG_HW_DFIFO))
		printk(KERN_DEBUG "%s\n", __func__);

	if (!dch->tx_skb)
		return;
	count = dch->tx_skb->len - dch->tx_idx;
	if (count <= 0)
		return;
	df = &((union fifo_area *) (hc->hw.fifos))->d_chan.d_tx;

	if (dch->debug & DEBUG_HW_DFIFO)
		printk(KERN_DEBUG "%s:f1(%d) f2(%d) z1(f1)(%x)\n", __func__,
		       df->f1, df->f2,
		       le16_to_cpu(df->za[df->f1 & D_FREG_MASK].z1));
	fcnt = df->f1 - df->f2;	
	if (fcnt < 0)
		fcnt += (MAX_D_FRAMES + 1);	
	if (fcnt > (MAX_D_FRAMES - 1)) {
		if (dch->debug & DEBUG_HW_DCHANNEL)
			printk(KERN_DEBUG
			       "hfcpci_fill_Dfifo more as 14 frames\n");
#ifdef ERROR_STATISTIC
		cs->err_tx++;
#endif
		return;
	}
	
	maxlen = le16_to_cpu(df->za[df->f2 & D_FREG_MASK].z2) -
		le16_to_cpu(df->za[df->f1 & D_FREG_MASK].z1) - 1;
	if (maxlen <= 0)
		maxlen += D_FIFO_SIZE;	

	if (dch->debug & DEBUG_HW_DCHANNEL)
		printk(KERN_DEBUG "hfcpci_fill_Dfifo count(%d/%d)\n",
		       count, maxlen);
	if (count > maxlen) {
		if (dch->debug & DEBUG_HW_DCHANNEL)
			printk(KERN_DEBUG "hfcpci_fill_Dfifo no fifo mem\n");
		return;
	}
	new_z1 = (le16_to_cpu(df->za[df->f1 & D_FREG_MASK].z1) + count) &
		(D_FIFO_SIZE - 1);
	new_f1 = ((df->f1 + 1) & D_FREG_MASK) | (D_FREG_MASK + 1);
	src = dch->tx_skb->data + dch->tx_idx;	
	dst = df->data + le16_to_cpu(df->za[df->f1 & D_FREG_MASK].z1);
	maxlen = D_FIFO_SIZE - le16_to_cpu(df->za[df->f1 & D_FREG_MASK].z1);
	
	if (maxlen > count)
		maxlen = count;	
	memcpy(dst, src, maxlen);	

	count -= maxlen;	
	if (count) {
		dst = df->data;	
		src += maxlen;	
		memcpy(dst, src, count);
	}
	df->za[new_f1 & D_FREG_MASK].z1 = cpu_to_le16(new_z1);
	
	df->za[df->f1 & D_FREG_MASK].z1 = cpu_to_le16(new_z1);
	
	df->f1 = new_f1;	
	dch->tx_idx = dch->tx_skb->len;
}

static void
hfcpci_fill_fifo(struct bchannel *bch)
{
	struct hfc_pci	*hc = bch->hw;
	int		maxlen, fcnt;
	int		count, new_z1;
	struct bzfifo	*bz;
	u_char		*bdata;
	u_char		new_f1, *src, *dst;
	__le16 *z1t, *z2t;

	if ((bch->debug & DEBUG_HW_BCHANNEL) && !(bch->debug & DEBUG_HW_BFIFO))
		printk(KERN_DEBUG "%s\n", __func__);
	if ((!bch->tx_skb) || bch->tx_skb->len <= 0)
		return;
	count = bch->tx_skb->len - bch->tx_idx;
	if ((bch->nr & 2) && (!hc->hw.bswapped)) {
		bz = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b2;
		bdata = ((union fifo_area *)(hc->hw.fifos))->b_chans.txdat_b2;
	} else {
		bz = &((union fifo_area *)(hc->hw.fifos))->b_chans.txbz_b1;
		bdata = ((union fifo_area *)(hc->hw.fifos))->b_chans.txdat_b1;
	}

	if (test_bit(FLG_TRANSPARENT, &bch->Flags)) {
		z1t = &bz->za[MAX_B_FRAMES].z1;
		z2t = z1t + 1;
		if (bch->debug & DEBUG_HW_BCHANNEL)
			printk(KERN_DEBUG "hfcpci_fill_fifo_trans ch(%x) "
			       "cnt(%d) z1(%x) z2(%x)\n", bch->nr, count,
			       le16_to_cpu(*z1t), le16_to_cpu(*z2t));
		fcnt = le16_to_cpu(*z2t) - le16_to_cpu(*z1t);
		if (fcnt <= 0)
			fcnt += B_FIFO_SIZE;
		
		fcnt = B_FIFO_SIZE - fcnt;
		

		
		if (test_bit(FLG_FILLEMPTY, &bch->Flags) && !fcnt) {
			
			count = HFCPCI_FILLEMPTY;
			new_z1 = le16_to_cpu(*z1t) + count;
			
			if (new_z1 >= (B_FIFO_SIZE + B_SUB_VAL))
				new_z1 -= B_FIFO_SIZE;	
			dst = bdata + (le16_to_cpu(*z1t) - B_SUB_VAL);
			maxlen = (B_FIFO_SIZE + B_SUB_VAL) - le16_to_cpu(*z1t);
			
			if (bch->debug & DEBUG_HW_BFIFO)
				printk(KERN_DEBUG "hfcpci_FFt fillempty "
				       "fcnt(%d) maxl(%d) nz1(%x) dst(%p)\n",
				       fcnt, maxlen, new_z1, dst);
			fcnt += count;
			if (maxlen > count)
				maxlen = count;		
			memset(dst, 0x2a, maxlen);	
			count -= maxlen;		
			if (count) {
				dst = bdata;		
				memset(dst, 0x2a, count);
			}
			*z1t = cpu_to_le16(new_z1);	
		}

	next_t_frame:
		count = bch->tx_skb->len - bch->tx_idx;
		
		if (count > (poll << 1) - fcnt)
			count = (poll << 1) - fcnt;
		if (count <= 0)
			return;
		
		new_z1 = le16_to_cpu(*z1t) + count;
		
		if (new_z1 >= (B_FIFO_SIZE + B_SUB_VAL))
			new_z1 -= B_FIFO_SIZE;	
		src = bch->tx_skb->data + bch->tx_idx;
		
		dst = bdata + (le16_to_cpu(*z1t) - B_SUB_VAL);
		maxlen = (B_FIFO_SIZE + B_SUB_VAL) - le16_to_cpu(*z1t);
		
		if (bch->debug & DEBUG_HW_BFIFO)
			printk(KERN_DEBUG "hfcpci_FFt fcnt(%d) "
			       "maxl(%d) nz1(%x) dst(%p)\n",
			       fcnt, maxlen, new_z1, dst);
		fcnt += count;
		bch->tx_idx += count;
		if (maxlen > count)
			maxlen = count;		
		memcpy(dst, src, maxlen);	
		count -= maxlen;	
		if (count) {
			dst = bdata;	
			src += maxlen;	
			memcpy(dst, src, count);
		}
		*z1t = cpu_to_le16(new_z1);	
		if (bch->tx_idx < bch->tx_skb->len)
			return;
		
		if (test_bit(FLG_TRANSPARENT, &bch->Flags))
			confirm_Bsend(bch);
		dev_kfree_skb(bch->tx_skb);
		if (get_next_bframe(bch))
			goto next_t_frame;
		return;
	}
	if (bch->debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG
		       "%s: ch(%x) f1(%d) f2(%d) z1(f1)(%x)\n",
		       __func__, bch->nr, bz->f1, bz->f2,
		       bz->za[bz->f1].z1);
	fcnt = bz->f1 - bz->f2;	
	if (fcnt < 0)
		fcnt += (MAX_B_FRAMES + 1);	
	if (fcnt > (MAX_B_FRAMES - 1)) {
		if (bch->debug & DEBUG_HW_BCHANNEL)
			printk(KERN_DEBUG
			       "hfcpci_fill_Bfifo more as 14 frames\n");
		return;
	}
	
	maxlen = le16_to_cpu(bz->za[bz->f2].z2) -
		le16_to_cpu(bz->za[bz->f1].z1) - 1;
	if (maxlen <= 0)
		maxlen += B_FIFO_SIZE;	

	if (bch->debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG "hfcpci_fill_fifo ch(%x) count(%d/%d)\n",
		       bch->nr, count, maxlen);

	if (maxlen < count) {
		if (bch->debug & DEBUG_HW_BCHANNEL)
			printk(KERN_DEBUG "hfcpci_fill_fifo no fifo mem\n");
		return;
	}
	new_z1 = le16_to_cpu(bz->za[bz->f1].z1) + count;
	
	if (new_z1 >= (B_FIFO_SIZE + B_SUB_VAL))
		new_z1 -= B_FIFO_SIZE;	

	new_f1 = ((bz->f1 + 1) & MAX_B_FRAMES);
	src = bch->tx_skb->data + bch->tx_idx;	
	dst = bdata + (le16_to_cpu(bz->za[bz->f1].z1) - B_SUB_VAL);
	maxlen = (B_FIFO_SIZE + B_SUB_VAL) - le16_to_cpu(bz->za[bz->f1].z1);
	
	if (maxlen > count)
		maxlen = count;	
	memcpy(dst, src, maxlen);	

	count -= maxlen;	
	if (count) {
		dst = bdata;	
		src += maxlen;	
		memcpy(dst, src, count);
	}
	bz->za[new_f1].z1 = cpu_to_le16(new_z1);	
	bz->f1 = new_f1;	
	dev_kfree_skb(bch->tx_skb);
	get_next_bframe(bch);
}




static void
ph_state_te(struct dchannel *dch)
{
	if (dch->debug)
		printk(KERN_DEBUG "%s: TE newstate %x\n",
		       __func__, dch->state);
	switch (dch->state) {
	case 0:
		l1_event(dch->l1, HW_RESET_IND);
		break;
	case 3:
		l1_event(dch->l1, HW_DEACT_IND);
		break;
	case 5:
	case 8:
		l1_event(dch->l1, ANYSIGNAL);
		break;
	case 6:
		l1_event(dch->l1, INFO2);
		break;
	case 7:
		l1_event(dch->l1, INFO4_P8);
		break;
	}
}


static void
handle_nt_timer3(struct dchannel *dch) {
	struct hfc_pci	*hc = dch->hw;

	test_and_clear_bit(FLG_HFC_TIMER_T3, &dch->Flags);
	hc->hw.int_m1 &= ~HFCPCI_INTS_TIMER;
	Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
	hc->hw.nt_timer = 0;
	test_and_set_bit(FLG_ACTIVE, &dch->Flags);
	if (test_bit(HFC_CFG_MASTER, &hc->cfg))
		hc->hw.mst_m |= HFCPCI_MASTER;
	Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
	_queue_data(&dch->dev.D, PH_ACTIVATE_IND,
		    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
}

static void
ph_state_nt(struct dchannel *dch)
{
	struct hfc_pci	*hc = dch->hw;

	if (dch->debug)
		printk(KERN_DEBUG "%s: NT newstate %x\n",
		       __func__, dch->state);
	switch (dch->state) {
	case 2:
		if (hc->hw.nt_timer < 0) {
			hc->hw.nt_timer = 0;
			test_and_clear_bit(FLG_HFC_TIMER_T3, &dch->Flags);
			test_and_clear_bit(FLG_HFC_TIMER_T1, &dch->Flags);
			hc->hw.int_m1 &= ~HFCPCI_INTS_TIMER;
			Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
			
			(void) Read_hfc(hc, HFCPCI_INT_S1);
			Write_hfc(hc, HFCPCI_STATES, 4 | HFCPCI_LOAD_STATE);
			udelay(10);
			Write_hfc(hc, HFCPCI_STATES, 4);
			dch->state = 4;
		} else if (hc->hw.nt_timer == 0) {
			hc->hw.int_m1 |= HFCPCI_INTS_TIMER;
			Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
			hc->hw.nt_timer = NT_T1_COUNT;
			hc->hw.ctmt &= ~HFCPCI_AUTO_TIMER;
			hc->hw.ctmt |= HFCPCI_TIM3_125;
			Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt |
				  HFCPCI_CLTIMER);
			test_and_clear_bit(FLG_HFC_TIMER_T3, &dch->Flags);
			test_and_set_bit(FLG_HFC_TIMER_T1, &dch->Flags);
			
			Write_hfc(hc, HFCPCI_STATES, 2 | HFCPCI_NT_G2_G3);
		} else {
			Write_hfc(hc, HFCPCI_STATES, 2 | HFCPCI_NT_G2_G3);
		}
		break;
	case 1:
		hc->hw.nt_timer = 0;
		test_and_clear_bit(FLG_HFC_TIMER_T3, &dch->Flags);
		test_and_clear_bit(FLG_HFC_TIMER_T1, &dch->Flags);
		hc->hw.int_m1 &= ~HFCPCI_INTS_TIMER;
		Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
		test_and_clear_bit(FLG_ACTIVE, &dch->Flags);
		hc->hw.mst_m &= ~HFCPCI_MASTER;
		Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
		test_and_clear_bit(FLG_L2_ACTIVATED, &dch->Flags);
		_queue_data(&dch->dev.D, PH_DEACTIVATE_IND,
			    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
		break;
	case 4:
		hc->hw.nt_timer = 0;
		test_and_clear_bit(FLG_HFC_TIMER_T3, &dch->Flags);
		test_and_clear_bit(FLG_HFC_TIMER_T1, &dch->Flags);
		hc->hw.int_m1 &= ~HFCPCI_INTS_TIMER;
		Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
		break;
	case 3:
		if (!test_and_set_bit(FLG_HFC_TIMER_T3, &dch->Flags)) {
			if (!test_and_clear_bit(FLG_L2_ACTIVATED,
						&dch->Flags)) {
				handle_nt_timer3(dch);
				break;
			}
			test_and_clear_bit(FLG_HFC_TIMER_T1, &dch->Flags);
			hc->hw.int_m1 |= HFCPCI_INTS_TIMER;
			Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
			hc->hw.nt_timer = NT_T3_COUNT;
			hc->hw.ctmt &= ~HFCPCI_AUTO_TIMER;
			hc->hw.ctmt |= HFCPCI_TIM3_125;
			Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt |
				  HFCPCI_CLTIMER);
		}
		break;
	}
}

static void
ph_state(struct dchannel *dch)
{
	struct hfc_pci	*hc = dch->hw;

	if (hc->hw.protocol == ISDN_P_NT_S0) {
		if (test_bit(FLG_HFC_TIMER_T3, &dch->Flags) &&
		    hc->hw.nt_timer < 0)
			handle_nt_timer3(dch);
		else
			ph_state_nt(dch);
	} else
		ph_state_te(dch);
}

static int
hfc_l1callback(struct dchannel *dch, u_int cmd)
{
	struct hfc_pci		*hc = dch->hw;

	switch (cmd) {
	case INFO3_P8:
	case INFO3_P10:
		if (test_bit(HFC_CFG_MASTER, &hc->cfg))
			hc->hw.mst_m |= HFCPCI_MASTER;
		Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
		break;
	case HW_RESET_REQ:
		Write_hfc(hc, HFCPCI_STATES, HFCPCI_LOAD_STATE | 3);
		
		udelay(6);
		Write_hfc(hc, HFCPCI_STATES, 3);	
		if (test_bit(HFC_CFG_MASTER, &hc->cfg))
			hc->hw.mst_m |= HFCPCI_MASTER;
		Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
		Write_hfc(hc, HFCPCI_STATES, HFCPCI_ACTIVATE |
			  HFCPCI_DO_ACTION);
		l1_event(dch->l1, HW_POWERUP_IND);
		break;
	case HW_DEACT_REQ:
		hc->hw.mst_m &= ~HFCPCI_MASTER;
		Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
		skb_queue_purge(&dch->squeue);
		if (dch->tx_skb) {
			dev_kfree_skb(dch->tx_skb);
			dch->tx_skb = NULL;
		}
		dch->tx_idx = 0;
		if (dch->rx_skb) {
			dev_kfree_skb(dch->rx_skb);
			dch->rx_skb = NULL;
		}
		test_and_clear_bit(FLG_TX_BUSY, &dch->Flags);
		if (test_and_clear_bit(FLG_BUSY_TIMER, &dch->Flags))
			del_timer(&dch->timer);
		break;
	case HW_POWERUP_REQ:
		Write_hfc(hc, HFCPCI_STATES, HFCPCI_DO_ACTION);
		break;
	case PH_ACTIVATE_IND:
		test_and_set_bit(FLG_ACTIVE, &dch->Flags);
		_queue_data(&dch->dev.D, cmd, MISDN_ID_ANY, 0, NULL,
			    GFP_ATOMIC);
		break;
	case PH_DEACTIVATE_IND:
		test_and_clear_bit(FLG_ACTIVE, &dch->Flags);
		_queue_data(&dch->dev.D, cmd, MISDN_ID_ANY, 0, NULL,
			    GFP_ATOMIC);
		break;
	default:
		if (dch->debug & DEBUG_HW)
			printk(KERN_DEBUG "%s: unknown command %x\n",
			       __func__, cmd);
		return -1;
	}
	return 0;
}

static inline void
tx_birq(struct bchannel *bch)
{
	if (bch->tx_skb && bch->tx_idx < bch->tx_skb->len)
		hfcpci_fill_fifo(bch);
	else {
		if (bch->tx_skb)
			dev_kfree_skb(bch->tx_skb);
		if (get_next_bframe(bch))
			hfcpci_fill_fifo(bch);
	}
}

static inline void
tx_dirq(struct dchannel *dch)
{
	if (dch->tx_skb && dch->tx_idx < dch->tx_skb->len)
		hfcpci_fill_dfifo(dch->hw);
	else {
		if (dch->tx_skb)
			dev_kfree_skb(dch->tx_skb);
		if (get_next_dframe(dch))
			hfcpci_fill_dfifo(dch->hw);
	}
}

static irqreturn_t
hfcpci_int(int intno, void *dev_id)
{
	struct hfc_pci	*hc = dev_id;
	u_char		exval;
	struct bchannel	*bch;
	u_char		val, stat;

	spin_lock(&hc->lock);
	if (!(hc->hw.int_m2 & 0x08)) {
		spin_unlock(&hc->lock);
		return IRQ_NONE; 
	}
	stat = Read_hfc(hc, HFCPCI_STATUS);
	if (HFCPCI_ANYINT & stat) {
		val = Read_hfc(hc, HFCPCI_INT_S1);
		if (hc->dch.debug & DEBUG_HW_DCHANNEL)
			printk(KERN_DEBUG
			       "HFC-PCI: stat(%02x) s1(%02x)\n", stat, val);
	} else {
		
		spin_unlock(&hc->lock);
		return IRQ_NONE;
	}
	hc->irqcnt++;

	if (hc->dch.debug & DEBUG_HW_DCHANNEL)
		printk(KERN_DEBUG "HFC-PCI irq %x\n", val);
	val &= hc->hw.int_m1;
	if (val & 0x40) {	
		exval = Read_hfc(hc, HFCPCI_STATES) & 0xf;
		if (hc->dch.debug & DEBUG_HW_DCHANNEL)
			printk(KERN_DEBUG "ph_state chg %d->%d\n",
			       hc->dch.state, exval);
		hc->dch.state = exval;
		schedule_event(&hc->dch, FLG_PHCHANGE);
		val &= ~0x40;
	}
	if (val & 0x80) {	
		if (hc->hw.protocol == ISDN_P_NT_S0) {
			if ((--hc->hw.nt_timer) < 0)
				schedule_event(&hc->dch, FLG_PHCHANGE);
		}
		val &= ~0x80;
		Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt | HFCPCI_CLTIMER);
	}
	if (val & 0x08) {	
		bch = Sel_BCS(hc, hc->hw.bswapped ? 2 : 1);
		if (bch)
			main_rec_hfcpci(bch);
		else if (hc->dch.debug)
			printk(KERN_DEBUG "hfcpci spurious 0x08 IRQ\n");
	}
	if (val & 0x10) {	
		bch = Sel_BCS(hc, 2);
		if (bch)
			main_rec_hfcpci(bch);
		else if (hc->dch.debug)
			printk(KERN_DEBUG "hfcpci spurious 0x10 IRQ\n");
	}
	if (val & 0x01) {	
		bch = Sel_BCS(hc, hc->hw.bswapped ? 2 : 1);
		if (bch)
			tx_birq(bch);
		else if (hc->dch.debug)
			printk(KERN_DEBUG "hfcpci spurious 0x01 IRQ\n");
	}
	if (val & 0x02) {	
		bch = Sel_BCS(hc, 2);
		if (bch)
			tx_birq(bch);
		else if (hc->dch.debug)
			printk(KERN_DEBUG "hfcpci spurious 0x02 IRQ\n");
	}
	if (val & 0x20)		
		receive_dmsg(hc);
	if (val & 0x04) {	
		if (test_and_clear_bit(FLG_BUSY_TIMER, &hc->dch.Flags))
			del_timer(&hc->dch.timer);
		tx_dirq(&hc->dch);
	}
	spin_unlock(&hc->lock);
	return IRQ_HANDLED;
}

static void
hfcpci_dbusy_timer(struct hfc_pci *hc)
{
}

static int
mode_hfcpci(struct bchannel *bch, int bc, int protocol)
{
	struct hfc_pci	*hc = bch->hw;
	int		fifo2;
	u_char		rx_slot = 0, tx_slot = 0, pcm_mode;

	if (bch->debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG
		       "HFCPCI bchannel protocol %x-->%x ch %x-->%x\n",
		       bch->state, protocol, bch->nr, bc);

	fifo2 = bc;
	pcm_mode = (bc >> 24) & 0xff;
	if (pcm_mode) { 
		if (!test_bit(HFC_CFG_PCM, &hc->cfg))
			printk(KERN_WARNING
			       "%s: pcm channel id without HFC_CFG_PCM\n",
			       __func__);
		rx_slot = (bc >> 8) & 0xff;
		tx_slot = (bc >> 16) & 0xff;
		bc = bc & 0xff;
	} else if (test_bit(HFC_CFG_PCM, &hc->cfg) && (protocol > ISDN_P_NONE))
		printk(KERN_WARNING "%s: no pcm channel id but HFC_CFG_PCM\n",
		       __func__);
	if (hc->chanlimit > 1) {
		hc->hw.bswapped = 0;	
		hc->hw.sctrl_e &= ~0x80;
	} else {
		if (bc & 2) {
			if (protocol != ISDN_P_NONE) {
				hc->hw.bswapped = 1; 
				hc->hw.sctrl_e |= 0x80;
			} else {
				hc->hw.bswapped = 0; 
				hc->hw.sctrl_e &= ~0x80;
			}
			fifo2 = 1;
		} else {
			hc->hw.bswapped = 0;	
			hc->hw.sctrl_e &= ~0x80;
		}
	}
	switch (protocol) {
	case (-1): 
		bch->state = -1;
		bch->nr = bc;
	case (ISDN_P_NONE):
		if (bch->state == ISDN_P_NONE)
			return 0;
		if (bc & 2) {
			hc->hw.sctrl &= ~SCTRL_B2_ENA;
			hc->hw.sctrl_r &= ~SCTRL_B2_ENA;
		} else {
			hc->hw.sctrl &= ~SCTRL_B1_ENA;
			hc->hw.sctrl_r &= ~SCTRL_B1_ENA;
		}
		if (fifo2 & 2) {
			hc->hw.fifo_en &= ~HFCPCI_FIFOEN_B2;
			hc->hw.int_m1 &= ~(HFCPCI_INTS_B2TRANS +
					   HFCPCI_INTS_B2REC);
		} else {
			hc->hw.fifo_en &= ~HFCPCI_FIFOEN_B1;
			hc->hw.int_m1 &= ~(HFCPCI_INTS_B1TRANS +
					   HFCPCI_INTS_B1REC);
		}
#ifdef REVERSE_BITORDER
		if (bch->nr & 2)
			hc->hw.cirm &= 0x7f;
		else
			hc->hw.cirm &= 0xbf;
#endif
		bch->state = ISDN_P_NONE;
		bch->nr = bc;
		test_and_clear_bit(FLG_HDLC, &bch->Flags);
		test_and_clear_bit(FLG_TRANSPARENT, &bch->Flags);
		break;
	case (ISDN_P_B_RAW):
		bch->state = protocol;
		bch->nr = bc;
		hfcpci_clear_fifo_rx(hc, (fifo2 & 2) ? 1 : 0);
		hfcpci_clear_fifo_tx(hc, (fifo2 & 2) ? 1 : 0);
		if (bc & 2) {
			hc->hw.sctrl |= SCTRL_B2_ENA;
			hc->hw.sctrl_r |= SCTRL_B2_ENA;
#ifdef REVERSE_BITORDER
			hc->hw.cirm |= 0x80;
#endif
		} else {
			hc->hw.sctrl |= SCTRL_B1_ENA;
			hc->hw.sctrl_r |= SCTRL_B1_ENA;
#ifdef REVERSE_BITORDER
			hc->hw.cirm |= 0x40;
#endif
		}
		if (fifo2 & 2) {
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B2;
			if (!tics)
				hc->hw.int_m1 |= (HFCPCI_INTS_B2TRANS +
						  HFCPCI_INTS_B2REC);
			hc->hw.ctmt |= 2;
			hc->hw.conn &= ~0x18;
		} else {
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B1;
			if (!tics)
				hc->hw.int_m1 |= (HFCPCI_INTS_B1TRANS +
						  HFCPCI_INTS_B1REC);
			hc->hw.ctmt |= 1;
			hc->hw.conn &= ~0x03;
		}
		test_and_set_bit(FLG_TRANSPARENT, &bch->Flags);
		break;
	case (ISDN_P_B_HDLC):
		bch->state = protocol;
		bch->nr = bc;
		hfcpci_clear_fifo_rx(hc, (fifo2 & 2) ? 1 : 0);
		hfcpci_clear_fifo_tx(hc, (fifo2 & 2) ? 1 : 0);
		if (bc & 2) {
			hc->hw.sctrl |= SCTRL_B2_ENA;
			hc->hw.sctrl_r |= SCTRL_B2_ENA;
		} else {
			hc->hw.sctrl |= SCTRL_B1_ENA;
			hc->hw.sctrl_r |= SCTRL_B1_ENA;
		}
		if (fifo2 & 2) {
			hc->hw.last_bfifo_cnt[1] = 0;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B2;
			hc->hw.int_m1 |= (HFCPCI_INTS_B2TRANS +
					  HFCPCI_INTS_B2REC);
			hc->hw.ctmt &= ~2;
			hc->hw.conn &= ~0x18;
		} else {
			hc->hw.last_bfifo_cnt[0] = 0;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B1;
			hc->hw.int_m1 |= (HFCPCI_INTS_B1TRANS +
					  HFCPCI_INTS_B1REC);
			hc->hw.ctmt &= ~1;
			hc->hw.conn &= ~0x03;
		}
		test_and_set_bit(FLG_HDLC, &bch->Flags);
		break;
	default:
		printk(KERN_DEBUG "prot not known %x\n", protocol);
		return -ENOPROTOOPT;
	}
	if (test_bit(HFC_CFG_PCM, &hc->cfg)) {
		if ((protocol == ISDN_P_NONE) ||
		    (protocol == -1)) {	
			rx_slot = 0;
			tx_slot = 0;
		} else {
			if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg)) {
				rx_slot |= 0xC0;
				tx_slot |= 0xC0;
			} else {
				rx_slot |= 0x80;
				tx_slot |= 0x80;
			}
		}
		if (bc & 2) {
			hc->hw.conn &= 0xc7;
			hc->hw.conn |= 0x08;
			printk(KERN_DEBUG "%s: Write_hfc: B2_SSL 0x%x\n",
			       __func__, tx_slot);
			printk(KERN_DEBUG "%s: Write_hfc: B2_RSL 0x%x\n",
			       __func__, rx_slot);
			Write_hfc(hc, HFCPCI_B2_SSL, tx_slot);
			Write_hfc(hc, HFCPCI_B2_RSL, rx_slot);
		} else {
			hc->hw.conn &= 0xf8;
			hc->hw.conn |= 0x01;
			printk(KERN_DEBUG "%s: Write_hfc: B1_SSL 0x%x\n",
			       __func__, tx_slot);
			printk(KERN_DEBUG "%s: Write_hfc: B1_RSL 0x%x\n",
			       __func__, rx_slot);
			Write_hfc(hc, HFCPCI_B1_SSL, tx_slot);
			Write_hfc(hc, HFCPCI_B1_RSL, rx_slot);
		}
	}
	Write_hfc(hc, HFCPCI_SCTRL_E, hc->hw.sctrl_e);
	Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	Write_hfc(hc, HFCPCI_SCTRL, hc->hw.sctrl);
	Write_hfc(hc, HFCPCI_SCTRL_R, hc->hw.sctrl_r);
	Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt);
	Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
#ifdef REVERSE_BITORDER
	Write_hfc(hc, HFCPCI_CIRM, hc->hw.cirm);
#endif
	return 0;
}

static int
set_hfcpci_rxtest(struct bchannel *bch, int protocol, int chan)
{
	struct hfc_pci	*hc = bch->hw;

	if (bch->debug & DEBUG_HW_BCHANNEL)
		printk(KERN_DEBUG
		       "HFCPCI bchannel test rx protocol %x-->%x ch %x-->%x\n",
		       bch->state, protocol, bch->nr, chan);
	if (bch->nr != chan) {
		printk(KERN_DEBUG
		       "HFCPCI rxtest wrong channel parameter %x/%x\n",
		       bch->nr, chan);
		return -EINVAL;
	}
	switch (protocol) {
	case (ISDN_P_B_RAW):
		bch->state = protocol;
		hfcpci_clear_fifo_rx(hc, (chan & 2) ? 1 : 0);
		if (chan & 2) {
			hc->hw.sctrl_r |= SCTRL_B2_ENA;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B2RX;
			if (!tics)
				hc->hw.int_m1 |= HFCPCI_INTS_B2REC;
			hc->hw.ctmt |= 2;
			hc->hw.conn &= ~0x18;
#ifdef REVERSE_BITORDER
			hc->hw.cirm |= 0x80;
#endif
		} else {
			hc->hw.sctrl_r |= SCTRL_B1_ENA;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B1RX;
			if (!tics)
				hc->hw.int_m1 |= HFCPCI_INTS_B1REC;
			hc->hw.ctmt |= 1;
			hc->hw.conn &= ~0x03;
#ifdef REVERSE_BITORDER
			hc->hw.cirm |= 0x40;
#endif
		}
		break;
	case (ISDN_P_B_HDLC):
		bch->state = protocol;
		hfcpci_clear_fifo_rx(hc, (chan & 2) ? 1 : 0);
		if (chan & 2) {
			hc->hw.sctrl_r |= SCTRL_B2_ENA;
			hc->hw.last_bfifo_cnt[1] = 0;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B2RX;
			hc->hw.int_m1 |= HFCPCI_INTS_B2REC;
			hc->hw.ctmt &= ~2;
			hc->hw.conn &= ~0x18;
		} else {
			hc->hw.sctrl_r |= SCTRL_B1_ENA;
			hc->hw.last_bfifo_cnt[0] = 0;
			hc->hw.fifo_en |= HFCPCI_FIFOEN_B1RX;
			hc->hw.int_m1 |= HFCPCI_INTS_B1REC;
			hc->hw.ctmt &= ~1;
			hc->hw.conn &= ~0x03;
		}
		break;
	default:
		printk(KERN_DEBUG "prot not known %x\n", protocol);
		return -ENOPROTOOPT;
	}
	Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
	Write_hfc(hc, HFCPCI_FIFO_EN, hc->hw.fifo_en);
	Write_hfc(hc, HFCPCI_SCTRL_R, hc->hw.sctrl_r);
	Write_hfc(hc, HFCPCI_CTMT, hc->hw.ctmt);
	Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
#ifdef REVERSE_BITORDER
	Write_hfc(hc, HFCPCI_CIRM, hc->hw.cirm);
#endif
	return 0;
}

static void
deactivate_bchannel(struct bchannel *bch)
{
	struct hfc_pci	*hc = bch->hw;
	u_long		flags;

	spin_lock_irqsave(&hc->lock, flags);
	mISDN_clear_bchannel(bch);
	mode_hfcpci(bch, bch->nr, ISDN_P_NONE);
	spin_unlock_irqrestore(&hc->lock, flags);
}

static int
channel_bctrl(struct bchannel *bch, struct mISDN_ctrl_req *cq)
{
	int	ret = 0;

	switch (cq->op) {
	case MISDN_CTRL_GETOP:
		cq->op = MISDN_CTRL_FILL_EMPTY;
		break;
	case MISDN_CTRL_FILL_EMPTY: 
		test_and_set_bit(FLG_FILLEMPTY, &bch->Flags);
		if (debug & DEBUG_HW_OPEN)
			printk(KERN_DEBUG "%s: FILL_EMPTY request (nr=%d "
			       "off=%d)\n", __func__, bch->nr, !!cq->p1);
		break;
	default:
		printk(KERN_WARNING "%s: unknown Op %x\n", __func__, cq->op);
		ret = -EINVAL;
		break;
	}
	return ret;
}
static int
hfc_bctrl(struct mISDNchannel *ch, u_int cmd, void *arg)
{
	struct bchannel	*bch = container_of(ch, struct bchannel, ch);
	struct hfc_pci	*hc = bch->hw;
	int		ret = -EINVAL;
	u_long		flags;

	if (bch->debug & DEBUG_HW)
		printk(KERN_DEBUG "%s: cmd:%x %p\n", __func__, cmd, arg);
	switch (cmd) {
	case HW_TESTRX_RAW:
		spin_lock_irqsave(&hc->lock, flags);
		ret = set_hfcpci_rxtest(bch, ISDN_P_B_RAW, (int)(long)arg);
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	case HW_TESTRX_HDLC:
		spin_lock_irqsave(&hc->lock, flags);
		ret = set_hfcpci_rxtest(bch, ISDN_P_B_HDLC, (int)(long)arg);
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	case HW_TESTRX_OFF:
		spin_lock_irqsave(&hc->lock, flags);
		mode_hfcpci(bch, bch->nr, ISDN_P_NONE);
		spin_unlock_irqrestore(&hc->lock, flags);
		ret = 0;
		break;
	case CLOSE_CHANNEL:
		test_and_clear_bit(FLG_OPEN, &bch->Flags);
		if (test_bit(FLG_ACTIVE, &bch->Flags))
			deactivate_bchannel(bch);
		ch->protocol = ISDN_P_NONE;
		ch->peer = NULL;
		module_put(THIS_MODULE);
		ret = 0;
		break;
	case CONTROL_CHANNEL:
		ret = channel_bctrl(bch, arg);
		break;
	default:
		printk(KERN_WARNING "%s: unknown prim(%x)\n",
		       __func__, cmd);
	}
	return ret;
}

static int
hfcpci_l2l1D(struct mISDNchannel *ch, struct sk_buff *skb)
{
	struct mISDNdevice	*dev = container_of(ch, struct mISDNdevice, D);
	struct dchannel		*dch = container_of(dev, struct dchannel, dev);
	struct hfc_pci		*hc = dch->hw;
	int			ret = -EINVAL;
	struct mISDNhead	*hh = mISDN_HEAD_P(skb);
	unsigned int		id;
	u_long			flags;

	switch (hh->prim) {
	case PH_DATA_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		ret = dchannel_senddata(dch, skb);
		if (ret > 0) { 
			id = hh->id; 
			hfcpci_fill_dfifo(dch->hw);
			ret = 0;
			spin_unlock_irqrestore(&hc->lock, flags);
			queue_ch_frame(ch, PH_DATA_CNF, id, NULL);
		} else
			spin_unlock_irqrestore(&hc->lock, flags);
		return ret;
	case PH_ACTIVATE_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		if (hc->hw.protocol == ISDN_P_NT_S0) {
			ret = 0;
			if (test_bit(HFC_CFG_MASTER, &hc->cfg))
				hc->hw.mst_m |= HFCPCI_MASTER;
			Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
			if (test_bit(FLG_ACTIVE, &dch->Flags)) {
				spin_unlock_irqrestore(&hc->lock, flags);
				_queue_data(&dch->dev.D, PH_ACTIVATE_IND,
					    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
				break;
			}
			test_and_set_bit(FLG_L2_ACTIVATED, &dch->Flags);
			Write_hfc(hc, HFCPCI_STATES, HFCPCI_ACTIVATE |
				  HFCPCI_DO_ACTION | 1);
		} else
			ret = l1_event(dch->l1, hh->prim);
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	case PH_DEACTIVATE_REQ:
		test_and_clear_bit(FLG_L2_ACTIVATED, &dch->Flags);
		spin_lock_irqsave(&hc->lock, flags);
		if (hc->hw.protocol == ISDN_P_NT_S0) {
			
			Write_hfc(hc, HFCPCI_STATES, 0x40);
			skb_queue_purge(&dch->squeue);
			if (dch->tx_skb) {
				dev_kfree_skb(dch->tx_skb);
				dch->tx_skb = NULL;
			}
			dch->tx_idx = 0;
			if (dch->rx_skb) {
				dev_kfree_skb(dch->rx_skb);
				dch->rx_skb = NULL;
			}
			test_and_clear_bit(FLG_TX_BUSY, &dch->Flags);
			if (test_and_clear_bit(FLG_BUSY_TIMER, &dch->Flags))
				del_timer(&dch->timer);
#ifdef FIXME
			if (test_and_clear_bit(FLG_L1_BUSY, &dch->Flags))
				dchannel_sched_event(&hc->dch, D_CLEARBUSY);
#endif
			hc->hw.mst_m &= ~HFCPCI_MASTER;
			Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
			ret = 0;
		} else {
			ret = l1_event(dch->l1, hh->prim);
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return ret;
}

static int
hfcpci_l2l1B(struct mISDNchannel *ch, struct sk_buff *skb)
{
	struct bchannel		*bch = container_of(ch, struct bchannel, ch);
	struct hfc_pci		*hc = bch->hw;
	int			ret = -EINVAL;
	struct mISDNhead	*hh = mISDN_HEAD_P(skb);
	unsigned int		id;
	u_long			flags;

	switch (hh->prim) {
	case PH_DATA_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		ret = bchannel_senddata(bch, skb);
		if (ret > 0) { 
			id = hh->id; 
			hfcpci_fill_fifo(bch);
			ret = 0;
			spin_unlock_irqrestore(&hc->lock, flags);
			if (!test_bit(FLG_TRANSPARENT, &bch->Flags))
				queue_ch_frame(ch, PH_DATA_CNF, id, NULL);
		} else
			spin_unlock_irqrestore(&hc->lock, flags);
		return ret;
	case PH_ACTIVATE_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		if (!test_and_set_bit(FLG_ACTIVE, &bch->Flags))
			ret = mode_hfcpci(bch, bch->nr, ch->protocol);
		else
			ret = 0;
		spin_unlock_irqrestore(&hc->lock, flags);
		if (!ret)
			_queue_data(ch, PH_ACTIVATE_IND, MISDN_ID_ANY, 0,
				    NULL, GFP_KERNEL);
		break;
	case PH_DEACTIVATE_REQ:
		deactivate_bchannel(bch);
		_queue_data(ch, PH_DEACTIVATE_IND, MISDN_ID_ANY, 0,
			    NULL, GFP_KERNEL);
		ret = 0;
		break;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return ret;
}


static void
inithfcpci(struct hfc_pci *hc)
{
	printk(KERN_DEBUG "inithfcpci: entered\n");
	hc->dch.timer.function = (void *) hfcpci_dbusy_timer;
	hc->dch.timer.data = (long) &hc->dch;
	init_timer(&hc->dch.timer);
	hc->chanlimit = 2;
	mode_hfcpci(&hc->bch[0], 1, -1);
	mode_hfcpci(&hc->bch[1], 2, -1);
}


static int
init_card(struct hfc_pci *hc)
{
	int	cnt = 3;
	u_long	flags;

	printk(KERN_DEBUG "init_card: entered\n");


	spin_lock_irqsave(&hc->lock, flags);
	disable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);
	if (request_irq(hc->irq, hfcpci_int, IRQF_SHARED, "HFC PCI", hc)) {
		printk(KERN_WARNING
		       "mISDN: couldn't get interrupt %d\n", hc->irq);
		return -EIO;
	}
	spin_lock_irqsave(&hc->lock, flags);
	reset_hfcpci(hc);
	while (cnt) {
		inithfcpci(hc);
		enable_hwirq(hc);
		spin_unlock_irqrestore(&hc->lock, flags);
		
		current->state = TASK_UNINTERRUPTIBLE;
		schedule_timeout((80 * HZ) / 1000);
		printk(KERN_INFO "HFC PCI: IRQ %d count %d\n",
		       hc->irq, hc->irqcnt);
		
		spin_lock_irqsave(&hc->lock, flags);
		hc->hw.int_m1 &= ~HFCPCI_INTS_TIMER;
		Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
		
		Write_hfc(hc, HFCPCI_MST_MODE, hc->hw.mst_m);
		if (!hc->irqcnt) {
			printk(KERN_WARNING
			       "HFC PCI: IRQ(%d) getting no interrupts "
			       "during init %d\n", hc->irq, 4 - cnt);
			if (cnt == 1)
				break;
			else {
				reset_hfcpci(hc);
				cnt--;
			}
		} else {
			spin_unlock_irqrestore(&hc->lock, flags);
			hc->initdone = 1;
			return 0;
		}
	}
	disable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);
	free_irq(hc->irq, hc);
	return -EIO;
}

static int
channel_ctrl(struct hfc_pci *hc, struct mISDN_ctrl_req *cq)
{
	int	ret = 0;
	u_char	slot;

	switch (cq->op) {
	case MISDN_CTRL_GETOP:
		cq->op = MISDN_CTRL_LOOP | MISDN_CTRL_CONNECT |
			MISDN_CTRL_DISCONNECT;
		break;
	case MISDN_CTRL_LOOP:
		
		if (cq->channel < 0 || cq->channel > 2) {
			ret = -EINVAL;
			break;
		}
		if (cq->channel & 1) {
			if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg))
				slot = 0xC0;
			else
				slot = 0x80;
			printk(KERN_DEBUG "%s: Write_hfc: B1_SSL/RSL 0x%x\n",
			       __func__, slot);
			Write_hfc(hc, HFCPCI_B1_SSL, slot);
			Write_hfc(hc, HFCPCI_B1_RSL, slot);
			hc->hw.conn = (hc->hw.conn & ~7) | 6;
			Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
		}
		if (cq->channel & 2) {
			if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg))
				slot = 0xC1;
			else
				slot = 0x81;
			printk(KERN_DEBUG "%s: Write_hfc: B2_SSL/RSL 0x%x\n",
			       __func__, slot);
			Write_hfc(hc, HFCPCI_B2_SSL, slot);
			Write_hfc(hc, HFCPCI_B2_RSL, slot);
			hc->hw.conn = (hc->hw.conn & ~0x38) | 0x30;
			Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
		}
		if (cq->channel & 3)
			hc->hw.trm |= 0x80;	
		else {
			hc->hw.conn = (hc->hw.conn & ~0x3f) | 0x09;
			Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
			hc->hw.trm &= 0x7f;	
		}
		Write_hfc(hc, HFCPCI_TRM, hc->hw.trm);
		break;
	case MISDN_CTRL_CONNECT:
		if (cq->channel == cq->p1) {
			ret = -EINVAL;
			break;
		}
		if (cq->channel < 1 || cq->channel > 2 ||
		    cq->p1 < 1 || cq->p1 > 2) {
			ret = -EINVAL;
			break;
		}
		if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg))
			slot = 0xC0;
		else
			slot = 0x80;
		printk(KERN_DEBUG "%s: Write_hfc: B1_SSL/RSL 0x%x\n",
		       __func__, slot);
		Write_hfc(hc, HFCPCI_B1_SSL, slot);
		Write_hfc(hc, HFCPCI_B2_RSL, slot);
		if (test_bit(HFC_CFG_SW_DD_DU, &hc->cfg))
			slot = 0xC1;
		else
			slot = 0x81;
		printk(KERN_DEBUG "%s: Write_hfc: B2_SSL/RSL 0x%x\n",
		       __func__, slot);
		Write_hfc(hc, HFCPCI_B2_SSL, slot);
		Write_hfc(hc, HFCPCI_B1_RSL, slot);
		hc->hw.conn = (hc->hw.conn & ~0x3f) | 0x36;
		Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
		hc->hw.trm |= 0x80;
		Write_hfc(hc, HFCPCI_TRM, hc->hw.trm);
		break;
	case MISDN_CTRL_DISCONNECT:
		hc->hw.conn = (hc->hw.conn & ~0x3f) | 0x09;
		Write_hfc(hc, HFCPCI_CONNECT, hc->hw.conn);
		hc->hw.trm &= 0x7f;	
		break;
	default:
		printk(KERN_WARNING "%s: unknown Op %x\n",
		       __func__, cq->op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int
open_dchannel(struct hfc_pci *hc, struct mISDNchannel *ch,
	      struct channel_req *rq)
{
	int err = 0;

	if (debug & DEBUG_HW_OPEN)
		printk(KERN_DEBUG "%s: dev(%d) open from %p\n", __func__,
		       hc->dch.dev.id, __builtin_return_address(0));
	if (rq->protocol == ISDN_P_NONE)
		return -EINVAL;
	if (rq->adr.channel == 1) {
		
		return -EINVAL;
	}
	if (!hc->initdone) {
		if (rq->protocol == ISDN_P_TE_S0) {
			err = create_l1(&hc->dch, hfc_l1callback);
			if (err)
				return err;
		}
		hc->hw.protocol = rq->protocol;
		ch->protocol = rq->protocol;
		err = init_card(hc);
		if (err)
			return err;
	} else {
		if (rq->protocol != ch->protocol) {
			if (hc->hw.protocol == ISDN_P_TE_S0)
				l1_event(hc->dch.l1, CLOSE_CHANNEL);
			if (rq->protocol == ISDN_P_TE_S0) {
				err = create_l1(&hc->dch, hfc_l1callback);
				if (err)
					return err;
			}
			hc->hw.protocol = rq->protocol;
			ch->protocol = rq->protocol;
			hfcpci_setmode(hc);
		}
	}

	if (((ch->protocol == ISDN_P_NT_S0) && (hc->dch.state == 3)) ||
	    ((ch->protocol == ISDN_P_TE_S0) && (hc->dch.state == 7))) {
		_queue_data(ch, PH_ACTIVATE_IND, MISDN_ID_ANY,
			    0, NULL, GFP_KERNEL);
	}
	rq->ch = ch;
	if (!try_module_get(THIS_MODULE))
		printk(KERN_WARNING "%s:cannot get module\n", __func__);
	return 0;
}

static int
open_bchannel(struct hfc_pci *hc, struct channel_req *rq)
{
	struct bchannel		*bch;

	if (rq->adr.channel == 0 || rq->adr.channel > 2)
		return -EINVAL;
	if (rq->protocol == ISDN_P_NONE)
		return -EINVAL;
	bch = &hc->bch[rq->adr.channel - 1];
	if (test_and_set_bit(FLG_OPEN, &bch->Flags))
		return -EBUSY; 
	test_and_clear_bit(FLG_FILLEMPTY, &bch->Flags);
	bch->ch.protocol = rq->protocol;
	rq->ch = &bch->ch; 
	if (!try_module_get(THIS_MODULE))
		printk(KERN_WARNING "%s:cannot get module\n", __func__);
	return 0;
}

static int
hfc_dctrl(struct mISDNchannel *ch, u_int cmd, void *arg)
{
	struct mISDNdevice	*dev = container_of(ch, struct mISDNdevice, D);
	struct dchannel		*dch = container_of(dev, struct dchannel, dev);
	struct hfc_pci		*hc = dch->hw;
	struct channel_req	*rq;
	int			err = 0;

	if (dch->debug & DEBUG_HW)
		printk(KERN_DEBUG "%s: cmd:%x %p\n",
		       __func__, cmd, arg);
	switch (cmd) {
	case OPEN_CHANNEL:
		rq = arg;
		if ((rq->protocol == ISDN_P_TE_S0) ||
		    (rq->protocol == ISDN_P_NT_S0))
			err = open_dchannel(hc, ch, rq);
		else
			err = open_bchannel(hc, rq);
		break;
	case CLOSE_CHANNEL:
		if (debug & DEBUG_HW_OPEN)
			printk(KERN_DEBUG "%s: dev(%d) close from %p\n",
			       __func__, hc->dch.dev.id,
			       __builtin_return_address(0));
		module_put(THIS_MODULE);
		break;
	case CONTROL_CHANNEL:
		err = channel_ctrl(hc, arg);
		break;
	default:
		if (dch->debug & DEBUG_HW)
			printk(KERN_DEBUG "%s: unknown command %x\n",
			       __func__, cmd);
		return -EINVAL;
	}
	return err;
}

static int
setup_hw(struct hfc_pci *hc)
{
	void	*buffer;

	printk(KERN_INFO "mISDN: HFC-PCI driver %s\n", hfcpci_revision);
	hc->hw.cirm = 0;
	hc->dch.state = 0;
	pci_set_master(hc->pdev);
	if (!hc->irq) {
		printk(KERN_WARNING "HFC-PCI: No IRQ for PCI card found\n");
		return 1;
	}
	hc->hw.pci_io =
		(char __iomem *)(unsigned long)hc->pdev->resource[1].start;

	if (!hc->hw.pci_io) {
		printk(KERN_WARNING "HFC-PCI: No IO-Mem for PCI card found\n");
		return 1;
	}
	
	
	pci_set_dma_mask(hc->pdev, 0xFFFF8000);
	buffer = pci_alloc_consistent(hc->pdev, 0x8000, &hc->hw.dmahandle);
	
	if (!buffer) {
		printk(KERN_WARNING
		       "HFC-PCI: Error allocating memory for FIFO!\n");
		return 1;
	}
	hc->hw.fifos = buffer;
	pci_write_config_dword(hc->pdev, 0x80, hc->hw.dmahandle);
	hc->hw.pci_io = ioremap((ulong) hc->hw.pci_io, 256);
	printk(KERN_INFO
	       "HFC-PCI: defined at mem %#lx fifo %#lx(%#lx) IRQ %d HZ %d\n",
	       (u_long) hc->hw.pci_io, (u_long) hc->hw.fifos,
	       (u_long) hc->hw.dmahandle, hc->irq, HZ);
	
	pci_write_config_word(hc->pdev, PCI_COMMAND, PCI_ENA_MEMIO);
	hc->hw.int_m2 = 0;
	disable_hwirq(hc);
	hc->hw.int_m1 = 0;
	Write_hfc(hc, HFCPCI_INT_M1, hc->hw.int_m1);
	
	
	hc->hw.timer.function = (void *) hfcpci_Timer;
	hc->hw.timer.data = (long) hc;
	init_timer(&hc->hw.timer);
	
	test_and_set_bit(HFC_CFG_MASTER, &hc->cfg);
	return 0;
}

static void
release_card(struct hfc_pci *hc) {
	u_long	flags;

	spin_lock_irqsave(&hc->lock, flags);
	hc->hw.int_m2 = 0; 
	disable_hwirq(hc);
	mode_hfcpci(&hc->bch[0], 1, ISDN_P_NONE);
	mode_hfcpci(&hc->bch[1], 2, ISDN_P_NONE);
	if (hc->dch.timer.function != NULL) {
		del_timer(&hc->dch.timer);
		hc->dch.timer.function = NULL;
	}
	spin_unlock_irqrestore(&hc->lock, flags);
	if (hc->hw.protocol == ISDN_P_TE_S0)
		l1_event(hc->dch.l1, CLOSE_CHANNEL);
	if (hc->initdone)
		free_irq(hc->irq, hc);
	release_io_hfcpci(hc); 
	mISDN_unregister_device(&hc->dch.dev);
	mISDN_freebchannel(&hc->bch[1]);
	mISDN_freebchannel(&hc->bch[0]);
	mISDN_freedchannel(&hc->dch);
	pci_set_drvdata(hc->pdev, NULL);
	kfree(hc);
}

static int
setup_card(struct hfc_pci *card)
{
	int		err = -EINVAL;
	u_int		i;
	char		name[MISDN_MAX_IDLEN];

	card->dch.debug = debug;
	spin_lock_init(&card->lock);
	mISDN_initdchannel(&card->dch, MAX_DFRAME_LEN_L1, ph_state);
	card->dch.hw = card;
	card->dch.dev.Dprotocols = (1 << ISDN_P_TE_S0) | (1 << ISDN_P_NT_S0);
	card->dch.dev.Bprotocols = (1 << (ISDN_P_B_RAW & ISDN_P_B_MASK)) |
		(1 << (ISDN_P_B_HDLC & ISDN_P_B_MASK));
	card->dch.dev.D.send = hfcpci_l2l1D;
	card->dch.dev.D.ctrl = hfc_dctrl;
	card->dch.dev.nrbchan = 2;
	for (i = 0; i < 2; i++) {
		card->bch[i].nr = i + 1;
		set_channelmap(i + 1, card->dch.dev.channelmap);
		card->bch[i].debug = debug;
		mISDN_initbchannel(&card->bch[i], MAX_DATA_MEM);
		card->bch[i].hw = card;
		card->bch[i].ch.send = hfcpci_l2l1B;
		card->bch[i].ch.ctrl = hfc_bctrl;
		card->bch[i].ch.nr = i + 1;
		list_add(&card->bch[i].ch.list, &card->dch.dev.bchannels);
	}
	err = setup_hw(card);
	if (err)
		goto error;
	snprintf(name, MISDN_MAX_IDLEN - 1, "hfc-pci.%d", HFC_cnt + 1);
	err = mISDN_register_device(&card->dch.dev, &card->pdev->dev, name);
	if (err)
		goto error;
	HFC_cnt++;
	printk(KERN_INFO "HFC %d cards installed\n", HFC_cnt);
	return 0;
error:
	mISDN_freebchannel(&card->bch[1]);
	mISDN_freebchannel(&card->bch[0]);
	mISDN_freedchannel(&card->dch);
	kfree(card);
	return err;
}

struct _hfc_map {
	u_int	subtype;
	u_int	flag;
	char	*name;
};

static const struct _hfc_map hfc_map[] =
{
	{HFC_CCD_2BD0, 0, "CCD/Billion/Asuscom 2BD0"},
	{HFC_CCD_B000, 0, "Billion B000"},
	{HFC_CCD_B006, 0, "Billion B006"},
	{HFC_CCD_B007, 0, "Billion B007"},
	{HFC_CCD_B008, 0, "Billion B008"},
	{HFC_CCD_B009, 0, "Billion B009"},
	{HFC_CCD_B00A, 0, "Billion B00A"},
	{HFC_CCD_B00B, 0, "Billion B00B"},
	{HFC_CCD_B00C, 0, "Billion B00C"},
	{HFC_CCD_B100, 0, "Seyeon B100"},
	{HFC_CCD_B700, 0, "Primux II S0 B700"},
	{HFC_CCD_B701, 0, "Primux II S0 NT B701"},
	{HFC_ABOCOM_2BD1, 0, "Abocom/Magitek 2BD1"},
	{HFC_ASUS_0675, 0, "Asuscom/Askey 675"},
	{HFC_BERKOM_TCONCEPT, 0, "German telekom T-Concept"},
	{HFC_BERKOM_A1T, 0, "German telekom A1T"},
	{HFC_ANIGMA_MC145575, 0, "Motorola MC145575"},
	{HFC_ZOLTRIX_2BD0, 0, "Zoltrix 2BD0"},
	{HFC_DIGI_DF_M_IOM2_E, 0,
	 "Digi International DataFire Micro V IOM2 (Europe)"},
	{HFC_DIGI_DF_M_E, 0,
	 "Digi International DataFire Micro V (Europe)"},
	{HFC_DIGI_DF_M_IOM2_A, 0,
	 "Digi International DataFire Micro V IOM2 (North America)"},
	{HFC_DIGI_DF_M_A, 0,
	 "Digi International DataFire Micro V (North America)"},
	{HFC_SITECOM_DC105V2, 0, "Sitecom Connectivity DC-105 ISDN TA"},
	{},
};

static struct pci_device_id hfc_ids[] =
{
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_2BD0),
	  (unsigned long) &hfc_map[0] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B000),
	  (unsigned long) &hfc_map[1] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B006),
	  (unsigned long) &hfc_map[2] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B007),
	  (unsigned long) &hfc_map[3] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B008),
	  (unsigned long) &hfc_map[4] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B009),
	  (unsigned long) &hfc_map[5] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B00A),
	  (unsigned long) &hfc_map[6] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B00B),
	  (unsigned long) &hfc_map[7] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B00C),
	  (unsigned long) &hfc_map[8] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B100),
	  (unsigned long) &hfc_map[9] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B700),
	  (unsigned long) &hfc_map[10] },
	{ PCI_VDEVICE(CCD, PCI_DEVICE_ID_CCD_B701),
	  (unsigned long) &hfc_map[11] },
	{ PCI_VDEVICE(ABOCOM, PCI_DEVICE_ID_ABOCOM_2BD1),
	  (unsigned long) &hfc_map[12] },
	{ PCI_VDEVICE(ASUSTEK, PCI_DEVICE_ID_ASUSTEK_0675),
	  (unsigned long) &hfc_map[13] },
	{ PCI_VDEVICE(BERKOM, PCI_DEVICE_ID_BERKOM_T_CONCEPT),
	  (unsigned long) &hfc_map[14] },
	{ PCI_VDEVICE(BERKOM, PCI_DEVICE_ID_BERKOM_A1T),
	  (unsigned long) &hfc_map[15] },
	{ PCI_VDEVICE(ANIGMA, PCI_DEVICE_ID_ANIGMA_MC145575),
	  (unsigned long) &hfc_map[16] },
	{ PCI_VDEVICE(ZOLTRIX, PCI_DEVICE_ID_ZOLTRIX_2BD0),
	  (unsigned long) &hfc_map[17] },
	{ PCI_VDEVICE(DIGI, PCI_DEVICE_ID_DIGI_DF_M_IOM2_E),
	  (unsigned long) &hfc_map[18] },
	{ PCI_VDEVICE(DIGI, PCI_DEVICE_ID_DIGI_DF_M_E),
	  (unsigned long) &hfc_map[19] },
	{ PCI_VDEVICE(DIGI, PCI_DEVICE_ID_DIGI_DF_M_IOM2_A),
	  (unsigned long) &hfc_map[20] },
	{ PCI_VDEVICE(DIGI, PCI_DEVICE_ID_DIGI_DF_M_A),
	  (unsigned long) &hfc_map[21] },
	{ PCI_VDEVICE(SITECOM, PCI_DEVICE_ID_SITECOM_DC105V2),
	  (unsigned long) &hfc_map[22] },
	{},
};

static int __devinit
hfc_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int		err = -ENOMEM;
	struct hfc_pci	*card;
	struct _hfc_map	*m = (struct _hfc_map *)ent->driver_data;

	card = kzalloc(sizeof(struct hfc_pci), GFP_ATOMIC);
	if (!card) {
		printk(KERN_ERR "No kmem for HFC card\n");
		return err;
	}
	card->pdev = pdev;
	card->subtype = m->subtype;
	err = pci_enable_device(pdev);
	if (err) {
		kfree(card);
		return err;
	}

	printk(KERN_INFO "mISDN_hfcpci: found adapter %s at %s\n",
	       m->name, pci_name(pdev));

	card->irq = pdev->irq;
	pci_set_drvdata(pdev, card);
	err = setup_card(card);
	if (err)
		pci_set_drvdata(pdev, NULL);
	return err;
}

static void __devexit
hfc_remove_pci(struct pci_dev *pdev)
{
	struct hfc_pci	*card = pci_get_drvdata(pdev);

	if (card)
		release_card(card);
	else
		if (debug)
			printk(KERN_DEBUG "%s: drvdata already removed\n",
			       __func__);
}


static struct pci_driver hfc_driver = {
	.name = "hfcpci",
	.probe = hfc_probe,
	.remove = __devexit_p(hfc_remove_pci),
	.id_table = hfc_ids,
};

static int
_hfcpci_softirq(struct device *dev, void *arg)
{
	struct hfc_pci  *hc = dev_get_drvdata(dev);
	struct bchannel *bch;
	if (hc == NULL)
		return 0;

	if (hc->hw.int_m2 & HFCPCI_IRQ_ENABLE) {
		spin_lock(&hc->lock);
		bch = Sel_BCS(hc, hc->hw.bswapped ? 2 : 1);
		if (bch && bch->state == ISDN_P_B_RAW) { 
			main_rec_hfcpci(bch);
			tx_birq(bch);
		}
		bch = Sel_BCS(hc, hc->hw.bswapped ? 1 : 2);
		if (bch && bch->state == ISDN_P_B_RAW) { 
			main_rec_hfcpci(bch);
			tx_birq(bch);
		}
		spin_unlock(&hc->lock);
	}
	return 0;
}

static void
hfcpci_softirq(void *arg)
{
	(void) driver_for_each_device(&hfc_driver.driver, NULL, arg,
				      _hfcpci_softirq);

	
	if ((s32)(hfc_jiffies + tics - jiffies) <= 0)
		hfc_jiffies = jiffies + 1;
	else
		hfc_jiffies += tics;
	hfc_tl.expires = hfc_jiffies;
	add_timer(&hfc_tl);
}

static int __init
HFC_init(void)
{
	int		err;

	if (!poll)
		poll = HFCPCI_BTRANS_THRESHOLD;

	if (poll != HFCPCI_BTRANS_THRESHOLD) {
		tics = (poll * HZ) / 8000;
		if (tics < 1)
			tics = 1;
		poll = (tics * 8000) / HZ;
		if (poll > 256 || poll < 8) {
			printk(KERN_ERR "%s: Wrong poll value %d not in range "
			       "of 8..256.\n", __func__, poll);
			err = -EINVAL;
			return err;
		}
	}
	if (poll != HFCPCI_BTRANS_THRESHOLD) {
		printk(KERN_INFO "%s: Using alternative poll value of %d\n",
		       __func__, poll);
		hfc_tl.function = (void *)hfcpci_softirq;
		hfc_tl.data = 0;
		init_timer(&hfc_tl);
		hfc_tl.expires = jiffies + tics;
		hfc_jiffies = hfc_tl.expires;
		add_timer(&hfc_tl);
	} else
		tics = 0; 

	err = pci_register_driver(&hfc_driver);
	if (err) {
		if (timer_pending(&hfc_tl))
			del_timer(&hfc_tl);
	}

	return err;
}

static void __exit
HFC_cleanup(void)
{
	if (timer_pending(&hfc_tl))
		del_timer(&hfc_tl);

	pci_unregister_driver(&hfc_driver);
}

module_init(HFC_init);
module_exit(HFC_cleanup);

MODULE_DEVICE_TABLE(pci, hfc_ids);
