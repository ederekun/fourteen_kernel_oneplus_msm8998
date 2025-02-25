/*
 * Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/usb/phy.h>
#include <linux/reset.h>

#ifdef CONFIG_ARCH_MSM8998
 #define QUSB2PHY_TUNE_BASE		0x1BC
 #define QUSB2PHY_PORT_BASE		0x194
#else
 #define QUSB2PHY_TUNE_BASE		0x0
 #define QUSB2PHY_PORT_BASE		0x0
#endif

#define QUSB2PHY_PORT_TEST_CTRL		QUSB2PHY_PORT_BASE + 0xB8

#define QUSB2PHY_PWR_CTRL1		0x210
#define PWR_CTRL1_CLAMP_N_EN		BIT(1)
#define PWR_CTRL1_POWR_DOWN		BIT(0)

#define QUSB2PHY_PLL_COMMON_STATUS_ONE	0x1A0
#define CORE_READY_STATUS		BIT(0)

#define QUSB2PHY_PLL_TEST		0x04

#define QUSB2PHY_PORT_TUNE2             QUSB2PHY_TUNE_BASE + 0x84

/* Get TUNE2's high nibble value read from efuse */
#define TUNE2_HIGH_NIBBLE_VAL(val, pos, mask)	((val >> pos) & mask)

#define QUSB2PHY_PORT_INTR_CTRL         0x22C
#define DMSE_INTR_HIGH_SEL              BIT(4)
#define DPSE_INTR_HIGH_SEL              BIT(3)
#define CHG_DET_INTR_EN                 BIT(2)
#define DMSE_INTR_EN                    BIT(1)
#define DPSE_INTR_EN                    BIT(0)

#define QUSB2PHY_PORT_UTMI_STATUS	0xF4
#define LINESTATE_DP			BIT(0)
#define LINESTATE_DM			BIT(1)


#define QUSB2PHY_1P2_VOL_MIN           1200000 /* uV */
#define QUSB2PHY_1P2_VOL_MAX           1200000 /* uV */
#define QUSB2PHY_1P2_HPM_LOAD          23000

#define QUSB2PHY_1P8_VOL_MIN           1800000 /* uV */
#define QUSB2PHY_1P8_VOL_MAX           1800000 /* uV */
#define QUSB2PHY_1P8_HPM_LOAD          30000   /* uA */

#define QUSB2PHY_3P3_VOL_MIN		3075000 /* uV */
#define QUSB2PHY_3P3_VOL_MAX		3200000 /* uV */
#define QUSB2PHY_3P3_HPM_LOAD		30000	/* uA */

#define QUSB2PHY_REFCLK_ENABLE		BIT(0)

#define QUSB2PHY_PLL_ANALOG_CONTROLS_ONE	0x0
#define QUSB2PHY_PLL_ANALOG_CONTROLS_TWO	0x4

