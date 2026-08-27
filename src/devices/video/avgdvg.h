// license:BSD-3-Clause
// copyright-holders:Mathis Rosenhauer
// thanks-to:Eric Smith, Brad Oliver, Bernd Wiebelt, Aaron Giles, Andrew Caldwell
#ifndef MAME_VIDEO_AVGDVG_H
#define MAME_VIDEO_AVGDVG_H

#pragma once

#include "video/vector.h"


class avgdvg_device_base : public device_t
{
public:
	template <typename T> void set_vector(T &&tag)
	{
		m_vector.set_tag(std::forward<T>(tag));
	}
	template <typename T> void set_memory(T &&tag, int no, offs_t base)
	{
		m_memspace.set_tag(std::forward<T>(tag), no);
		m_membase = base;
	}

	int done_r();
	void go_w(u8 data = 0);
	void reset_w(u8 data = 0);

	void go_word_w(u16 data = 0);
	void reset_word_w(u16 data = 0);

	// Tempest and Quantum use this capability
	void set_flip_x(bool flip) { m_flip_x = flip; }
	void set_flip_y(bool flip) { m_flip_y = flip; }

protected:
	static constexpr unsigned MAXVECT = 10000;

	struct vgvector
	{
		int x; int y;
		rgb_t color;
		int intensity;
		int arg1; int arg2;
		int status;
		u8 clip_latched;    // VGCLIP: which edges the hardware re-sampled here (window_edge bits)
		float beam_energy;  // normalized (0..1) raw beam energy, or < 0 when the device has none
		attotime t0, t1;    // absolute machine time the beam spent drawing this event (never = untimed)
	};

	avgdvg_device_base(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock);

	virtual void device_start() override ATTR_COLD;

	virtual int handler_0() = 0;
	virtual int handler_1() = 0;
	virtual int handler_2() = 0;
	virtual int handler_3() = 0;
	virtual int handler_4() = 0;
	virtual int handler_5() = 0;
	virtual int handler_6() = 0;
	virtual int handler_7() = 0;
	virtual u8 state_addr() = 0;
	virtual void update_databus() = 0;
	virtual void vggo() = 0;
	virtual void vgrst() = 0;

	u8 OP0() const { return BIT(m_op, 0); }
	u8 OP1() const { return BIT(m_op, 1); }
	u8 OP2() const { return BIT(m_op, 2); }
	u8 OP3() const { return BIT(m_op, 3); }

	u8 ST3() const { return BIT(m_state_latch, 3); }

	void apply_flipping(int &x, int &y) const;
	void vg_set_halt(int dummy);

	void vg_flush();
	void vg_flush_list_end();
	void vg_add_point_buf(int x, int y, rgb_t color, int intensity, float beam_energy = -1.0f);
	void vg_add_clip(int xmin, int ymin, int xmax, int ymax, u8 latched = 0);

	required_device<vector_device> m_vector;
	required_address_space m_memspace;
	offs_t m_membase;

	// Clip-window sample-and-hold model. The hardware latches the CURRENT beam position into a
	// 1000pF mylar cap through an LF13201 analog switch, buffers it with a JFET-input follower and
	// compares the live position against it - so a window edge is not a fixed voltage but a held
	// sample, and it wanders as the cap droops. window_clip() reproduces that; with the options at 0
	// it returns the latched value unchanged (ideal hold = stock).
	//
	// Major Havoc holds ONE edge (ymin; the other three are constants in its vg_add_clip call) with
	// TL082 + LM819. Battlezone runs the same circuit twice: HST latches (xmax, ymin) and LST latches
	// (xmin, ymax), each at the instant its own switch opens, so all four edges are held - same
	// analog switch and the same 1000pF cap, TL084 + LM319 around it. Each edge therefore needs its
	// own sample time, bias and dielectric memory: Battlezone re-latches ymin part-way through the
	// frame while the other three hold for the whole of it, so they droop by different amounts.
	enum : u8 { WINDOW_XMIN = 1, WINDOW_YMIN = 2, WINDOW_XMAX = 4, WINDOW_YMAX = 8 };
	static constexpr int WINDOW_EDGES = 4;               // index order: xmin, ymin, xmax, ymax
	u8 m_window_hold_edges = 0;                          // which edges this variant holds; 0 = no circuit
	bool m_window_sampled[WINDOW_EDGES] = { false, false, false, false };
	attotime m_window_sample_time[WINDOW_EDGES] = { attotime::never, attotime::never, attotime::never, attotime::never };
	int m_window_hold_bias[WINDOW_EDGES] = { 0, 0, 0, 0 };   // offset decided when the switch opened
	int m_window_da_mem[WINDOW_EDGES] = { 0, 0, 0, 0 };      // relaxed memory of earlier held values

	int window_clip(int edge, int value, const attotime &when) const;
	void window_latch(int edge, int value, const attotime &when);
	double window_percent_unit() const;                  // 1% of screen height in clip units

	int m_xmin, m_ymin;
	int m_xcenter, m_ycenter;

	int m_nvect;
	vgvector m_vectbuf[MAXVECT];

