/*
 * intel_mid_ssp_spi.c
 * This driver supports Bulverde SSP core used on Intel MID platforms
 * It supports SSP of Moorestown & Medfield platforms and handles clock
 * slave & master modes.
 *
 * Copyright (c) 2010, Intel Corporation.
 *  Ken Mills <ken.k.mills@intel.com>
 *  Sylvain Centelles <sylvain.centelles@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*
 * Note:
 *
 * Supports DMA and non-interrupt polled transfers.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/highmem.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/intel_mid_dma.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/completion.h>
#include <asm/intel-mid.h>

#include <linux/spi/spi.h>
#include <linux/spi/intel_mid_ssp_spi.h>

#define DRIVER_NAME "intel_mid_ssp_spi_unified"

MODULE_AUTHOR("Ken Mills");
MODULE_DESCRIPTION("Bulverde SSP core SPI contoller");
MODULE_LICENSE("GPL");

static int ssp_timing_wr;

#ifdef DUMP_RX
static void dump_trailer(const struct device *dev, char *buf, int len, int sz)
{
	int tlen1 = (len < sz ? len : sz);
	int tlen2 =  ((len - sz) > sz) ? sz : (len - sz);
	unsigned char *p;
	static char msg[MAX_SPI_TRANSFER_SIZE];

	memset(msg, '\0', sizeof(msg));
	p = buf;
	while (p < buf + tlen1)
		sprintf(msg, "%s%02x", msg, (unsigned int)*p++);

	if (tlen2 > 0) {
		sprintf(msg, "%s .....", msg);
		p = (buf+len) - tlen2;
		while (p < buf + len)
			sprintf(msg, "%s%02x", msg, (unsigned int)*p++);
	}

	dev_info(dev, "DUMP: %p[0:%d ... %d:%d]:%s", buf, tlen1 - 1,
		   len-tlen2, len - 1, msg);
}
#endif

static inline u8 ssp_cfg_get_mode(u8 ssp_cfg)
{
	if (intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER ||
	    intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_ANNIEDALE)
		return (ssp_cfg) & 0x03;
	else
		return (ssp_cfg) & 0x07;
}

static inline u8 ssp_cfg_get_spi_bus_nb(u8 ssp_cfg)
{
	if (intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER ||
	    intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_ANNIEDALE)
		return ((ssp_cfg) >> 2) & 0x07;
	else
		return ((ssp_cfg) >> 3) & 0x07;
}

static inline u8 ssp_cfg_is_spi_slave(u8 ssp_cfg)
{
	if (intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER ||
	    intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_ANNIEDALE)
		return (ssp_cfg) & 0x20;
	else
		return (ssp_cfg) & 0x40;
}

static inline u32 is_tx_fifo_empty(struct ssp_drv_context *sspc)
{
	u32 sssr;
	sssr = read_SSSR(sspc->ioaddr);
	if ((sssr & SSSR_TFL_MASK) || (sssr & SSSR_TNF) == 0)
		return 0;
	else
		return 1;
}

static inline u32 is_rx_fifo_empty(struct ssp_drv_context *sspc)
{
	return ((read_SSSR(sspc->ioaddr) & SSSR_RNE) == 0);
}

static inline void disable_interface(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	write_SSCR0(read_SSCR0(reg) & ~SSCR0_SSE, reg);
}

static inline void disable_triggers(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	write_SSCR1(read_SSCR1(reg) & ~sspc->cr1_sig, reg);
}


static void flush(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	u32 i = 0;

	/* If the transmit fifo is not empty, reset the interface. */
	if (!is_tx_fifo_empty(sspc)) {
		dev_err(&sspc->pdev->dev, "TX FIFO not empty. Reset of SPI IF");
		disable_interface(sspc);
		return;
	}

	dev_dbg(&sspc->pdev->dev, " SSSR=%x\r\n", read_SSSR(reg));
	while (!is_rx_fifo_empty(sspc) && (i < SPI_FIFO_SIZE + 1)) {
		read_SSDR(reg);
		i++;
	}
	WARN(i > 0, "%d words flush occured\n", i);

	return;
}

static int null_writer(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	u8 n_bytes = sspc->n_bytes;

	if (((read_SSSR(reg) & SSSR_TFL_MASK) == SSSR_TFL_MASK)
		|| (sspc->tx == sspc->tx_end))
		return 0;

	write_SSDR(0, reg);
	sspc->tx += n_bytes;

	return n_bytes;
}

static int null_reader(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	u8 n_bytes = sspc->n_bytes;
	size_t pkg_len = sspc->len;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (pkg_len > 0)) {
		read_SSDR(reg);
		sspc->rx += n_bytes;
		pkg_len -= n_bytes;
	}

	return sspc->rx == sspc->rx_end;
}

static int u8_writer(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	if (((read_SSSR(reg) & SSSR_TFL_MASK) == SSSR_TFL_MASK)
		|| (sspc->tx == sspc->tx_end))
		return 0;

	write_SSDR(*(u8 *)(sspc->tx), reg);
	++sspc->tx;

	return 1;
}

static int u8_reader(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	size_t pkg_len = sspc->len;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (pkg_len > 0)) {
		*(u8 *)(sspc->rx) = read_SSDR(reg);
		++sspc->rx;
		--pkg_len;
	}

	return sspc->rx == sspc->rx_end;
}

static int u16_writer(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	if (((read_SSSR(reg) & SSSR_TFL_MASK) == SSSR_TFL_MASK)
		|| (sspc->tx == sspc->tx_end))
		return 0;

	write_SSDR(*(u16 *)(sspc->tx), reg);
	sspc->tx += 2;

	return 2;
}

static int u16_reader(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	size_t pkg_len = sspc->len;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (pkg_len > 0)) {
		*(u16 *)(sspc->rx) = read_SSDR(reg);
		sspc->rx += 2;
		pkg_len -= 2;
	}

	return sspc->rx == sspc->rx_end;
}

static int u32_writer(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	if (((read_SSSR(reg) & SSSR_TFL_MASK) == SSSR_TFL_MASK)
		|| (sspc->tx == sspc->tx_end))
		return 0;

	write_SSDR(*(u32 *)(sspc->tx), reg);
	sspc->tx += 4;

	return 4;
}

static int u32_reader(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	size_t pkg_len = sspc->len;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (pkg_len > 0)) {
		*(u32 *)(sspc->rx) = read_SSDR(reg);
		sspc->rx += 4;
		pkg_len -= 4;
	}

	return sspc->rx == sspc->rx_end;
}

static bool chan_filter(struct dma_chan *chan, void *param)
{
	struct ssp_drv_context *sspc = param;
	bool ret = false;

	if (!sspc->dmac1)
		return ret;

	if (chan->device->dev == &sspc->dmac1->dev)
		ret = true;

	return ret;
}

/**
 * unmap_dma_buffers() - Unmap the DMA buffers used during the last transfer.
 * @sspc:	Pointer to the private driver context
 */
static void unmap_dma_buffers(struct ssp_drv_context *sspc)
{
	struct device *dev = &sspc->pdev->dev;

	if (!sspc->dma_mapped)
		return;
	dma_unmap_single(dev, sspc->rx_dma, sspc->len, PCI_DMA_FROMDEVICE);
	dma_unmap_single(dev, sspc->tx_dma, sspc->len, PCI_DMA_TODEVICE);
	sspc->dma_mapped = 0;
}

/**
 * intel_mid_ssp_spi_dma_done() - End of DMA transfer callback
 * @arg:	Pointer to the data provided at callback registration
 *
 * This function is set as callback for both RX and TX DMA transfers. The
 * RX or TX 'done' flag is set acording to the direction of the ended
 * transfer. Then, if both RX and TX flags are set, it means that the
 * transfer job is completed.
 */