unsigned int phy_tune2;
module_param(phy_tune2, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(phy_tune2, "QUSB PHY v2 TUNE2");

struct qusb_phy {
	struct usb_phy		phy;
	struct mutex		power_lock;
	void __iomem		*base;
	void __iomem		*tune2_efuse_reg;
	void __iomem		*tcsr_clamp_dig_n;

	struct clk		*ref_clk_src;
	struct clk		*ref_clk;
	struct clk		*cfg_ahb_clk;
	struct reset_control	*phy_reset;

	struct regulator	*vdd;
	struct regulator	*vdda33;
	struct regulator	*vdda18;
	struct regulator	*vdda12;
	int			vdd_levels[3]; /* none, low, high */
	int			vdda33_levels[3];
	int			init_seq_len;
	int			*qusb_phy_init_seq;

	u32			tune2_val;
	int			tune2_efuse_bit_pos;
	int			tune2_efuse_num_of_bits;
	int			tune2_efuse_correction;

	bool			power_enabled_ref;
	bool			cable_connected;
	bool			suspended;
	bool			rm_pulldown;

	struct regulator_desc	dpdm_rdesc;
	struct regulator_dev	*dpdm_rdev;

	/* emulation targets specific */
	void __iomem		*emu_phy_base;
	bool			emulation;
	int			*emu_init_seq;
	int			emu_init_seq_len;
	int			*phy_pll_reset_seq;
	int			phy_pll_reset_seq_len;
	int			*emu_dcm_reset_seq;
	int			emu_dcm_reset_seq_len;
};

static void qusb_phy_enable_clocks(struct qusb_phy *qphy, bool on)
{
	dev_dbg(qphy->phy.dev, "%s(): on:%d\n", __func__, on);

	if (on) {
		clk_prepare_enable(qphy->ref_clk_src);
		clk_prepare_enable(qphy->ref_clk);
		clk_prepare_enable(qphy->cfg_ahb_clk);
	} else {
		clk_disable_unprepare(qphy->cfg_ahb_clk);
		clk_disable_unprepare(qphy->ref_clk);
		clk_disable_unprepare(qphy->ref_clk_src);
	}

}

static int qusb_phy_config_vdd(struct qusb_phy *qphy, int high)
{
	int min, ret;

	min = high ? 1 : 0; /* low or none? */
	ret = regulator_set_voltage(qphy->vdd, qphy->vdd_levels[min],
						qphy->vdd_levels[2]);
	if (ret) {
		dev_err(qphy->phy.dev, "unable to set voltage for qusb vdd\n");
		return ret;
	}

	dev_dbg(qphy->phy.dev, "min_vol:%d max_vol:%d\n",
			qphy->vdd_levels[min], qphy->vdd_levels[2]);
	return ret;
}

static int qusb_phy_enable_power(struct qusb_phy *qphy, bool on)
{
	int ret = 0;

	mutex_lock(&qphy->power_lock);

	dev_dbg(qphy->phy.dev,
		"%s:req to turn %s regulators. power_enabled_ref:%d\n",
			__func__, on ? "on" : "off", qphy->power_enabled_ref);

	if (on && ++qphy->power_enabled_ref > 1) {
		dev_dbg(qphy->phy.dev, "PHYs' regulators are already on\n");
		goto done;
	}

	if (!on) {
		if (on == qphy->power_enabled_ref) {
			dev_dbg(qphy->phy.dev,
				"PHYs' regulators are already off\n");
			goto done;
		}

		qphy->power_enabled_ref--;
		if (!qphy->power_enabled_ref)
			goto disable_vdda33;

		dev_dbg(qphy->phy.dev, "Skip turning off PHYs' regulators\n");
		goto done;
	}

	ret = qusb_phy_config_vdd(qphy, true);
	if (ret) {
		dev_err(qphy->phy.dev, "Unable to config VDD:%d\n",
							ret);
		goto err_vdd;
	}

	ret = regulator_enable(qphy->vdd);
	if (ret) {
		dev_err(qphy->phy.dev, "Unable to enable VDD\n");
		goto unconfig_vdd;
	}

	ret = regulator_set_load(qphy->vdda12, QUSB2PHY_1P2_HPM_LOAD);
	if (ret < 0) {
		dev_err(qphy->phy.dev, "Unable to set HPM of vdda12:%d\n", ret);
		goto disable_vdd;
	}

	ret = regulator_set_voltage(qphy->vdda12, QUSB2PHY_1P2_VOL_MIN,
						QUSB2PHY_1P2_VOL_MAX);
	if (ret) {
		dev_err(qphy->phy.dev,
				"Unable to set voltage for vdda12:%d\n", ret);
		goto put_vdda12_lpm;
	}

	ret = regulator_enable(qphy->vdda12);
	if (ret) {
		dev_err(qphy->phy.dev, "Unable to enable vdda12:%d\n", ret);
		goto unset_vdda12;
	}

	ret = regulator_set_load(qphy->vdda18, QUSB2PHY_1P8_HPM_LOAD);
	if (ret < 0) {
		dev_err(qphy->phy.dev, "Unable to set HPM of vdda18:%d\n", ret);
		goto disable_vdda12;
	}

	ret = regulator_set_voltage(qphy->vdda18, QUSB2PHY_1P8_VOL_MIN,
						QUSB2PHY_1P8_VOL_MAX);
	if (ret) {
		dev_err(qphy->phy.dev,
				"Unable to set voltage for vdda18:%d\n", ret);
		goto put_vdda18_lpm;
	}

	ret = regulator_enable(qphy->vdda18);
	if (ret) {
		dev_err(qphy->phy.dev, "Unable to enable vdda18:%d\n", ret);
		goto unset_vdda18;
	}

	ret = regulator_set_load(qphy->vdda33, QUSB2PHY_3P3_HPM_LOAD);
	if (ret < 0) {
		dev_err(qphy->phy.dev, "Unable to set HPM of vdda33:%d\n", ret);
		goto disable_vdda18;
	}

	ret = regulator_set_voltage(qphy->vdda33, qphy->vdda33_levels[0],
						qphy->vdda33_levels[2]);
	if (ret) {
		dev_err(qphy->phy.dev,
				"Unable to set voltage for vdda33:%d\n", ret);
		goto put_vdda33_lpm;
	}

	ret = regulator_enable(qphy->vdda33);
	if (ret) {
		dev_err(qphy->phy.dev, "Unable to enable vdda33:%d\n", ret);
		goto unset_vdd33;
	}

	pr_debug("%s(): QUSB PHY's regulators are turned ON.\n", __func__);

	mutex_unlock(&qphy->power_lock);
	return ret;

disable_vdda33:
	ret = regulator_disable(qphy->vdda33);
	if (ret)
		dev_err(qphy->phy.dev, "Unable to disable vdda33:%d\n", ret);

unset_vdd33:
	ret = regulator_set_voltage(qphy->vdda33, 0, qphy->vdda33_levels[2]);
	if (ret)
		dev_err(qphy->phy.dev,
			"Unable to set (0) voltage for vdda33:%d\n", ret);

put_vdda33_lpm:
	ret = regulator_set_load(qphy->vdda33, 0);
	if (ret < 0)
		dev_err(qphy->phy.dev, "Unable to set (0) HPM of vdda33\n");

disable_vdda18:
	ret = regulator_disable(qphy->vdda18);
	if (ret)
		dev_err(qphy->phy.dev, "Unable to disable vdda18:%d\n", ret);

unset_vdda18:
	ret = regulator_set_voltage(qphy->vdda18, 0, QUSB2PHY_1P8_VOL_MAX);
	if (ret)
		dev_err(qphy->phy.dev,
			"Unable to set (0) voltage for vdda18:%d\n", ret);

put_vdda18_lpm:
	ret = regulator_set_load(qphy->vdda18, 0);
	if (ret < 0)
		dev_err(qphy->phy.dev, "Unable to set LPM of vdda18\n");

disable_vdda12:
	ret = regulator_disable(qphy->vdda12);
	if (ret)
		dev_err(qphy->phy.dev, "Unable to disable vdda12:%d\n", ret);
unset_vdda12:
	ret = regulator_set_voltage(qphy->vdda12, 0, QUSB2PHY_1P2_VOL_MAX);
	if (ret)
		dev_err(qphy->phy.dev,
			"Unable to set (0) voltage for vdda12:%d\n", ret);
put_vdda12_lpm:
	ret = regulator_set_load(qphy->vdda12, 0);
	if (ret < 0)
		dev_err(qphy->phy.dev, "Unable to set LPM of vdda12\n");

disable_vdd:
	ret = regulator_disable(qphy->vdd);
	if (ret)
		dev_err(qphy->phy.dev, "Unable to disable vdd:%d\n",
								ret);

unconfig_vdd:
	ret = qusb_phy_config_vdd(qphy, false);
	if (ret)
		dev_err(qphy->phy.dev, "Unable unconfig VDD:%d\n",
								ret);
err_vdd:
	dev_dbg(qphy->phy.dev, "QUSB PHY's regulators are turned OFF.\n");

	/* in case of error in turning on regulators */
	if (qphy->power_enabled_ref)
		qphy->power_enabled_ref--;
done:
	mutex_unlock(&qphy->power_lock);
	return ret;
}

static void qusb_phy_get_tune2_param(struct qusb_phy *qphy)
{
	u8 num_of_bits;
	u32 bit_mask = 1;
	u8 reg_val;

	pr_debug("%s(): num_of_bits:%d bit_pos:%d\n", __func__,
				qphy->tune2_efuse_num_of_bits,
				qphy->tune2_efuse_bit_pos);

	/* get bit mask based on number of bits to use with efuse reg */
	if (qphy->tune2_efuse_num_of_bits) {
		num_of_bits = qphy->tune2_efuse_num_of_bits;
		bit_mask = (bit_mask << num_of_bits) - 1;
	}

	/*
	 * Read EFUSE register having TUNE2 parameter's high nibble.
	 * If efuse register shows value as 0x0, then use previous value
	 * as it is. Otherwise use efuse register based value for this purpose.
	 */
	qphy->tune2_val = readl_relaxed(qphy->tune2_efuse_reg);
	pr_debug("%s(): bit_mask:%d efuse based tune2 value:%d\n",
				__func__, bit_mask, qphy->tune2_val);

	qphy->tune2_val = TUNE2_HIGH_NIBBLE_VAL(qphy->tune2_val,
				qphy->tune2_efuse_bit_pos, bit_mask);

	/* Update higher nibble of TUNE2 value for better rise/fall times */
	if (qphy->tune2_efuse_correction && qphy->tune2_val) {
		if (qphy->tune2_efuse_correction > 5 ||
				qphy->tune2_efuse_correction < -10)
			pr_warn("Correction value is out of range : %d\n",
					qphy->tune2_efuse_correction);
		else
			qphy->tune2_val = qphy->tune2_val +
						qphy->tune2_efuse_correction;
	}

	reg_val = readb_relaxed(qphy->base + QUSB2PHY_PORT_TUNE2);
	if (qphy->tune2_val) {
		reg_val  &= 0x0f;
		reg_val |= (qphy->tune2_val << 4);
	}

	qphy->tune2_val = reg_val;
}

static void qusb_phy_write_seq(void __iomem *base, u32 *seq, int cnt,
		unsigned long delay)
{
	int i;

	pr_debug("Seq count:%d\n", cnt);
	for (i = 0; i < cnt; i = i+2) {
		pr_debug("write 0x%02x to 0x%02x\n", seq[i], seq[i+1]);
		writel_relaxed(seq[i], base + seq[i+1]);
		if (delay)
			usleep_range(delay, (delay + 2000));
	}
}

static int qusb_phy_init(struct usb_phy *phy)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);
	int ret;
	u8 reg;
	bool pll_lock_fail = false;

	dev_dbg(phy->dev, "%s\n", __func__);

	/* bump up vdda33 voltage to operating level*/
	ret = regulator_set_voltage(qphy->vdda33, qphy->vdda33_levels[1],
						qphy->vdda33_levels[2]);
	if (ret) {
		dev_err(qphy->phy.dev,
				"Unable to set voltage for vdda33:%d\n", ret);
		return ret;
	}

	/* Perform phy reset */
	ret = reset_control_assert(qphy->phy_reset);
	if (ret)
		dev_err(phy->dev, "%s: phy_reset assert failed\n", __func__);
	usleep_range(100, 150);
	ret = reset_control_deassert(qphy->phy_reset);
	if (ret)
		dev_err(phy->dev, "%s: phy_reset deassert failed\n", __func__);

	if (qphy->emulation) {
		if (qphy->emu_init_seq)
			qusb_phy_write_seq(qphy->emu_phy_base,
				qphy->emu_init_seq, qphy->emu_init_seq_len, 0);

		if (qphy->qusb_phy_init_seq)
			qusb_phy_write_seq(qphy->base, qphy->qusb_phy_init_seq,
					qphy->init_seq_len, 0);

		/* Wait for 5ms as per QUSB2 RUMI sequence */
		usleep_range(5000, 7000);

		if (qphy->phy_pll_reset_seq)
			qusb_phy_write_seq(qphy->base, qphy->phy_pll_reset_seq,
					qphy->phy_pll_reset_seq_len, 10000);

		if (qphy->emu_dcm_reset_seq)
			qusb_phy_write_seq(qphy->emu_phy_base,
					qphy->emu_dcm_reset_seq,
					qphy->emu_dcm_reset_seq_len, 10000);

		return 0;
	}

	/* Disable the PHY */
	writel_relaxed(readl_relaxed(qphy->base + QUSB2PHY_PWR_CTRL1) |
			PWR_CTRL1_POWR_DOWN,
			qphy->base + QUSB2PHY_PWR_CTRL1);

	if (qphy->qusb_phy_init_seq)
		qusb_phy_write_seq(qphy->base, qphy->qusb_phy_init_seq,
				qphy->init_seq_len, 0);

	/*
	 * Check for EFUSE value only if tune2_efuse_reg is available
	 * and try to read EFUSE value only once i.e. not every USB
	 * cable connect case.
	 */
	if (qphy->tune2_efuse_reg && !phy_tune2) {
		if (!qphy->tune2_val)
			qusb_phy_get_tune2_param(qphy);

		pr_debug("%s(): Programming TUNE2 parameter as:%x\n", __func__,
				qphy->tune2_val);
		writel_relaxed(qphy->tune2_val,
				qphy->base + QUSB2PHY_PORT_TUNE2);
	}

	/* If phy_tune2 modparam set, override tune2 value */
	if (phy_tune2) {
		pr_debug("%s(): (modparam) TUNE2 val:0x%02x\n",
						__func__, phy_tune2);
		writel_relaxed(phy_tune2,
				qphy->base + QUSB2PHY_PORT_TUNE2);
	}

	/* ensure above writes are completed before re-enabling PHY */
	wmb();

	/* Enable the PHY */
	writel_relaxed(readl_relaxed(qphy->base + QUSB2PHY_PWR_CTRL1) &
			~PWR_CTRL1_POWR_DOWN,
			qphy->base + QUSB2PHY_PWR_CTRL1);

	/* Ensure above write is completed before turning ON ref clk */
	wmb();

	/* Require to get phy pll lock successfully */
	usleep_range(150, 160);

	reg = readb_relaxed(qphy->base +
			QUSB2PHY_PLL_COMMON_STATUS_ONE);
	dev_dbg(phy->dev, "QUSB2PHY_PLL_COMMON_STATUS_ONE:%x\n", reg);
	if (!(reg & CORE_READY_STATUS))
		pll_lock_fail = true;

	if (pll_lock_fail) {
		dev_err(phy->dev, "QUSB PHY PLL LOCK fails:%x\n", reg);
	}

	return 0;
}

