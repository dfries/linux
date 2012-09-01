#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/usb/otg.h>
#include <linux/i2c/twl4030.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include "../musb/musb_core.h"

#include <asm/mach-types.h>
#include <mach/board-rx51.h>

/* copied from twl4030-usb.c because they aren't in a header file */
#define VENDOR_ID_LO			0x00
#define VENDOR_ID_HI			0x01
#define PRODUCT_ID_LO			0x02
#define PRODUCT_ID_HI			0x03

#define FUNC_CTRL			0x04
#define FUNC_CTRL_SET			0x05
#define FUNC_CTRL_CLR			0x06
#define FUNC_CTRL_SUSPENDM		(1 << 6)
#define FUNC_CTRL_RESET			(1 << 5)
#define FUNC_CTRL_OPMODE_MASK		(3 << 3) /* bits 3 and 4 */
#define FUNC_CTRL_OPMODE_NORMAL		(0 << 3)
#define FUNC_CTRL_OPMODE_NONDRIVING	(1 << 3)
#define FUNC_CTRL_OPMODE_DISABLE_BIT_NRZI	(2 << 3)
#define FUNC_CTRL_TERMSELECT		(1 << 2)
#define FUNC_CTRL_XCVRSELECT_MASK	(3 << 0) /* bits 0 and 1 */
#define FUNC_CTRL_XCVRSELECT_HS		(0 << 0)
#define FUNC_CTRL_XCVRSELECT_FS		(1 << 0)
#define FUNC_CTRL_XCVRSELECT_LS		(2 << 0)
#define FUNC_CTRL_XCVRSELECT_FS4LS	(3 << 0)

#define IFC_CTRL			0x07
#define IFC_CTRL_SET			0x08
#define IFC_CTRL_CLR			0x09
#define IFC_CTRL_INTERFACE_PROTECT_DISABLE	(1 << 7)
#define IFC_CTRL_AUTORESUME		(1 << 4)
#define IFC_CTRL_CLOCKSUSPENDM		(1 << 3)
#define IFC_CTRL_CARKITMODE		(1 << 2)
#define IFC_CTRL_FSLSSERIALMODE_3PIN	(1 << 1)

#define TWL4030_OTG_CTRL		0x0A
#define TWL4030_OTG_CTRL_SET		0x0B
#define TWL4030_OTG_CTRL_CLR		0x0C
#define TWL4030_OTG_CTRL_DRVVBUS	(1 << 5)
#define TWL4030_OTG_CTRL_CHRGVBUS	(1 << 4)
#define TWL4030_OTG_CTRL_DISCHRGVBUS	(1 << 3)
#define TWL4030_OTG_CTRL_DMPULLDOWN	(1 << 2)
#define TWL4030_OTG_CTRL_DPPULLDOWN	(1 << 1)
#define TWL4030_OTG_CTRL_IDPULLUP	(1 << 0)

#define USB_INT_EN_RISE			0x0D
#define USB_INT_EN_RISE_SET		0x0E
#define USB_INT_EN_RISE_CLR		0x0F
#define USB_INT_EN_FALL			0x10
#define USB_INT_EN_FALL_SET		0x11
#define USB_INT_EN_FALL_CLR		0x12
#define USB_INT_STS			0x13
#define USB_INT_LATCH			0x14
#define USB_INT_IDGND			(1 << 4)
#define USB_INT_SESSEND			(1 << 3)
#define USB_INT_SESSVALID		(1 << 2)
#define USB_INT_VBUSVALID		(1 << 1)
#define USB_INT_HOSTDISCONNECT		(1 << 0)

#define CARKIT_CTRL			0x19
#define CARKIT_CTRL_SET			0x1A
#define CARKIT_CTRL_CLR			0x1B
#define CARKIT_CTRL_MICEN		(1 << 6)
#define CARKIT_CTRL_SPKRIGHTEN		(1 << 5)
#define CARKIT_CTRL_SPKLEFTEN		(1 << 4)
#define CARKIT_CTRL_RXDEN		(1 << 3)
#define CARKIT_CTRL_TXDEN		(1 << 2)
#define CARKIT_CTRL_IDGNDDRV		(1 << 1)
#define CARKIT_CTRL_CARKITPWR		(1 << 0)
#define CARKIT_PLS_CTRL			0x22
#define CARKIT_PLS_CTRL_SET		0x23
#define CARKIT_PLS_CTRL_CLR		0x24
#define CARKIT_PLS_CTRL_SPKRRIGHT_BIASEN	(1 << 3)
#define CARKIT_PLS_CTRL_SPKRLEFT_BIASEN	(1 << 2)
#define CARKIT_PLS_CTRL_RXPLSEN		(1 << 1)
#define CARKIT_PLS_CTRL_TXPLSEN		(1 << 0)