static void intel_mid_ssp_spi_dma_done(void *arg)
{
	struct callback_param *cb_param = (struct callback_param *)arg;
	struct ssp_drv_context *sspc = cb_param->drv_context;
	struct device *dev = &sspc->pdev->dev;
	void *reg = sspc->ioaddr;

	if (cb_param->direction == TX_DIRECTION) {
		dma_sync_single_for_cpu(dev, sspc->tx_dma,
			sspc->len, DMA_TO_DEVICE);
		sspc->txdma_done = 1;
	} else {
		sspc->rxdma_done = 1;
		dma_sync_single_for_cpu(dev, sspc->rx_dma,
			sspc->len, DMA_FROM_DEVICE);
	}

	dev_dbg(dev, "DMA callback for direction %d [RX done:%d] [TX done:%d]\n",
		cb_param->direction, sspc->rxdma_done,
		sspc->txdma_done);

	if (sspc->txdma_done && sspc->rxdma_done) {
		/* Clear Status Register */
		write_SSSR(sspc->clear_sr, reg);
		dev_dbg(dev, "DMA done\n");
		/* Disable Triggers to DMA or to CPU*/
		disable_triggers(sspc);
		unmap_dma_buffers(sspc);

		queue_work(sspc->dma_wq, &sspc->complete_work);
	}
}

/**
 * intel_mid_ssp_spi_dma_init() - Initialize DMA
 * @sspc:	Pointer to the private driver context
 *
 * This function is called at driver setup phase to allocate DMA
 * ressources.
 */
static void intel_mid_ssp_spi_dma_init(struct ssp_drv_context *sspc)
{
	struct intel_mid_dma_slave *rxs, *txs;
	struct dma_slave_config *ds;
	dma_cap_mask_t mask;
	struct device *dev = &sspc->pdev->dev;
	unsigned int device_id;

	/* Configure RX channel parameters */
	rxs = &sspc->dmas_rx;
	ds = &rxs->dma_slave;

	ds->direction = DMA_FROM_DEVICE;
	rxs->hs_mode = LNW_DMA_HW_HS;
	rxs->cfg_mode = LNW_DMA_PER_TO_MEM;
	ds->dst_addr_width = sspc->n_bytes;
	ds->src_addr_width = sspc->n_bytes;

	if (sspc->quirks & QUIRKS_PLATFORM_BYT) {
		/*These are fixed HW info from Baytrail datasheet*/
		rxs->device_instance = 1; /*DMA Req line*/
	} else if (sspc->quirks & QUIRKS_PLATFORM_MRFL)
		rxs->device_instance = sspc->master->bus_num;
	else
		rxs->device_instance = 0;

	/* Use a DMA burst according to the FIFO thresholds */
	if (sspc->rx_fifo_threshold == 8) {
		ds->src_maxburst = LNW_DMA_MSIZE_8;
		ds->dst_maxburst = LNW_DMA_MSIZE_8;
	} else if (sspc->rx_fifo_threshold == 4) {
		ds->src_maxburst = LNW_DMA_MSIZE_4;
		ds->dst_maxburst = LNW_DMA_MSIZE_4;
	} else {
		ds->src_maxburst = LNW_DMA_MSIZE_1;
		ds->dst_maxburst = LNW_DMA_MSIZE_1;
	}

	/* Configure TX channel parameters */
	txs = &sspc->dmas_tx;
	ds = &txs->dma_slave;

	ds->direction = DMA_TO_DEVICE;
	txs->hs_mode = LNW_DMA_HW_HS;
	txs->cfg_mode = LNW_DMA_MEM_TO_PER;
	ds->src_addr_width = sspc->n_bytes;
	ds->dst_addr_width = sspc->n_bytes;

	if (sspc->quirks & QUIRKS_PLATFORM_BYT) {
		/*These are fixed HW info from Baytrail datasheet*/
		txs->device_instance = 0;/*DMA Req Line*/
	} else if (sspc->quirks & QUIRKS_PLATFORM_MRFL)
		txs->device_instance = sspc->master->bus_num;
	else
		txs->device_instance = 0;

	/* Use a DMA burst according to the FIFO thresholds */
	if (sspc->rx_fifo_threshold == 8) {
		ds->src_maxburst = LNW_DMA_MSIZE_8;
		ds->dst_maxburst = LNW_DMA_MSIZE_8;
	} else if (sspc->rx_fifo_threshold == 4) {
		ds->src_maxburst = LNW_DMA_MSIZE_4;
		ds->dst_maxburst = LNW_DMA_MSIZE_4;
	} else {
		ds->src_maxburst = LNW_DMA_MSIZE_1;
		ds->dst_maxburst = LNW_DMA_MSIZE_1;
	}

	/* Nothing more to do if already initialized */
	if (sspc->dma_initialized)
		return;

	/* Use DMAC1 */
	if (sspc->quirks & QUIRKS_PLATFORM_MRST)
		device_id = PCI_MRST_DMAC1_ID;
	else if (sspc->quirks & QUIRKS_PLATFORM_BYT)
		device_id = PCI_BYT_DMAC1_ID;
	else if (sspc->quirks & QUIRKS_PLATFORM_MRFL)
		device_id = PCI_MRFL_DMAC_ID;
	else
		device_id = PCI_MDFL_DMAC1_ID;

	sspc->dmac1 = pci_get_device(PCI_VENDOR_ID_INTEL, device_id, NULL);
	if (!sspc->dmac1) {
		dev_err(dev, "Can't find DMAC1");
		return;
	}

	if (sspc->quirks & QUIRKS_SRAM_ADDITIONAL_CPY) {
		sspc->virt_addr_sram_rx = ioremap_nocache(SRAM_BASE_ADDR,
				2 * MAX_SPI_TRANSFER_SIZE);
		if (sspc->virt_addr_sram_rx)
			sspc->virt_addr_sram_tx = sspc->virt_addr_sram_rx +
							MAX_SPI_TRANSFER_SIZE;
		else
			dev_err(dev, "Virt_addr_sram_rx is null\n");
	}

	/* 1. Allocate rx channel */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	dma_cap_set(DMA_SLAVE, mask);

	sspc->rxchan = dma_request_channel(mask, chan_filter, sspc);
	if (!sspc->rxchan)
		goto err_exit;

	sspc->rxchan->private = rxs;

	/* 2. Allocate tx channel */
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_MEMCPY, mask);

	sspc->txchan = dma_request_channel(mask, chan_filter, sspc);
	if (!sspc->txchan)
		goto free_rxchan;
	else
		sspc->txchan->private = txs;

	/* set the dma done bit to 1 */
	sspc->txdma_done = 1;
	sspc->rxdma_done = 1;

	sspc->tx_param.drv_context  = sspc;
	sspc->tx_param.direction = TX_DIRECTION;
	sspc->rx_param.drv_context  = sspc;
	sspc->rx_param.direction = RX_DIRECTION;

	sspc->dma_initialized = 1;
	return;

free_rxchan:
	dma_release_channel(sspc->rxchan);
err_exit:
	dev_err(dev, "Error : DMA Channel Not available\n");

	if (sspc->quirks & QUIRKS_SRAM_ADDITIONAL_CPY)
		iounmap(sspc->virt_addr_sram_rx);

	pci_dev_put(sspc->dmac1);
	return;
}

/**
 * intel_mid_ssp_spi_dma_exit() - Release DMA ressources
 * @sspc:	Pointer to the private driver context
 */
static void intel_mid_ssp_spi_dma_exit(struct ssp_drv_context *sspc)
{
	dma_release_channel(sspc->txchan);
	dma_release_channel(sspc->rxchan);

	if (sspc->quirks & QUIRKS_SRAM_ADDITIONAL_CPY)
		iounmap(sspc->virt_addr_sram_rx);

	pci_dev_put(sspc->dmac1);
}

/**
 * dma_transfer() - Initiate a DMA transfer
 * @sspc:	Pointer to the private driver context
 */