static void qusb_phy_shutdown(struct usb_phy *phy)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	dev_dbg(phy->dev, "%s\n", __func__);

	qusb_phy_enable_clocks(qphy, true);

	/* Disable the PHY */
	writel_relaxed(readl_relaxed(qphy->base + QUSB2PHY_PWR_CTRL1) |
			PWR_CTRL1_POWR_DOWN,
			qphy->base + QUSB2PHY_PWR_CTRL1);

	/* Make sure above write complete before turning off clocks */
	wmb();

	qusb_phy_enable_clocks(qphy, false);
}
/**
 * Performs QUSB2 PHY suspend/resume functionality.
 *
 * @uphy - usb phy pointer.
 * @suspend - to enable suspend or not. 1 - suspend, 0 - resume
 *
 */
static int qusb_phy_set_suspend(struct usb_phy *phy, int suspend)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);
	u32 linestate = 0, intr_mask = 0;
	static u8 analog_ctrl_two;
	int ret = 0;

	if (qphy->suspended == suspend) {
		dev_dbg(phy->dev, "%s: USB PHY is already suspended\n",
			__func__);
		return 0;
	}

	if (suspend) {
		/* Bus suspend case */
		if (qphy->cable_connected ||
			(qphy->phy.flags & PHY_HOST_MODE)) {

			/* store clock settings like cmos/cml */
			analog_ctrl_two =
				readl_relaxed(qphy->base +
					QUSB2PHY_PLL_ANALOG_CONTROLS_TWO);

			/* use CSR & switch to SE clk */
			writel_relaxed(0xb,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_TWO);

			/* enable clock bypass */
			writel_relaxed(0x90,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_ONE);

			/* Clear all interrupts */
			writel_relaxed(0x00,
				qphy->base + QUSB2PHY_PORT_INTR_CTRL);

			linestate = readl_relaxed(qphy->base +
					QUSB2PHY_PORT_UTMI_STATUS);

			/*
			 * D+/D- interrupts are level-triggered, but we are
			 * only interested if the line state changes, so enable
			 * the high/low trigger based on current state. In
			 * other words, enable the triggers _opposite_ of what
			 * the current D+/D- levels are.
			 * e.g. if currently D+ high, D- low (HS 'J'/Suspend),
			 * configure the mask to trigger on D+ low OR D- high
			 */
			intr_mask = DPSE_INTR_EN | DMSE_INTR_EN;
			if (!(linestate & LINESTATE_DP)) /* D+ low */
				intr_mask |= DPSE_INTR_HIGH_SEL;
			if (!(linestate & LINESTATE_DM)) /* D- low */
				intr_mask |= DMSE_INTR_HIGH_SEL;

			writel_relaxed(intr_mask,
				qphy->base + QUSB2PHY_PORT_INTR_CTRL);

			if (linestate & (LINESTATE_DP | LINESTATE_DM)) {
				/* enable phy auto-resume */
				writel_relaxed(0x0C,
					qphy->base + QUSB2PHY_PORT_TEST_CTRL);
				/* flush the previous write before next write */
				wmb();
				writel_relaxed(0x04,
					qphy->base + QUSB2PHY_PORT_TEST_CTRL);
			}


			dev_dbg(phy->dev, "%s: intr_mask = %x\n",
			__func__, intr_mask);

			/* Makes sure that above write goes through */
			wmb();

			qusb_phy_enable_clocks(qphy, false);
		} else { /* Disconnect case */
			ret = reset_control_assert(qphy->phy_reset);
			if (ret)
				dev_err(qphy->phy.dev, "phyassert failed\n");
			usleep_range(100, 150);
			ret = reset_control_deassert(qphy->phy_reset);
			if (ret)
				dev_err(qphy->phy.dev, "deassert failed\n");

			writel_relaxed(0x1b,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_TWO);

			/* enable clock bypass */
			writel_relaxed(0x90,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_ONE);

			if (qphy->tcsr_clamp_dig_n)
				writel_relaxed(0x0,
					qphy->tcsr_clamp_dig_n);

			/*
			 * clamp needs asserted before
			 * power/clocks can be turned off
			 */
			wmb();

			qusb_phy_enable_clocks(qphy, false);
			qusb_phy_enable_power(qphy, false);
		}
		qphy->suspended = true;
	} else {
		/* Bus suspend case */
		if (qphy->cable_connected ||
			(qphy->phy.flags & PHY_HOST_MODE)) {
			qusb_phy_enable_clocks(qphy, true);

			/* restore the default clock settings */
			writel_relaxed(analog_ctrl_two,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_TWO);

			/* disable clock bypass */
			writel_relaxed(0x80,
				qphy->base + QUSB2PHY_PLL_ANALOG_CONTROLS_ONE);

			/* Clear all interrupts on resume */
			writel_relaxed(0x00,
				qphy->base + QUSB2PHY_PORT_INTR_CTRL);
		} else {
			if (qphy->tcsr_clamp_dig_n)
				writel_relaxed(0x1,
					qphy->tcsr_clamp_dig_n);

			/*
			 * clamp needs de-asserted before
			 * power/clocks can be turned on
			 */
			wmb();

			qusb_phy_enable_power(qphy, true);
			ret = reset_control_assert(qphy->phy_reset);
			if (ret)
				dev_err(phy->dev, "%s: phy_reset assert failed\n",
						__func__);
			usleep_range(100, 150);
			ret = reset_control_deassert(qphy->phy_reset);
			if (ret)
				dev_err(qphy->phy.dev, "deassert failed\n");

			qusb_phy_enable_clocks(qphy, true);
		}
		qphy->suspended = false;
	}

	return 0;
}

