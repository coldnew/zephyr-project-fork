/* adc_qmsi.c - QMSI ADC driver */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <init.h>
#include <nanokernel.h>
#include <string.h>
#include <stdlib.h>
#include <board.h>
#include <adc.h>
#include <arch/cpu.h>
#include <atomic.h>

#include "qm_adc.h"
#include "qm_scss.h"

enum {
	ADC_STATE_IDLE,
	ADC_STATE_BUSY,
	ADC_STATE_ERROR
};

struct adc_info  {
	atomic_t  state;
	device_sync_call_t sync;
	struct nano_sem sem;
};

static struct adc_info *adc_context;

static void adc_config_irq(void);

static void complete_callback(void)
{
	if (adc_context) {
		device_sync_call_complete(&adc_context->sync);
	}
}

static void error_callback(void)
{
	if (adc_context) {
		adc_context->state = ADC_STATE_ERROR;
		device_sync_call_complete(&adc_context->sync);
	}
}

static void adc_lock(struct adc_info *data)
{
	nano_sem_take(&data->sem, TICKS_UNLIMITED);
	data->state = ADC_STATE_BUSY;

}
static void adc_unlock(struct adc_info *data)
{
	nano_sem_give(&data->sem);
	data->state = ADC_STATE_IDLE;

}
#if (CONFIG_ADC_QMSI_CALIBRATION)
static void adc_qmsi_enable(struct device *dev)
{
	struct adc_info *info = dev->driver_data;

	adc_lock(info);
	qm_adc_set_mode(QM_ADC_0, QM_ADC_MODE_NORM_CAL);
	qm_adc_calibrate(QM_ADC_0);
	adc_unlock(info);
}

#else
static void adc_qmsi_enable(struct device *dev)
{
	struct adc_info *info = dev->driver_data;

	adc_lock(info);
	qm_adc_set_mode(QM_ADC_0, QM_ADC_MODE_NORM_NO_CAL);
	adc_unlock(info);
}
#endif /* CONFIG_ADC_QMSI_CALIBRATION */

static void adc_qmsi_disable(struct device *dev)
{
	struct adc_info *info = dev->driver_data;

	adc_lock(info);
	/* Go to deep sleep */
	qm_adc_set_mode(QM_ADC_0, QM_ADC_MODE_DEEP_PWR_DOWN);
	adc_unlock(info);
}