#define MCPC_CTRL			0x30
#define MCPC_CTRL_SET			0x31
#define MCPC_CTRL_CLR			0x32
#define MCPC_CTRL_RTSOL			(1 << 7)
#define MCPC_CTRL_EXTSWR		(1 << 6)
#define MCPC_CTRL_EXTSWC		(1 << 5)
#define MCPC_CTRL_VOICESW		(1 << 4)
#define MCPC_CTRL_OUT64K		(1 << 3)
#define MCPC_CTRL_RTSCTSSW		(1 << 2)
#define MCPC_CTRL_HS_UART		(1 << 0)

#define MCPC_IO_CTRL			0x33
#define MCPC_IO_CTRL_SET		0x34
#define MCPC_IO_CTRL_CLR		0x35
#define MCPC_IO_CTRL_MICBIASEN		(1 << 5)
#define MCPC_IO_CTRL_CTS_NPU		(1 << 4)
#define MCPC_IO_CTRL_RXD_PU		(1 << 3)
#define MCPC_IO_CTRL_TXDTYP		(1 << 2)
#define MCPC_IO_CTRL_CTSTYP		(1 << 1)
#define MCPC_IO_CTRL_RTSTYP		(1 << 0)

#define MCPC_CTRL2			0x36
#define MCPC_CTRL2_SET			0x37
#define MCPC_CTRL2_CLR			0x38
#define MCPC_CTRL2_MCPC_CK_EN		(1 << 0)

#define OTHER_FUNC_CTRL			0x80
#define OTHER_FUNC_CTRL_SET		0x81
#define OTHER_FUNC_CTRL_CLR		0x82
#define OTHER_FUNC_CTRL_BDIS_ACON_EN	(1 << 4)
#define OTHER_FUNC_CTRL_FIVEWIRE_MODE	(1 << 2)

#define OTHER_IFC_CTRL			0x83
#define OTHER_IFC_CTRL_SET		0x84
#define OTHER_IFC_CTRL_CLR		0x85
#define OTHER_IFC_CTRL_OE_INT_EN	(1 << 6)
#define OTHER_IFC_CTRL_CEA2011_MODE	(1 << 5)
#define OTHER_IFC_CTRL_FSLSSERIALMODE_4PIN	(1 << 4)
#define OTHER_IFC_CTRL_HIZ_ULPI_60MHZ_OUT	(1 << 3)
#define OTHER_IFC_CTRL_HIZ_ULPI		(1 << 2)
#define OTHER_IFC_CTRL_ALT_INT_REROUTE	(1 << 0)

#define OTHER_INT_EN_RISE		0x86
#define OTHER_INT_EN_RISE_SET		0x87
#define OTHER_INT_EN_RISE_CLR		0x88
#define OTHER_INT_EN_FALL		0x89
#define OTHER_INT_EN_FALL_SET		0x8A
#define OTHER_INT_EN_FALL_CLR		0x8B
#define OTHER_INT_STS			0x8C
#define OTHER_INT_LATCH			0x8D
#define OTHER_INT_VB_SESS_VLD		(1 << 7)
#define OTHER_INT_DM_HI			(1 << 6) /* not valid for "latch" reg */
#define OTHER_INT_DP_HI			(1 << 5) /* not valid for "latch" reg */
#define OTHER_INT_BDIS_ACON		(1 << 3) /* not valid for "fall" regs */
#define OTHER_INT_MANU			(1 << 1)
#define OTHER_INT_ABNORMAL_STRESS	(1 << 0)

#define ID_STATUS			0x96
#define ID_RES_FLOAT			(1 << 4)
#define ID_RES_440K			(1 << 3)
#define ID_RES_200K			(1 << 2)
#define ID_RES_102K			(1 << 1)
#define ID_RES_GND			(1 << 0)

#define POWER_CTRL			0xAC
#define POWER_CTRL_SET			0xAD
#define POWER_CTRL_CLR			0xAE
#define POWER_CTRL_OTG_ENAB		(1 << 5)

#define OTHER_IFC_CTRL2			0xAF
#define OTHER_IFC_CTRL2_SET		0xB0
#define OTHER_IFC_CTRL2_CLR		0xB1
#define OTHER_IFC_CTRL2_ULPI_STP_LOW	(1 << 4)
#define OTHER_IFC_CTRL2_ULPI_TXEN_POL	(1 << 3)
#define OTHER_IFC_CTRL2_ULPI_4PIN_2430	(1 << 2)
#define OTHER_IFC_CTRL2_USB_INT_OUTSEL_MASK	(3 << 0) /* bits 0 and 1 */
#define OTHER_IFC_CTRL2_USB_INT_OUTSEL_INT1N	(0 << 0)
#define OTHER_IFC_CTRL2_USB_INT_OUTSEL_INT2N	(1 << 0)