static int qusb_phy_notify_connect(struct usb_phy *phy,
					enum usb_device_speed speed)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	qphy->cable_connected = true;

	dev_dbg(phy->dev, "QUSB PHY: connect notification cable_connected=%d\n",
							qphy->cable_connected);
	return 0;
}

static int qusb_phy_notify_disconnect(struct usb_phy *phy,
					enum usb_device_speed speed)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);

	qphy->cable_connected = false;

	dev_dbg(phy->dev, "QUSB PHY: connect notification cable_connected=%d\n",
							qphy->cable_connected);
	return 0;
}

static int qusb_phy_update_dpdm(struct usb_phy *phy, int value)
{
	struct qusb_phy *qphy = container_of(phy, struct qusb_phy, phy);
	int ret = 0;

	dev_dbg(phy->dev, "%s value:%d rm_pulldown:%d\n",
				__func__, value, qphy->rm_pulldown);

	switch (value) {
	case POWER_SUPPLY_DP_DM_DPF_DMF:
		dev_dbg(phy->dev, "POWER_SUPPLY_DP_DM_DPF_DMF\n");
		if (!qphy->rm_pulldown) {
			ret = qusb_phy_enable_power(qphy, true);
			if (ret >= 0) {
				qphy->rm_pulldown = true;
				dev_dbg(phy->dev, "DP_DM_F: rm_pulldown:%d\n",
						qphy->rm_pulldown);
			}
		}
		break;

	case POWER_SUPPLY_DP_DM_DPR_DMR:
		dev_dbg(phy->dev, "POWER_SUPPLY_DP_DM_DPR_DMR\n");
		if (qphy->rm_pulldown) {
			ret = qusb_phy_enable_power(qphy, false);
			if (ret >= 0) {
				qphy->rm_pulldown = false;
				dev_dbg(phy->dev, "DP_DM_R: rm_pulldown:%d\n",
						qphy->rm_pulldown);
			}
		}
		break;

	default:
		ret = -EINVAL;
		dev_err(phy->dev, "Invalid power supply property(%d)\n", value);
		break;
	}

	return ret;
}