	u16 m_pc;
	u8 m_sp;
	u16 m_dvx;
	u16 m_dvy;
	u16 m_stack[4];
	u16 m_data;

	u8 m_state_latch;
	u8 m_scale;
	u8 m_intensity;

	u8 m_op;
	u8 m_halt;
	u8 m_sync_halt;

	s32 m_xpos;
	s32 m_ypos;

private:
	TIMER_CALLBACK_MEMBER(vg_set_halt_callback);
	TIMER_CALLBACK_MEMBER(run_state_machine);

	void vg_flush_reset();

	required_region_ptr<u8> m_prom;
	emu_timer *m_vg_run_timer, *m_vg_halt_timer;

	bool m_flip_x, m_flip_y;

	// Resumable vg_flush state (clip window + beam position chain). Persists across vg_flush
	// calls; re-primed at each list boundary for the stock one-flush-per-list behaviour.
	int m_flush_cx0, m_flush_cy0, m_flush_cx1, m_flush_cy1;

	int m_flush_xs, m_flush_ys;
	bool m_flush_primed;
};


class dvg_device : public avgdvg_device_base
{
public:
	dvg_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual void device_start() override ATTR_COLD;

	virtual int handler_0() override;
	virtual int handler_1() override;
	virtual int handler_2() override;
	virtual int handler_3() override;
	virtual int handler_4() override;
	virtual int handler_5() override;
	virtual int handler_6() override;
	virtual int handler_7() override;
	virtual u8 state_addr() override;
	virtual void update_databus() override;
	virtual void vggo() override;
	virtual void vgrst() override;

private:
	void dvg_draw_to(int x, int y, int intensity);
};


class avg_device : public avgdvg_device_base
{
public:
	avg_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	avg_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock);

	virtual void device_start() override ATTR_COLD;

	virtual int handler_0() override;
	virtual int handler_1() override;
	virtual int handler_2() override;
	virtual int handler_3() override;
	virtual int handler_4() override;
	virtual int handler_5() override;
	virtual int handler_6() override;
	virtual int handler_7() override;
	virtual u8 state_addr() override;
	virtual void update_databus() override;
	virtual void vggo() override;
	virtual void vgrst() override;

	int avg_common_strobe1();
	int avg_common_strobe2();
	int avg_common_strobe3();

	int m_xmax = 0, m_ymax = 0;

	u8 m_dvy12 = 0;
	u16 m_timer = 0;

	u8 m_int_latch = 0;
	u8 m_bin_scale = 0;
	u8 m_color = 0;

	u16 m_xdac_xor = 0;
	u16 m_ydac_xor = 0;
};


class avg_tempest_device : public avg_device
{
public:
	avg_tempest_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual int handler_6() override;
	virtual int handler_7() override;
	//virtual void vggo();

private:
	required_shared_ptr<u8> m_colorram;
};


class avg_mhavoc_device : public avg_device
{
public:
	avg_mhavoc_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual void device_start() override ATTR_COLD;

	virtual int handler_1() override;
	virtual int handler_6() override;
	virtual int handler_7() override;
	virtual void update_databus() override;
	virtual void vgrst() override;

private:
	required_shared_ptr<u8> m_colorram;
	required_region_ptr<u8> m_bank_region;

	u8 m_enspkl = 0;
	u8 m_spkl_shift = 0;
	u8 m_map = 0;

	u16 m_lst = 0;
};


class avg_starwars_device : public avg_device
{
public:
	avg_starwars_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual int handler_6() override;
	virtual int handler_7() override;
	virtual void update_databus() override;
};


class avg_quantum_device : public avg_device
{
public:
	avg_quantum_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual int handler_0() override;
	virtual int handler_1() override;
	virtual int handler_2() override;
	virtual int handler_3() override;
	virtual int handler_4() override;
	virtual int handler_5() override;
	virtual int handler_6() override;
	virtual int handler_7() override;
	virtual void update_databus() override;
	virtual void vggo() override;

private:
	required_shared_ptr<u16> m_colorram;
};


class avg_bzone_device : public avg_device
{
public:
	avg_bzone_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

protected:
	virtual void device_start() override ATTR_COLD;

	virtual int handler_1() override;
	virtual int handler_6() override;
	virtual int handler_7() override;

private:
	u16 m_hst = 0;
	u16 m_lst = 0;
	u16 m_izblank = 0;

	s32 m_clipx_min = 0;
	s32 m_clipy_min = 0;
	s32 m_clipx_max = 0;
	s32 m_clipy_max = 0;
};


// device type declarations
DECLARE_DEVICE_TYPE(DVG,          dvg_device)
DECLARE_DEVICE_TYPE(AVG,          avg_device)
DECLARE_DEVICE_TYPE(AVG_TEMPEST,  avg_tempest_device)
DECLARE_DEVICE_TYPE(AVG_MHAVOC,   avg_mhavoc_device)
DECLARE_DEVICE_TYPE(AVG_STARWARS, avg_starwars_device)
DECLARE_DEVICE_TYPE(AVG_QUANTUM,  avg_quantum_device)
DECLARE_DEVICE_TYPE(AVG_BZONE,    avg_bzone_device)

#endif // MAME_VIDEO_AVGDVG_H