#define REG_CTRL_EN			0xB2
#define REG_CTRL_EN_SET			0xB3
#define REG_CTRL_EN_CLR			0xB4
#define REG_CTRL_ERROR			0xB5
#define ULPI_I2C_CONFLICT_INTEN		(1 << 0)

#define OTHER_FUNC_CTRL2		0xB8
#define OTHER_FUNC_CTRL2_SET		0xB9
#define OTHER_FUNC_CTRL2_CLR		0xBA
#define OTHER_FUNC_CTRL2_VBAT_TIMER_EN	(1 << 0)

/* following registers do not have separate _clr and _set registers */
#define VBUS_DEBOUNCE			0xC0
#define ID_DEBOUNCE			0xC1
#define VBAT_TIMER			0xD3
#define PHY_PWR_CTRL			0xFD
/* 0 normal state, 1 power down */
#define PHY_PWR_PHYPWD			(1 << 0)
#define PHY_CLK_CTRL			0xFE
#define PHY_CLK_CTRL_CLOCKGATING_EN	(1 << 2)
#define PHY_CLK_CTRL_CLK32K_EN		(1 << 1)
#define REQ_PHY_DPLL_CLK		(1 << 0)
#define PHY_CLK_CTRL_STS		0xFF
#define PHY_DPLL_CLK			(1 << 0)

/* In module TWL4030_MODULE_PM_MASTER */
#define PROTECT_KEY			0x0E

/* In module TWL4030_MODULE_PM_RECEIVER */
#define VUSB_DEDICATED1			0x7D
#define VUSB_DEDICATED2			0x7E
#define VUSB1V5_DEV_GRP			0x71
#define VUSB1V5_TYPE			0x72
#define VUSB1V5_REMAP			0x73
#define VUSB1V8_DEV_GRP			0x74
#define VUSB1V8_TYPE			0x75
#define VUSB1V8_REMAP			0x76
#define VUSB3V1_DEV_GRP			0x77
#define VUSB3V1_TYPE			0x78
#define VUSB3V1_REMAP			0x79

/* In module TWL4030_MODULE_INTBR */
#define PMBR1				0x0D
#define GPIO_USB_4PIN_ULPI_2430C	(3 << 0)



enum linkstat {
	USB_LINK_UNKNOWN = 0,
	USB_LINK_NONE,
	USB_LINK_VBUS,
	USB_LINK_ID,
};

struct twl4030_usb {
	struct otg_transceiver	otg;
	struct device		*dev;

	/* TWL4030 internal USB regulator supplies */
	struct regulator	*usb1v5;
	struct regulator	*usb1v8;
	struct regulator	*usb3v1;

	/* for vbus reporting with irqs disabled */
	spinlock_t		lock;

	/* pin configuration */
	enum twl4030_usb_mode	usb_mode;

	int			irq;
	u8			linkstat;
	u8			asleep;
	bool			irq_enabled;

	struct delayed_work	work;
	int			work_inited;
};
extern struct twl4030_usb *g_twl;

/* internal define on top of container_of */
#define xceiv_to_twl(x)         container_of((x), struct twl4030_usb, otg);

int twl4030_usb_write(struct twl4030_usb *twl, u8 address, u8 data);
int twl4030_usb_read(struct twl4030_usb *twl, u8 address);
int twl4030_usb_clear_bits(struct twl4030_usb *twl, u8 reg, u8 bits);
void twl4030_usb_set_mode(struct twl4030_usb *twl, int mode);
void twl4030_phy_power(struct twl4030_usb *twl, int on);
int twl4030_set_suspend(struct otg_transceiver *x, int suspend);
int twl4030_is_asleep(struct twl4030_usb *twl);

