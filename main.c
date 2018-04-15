
#include <bsp/gctl.h>
#include <bsp/gpio.h>
#include <bsp/uart.h>
#include <bsp/usb.h>
#include <bsp/irq.h>
#include <bsp/cache.h>
#include <bsp/util.h>

#include <string.h>
#include <stdio.h>

#include "descriptors.h"
#include "acquisition.h"

#define CMD_GET_FW_VERSION		0xb0
#define CMD_START			0xb1
#define CMD_GET_REVID_VERSION		0xb2

#define CLOCK_DIVISOR_X2_FOR(f) ((SYS_CLK + (f)/4) / ((f)/2))

static volatile uint8_t DmaBuf[32] __attribute__((aligned(32)));

static void VendorCommand(uint8_t request_type, uint8_t request, uint16_t value,
			  uint16_t index, uint16_t length)
{
  switch(request) {
  case CMD_START:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 3)
      goto stall;
    Fx3UartTxString("CMD_START\n");
    Fx3UsbUnstallEp0();
    Fx3UsbDmaDataOut(0, DmaBuf, 3);
    Fx3CacheInvalidateDCacheEntry(DmaBuf);
    {
      char buf[64];
      snprintf(buf, sizeof(buf), "flags=%02x, sample_delay_h=%02x, sample_delay_l=%02x\n",
	       (unsigned)DmaBuf[0], (unsigned)DmaBuf[1], (unsigned)DmaBuf[2]);
      Fx3UartTxString(buf);
    }

    uint8_t flags = DmaBuf[0];
    uint16_t sample_delay = (DmaBuf[1]<<8)|DmaBuf[2];
    uint8_t bits = 8;
    if (flags & (1<<5))
      bits = 16;
    uint16_t clock_divisor_x2;
    if (flags & (1<<6))
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(48000000); /* 48 MHz */
    else
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(30000000); /* 30 MHz */

    start_acquisition(bits, sample_delay, clock_divisor_x2);

    return;
  case CMD_GET_FW_VERSION:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 2)
      goto stall;
    Fx3UartTxString("CMD_GET_FW_VERSION\n");
    DmaBuf[0] = 1;
    DmaBuf[1] = 3;
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3UsbUnstallEp0();
    Fx3UsbDmaDataIn(0, DmaBuf, 2);
    return;
  case CMD_GET_REVID_VERSION:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 1)
      goto stall;
    Fx3UartTxString("CMD_GET_REVID_VERSION\n");
    DmaBuf[0] = 1;
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3UsbUnstallEp0();
    Fx3UsbDmaDataIn(0, DmaBuf, 1);
    return;
  }
 stall:
  Fx3UsbStallEp0();
}

static void SetupData(uint8_t request_type, uint8_t request, uint16_t value,
		      uint16_t index, uint16_t length)
{
  char buf[64];
  snprintf(buf, sizeof(buf),
	   "req: %02x %02x value: %04x index: %04x length: %04x\n",
	   (unsigned)request_type, (unsigned)request,
	   (unsigned)value, (unsigned)index, (unsigned)length);
  Fx3UartTxString(buf);

  if ((request_type & FX3_USB_REQTYPE_TYPE_MASK) == FX3_USB_REQTYPE_TYPE_VENDOR) {
    VendorCommand(request_type, request, value, index, length);
    return;
  }

  if ((request_type & FX3_USB_REQTYPE_TYPE_MASK) != FX3_USB_REQTYPE_TYPE_STD)
    goto stall;

  switch(request) {
  case FX3_USB_STD_REQUEST_GET_DESCRIPTOR:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_STD | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    const uint8_t *descr = GetDescriptor(value>>8, value&0xff);
    if (!descr) goto stall;
    uint8_t descr_type = descr[1];
    uint16_t len = (descr_type == FX3_USB_DESCRIPTOR_CONFIGURATION ||
		    descr_type == FX3_USB_DESCRIPTOR_BOS?
		    *(const uint16_t *)(descr+2) : *descr);
    if (len < length)
      length = len;
    Fx3UsbUnstallEp0();
    Fx3UsbDmaDataIn(0, descr, length);
    return;

  case FX3_USB_STD_REQUEST_SET_CONFIGURATION:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_STD | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 1)
      goto stall;

    Fx3UsbEnableInEndpoint(2, FX3_USB_EP_BULK, 512);
    Fx3UsbUnstallEp0();
    return;
  }

 stall:
  Fx3UsbStallEp0();
}

int main(void)
{
  static const struct Fx3UsbCallbacks callbacks = {
    .sutok = SetupData
  };

  Fx3CacheEnableCaches();
  Fx3IrqInit();

  Fx3GctlInitClock();
  *(volatile uint32_t *)(void *)0x400020e8 = 0;
  Fx3GctlInitIoMatrix(FX3_GCTL_ALTFUNC_GPIF32BIT_UART_I2S);
  Fx3UartInit(115200, FX3_UART_NO_PARITY, FX3_UART_1_STOP_BIT);
  Fx3UartTxString("\nGood moaning!\n");
  Fx3UartTxFlush();

  Fx3GpioInitClock();
  Fx3GpioSetupSimple(45,
		     FX3_GPIO_SIMPLE_ENABLE |
		     FX3_GPIO_SIMPLE_INPUT_EN);
  Fx3GpioSetupSimple(54,
		     FX3_GPIO_SIMPLE_ENABLE |
		     FX3_GPIO_SIMPLE_DRIVE_HI_EN |
		     FX3_GPIO_SIMPLE_DRIVE_LO_EN);

  // Divide the GPIO fast clock into a 1kHz clock output for reference
  Fx3GpioSetupComplex(50,
		      FX3_PIN_STATUS_ENABLE |
		      (FX3_GPIO_TIMER_MODE_FAST_CLK << FX3_PIN_STATUS_TIMER_MODE_SHIFT) |
		      (FX3_GPIO_PIN_MODE_PWM << FX3_PIN_STATUS_MODE_SHIFT) |
		      FX3_PIN_STATUS_DRIVE_HI_EN |
		      FX3_PIN_STATUS_DRIVE_LO_EN,
		      0, GPIO_FAST_CLK/1000, GPIO_FAST_CLK/2000);

  Fx3IrqEnableInterrupts();

  Fx3UsbInit(&callbacks);
  Fx3UsbConnect();

  for(;;) {
    Fx3GpioSetOutputValueSimple(54, 1);
    Fx3UtilDelayUs(500000);
    if (!Fx3GpioGetInputValueSimple(45)) {
      Fx3UartTxString("BUTTON\n");
      Fx3UartTxFlush();
      Fx3GctlHardReset();
    }
    Fx3GpioSetOutputValueSimple(54, 0);
    Fx3UtilDelayUs(500000);
  }
}