static int qusb_phy_dpdm_regulator_enable(struct regulator_dev *rdev)
{
	struct qusb_phy *qphy = rdev_get_drvdata(rdev);

	dev_dbg(qphy->phy.dev, "%s\n", __func__);
	return qusb_phy_update_dpdm(&qphy->phy, POWER_SUPPLY_DP_DM_DPF_DMF);
}

static int qusb_phy_dpdm_regulator_disable(struct regulator_dev *rdev)
{
	struct qusb_phy *qphy = rdev_get_drvdata(rdev);

	dev_dbg(qphy->phy.dev, "%s\n", __func__);
	return qusb_phy_update_dpdm(&qphy->phy, POWER_SUPPLY_DP_DM_DPR_DMR);
}

static int qusb_phy_dpdm_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct qusb_phy *qphy = rdev_get_drvdata(rdev);

	dev_dbg(qphy->phy.dev, "%s qphy->rm_pulldown = %d\n", __func__,
					qphy->rm_pulldown);
	return qphy->rm_pulldown;
}

static struct regulator_ops qusb_phy_dpdm_regulator_ops = {
	.enable		= qusb_phy_dpdm_regulator_enable,
	.disable	= qusb_phy_dpdm_regulator_disable,
	.is_enabled	= qusb_phy_dpdm_regulator_is_enabled,
};