static void dma_transfer(struct ssp_drv_context *sspc)
{
	dma_addr_t ssdr_addr;
	struct dma_async_tx_descriptor *txdesc = NULL, *rxdesc = NULL;
	struct dma_chan *txchan, *rxchan;
	enum dma_ctrl_flags flag;
	struct device *dev = &sspc->pdev->dev;

	/* get Data Read/Write address */
	ssdr_addr = (dma_addr_t)(sspc->paddr + 0x10);

	if (sspc->tx_dma)
		sspc->txdma_done = 0;

	if (sspc->rx_dma)
		sspc->rxdma_done = 0;

	/* 2. prepare the RX dma transfer */
	txchan = sspc->txchan;
	rxchan = sspc->rxchan;

	flag = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;

	if (likely(sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL)) {
		/* Since the DMA is configured to do 32bits access */
		/* to/from the DDR, the DMA transfer size must be  */
		/* a multiple of 4 bytes                           */
		sspc->len_dma_rx = sspc->len & ~(4 - 1);
		sspc->len_dma_tx = sspc->len_dma_rx;

		/* In Rx direction, TRAIL Bytes are handled by memcpy */
		if (sspc->rx_dma &&
			(sspc->len_dma_rx >
				sspc->rx_fifo_threshold * sspc->n_bytes))
		{
			sspc->len_dma_rx = TRUNCATE(sspc->len_dma_rx,
				sspc->rx_fifo_threshold * sspc->n_bytes);
			sspc->len_dma_tx = sspc->len_dma_rx;
		}
		else if (!sspc->rx_dma)
			dev_err(dev, "ERROR : rx_dma is null\r\n");
	} else {
		/* TRAIL Bytes are handled by DMA */
		if (sspc->rx_dma) {
			sspc->len_dma_rx = sspc->len;
			sspc->len_dma_tx = sspc->len;
		} else
			dev_err(dev, "ERROR : sspc->rx_dma is null!\n");
	}

	sspc->dmas_rx.dma_slave.src_addr = ssdr_addr;
	rxchan->device->device_control(rxchan, DMA_SLAVE_CONFIG,
		(unsigned long)&(sspc->dmas_rx.dma_slave));
	dma_sync_single_for_device(dev, sspc->rx_dma,
		sspc->len, DMA_FROM_DEVICE);

	rxdesc = rxchan->device->device_prep_dma_memcpy
		(rxchan,			/* DMA Channel */
		sspc->rx_dma,			/* DAR */
		ssdr_addr,			/* SAR */
		sspc->len_dma_rx,		/* Data Length */
		flag);					/* Flag */

	if (rxdesc) {
		rxdesc->callback = intel_mid_ssp_spi_dma_done;
		rxdesc->callback_param = &sspc->rx_param;
	} else {
		dev_dbg(dev, "rxdesc is null! (len_dma_rx:%d)\n",
			sspc->len_dma_rx);
		sspc->rxdma_done = 1;
	}

	/* 3. prepare the TX dma transfer */
	sspc->dmas_tx.dma_slave.dst_addr = ssdr_addr;
	txchan->device->device_control(txchan, DMA_SLAVE_CONFIG,
		(unsigned long)&(sspc->dmas_tx.dma_slave));
	dma_sync_single_for_device(dev, sspc->tx_dma,
		sspc->len, DMA_TO_DEVICE);

	if (sspc->tx_dma) {
		txdesc = txchan->device->device_prep_dma_memcpy
			(txchan,			/* DMA Channel */
			ssdr_addr,			/* DAR */
			sspc->tx_dma,			/* SAR */
			sspc->len_dma_tx,		/* Data Length */
			flag);				/* Flag */
		if (txdesc) {
			txdesc->callback = intel_mid_ssp_spi_dma_done;
			txdesc->callback_param = &sspc->tx_param;
		} else {
			dev_dbg(dev, "txdesc is null! (len_dma_tx:%d)\n",
				sspc->len_dma_tx);
			sspc->txdma_done = 1;
		}
	} else {
		dev_err(dev, "ERROR : sspc->tx_dma is null!\n");
		return;
	}

	dev_dbg(dev, "DMA transfer len:%d len_dma_tx:%d len_dma_rx:%d\n",
		sspc->len, sspc->len_dma_tx, sspc->len_dma_rx);

	if (rxdesc || txdesc) {
		if (rxdesc) {
			dev_dbg(dev, "Firing DMA RX channel\n");
			rxdesc->tx_submit(rxdesc);
		}
		if (txdesc) {
			dev_dbg(dev, "Firing DMA TX channel\n");
			txdesc->tx_submit(txdesc);
		}
	} else {
		struct callback_param cb_param;
		cb_param.drv_context = sspc;
		dev_dbg(dev, "Bypassing DMA transfer\n");
		intel_mid_ssp_spi_dma_done(&cb_param);
	}
}

/**
 * map_dma_buffers() - Map DMA buffer before a transfer
 * @sspc:	Pointer to the private drivzer context
 */
static int map_dma_buffers(struct ssp_drv_context *sspc)
{
	struct device *dev = &sspc->pdev->dev;

	if (unlikely(sspc->dma_mapped)) {
		dev_err(dev, "ERROR : DMA buffers already mapped\n");
		return 0;
	}
	if (unlikely(sspc->quirks & QUIRKS_SRAM_ADDITIONAL_CPY)) {
		/* Copy sspc->tx into sram_tx */
		memcpy_toio(sspc->virt_addr_sram_tx, sspc->tx, sspc->len);
#ifdef DUMP_RX
		dump_trailer(&sspc->pdev->dev, sspc->tx, sspc->len, 16);
#endif
		sspc->rx_dma = SRAM_RX_ADDR;
		sspc->tx_dma = SRAM_TX_ADDR;
	} else {
		/* no QUIRKS_SRAM_ADDITIONAL_CPY */
		if (unlikely(sspc->dma_mapped))
			return 1;

		sspc->tx_dma = dma_map_single(dev, sspc->tx, sspc->len,
						PCI_DMA_TODEVICE);
		if (unlikely(dma_mapping_error(dev, sspc->tx_dma))) {
			dev_err(dev, "ERROR : tx dma mapping failed\n");
			return 0;
		}

		sspc->rx_dma = dma_map_single(dev, sspc->rx, sspc->len,
						PCI_DMA_FROMDEVICE);
		if (unlikely(dma_mapping_error(dev, sspc->rx_dma))) {
			dma_unmap_single(dev, sspc->tx_dma,
				sspc->len, DMA_TO_DEVICE);
			dev_err(dev, "ERROR : rx dma mapping failed\n");
			return 0;
		}
	}
	return 1;
}

/**
 * drain_trail() - Handle trailing bytes of a transfer
 * @sspc:	Pointer to the private driver context
 *
 * This function handles the trailing bytes of a transfer for the case
 * they are not handled by the DMA.
 */
void drain_trail(struct ssp_drv_context *sspc)
{
	struct device *dev = &sspc->pdev->dev;
	void *reg = sspc->ioaddr;

	if (sspc->len != sspc->len_dma_rx) {
		dev_dbg(dev, "Handling trailing bytes. SSSR:%08x\n",
			read_SSSR(reg));
		sspc->rx += sspc->len_dma_rx;
		sspc->tx += sspc->len_dma_tx;
		sspc->len = sspc->len - sspc->len_dma_rx;
		sspc->cur_msg->actual_length = sspc->len_dma_rx;

		while ((sspc->tx < sspc->tx_end) ||
			(sspc->rx < sspc->rx_end)) {
			sspc->read(sspc);
			sspc->write(sspc);
		}
	}
}

/**
 * sram_to_ddr_cpy() - Copy data from Langwell SDRAM to DDR
 * @sspc:	Pointer to the private driver context
 */
static void sram_to_ddr_cpy(struct ssp_drv_context *sspc)
{
	u32 length = sspc->len;

	if ((sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL)
		&& (sspc->len > sspc->rx_fifo_threshold * sspc->n_bytes))
		length = TRUNCATE(sspc->len,
			sspc->rx_fifo_threshold * sspc->n_bytes);

	memcpy_fromio(sspc->rx, sspc->virt_addr_sram_rx, length);
}