#define read_print(reg) \
	printk(#reg " 0x%x\n", twl4030_usb_read(g_twl, reg));

static int __init twl4030_hack_init(void)
{
	u8 val;
	int asleep=twl4030_is_asleep(g_twl);
	printk("%s twl4030 asleep %d\n", __func__, asleep);
	/* suspend calls into save/restore musb*ctx, not needed when
	 * it is going right back to sleep
	#define USE_SUSPEND
	*/
	#define USE_PHY_POWER
	/* note some or all of the regulators must be
	 * enabled, so this isn't sufficient, might as well use
	 * USE_PHY_POWER which enables the regulators
	#define DIRECT_ENABLE
	*/

	#if defined(USE_SUSPEND)
	twl4030_set_suspend(&g_twl->otg, 0);
	#elif defined(USE_PHY_POWER)
	if(asleep)
		twl4030_phy_power(g_twl, 1);
	#elif defined(DIRECT_ENABLE)
	/* 0 enable PHY */
	twl4030_usb_write(g_twl, PHY_PWR_CTRL, 0);
	for(val=0; val<255; ++val)
	{
		if(twl4030_usb_read(g_twl, VENDOR_ID_LO) == 0x51)
			break;
		msleep(1);
	}
	printk("%s %d to turn on PHY\n", __func__, val);
	#endif
	read_print(VENDOR_ID_LO);
	read_print(VENDOR_ID_HI);
	read_print(PRODUCT_ID_LO);
	read_print(PRODUCT_ID_HI);
	read_print(FUNC_CTRL);
	read_print(POWER_CTRL);
	read_print(OTHER_FUNC_CTRL2);
	read_print(VBAT_TIMER);
	read_print(PHY_PWR_CTRL);
	read_print(PHY_CLK_CTRL);
	read_print(PHY_CLK_CTRL_STS);

	#if 0
	read_print(POWER_CTRL);
	/* disable complete OTG block */
	twl4030_usb_clear_bits(g_twl, POWER_CTRL, POWER_CTRL_OTG_ENAB);
	read_print(POWER_CTRL);
	#endif

	#if defined(USE_SUSPEND)
	twl4030_set_suspend(&g_twl->otg, asleep);
	#elif defined(USE_PHY_POWER)
	/* leave it like we found it, asleep if it was asleep */
	if(asleep)
		twl4030_phy_power(g_twl, 0);
	#elif defined(DIRECT_ENABLE)
	/* 1 disable PHY */
	twl4030_usb_write(g_twl, PHY_PWR_CTRL, PHY_PWR_PHYPWD);
	#endif

	#if 0
	val = twl4030_usb_read(g_twl, PHY_CLK_CTRL);
	read_print(PHY_CLK_CTRL);
	/* autogate 60MHz ULPI clock,
	 * clear dpll clock request for i2c access,
	 * disable 32KHz
	 */
	val |= PHY_CLK_CTRL_CLOCKGATING_EN;
	val &= ~(PHY_CLK_CTRL_CLK32K_EN | REQ_PHY_DPLL_CLK);
	printk(KERN_DEBUG "set PHY_CLK_CTRL %x\n", val);
	twl4030_usb_write(g_twl, PHY_CLK_CTRL, val);
	read_print(PHY_CLK_CTRL);
	read_print(PHY_CLK_CTRL_STS);
	#endif

	#if 0
	/* powers down entire PHY and stops MCLK, 1 power down */
	read_print(PHY_PWR_CTRL);
	val = twl4030_usb_read(g_twl, PHY_PWR_CTRL);
	val |= PHY_PWR_PHYPWD;
	twl4030_usb_write(g_twl, PHY_PWR_CTRL, val);
	read_print(PHY_PWR_CTRL);
	twl4030_phy_power(g_twl, 0);
	#endif

	#if 0
	printk(KERN_DEBUG "disable regulators\n");
	regulator_put(g_twl->usb1v5);
	regulator_put(g_twl->usb1v8);
	regulator_disable(g_twl->usb3v1);
	regulator_put(g_twl->usb3v1);
	g_twl->usb1v5 = NULL;
	g_twl->usb1v8 = NULL;
	g_twl->usb3v1 = NULL;
	msleep(60000);
	printk(KERN_DEBUG "getting regulators\n");
	g_twl->usb3v1 = regulator_get(g_twl->dev, "usb3v1");
	regulator_enable(g_twl->usb3v1);
	g_twl->usb1v5 = regulator_get(g_twl->dev, "usb1v5");
	g_twl->usb1v8 = regulator_get(g_twl->dev, "usb1v8");
	#endif

	return -ENODEV;
}
subsys_initcall(twl4030_hack_init);

static void __exit twl4030_hack_exit(void)
{
}
module_exit(twl4030_hack_exit);

MODULE_AUTHOR("David Fries");
/* removing the twl4030-usb module on the N900 (after being modified to
 * be modular), is causing a power drain double that of the screen and
 * backlight on.  This allows poking around the twl4030-usb while it is
 * loaded.
 */
MODULE_DESCRIPTION("Hack module to debug TWL4030 USB transceiver driver");
MODULE_LICENSE("GPL");