static int qusb_phy_regulator_init(struct qusb_phy *qphy)
{
	struct device *dev = qphy->phy.dev;
	struct regulator_config cfg = {};
	struct regulator_init_data *init_data;

	init_data = devm_kzalloc(dev, sizeof(*init_data), GFP_KERNEL);
	if (!init_data)
		return -ENOMEM;

	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS;
	qphy->dpdm_rdesc.owner = THIS_MODULE;
	qphy->dpdm_rdesc.type = REGULATOR_VOLTAGE;
	qphy->dpdm_rdesc.ops = &qusb_phy_dpdm_regulator_ops;
	qphy->dpdm_rdesc.name = kbasename(dev->of_node->full_name);

	cfg.dev = dev;
	cfg.init_data = init_data;
	cfg.driver_data = qphy;
	cfg.of_node = dev->of_node;

	qphy->dpdm_rdev = devm_regulator_register(dev, &qphy->dpdm_rdesc, &cfg);
	if (IS_ERR(qphy->dpdm_rdev))
		return PTR_ERR(qphy->dpdm_rdev);

	return 0;
}

static int qusb_phy_probe(struct platform_device *pdev)
{
	struct qusb_phy *qphy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret = 0, size = 0;
	bool hold_phy_reset;

	qphy = devm_kzalloc(dev, sizeof(*qphy), GFP_KERNEL);
	if (!qphy)
		return -ENOMEM;

	qphy->phy.dev = dev;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qusb_phy_base");
	qphy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(qphy->base))
		return PTR_ERR(qphy->base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"emu_phy_base");
	if (res) {
		qphy->emu_phy_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(qphy->emu_phy_base)) {
			dev_dbg(dev, "couldn't ioremap emu_phy_base\n");
			qphy->emu_phy_base = NULL;
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"tune2_efuse_addr");
	if (res) {
		qphy->tune2_efuse_reg = devm_ioremap_nocache(dev, res->start,
							resource_size(res));
		if (!IS_ERR_OR_NULL(qphy->tune2_efuse_reg)) {
			ret = of_property_read_u32(dev->of_node,
					"qcom,tune2-efuse-bit-pos",
					&qphy->tune2_efuse_bit_pos);
			if (!ret) {
				ret = of_property_read_u32(dev->of_node,
						"qcom,tune2-efuse-num-bits",
						&qphy->tune2_efuse_num_of_bits);
			}
			of_property_read_u32(dev->of_node,
						"qcom,tune2-efuse-correction",
						&qphy->tune2_efuse_correction);

			if (ret) {
				dev_err(dev, "DT Value for tune2 efuse is invalid.\n");
				return -EINVAL;
			}
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"tcsr_clamp_dig_n_1p8");
	if (res) {
		qphy->tcsr_clamp_dig_n = devm_ioremap_nocache(dev,
				res->start, resource_size(res));
		if (IS_ERR(qphy->tcsr_clamp_dig_n)) {
			dev_err(dev, "err reading tcsr_clamp_dig_n\n");
			qphy->tcsr_clamp_dig_n = NULL;
		}
	}

	qphy->ref_clk_src = devm_clk_get(dev, "ref_clk_src");
	if (IS_ERR(qphy->ref_clk_src))
		dev_dbg(dev, "clk get failed for ref_clk_src\n");

	/* ref_clk is needed only for DIFF_CLK case, hence make it optional. */
	if (of_property_match_string(pdev->dev.of_node,
				"clock-names", "ref_clk") >= 0) {
		qphy->ref_clk = devm_clk_get(dev, "ref_clk");
		if (IS_ERR(qphy->ref_clk)) {
			ret = PTR_ERR(qphy->ref_clk);
			if (ret != -EPROBE_DEFER)
				dev_dbg(dev,
					"clk get failed for ref_clk\n");
			return ret;
		}

		clk_set_rate(qphy->ref_clk, 19200000);
	}

	if (of_property_match_string(pdev->dev.of_node,
				"clock-names", "cfg_ahb_clk") >= 0) {
		qphy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb_clk");
		if (IS_ERR(qphy->cfg_ahb_clk)) {
			ret = PTR_ERR(qphy->cfg_ahb_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
				"clk get failed for cfg_ahb_clk ret %d\n", ret);
			return ret;
		}
	}

	qphy->phy_reset = devm_reset_control_get(dev, "phy_reset");
	if (IS_ERR(qphy->phy_reset))
		return PTR_ERR(qphy->phy_reset);

	qphy->emulation = of_property_read_bool(dev->of_node,
					"qcom,emulation");

	of_get_property(dev->of_node, "qcom,emu-init-seq", &size);
	if (size) {
		qphy->emu_init_seq = devm_kzalloc(dev,
						size, GFP_KERNEL);
		if (qphy->emu_init_seq) {
			qphy->emu_init_seq_len =
				(size / sizeof(*qphy->emu_init_seq));
			if (qphy->emu_init_seq_len % 2) {
				dev_err(dev, "invalid emu_init_seq_len\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,emu-init-seq",
				qphy->emu_init_seq,
				qphy->emu_init_seq_len);
		} else {
			dev_dbg(dev, "error allocating memory for emu_init_seq\n");
		}
	}

	size = 0;
	of_get_property(dev->of_node, "qcom,phy-pll-reset-seq", &size);
	if (size) {
		qphy->phy_pll_reset_seq = devm_kzalloc(dev,
						size, GFP_KERNEL);
		if (qphy->phy_pll_reset_seq) {
			qphy->phy_pll_reset_seq_len =
				(size / sizeof(*qphy->phy_pll_reset_seq));
			if (qphy->phy_pll_reset_seq_len % 2) {
				dev_err(dev, "invalid phy_pll_reset_seq_len\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,phy-pll-reset-seq",
				qphy->phy_pll_reset_seq,
				qphy->phy_pll_reset_seq_len);
		} else {
			dev_dbg(dev, "error allocating memory for phy_pll_reset_seq\n");
		}
	}

	size = 0;
	of_get_property(dev->of_node, "qcom,emu-dcm-reset-seq", &size);
	if (size) {
		qphy->emu_dcm_reset_seq = devm_kzalloc(dev,
						size, GFP_KERNEL);
		if (qphy->emu_dcm_reset_seq) {
			qphy->emu_dcm_reset_seq_len =
				(size / sizeof(*qphy->emu_dcm_reset_seq));
			if (qphy->emu_dcm_reset_seq_len % 2) {
				dev_err(dev, "invalid emu_dcm_reset_seq_len\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,emu-dcm-reset-seq",
				qphy->emu_dcm_reset_seq,
				qphy->emu_dcm_reset_seq_len);
		} else {
			dev_dbg(dev, "error allocating memory for emu_dcm_reset_seq\n");
		}
	}

	size = 0;
	of_get_property(dev->of_node, "qcom,qusb-phy-init-seq", &size);
	if (size) {
		qphy->qusb_phy_init_seq = devm_kzalloc(dev,
						size, GFP_KERNEL);
		if (qphy->qusb_phy_init_seq) {
			qphy->init_seq_len =
				(size / sizeof(*qphy->qusb_phy_init_seq));
			if (qphy->init_seq_len % 2) {
				dev_err(dev, "invalid init_seq_len\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,qusb-phy-init-seq",
				qphy->qusb_phy_init_seq,
				qphy->init_seq_len);
		} else {
			dev_err(dev, "error allocating memory for phy_init_seq\n");
		}
	}

	ret = of_property_read_u32_array(dev->of_node, "qcom,vdd-voltage-level",
					 (u32 *) qphy->vdd_levels,
					 ARRAY_SIZE(qphy->vdd_levels));
	if (ret) {
		dev_err(dev, "error reading qcom,vdd-voltage-level property\n");
		return ret;
	}

	ret = of_property_read_u32_array(dev->of_node,
					"qcom,vdda33-voltage-level",
					 (u32 *) qphy->vdda33_levels,
					 ARRAY_SIZE(qphy->vdda33_levels));
	if (ret == -EINVAL) {
		qphy->vdda33_levels[0] = QUSB2PHY_3P3_VOL_MIN;
		qphy->vdda33_levels[1] = QUSB2PHY_3P3_VOL_MIN;
		qphy->vdda33_levels[2] = QUSB2PHY_3P3_VOL_MAX;
	} else if (ret) {
		dev_err(dev, "error reading qcom,vdda33-voltage-level property\n");
		return ret;
	}

	qphy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(qphy->vdd)) {
		dev_err(dev, "unable to get vdd supply\n");
		return PTR_ERR(qphy->vdd);
	}

	qphy->vdda33 = devm_regulator_get(dev, "vdda33");
	if (IS_ERR(qphy->vdda33)) {
		dev_err(dev, "unable to get vdda33 supply\n");
		return PTR_ERR(qphy->vdda33);
	}

	qphy->vdda18 = devm_regulator_get(dev, "vdda18");
	if (IS_ERR(qphy->vdda18)) {
		dev_err(dev, "unable to get vdda18 supply\n");
		return PTR_ERR(qphy->vdda18);
	}

	qphy->vdda12 = devm_regulator_get(dev, "vdda12");
	if (IS_ERR(qphy->vdda12)) {
		dev_err(dev, "unable to get vdda12 supply\n");
		return PTR_ERR(qphy->vdda12);
	}

	mutex_init(&qphy->power_lock);
	platform_set_drvdata(pdev, qphy);

	qphy->phy.label			= "msm-qusb-phy";
	qphy->phy.init			= qusb_phy_init;
	qphy->phy.set_suspend           = qusb_phy_set_suspend;
	qphy->phy.shutdown		= qusb_phy_shutdown;
	qphy->phy.type			= USB_PHY_TYPE_USB2;
	qphy->phy.notify_connect        = qusb_phy_notify_connect;
	qphy->phy.notify_disconnect     = qusb_phy_notify_disconnect;

	/*
	 * On some platforms multiple QUSB PHYs are available. If QUSB PHY is
	 * not used, there is leakage current seen with QUSB PHY related voltage
	 * rail. Hence keep QUSB PHY into reset state explicitly here.
	 */
	if (hold_phy_reset) {
		ret = reset_control_assert(qphy->phy_reset);
		if (ret)
			dev_err(dev, "%s:phy_reset assert failed\n", __func__);
	}

	ret = usb_add_phy_dev(&qphy->phy);
	if (ret)
		return ret;

	ret = qusb_phy_regulator_init(qphy);
	if (ret)
		usb_remove_phy(&qphy->phy);

	/* de-assert clamp dig n to reduce leakage on 1p8 upon boot up */
	if (qphy->tcsr_clamp_dig_n)
		writel_relaxed(0x0, qphy->tcsr_clamp_dig_n);

	qphy->suspended = true;

	return ret;
}

static int qusb_phy_remove(struct platform_device *pdev)
{
	struct qusb_phy *qphy = platform_get_drvdata(pdev);

	usb_remove_phy(&qphy->phy);
	qphy->cable_connected = false;
	qusb_phy_set_suspend(&qphy->phy, true);

	return 0;
}

static const struct of_device_id qusb_phy_id_table[] = {
	{ .compatible = "qcom,qusb2phy-v2", },
	{ },
};
MODULE_DEVICE_TABLE(of, qusb_phy_id_table);

static struct platform_driver qusb_phy_driver = {
	.probe		= qusb_phy_probe,
	.remove		= qusb_phy_remove,
	.driver = {
		.name	= "msm-qusb-phy-v2",
		.of_match_table = of_match_ptr(qusb_phy_id_table),
	},
};

module_platform_driver(qusb_phy_driver);

MODULE_DESCRIPTION("MSM QUSB2 PHY v2 driver");
MODULE_LICENSE("GPL v2");