static void int_transfer_complete(struct ssp_drv_context *sspc)
{
	void *reg = sspc->ioaddr;
	struct spi_message *msg;
	struct device *dev = &sspc->pdev->dev;

	if (unlikely(sspc->quirks & QUIRKS_USE_PM_QOS))
		pm_qos_update_request(&sspc->pm_qos_req,
					PM_QOS_DEFAULT_VALUE);

	if (unlikely(sspc->quirks & QUIRKS_SRAM_ADDITIONAL_CPY))
		sram_to_ddr_cpy(sspc);

	if (likely(sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL))
		drain_trail(sspc);
	else
		/* Stop getting Time Outs */
		write_SSTO(0, reg);

	sspc->cur_msg->status = 0;
	sspc->cur_msg->actual_length += sspc->len;

#ifdef DUMP_RX
	dump_trailer(dev, sspc->rx, sspc->len, 16);
#endif

	if (sspc->cs_control)
		sspc->cs_control(!sspc->cs_assert);

	dev_dbg(dev, "End of transfer. SSSR:%08X\n", read_SSSR(reg));
	complete(&sspc->msg_done);
}

static void int_transfer_complete_work(struct work_struct *work)
{
	struct ssp_drv_context *sspc = container_of(work,
				struct ssp_drv_context, complete_work);

	int_transfer_complete(sspc);
}

static void poll_transfer_complete(struct ssp_drv_context *sspc)
{
	/* Update total byte transfered return count actual bytes read */
	sspc->cur_msg->actual_length += sspc->len - (sspc->rx_end - sspc->rx);

	sspc->cur_msg->status = 0;
}

/**
 * ssp_int() - Interrupt handler
 * @irq
 * @dev_id
 *
 * The SSP interrupt is not used for transfer which are handled by
 * DMA or polling: only under/over run are catched to detect
 * broken transfers.
 */
static irqreturn_t ssp_int(int irq, void *dev_id)
{
	struct ssp_drv_context *sspc = dev_id;
	void *reg = sspc->ioaddr;
	struct device *dev = &sspc->pdev->dev;
	u32 status = read_SSSR(reg);

	/* It should never be our interrupt since SSP will */
	/* only trigs interrupt for under/over run.*/
	if (likely(!(status & sspc->mask_sr)))
		return IRQ_NONE;

	if (status & SSSR_ROR || status & SSSR_TUR) {
		dev_err(dev, "--- SPI ROR or TUR occurred : SSSR=%x\n",	status);
		WARN_ON(1);
		if (status & SSSR_ROR)
			dev_err(dev, "we have Overrun\n");
		if (status & SSSR_TUR)
			dev_err(dev, "we have Underrun\n");
	}

	/* We can fall here when not using DMA mode */
	if (!sspc->cur_msg) {
		disable_interface(sspc);
		disable_triggers(sspc);
	}
	/* clear status register */
	write_SSSR(sspc->clear_sr, reg);
	return IRQ_HANDLED;
}

static void poll_writer(struct work_struct *work)
{
	struct ssp_drv_context *sspc =
		container_of(work, struct ssp_drv_context, poll_write);
	struct device *dev = &sspc->pdev->dev;
	size_t pkg_len = sspc->len;
	int ret;

	while ((pkg_len > 0)) {
		ret = sspc->write(sspc);
		pkg_len -= ret;
	}
}

/*
 * Perform a single transfer.
 */
static void poll_transfer(unsigned long data)
{
	struct ssp_drv_context *sspc = (void *)data;

	while (!sspc->read(sspc))
		cpu_relax();

	poll_transfer_complete(sspc);
}

/**
 * start_bitbanging() - Clock synchronization by bit banging
 * @sspc:	Pointer to private driver context
 *
 * This clock synchronization will be removed as soon as it is
 * handled by the SCU.
 */
static void start_bitbanging(struct ssp_drv_context *sspc)
{
	u32 sssr;
	u32 count = 0;
	u32 cr0;
	void *i2c_reg = sspc->I2C_ioaddr;
	struct device *dev = &sspc->pdev->dev;
	void *reg = sspc->ioaddr;
	struct chip_data *chip = spi_get_ctldata(sspc->cur_msg->spi);
	cr0 = chip->cr0;

	dev_warn(dev, "In %s : Starting bit banging\n",
		__func__);
	if (read_SSSR(reg) & SSP_NOT_SYNC)
		dev_warn(dev, "SSP clock desynchronized.\n");
	if (!(read_SSCR0(reg) & SSCR0_SSE))
		dev_warn(dev, "in SSCR0, SSP disabled.\n");

	dev_dbg(dev, "SSP not ready, start CLK sync\n");

	write_SSCR0(cr0 & ~SSCR0_SSE, reg);
	write_SSPSP(0x02010007, reg);

	write_SSTO(chip->timeout, reg);
	write_SSCR0(cr0, reg);

	/*
	*  This routine uses the DFx block to override the SSP inputs
	*  and outputs allowing us to bit bang SSPSCLK. On Langwell,
	*  we have to generate the clock to clear busy.
	*/
	write_I2CDATA(0x3, i2c_reg);
	udelay(I2C_ACCESS_USDELAY);
	write_I2CCTRL(0x01070034, i2c_reg);
	udelay(I2C_ACCESS_USDELAY);
	write_I2CDATA(0x00000099, i2c_reg);
	udelay(I2C_ACCESS_USDELAY);
	write_I2CCTRL(0x01070038, i2c_reg);
	udelay(I2C_ACCESS_USDELAY);
	sssr = read_SSSR(reg);

	/* Bit bang the clock until CSS clears */
	while ((sssr & 0x400000) && (count < MAX_BITBANGING_LOOP)) {
		write_I2CDATA(0x2, i2c_reg);
		udelay(I2C_ACCESS_USDELAY);
		write_I2CCTRL(0x01070034, i2c_reg);
		udelay(I2C_ACCESS_USDELAY);
		write_I2CDATA(0x3, i2c_reg);
		udelay(I2C_ACCESS_USDELAY);
		write_I2CCTRL(0x01070034, i2c_reg);
		udelay(I2C_ACCESS_USDELAY);
		sssr = read_SSSR(reg);
		count++;
	}
	if (count >= MAX_BITBANGING_LOOP)
		dev_err(dev, "ERROR in %s : infinite loop on bit banging. Aborting\n",
								__func__);

	dev_dbg(dev, "---Bit bang count=%d\n", count);

	write_I2CDATA(0x0, i2c_reg);
	udelay(I2C_ACCESS_USDELAY);
	write_I2CCTRL(0x01070038, i2c_reg);
}

static unsigned int ssp_get_clk_div(struct ssp_drv_context *sspc, int speed)
{
	if (sspc->quirks & QUIRKS_PLATFORM_MRFL)
		/* The clock divider shall stay between 0 and 4095. */
		return clamp(25000000 / speed - 1, 0, 4095);
	else
		return clamp(100000000 / speed - 1, 3, 4095);
}


static int ssp_get_speed(struct ssp_drv_context *sspc, int clk_div)
{
	if (sspc->quirks & QUIRKS_PLATFORM_MRFL)
		return 25000000 / (clk_div + 1);
	else
		return 100000000 / (clk_div + 1);
}

/**
 * transfer() - Start a SPI transfer
 * @spi:	Pointer to the spi_device struct
 * @msg:	Pointer to the spi_message struct
 */
static int transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct ssp_drv_context *sspc = spi_master_get_devdata(spi->master);
	unsigned long flags;

	msg->actual_length = 0;
	msg->status = -EINPROGRESS;
	spin_lock_irqsave(&sspc->lock, flags);
	list_add_tail(&msg->queue, &sspc->queue);
	if (!sspc->suspended)
		queue_work(sspc->workqueue, &sspc->pump_messages);
	spin_unlock_irqrestore(&sspc->lock, flags);

	return 0;
}