#if (CONFIG_ADC_QMSI_POLL)
static int adc_qmsi_read(struct device *dev, struct adc_seq_table *seq_tbl)
{
	int i, ret = DEV_OK;
	qm_adc_xfer_t xfer;
	qm_adc_config_t cfg;

	struct adc_info *info = dev->driver_data;

	if (qm_adc_get_config(QM_ADC_0, &cfg) != QM_RC_OK) {
		return DEV_INVALID_OP;
	}

	for (i = 0; i < seq_tbl->num_entries; i++) {

		xfer.ch = (qm_adc_channel_t *)&seq_tbl->entries[i].channel_id;
		/* Just one channel at the time using the Zephyr sequence table */
		xfer.ch_len = 1;
		xfer.samples = (uint32_t *)seq_tbl->entries[i].buffer;

		/* buffer length (bytes) the number of samples, the QMSI Driver does
		 * not allow more than QM_ADC_FIFO_LEN samples at the time in polling
		 * mode, if that happens, the qm_adc_convert api will return with an
		 * error
		 */
		xfer.samples_len = (seq_tbl->entries[i].buffer_length);
		xfer.complete_callback = NULL;
		xfer.error_callback = NULL;

		cfg.window = seq_tbl->entries[i].sampling_delay;

		adc_lock(info);

		if (qm_adc_set_config(QM_ADC_0, &cfg) != QM_RC_OK) {
			ret =  DEV_INVALID_CONF;
			adc_unlock(info);
			break;
		}

		/* Run the conversion, here the function will poll for the samples
		 * The function will constantly read  the status register to check if
		 * the number of samples required has been captured
		 */
		if (qm_adc_convert(QM_ADC_0, &xfer) != QM_RC_OK) {
			ret =  DEV_FAIL;
			adc_unlock(info);
			break;
		}

		/* Successful Analog to Digital conversion */
		adc_unlock(info);
	}

	return ret;
}
#else
static int adc_qmsi_read(struct device *dev, struct adc_seq_table *seq_tbl)
{
	int i, ret = DEV_OK;
	qm_adc_xfer_t xfer;
	qm_adc_config_t cfg;

	struct adc_info *info = dev->driver_data;

	if (qm_adc_get_config(QM_ADC_0, &cfg) != QM_RC_OK) {
		return DEV_INVALID_OP;
	}

	for (i = 0; i < seq_tbl->num_entries; i++) {

		xfer.ch = (qm_adc_channel_t *)&seq_tbl->entries[i].channel_id;
		/* Just one channel at the time using the Zephyr sequence table */
		xfer.ch_len = 1;
		xfer.samples = (uint32_t *)seq_tbl->entries[i].buffer;
		xfer.samples_len = (seq_tbl->entries[i].buffer_length) >> 2;
		xfer.complete_callback = complete_callback;
		xfer.error_callback = error_callback;

		cfg.window = seq_tbl->entries[i].sampling_delay;

		adc_lock(info);

		if (qm_adc_set_config(QM_ADC_0, &cfg) != QM_RC_OK) {
			ret =  DEV_INVALID_CONF;
			adc_unlock(info);
			break;
		}

		/* ADC info used by the callbacks */
		adc_context = info;

		/* This is the interrupt driven API, will generate and interrupt and
		 * call the complete_callback function once the samples have been
		 * obtained
		 */
		if (qm_adc_irq_convert(QM_ADC_0, &xfer) != QM_RC_OK) {
			adc_context = NULL;
			ret =  DEV_FAIL;
			adc_unlock(info);
			break;
		}

		/* Wait for the interrupt to finish */
		device_sync_call_wait(&info->sync);

		if (info->state == ADC_STATE_ERROR) {
			ret =  DEV_FAIL;
			adc_unlock(info);
			break;
		}
		adc_context = NULL;

		/* Successful Analog to Digital conversion */
		adc_unlock(info);
	}

	return ret;
}
#endif /* CONFIG_ADC_QMSI_POLL */

void adc_qmsi_isr(void *arg)
{
	qm_adc_0_isr();
}

static struct adc_driver_api api_funcs = {
	.enable  = adc_qmsi_enable,
	.disable = adc_qmsi_disable,
	.read    = adc_qmsi_read,
};

int adc_qmsi_init(struct device *dev)
{
	qm_adc_config_t cfg;

	struct adc_info *info = dev->driver_data;

	dev->driver_api = &api_funcs;

	/* Enable the ADC and set the clock divisor */
	clk_periph_enable(CLK_PERIPH_CLK | CLK_PERIPH_ADC |
				CLK_PERIPH_ADC_REGISTER);
	/* ADC clock  divider*/
	clk_adc_set_div(CONFIG_ADC_QMSI_CLOCK_RATIO);

	/* Set up config */
	/* Clock cycles between the start of each sample */
	cfg.window = CONFIG_ADC_QMSI_SERIAL_DELAY;
	cfg.resolution = CONFIG_ADC_QMSI_SAMPLE_WIDTH;

	qm_adc_set_config(QM_ADC_0, &cfg);

	device_sync_call_init(&info->sync);

	nano_sem_init(&info->sem);
	nano_sem_give(&info->sem);
	info->state = ADC_STATE_IDLE;

	adc_config_irq();

	return DEV_OK;
}

struct adc_info adc_info_dev;

DEVICE_INIT(adc_qmsi, CONFIG_ADC_QMSI_NAME, &adc_qmsi_init,
		    &adc_info_dev, NULL,
			SECONDARY, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static void adc_config_irq(void)
{
	IRQ_CONNECT(CONFIG_ADC_IRQ, CONFIG_ADC_PRI, qm_adc_0_isr,
		NULL, (IOAPIC_LEVEL | IOAPIC_HIGH));

	irq_enable(CONFIG_ADC_IRQ);

	QM_SCSS_INT->int_adc_calib_mask &= ~BIT(0);
}