static int handle_message(struct ssp_drv_context *sspc)
{
	struct chip_data *chip = NULL;
	struct spi_transfer *transfer = NULL;
	void *reg = sspc->ioaddr;
	u32 cr0, saved_cr0, cr1, saved_cr1;
	struct device *dev = &sspc->pdev->dev;
	struct spi_message *msg = sspc->cur_msg;
	u32 clk_div, saved_speed_hz, speed_hz;
	u8 dma_enabled;
	u32 timeout;
	u8 chip_select;
	u32 mask = 0;
	int bits_per_word, saved_bits_per_word;
	unsigned long flags;
	u8 normal_enabled = 0;

	chip = spi_get_ctldata(msg->spi);

	/* get every chip data we need to handle atomically the full message */
	spin_lock_irqsave(&sspc->lock, flags);
	saved_cr0 = chip->cr0;
	saved_cr1 = chip->cr1;
	saved_bits_per_word = msg->spi->bits_per_word;
	saved_speed_hz = chip->speed_hz;
	sspc->cs_control = chip->cs_control;
	timeout = chip->timeout;
	chip_select = chip->chip_select;
	dma_enabled = chip->dma_enabled;
	spin_unlock_irqrestore(&sspc->lock, flags);

	complete(&sspc->msg_done);

	list_for_each_entry(transfer, &msg->transfers, transfer_list) {
		wait_for_completion(&sspc->msg_done);
		INIT_COMPLETION(sspc->msg_done);

		/* Check transfer length */
		if (unlikely((transfer->len > MAX_SPI_TRANSFER_SIZE) ||
					(transfer->len == 0))) {
			dev_warn(dev, "transfer length null or greater than %d\n",
					MAX_SPI_TRANSFER_SIZE);
			dev_warn(dev, "length = %d\n", transfer->len);
			msg->status = -EINVAL;

			if (msg->complete)
				msg->complete(msg->context);
			complete(&sspc->msg_done);
			return 0;
		}

		/* If the bits_per_word field in non-zero in the spi_transfer provided
		 * by the user-space, consider this value. Otherwise consider the
		 * default bits_per_word field from the spi setting. */
		if (transfer->bits_per_word) {
			bits_per_word = transfer->bits_per_word;
			cr0 = saved_cr0;
			cr0 &= ~(SSCR0_EDSS | SSCR0_DSS);
			cr0 |= SSCR0_DataSize(bits_per_word > 16 ?
					bits_per_word - 16 : bits_per_word)
				| (bits_per_word > 16 ? SSCR0_EDSS : 0);
		} else {
			/* Use default value. */
			bits_per_word = saved_bits_per_word;
			cr0 = saved_cr0;
		}

		if ((bits_per_word < MIN_BITS_PER_WORD
					|| bits_per_word > MAX_BITS_PER_WORD)) {
			dev_warn(dev, "invalid wordsize\n");
			msg->status = -EINVAL;
			if (msg->complete)
				msg->complete(msg->context);
			complete(&sspc->msg_done);
			return 0;
		}

		/* Check message length and bit per words consistency */
		if (bits_per_word <= 8)
			mask = 0;
		else if (bits_per_word <= 16)
			mask = 1;
		else if (bits_per_word <= 32)
			mask = 3;

		if (transfer->len & mask) {
			dev_warn(dev,
					"message rejected : data length %d not multiple of %d "
					"while in %d bits mode\n",
					transfer->len,
					mask + 1,
					(mask == 1) ? 16 : 32);
			msg->status = -EINVAL;
			if (msg->complete)
				msg->complete(msg->context);
			complete(&sspc->msg_done);
			return 0;
		}

		/* Flush any remaining data (in case of failed previous transfer) */
		flush(sspc);

		dev_dbg(dev, "%d bits/word, mode %d\n",
				bits_per_word, msg->spi->mode & 0x3);
		if (bits_per_word <= 8) {
			sspc->n_bytes = 1;
			sspc->read = u8_reader;
			sspc->write = u8_writer;
			/* It maybe has some unclear issue in dma mode, as workaround,
			use normal mode to transfer when len equal 8 bytes */
			if (transfer->len == 8)
				normal_enabled = 1;
		} else if (bits_per_word <= 16) {
			sspc->n_bytes = 2;
			sspc->read = u16_reader;
			sspc->write = u16_writer;
			/* It maybe has some unclear issue in dma mode, as workaround,
			use normal mode to transfer when len equal 16 bytes */
			if (transfer->len == 16)
				normal_enabled = 1;
		} else if (bits_per_word <= 32) {
			if (!ssp_timing_wr)
				cr0 |= SSCR0_EDSS;
			sspc->n_bytes = 4;
			sspc->read = u32_reader;
			sspc->write = u32_writer;
			/* It maybe has some unclear issue in dma mode, as workaround,
			use normal mode to transfer when len equal 32 bytes */
			if (transfer->len == 32)
				normal_enabled = 1;
		}

		sspc->tx  = (void *)transfer->tx_buf;
		sspc->rx  = (void *)transfer->rx_buf;
		sspc->len = transfer->len;
		sspc->cs_control = chip->cs_control;
		sspc->cs_change = transfer->cs_change;

		if (likely(chip->dma_enabled)) {
			sspc->dma_mapped = map_dma_buffers(sspc);
			if (unlikely(!sspc->dma_mapped))
				return 0;
		}

		sspc->write = sspc->tx ? sspc->write : null_writer;
		sspc->read  = sspc->rx ? sspc->read : null_reader;

		sspc->tx_end = sspc->tx + transfer->len;
		sspc->rx_end = sspc->rx + transfer->len;

		/* [REVERT ME] Bug in status register clear for Tangier simulation */
		if ((intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER) ||
				(intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_ANNIEDALE)) {
			if ((intel_mid_identify_sim() != INTEL_MID_CPU_SIMULATION_VP &&
						(intel_mid_identify_sim() != INTEL_MID_CPU_SIMULATION_HVP)))
				write_SSSR(sspc->clear_sr, reg);
		} else /* Clear status  */
			write_SSSR(sspc->clear_sr, reg);

		/* setup the CR1 control register */
		cr1 = saved_cr1 | sspc->cr1_sig;

		if (likely(sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL)) {
			/* in case of len smaller than burst size, adjust the RX     */
			/* threshold. All other cases will use the default threshold */
			/* value. The RX fifo threshold must be aligned with the DMA */
			/* RX transfer size, which may be limited to a multiple of 4 */
			/* bytes due to 32bits DDR access.                           */
			if  (sspc->len / sspc->n_bytes <= sspc->rx_fifo_threshold) {
				u32 rx_fifo_threshold;

				rx_fifo_threshold = (sspc->len & ~(4 - 1)) /
					sspc->n_bytes;
				cr1 &= ~(SSCR1_RFT);
				cr1 |= SSCR1_RxTresh(rx_fifo_threshold) & SSCR1_RFT;
			} else
				write_SSTO(timeout, reg);
		}
		dev_dbg(dev, "transfer len:%d  n_bytes:%d  cr0:%x  cr1:%x",
				sspc->len, sspc->n_bytes, cr0, cr1);

		/* first set CR1 */
		write_SSCR1(cr1, reg);

		if (intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER)
			write_SSFS((1 << chip_select), reg);

		/* recalculate the frequency for each transfer */
		if (transfer->speed_hz)
			speed_hz = transfer->speed_hz;
		else
			speed_hz = saved_speed_hz;

		clk_div = ssp_get_clk_div(sspc, speed_hz);

		cr0 &= ~SSCR0_SCR;
		cr0 |= (clk_div & 0xFFF) << 8;

		/* Do bitbanging only if SSP not-enabled or not-synchronized */
		if (unlikely(((read_SSSR(reg) & SSP_NOT_SYNC) ||
						(!(read_SSCR0(reg) & SSCR0_SSE))) &&
					(sspc->quirks & QUIRKS_BIT_BANGING))) {
			start_bitbanging(sspc);
		} else {

			/* if speed is higher than 6.25Mhz, enable clock delay */
			if (speed_hz > 6250000)
				write_SSCR2((read_SSCR2(reg) | SSCR2_CLK_DEL_EN), reg);
			else
				write_SSCR2((read_SSCR2(reg) & ~SSCR2_CLK_DEL_EN), reg);

			/* (re)start the SSP */
			if (ssp_timing_wr) {
				dev_dbg(dev, "original cr0 before reset:%x",
						cr0);
				/*we should not disable TUM and RIM interrup*/
				write_SSCR0(0x0000000F, reg);
				cr0 &= ~(SSCR0_SSE);
				dev_dbg(dev, "reset ssp:cr0:%x", cr0);
				write_SSCR0(cr0, reg);
				cr0 |= SSCR0_SSE;
				dev_dbg(dev, "reset ssp:cr0:%x", cr0);
				write_SSCR0(cr0, reg);
			} else
				write_SSCR0(cr0, reg);
		}

		if (sspc->cs_control)
			sspc->cs_control(sspc->cs_assert);

		if (likely(dma_enabled) && (!normal_enabled)) {
			if (unlikely(sspc->quirks & QUIRKS_USE_PM_QOS))
				pm_qos_update_request(&sspc->pm_qos_req,
						MIN_EXIT_LATENCY);
			dma_transfer(sspc);
		} else {
			/* Do the transfer syncronously */
			queue_work(sspc->wq_poll_write, &sspc->poll_write);
			poll_transfer((unsigned long)sspc);
			unmap_dma_buffers(sspc);
			complete(&sspc->msg_done);
		}

		if (list_is_last(&transfer->transfer_list, &msg->transfers)
				|| sspc->cs_change) {
			if (sspc->cs_control)
				sspc->cs_control(!sspc->cs_assert);
		}

	} /* end of list_for_each_entry */

	wait_for_completion(&sspc->msg_done);

	/* Now we are done with this entire message */
	if (likely(msg->complete))
		msg->complete(msg->context);

	return 0;
}

static void pump_messages(struct work_struct *work)
{
	struct ssp_drv_context *sspc =
		container_of(work, struct ssp_drv_context, pump_messages);
	struct device *dev = &sspc->pdev->dev;
	unsigned long flags;
	struct spi_message *msg;

	pm_runtime_get_sync(dev);
	spin_lock_irqsave(&sspc->lock, flags);
	while (!list_empty(&sspc->queue)) {
		if (sspc->suspended)
			break;
		msg = list_entry(sspc->queue.next, struct spi_message, queue);
		list_del_init(&msg->queue);
		sspc->cur_msg = msg;
		spin_unlock_irqrestore(&sspc->lock, flags);
		handle_message(sspc);
		spin_lock_irqsave(&sspc->lock, flags);
		sspc->cur_msg = NULL;
	}
	spin_unlock_irqrestore(&sspc->lock, flags);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

/**
 * setup() - Driver setup procedure
 * @spi:	Pointeur to the spi_device struct
 */
static int setup(struct spi_device *spi)
{
	struct intel_mid_ssp_spi_chip *chip_info = NULL;
	struct chip_data *chip;
	struct ssp_drv_context *sspc =
		spi_master_get_devdata(spi->master);
	u32 tx_fifo_threshold;
	u32 burst_size;
	u32 clk_div;
	static u32 one_time_setup = 1;
	unsigned long flags;

	spin_lock_irqsave(&sspc->lock, flags);
	if (!spi->bits_per_word)
		spi->bits_per_word = DFLT_BITS_PER_WORD;

	if ((spi->bits_per_word < MIN_BITS_PER_WORD
		|| spi->bits_per_word > MAX_BITS_PER_WORD)) {
		spin_unlock_irqrestore(&sspc->lock, flags);
		return -EINVAL;
	}

	chip = spi_get_ctldata(spi);
	if (!chip) {
		chip = kzalloc(sizeof(struct chip_data), GFP_KERNEL);
		if (!chip) {
			dev_err(&spi->dev,
			"failed setup: can't allocate chip data\n");
			spin_unlock_irqrestore(&sspc->lock, flags);
			return -ENOMEM;
		}
	}
	chip->cr0 = SSCR0_Motorola | SSCR0_DataSize(spi->bits_per_word > 16 ?
		spi->bits_per_word - 16 : spi->bits_per_word)
			| SSCR0_SSE
			| (spi->bits_per_word > 16 ? SSCR0_EDSS : 0);

	/* protocol drivers may change the chip settings, so...  */
	/* if chip_info exists, use it                           */
	chip_info = spi->controller_data;

	/* chip_info isn't always needed */
	chip->cr1 = 0;
	if (chip_info) {
		/* If user requested CS Active High need to verify that there
		 * is no transfer pending. If this is the case, kindly fail.  */
		if ((spi->mode & SPI_CS_HIGH) != sspc->cs_assert) {
			if (sspc->cur_msg) {
				dev_err(&spi->dev, "message pending... Failing\n");
				/* A message is currently in transfer. Do not toggle CS */
				spin_unlock_irqrestore(&sspc->lock, flags);
				return -EAGAIN;
			}
			if (!chip_info->cs_control) {
				/* unable to control cs by hand */
				dev_err(&spi->dev,
						"This CS does not support SPI_CS_HIGH flag\n");
				spin_unlock_irqrestore(&sspc->lock, flags);
				return -EINVAL;
			}
			sspc->cs_assert = spi->mode & SPI_CS_HIGH;
			chip_info->cs_control(!sspc->cs_assert);
		}

		burst_size = chip_info->burst_size;
		if (burst_size > IMSS_FIFO_BURST_8)
			burst_size = DFLT_FIFO_BURST_SIZE;

		chip->timeout = chip_info->timeout;

		if (chip_info->enable_loopback)
			chip->cr1 |= SSCR1_LBM;

		chip->dma_enabled = chip_info->dma_enabled;
		chip->cs_control = chip_info->cs_control;

		/* Request platform-specific gpio and pinmux here since
		 * it is not possible to get the intel_mid_ssp_spi_chip
		 * structure in probe */
		if (one_time_setup && !chip_info->dma_enabled
				&& chip_info->platform_pinmux) {
			chip_info->platform_pinmux();
			one_time_setup = 0;
		}

	} else {
		/* if no chip_info provided by protocol driver, */
		/* set default values                           */
		dev_info(&spi->dev, "setting default chip values\n");

		burst_size = DFLT_FIFO_BURST_SIZE;
		chip->dma_enabled = 1;
		if (sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL)
			chip->timeout = 0;
		else
			chip->timeout = DFLT_TIMEOUT_VAL;
	}
	/* Set FIFO thresholds according to burst_size */
	if (burst_size == IMSS_FIFO_BURST_8)
		sspc->rx_fifo_threshold = 8;
	else if (burst_size == IMSS_FIFO_BURST_4)
		sspc->rx_fifo_threshold = 4;
	else
		sspc->rx_fifo_threshold = 1;
	/* FIXME: This is a workaround. */
	/* When speed is lower than 800KHz, the transfer data will be */
	/* incorrect on MRFL by DMA method*/
	if (sspc->quirks & QUIRKS_PLATFORM_MRFL && chip->dma_enabled
			&& (spi->max_speed_hz < 800000))
		sspc->rx_fifo_threshold = 1;
	tx_fifo_threshold = SPI_FIFO_SIZE - sspc->rx_fifo_threshold;
	chip->cr1 |= (SSCR1_RxTresh(sspc->rx_fifo_threshold) &
		SSCR1_RFT) | (SSCR1_TxTresh(tx_fifo_threshold) & SSCR1_TFT);

	sspc->dma_mapped = 0;

	/* setting phase and polarity. spi->mode comes from boardinfo */
	if ((spi->mode & SPI_CPHA) != 0)
		chip->cr1 |= SSCR1_SPH;
	if ((spi->mode & SPI_CPOL) != 0)
		chip->cr1 |= SSCR1_SPO;

	if (sspc->quirks & QUIRKS_SPI_SLAVE_CLOCK_MODE)
		/* set slave mode */
		chip->cr1 |= SSCR1_SCLKDIR | SSCR1_SFRMDIR;
	chip->cr1 |= SSCR1_SCFR;        /* clock is not free running */

	if (spi->bits_per_word <= 8) {
		chip->n_bytes = 1;
	} else if (spi->bits_per_word <= 16) {
		chip->n_bytes = 2;
	} else if (spi->bits_per_word <= 32) {
		chip->n_bytes = 4;
	} else {
		dev_err(&spi->dev, "invalid wordsize\n");
		spin_unlock_irqrestore(&sspc->lock, flags);
		return -EINVAL;
	}

	if ((sspc->quirks & QUIRKS_SPI_SLAVE_CLOCK_MODE) == 0) {
		clk_div = ssp_get_clk_div(sspc, spi->max_speed_hz);
		chip->cr0 |= (clk_div & 0xFFF) << 8;
		spi->max_speed_hz = ssp_get_speed(sspc, clk_div);
		chip->speed_hz = spi->max_speed_hz;
		dev_dbg(&spi->dev, "spi->max_speed_hz:%d clk_div:%x cr0:%x",
			spi->max_speed_hz, clk_div, chip->cr0);
	}
	chip->bits_per_word = spi->bits_per_word;
	chip->chip_select = spi->chip_select;

	spi_set_ctldata(spi, chip);

	/* setup of sspc members that will not change across transfers */

	if (chip->dma_enabled) {
		sspc->n_bytes = chip->n_bytes;
		spin_unlock_irqrestore(&sspc->lock, flags);
		intel_mid_ssp_spi_dma_init(sspc);
		spin_lock_irqsave(&sspc->lock, flags);
		sspc->cr1_sig = SSCR1_TSRE | SSCR1_RSRE;
		sspc->mask_sr = SSSR_ROR | SSSR_TUR;
		if (sspc->quirks & QUIRKS_DMA_USE_NO_TRAIL)
			sspc->cr1_sig |= SSCR1_TRAIL;
	} else {
		sspc->cr1_sig = SSCR1_TINTE;
		sspc->mask_sr = SSSR_ROR | SSSR_TUR | SSSR_TINT;
	}
	sspc->clear_sr = SSSR_TUR | SSSR_ROR | SSSR_TINT;

	spin_unlock_irqrestore(&sspc->lock, flags);
	return 0;
}

/**
 * cleanup() - Driver cleanup procedure
 * @spi:	Pointer to the spi_device struct
 */
static void cleanup(struct spi_device *spi)
{
	struct chip_data *chip = spi_get_ctldata(spi);
	struct ssp_drv_context *sspc =
		spi_master_get_devdata(spi->master);

	if (sspc->dma_initialized)
		intel_mid_ssp_spi_dma_exit(sspc);

	/* Remove the PM_QOS request */
	if (sspc->quirks & QUIRKS_USE_PM_QOS)
		pm_qos_remove_request(&sspc->pm_qos_req);

	kfree(chip);
	spi_set_ctldata(spi, NULL);
}

/**
 * intel_mid_ssp_spi_probe() - Driver probe procedure
 * @pdev:	Pointer to the pci_dev struct
 * @ent:	Pointer to the pci_device_id struct
 */
static int intel_mid_ssp_spi_probe(struct pci_dev *pdev,
	const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct spi_master *master;
	struct ssp_drv_context *sspc = 0;
	int status;
	u32 iolen = 0;
	u8 ssp_cfg;
	int pos;
	void __iomem *syscfg_ioaddr;
	unsigned long syscfg;

	/* Check if the SSP we are probed for has been allocated */
	/* to operate as SPI. This information is retreived from */
	/* the field adid of the Vendor-Specific PCI capability  */
	/* which is used as a configuration register.            */
	pos = pci_find_capability(pdev, PCI_CAP_ID_VNDR);
	if (pos > 0) {
		pci_read_config_byte(pdev,
			pos + VNDR_CAPABILITY_ADID_OFFSET,
			&ssp_cfg);
	} else {
		dev_info(dev, "No Vendor Specific PCI capability\n");
		goto err_abort_probe;
	}

	if (ssp_cfg_get_mode(ssp_cfg) != SSP_CFG_SPI_MODE_ID) {
		dev_info(dev, "Unsupported SSP mode (%02xh)\n", ssp_cfg);
		goto err_abort_probe;
	}

	dev_info(dev, "found PCI SSP controller (ID: %04xh:%04xh cfg: %02xh)\n",
		pdev->vendor, pdev->device, ssp_cfg);

	status = pci_enable_device(pdev);
	if (status)
		return status;

	/* Allocate Slave with space for sspc and null dma buffer */
	master = spi_alloc_master(dev, sizeof(struct ssp_drv_context));

	if (!master) {
		dev_err(dev, "cannot alloc spi_slave\n");
		status = -ENOMEM;
		goto err_free_0;
	}

	sspc = spi_master_get_devdata(master);
	sspc->master = master;

	sspc->pdev = pdev;
	sspc->quirks = ent->driver_data;

	/* Set platform & configuration quirks */
	if (sspc->quirks & QUIRKS_PLATFORM_MRST) {
		/* Apply bit banging workarround on MRST */
		sspc->quirks |= QUIRKS_BIT_BANGING;
		/* MRST slave mode workarrounds */
		if (ssp_cfg_is_spi_slave(ssp_cfg))
			sspc->quirks |= QUIRKS_USE_PM_QOS |
					QUIRKS_SRAM_ADDITIONAL_CPY;
	}
	sspc->quirks |= QUIRKS_DMA_USE_NO_TRAIL;
	if (ssp_cfg_is_spi_slave(ssp_cfg))
		sspc->quirks |= QUIRKS_SPI_SLAVE_CLOCK_MODE;

	master->mode_bits = SPI_CS_HIGH | SPI_CPOL | SPI_CPHA;
	master->bus_num = ssp_cfg_get_spi_bus_nb(ssp_cfg);
	master->num_chipselect = 4;
	master->cleanup = cleanup;
	master->setup = setup;
	master->transfer = transfer;
	sspc->dma_wq = create_workqueue("intel_mid_ssp_spi");
	INIT_WORK(&sspc->complete_work, int_transfer_complete_work);

	sspc->dma_initialized = 0;
	sspc->suspended = 0;
	sspc->cur_msg = NULL;

	/* get basic io resource and map it */
	sspc->paddr = pci_resource_start(pdev, 0);
	iolen = pci_resource_len(pdev, 0);

	status = pci_request_region(pdev, 0, dev_name(&pdev->dev));
	if (status)
		goto err_free_1;

	sspc->ioaddr = ioremap_nocache(sspc->paddr, iolen);
	if (!sspc->ioaddr) {
		status = -ENOMEM;
		goto err_free_2;
	}
	dev_dbg(dev, "paddr = : %08lx", sspc->paddr);
	dev_dbg(dev, "ioaddr = : %p\n", sspc->ioaddr);
	dev_dbg(dev, "attaching to IRQ: %04x\n", pdev->irq);
	dev_dbg(dev, "quirks = : %08lx\n", sspc->quirks);

	if (sspc->quirks & QUIRKS_BIT_BANGING) {
		/* Bit banging on the clock is done through */
		/* DFT which is available through I2C.      */
		/* get base address of I2C_Serbus registers */
		sspc->I2C_paddr = 0xff12b000;
		sspc->I2C_ioaddr = ioremap_nocache(sspc->I2C_paddr, 0x10);
		if (!sspc->I2C_ioaddr) {
			status = -ENOMEM;
			goto err_free_3;
		}
	}

	/* Attach to IRQ */
	sspc->irq = pdev->irq;
	status = request_irq(sspc->irq, ssp_int, IRQF_SHARED,
		"intel_mid_ssp_spi", sspc);

	if (intel_mid_identify_cpu() == INTEL_MID_CPU_CHIP_TANGIER) {
		if ((intel_mid_identify_sim() ==
				INTEL_MID_CPU_SIMULATION_SLE) ||
		    (intel_mid_identify_sim() ==
				INTEL_MID_CPU_SIMULATION_NONE)) {
			/* [REVERT ME] Tangier SLE not supported.
			 * Requires debug before removal.  Assume
			 * also required in Si. */
			disable_irq_nosync(sspc->irq);
		}
		if (intel_mid_identify_sim() == INTEL_MID_CPU_SIMULATION_NONE)
			ssp_timing_wr = 1;
	}

	if (status < 0) {
		dev_err(&pdev->dev, "can not get IRQ\n");
		goto err_free_4;
	}

	if (sspc->quirks & QUIRKS_PLATFORM_MDFL) {
		/* get base address of DMA selector. */
		syscfg = sspc->paddr - SYSCFG;
		syscfg_ioaddr = ioremap_nocache(syscfg, 0x10);
		if (!syscfg_ioaddr) {
			status = -ENOMEM;
			goto err_free_5;
		}
		iowrite32(ioread32(syscfg_ioaddr) | 2, syscfg_ioaddr);
	}

	INIT_LIST_HEAD(&sspc->queue);
	init_completion(&sspc->msg_done);
	spin_lock_init(&sspc->lock);
	INIT_WORK(&sspc->pump_messages, pump_messages);
	sspc->workqueue = create_singlethread_workqueue(dev_name(&pdev->dev));

	INIT_WORK(&sspc->poll_write, poll_writer);
	sspc->wq_poll_write = create_singlethread_workqueue("spi_poll_wr");

	/* Register with the SPI framework */
	dev_info(dev, "register with SPI framework (bus spi%d)\n",
			master->bus_num);

	status = spi_register_master(master);
	if (status) {
		dev_err(dev, "problem registering spi\n");
		goto err_free_5;
	}

	pci_set_drvdata(pdev, sspc);

	/* Create the PM_QOS request */
	if (sspc->quirks & QUIRKS_USE_PM_QOS)
		pm_qos_add_request(&sspc->pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
				PM_QOS_DEFAULT_VALUE);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 25);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	if (!pm_runtime_enabled(&pdev->dev))
		dev_err(&pdev->dev, "spi runtime pm not enabled!\n");
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return status;

err_free_5:
	free_irq(sspc->irq, sspc);
err_free_4:
	iounmap(sspc->I2C_ioaddr);
err_free_3:
	iounmap(sspc->ioaddr);
err_free_2:
	pci_release_region(pdev, 0);
err_free_1:
	spi_master_put(master);
err_free_0:
	pci_disable_device(pdev);

	return status;
err_abort_probe:
	dev_info(dev, "Abort probe for SSP %04xh:%04xh\n",
		pdev->vendor, pdev->device);
	return -ENODEV;
}

/**
 * intel_mid_ssp_spi_remove() - driver remove procedure
 * @pdev:	Pointer to the pci_dev struct
 */
static void intel_mid_ssp_spi_remove(struct pci_dev *pdev)
{
	struct ssp_drv_context *sspc = pci_get_drvdata(pdev);

	if (!sspc)
		return;

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	if (sspc->dma_wq)
		destroy_workqueue(sspc->dma_wq);
	if (sspc->workqueue)
		destroy_workqueue(sspc->workqueue);

	/* Release IRQ */
	free_irq(sspc->irq, sspc);

	if (sspc->ioaddr)
		iounmap(sspc->ioaddr);
	if (sspc->quirks & QUIRKS_BIT_BANGING && sspc->I2C_ioaddr)
		iounmap(sspc->I2C_ioaddr);

	/* disconnect from the SPI framework */
	if (sspc->master)
		spi_unregister_master(sspc->master);

	pci_set_drvdata(pdev, NULL);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);

	return;
}

#ifdef CONFIG_PM
static int intel_mid_ssp_spi_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ssp_drv_context *sspc = pci_get_drvdata(pdev);
	unsigned long flags;
	int loop = 26;

	dev_dbg(dev, "suspend\n");

	spin_lock_irqsave(&sspc->lock, flags);
	sspc->suspended = 1;
	/*
	 * If there is one msg being handled, wait 500ms at most,
	 * if still not done, return busy
	 */
	while (sspc->cur_msg && --loop) {
		spin_unlock_irqrestore(&sspc->lock, flags);
		msleep(20);
		spin_lock_irqsave(&sspc->lock, flags);
		if (!loop)
			sspc->suspended = 0;
	}
	spin_unlock_irqrestore(&sspc->lock, flags);

	if (loop)
		return 0;
	else
		return -EBUSY;
}

static int intel_mid_ssp_spi_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ssp_drv_context *sspc = pci_get_drvdata(pdev);

	dev_dbg(dev, "resume\n");
	spin_lock(&sspc->lock);
	sspc->suspended = 0;
	if (!list_empty(&sspc->queue))
		queue_work(sspc->workqueue, &sspc->pump_messages);
	spin_unlock(&sspc->lock);
	return 0;
}

static int intel_mid_ssp_spi_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "runtime suspend called\n");
	return 0;
}

static int intel_mid_ssp_spi_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "runtime resume called\n");
	return 0;
}

static int intel_mid_ssp_spi_runtime_idle(struct device *dev)
{
	int err;

	dev_dbg(dev, "runtime idle called\n");
	if (system_state == SYSTEM_BOOTING)
		/* if SSP SPI UART is set as default console and earlyprintk
		 * is enabled, it cannot shutdown SSP controller during booting.
		 */
		err = pm_schedule_suspend(dev, 30000);
	else
		err = pm_schedule_suspend(dev, 500);

	return err;
}
#else
#define intel_mid_ssp_spi_suspend NULL
#define intel_mid_ssp_spi_resume NULL
#define intel_mid_ssp_spi_runtime_suspend NULL
#define intel_mid_ssp_spi_runtime_resume NULL
#define intel_mid_ssp_spi_runtime_idle NULL
#endif /* CONFIG_PM */


static DEFINE_PCI_DEVICE_TABLE(pci_ids) = {
	/* MRST SSP0 */
	{ PCI_VDEVICE(INTEL, 0x0815), QUIRKS_PLATFORM_MRST},
	/* MDFL SSP0 */
	{ PCI_VDEVICE(INTEL, 0x0832), QUIRKS_PLATFORM_MDFL},
	/* MDFL SSP1 */
	{ PCI_VDEVICE(INTEL, 0x0825), QUIRKS_PLATFORM_MDFL},
	/* MDFL SSP3 */
	{ PCI_VDEVICE(INTEL, 0x0816), QUIRKS_PLATFORM_MDFL},
	/* MRFL SSP5 */
	{ PCI_VDEVICE(INTEL, 0x1194), QUIRKS_PLATFORM_MRFL},
	/* BYT SSP3 */
	{ PCI_VDEVICE(INTEL, 0x0f0e), QUIRKS_PLATFORM_BYT},
	{},
};

static const struct dev_pm_ops intel_mid_ssp_spi_pm_ops = {
	.suspend = intel_mid_ssp_spi_suspend,
	.resume = intel_mid_ssp_spi_resume,
	.runtime_suspend = intel_mid_ssp_spi_runtime_suspend,
	.runtime_resume = intel_mid_ssp_spi_runtime_resume,
	.runtime_idle = intel_mid_ssp_spi_runtime_idle,
};

static struct pci_driver intel_mid_ssp_spi_driver = {
	.name =		DRIVER_NAME,
	.id_table =	pci_ids,
	.probe =	intel_mid_ssp_spi_probe,
	.remove =	intel_mid_ssp_spi_remove,
	.driver =	{
		.pm	= &intel_mid_ssp_spi_pm_ops,
	},
};

static int __init intel_mid_ssp_spi_init(void)
{
	return pci_register_driver(&intel_mid_ssp_spi_driver);
}

late_initcall(intel_mid_ssp_spi_init);

static void __exit intel_mid_ssp_spi_exit(void)
{
	pci_unregister_driver(&intel_mid_ssp_spi_driver);
}

module_exit(intel_mid_ssp_spi_exit);
