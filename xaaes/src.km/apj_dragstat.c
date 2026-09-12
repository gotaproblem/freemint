/*
 * apj_dragstat.c - APJ-OS: measure the live window-drag pipeline
 * See apj_dragstat.h.
 */

#include "xa_global.h"
#include "apj_dragstat.h"
#include "mint/fcntl.h"

struct apj_ds
{
	short active;
	short have_prev;
	short px, py;		/* previous sampled position */
	long t_start;
	long t_sample;		/* position posted to owner (block=1) */
	long t_moved;		/* WM_MOVED queued (block=2) */
	long t_set;		/* owner called wind_set (block=3 or 0) */
	long n_steps;
	long sum_wake, max_wake;	/* sample -> WM_MOVED queued */
	long sum_app,  max_app;		/* WM_MOVED queued -> wind_set */
	long sum_rdrw, max_rdrw;	/* wind_set -> all redraws done */
	long sum_red,  max_red;		/* WM_REDRAWs generated per step */
	long sum_step, max_step;	/* px per step (larger axis) */
	long max_gap;			/* longest time between two samples */
	long hist[7];			/* 1, 2, 3-4, 5-8, 9-16, 17-32, >32 */
	long motion, skipped;		/* pointer packets: all, while blocked */
	long why[5];
};

static struct apj_ds ds;

/* The kernel's xtime (utc) only advances when somebody calls
 * gettimeofday, so it is useless inside one drag step: every phase read
 * 0 and the sec/usec pair tore once into a 1000 ms "maximum". The 200 Hz
 * system counter is what the kernel itself derives xtime from; the
 * module runs in supervisor mode, so read it directly. 5 ms resolution. */
static long
now_ms(void)
{
	return *(volatile long *)0x4baL * 5L;
}

static void
acc(long *sum, long *max, long v)
{
	*sum += v;
	if (v > *max)
		*max = v;
}

void
apj_ds_sample(short x, short y)
{
	long t = now_ms();

	if (!ds.active)
	{
		bzero(&ds, sizeof(ds));
		ds.active = 1;
		ds.t_start = t;
	}
	if (ds.have_prev)
	{
		long dx = x - ds.px, dy = y - ds.py, d, i;

		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		d = dx > dy ? dx : dy;
		acc(&ds.sum_step, &ds.max_step, d);
		i = d <= 1 ? 0 : d == 2 ? 1 : d <= 4 ? 2 : d <= 8 ? 3 : d <= 16 ? 4 : d <= 32 ? 5 : 6;
		ds.hist[i]++;
	}
	if (ds.have_prev && t - ds.t_sample > ds.max_gap)
		ds.max_gap = t - ds.t_sample;
	ds.px = x, ds.py = y;
	ds.have_prev = 1;
	ds.t_sample = t;
	ds.t_moved = ds.t_set = 0;
	ds.n_steps++;
}

void
apj_ds_motion(short blocked)
{
	if (!ds.active)
		return;
	ds.motion++;
	if (blocked)
		ds.skipped++;
}

void
apj_ds_moved(void)
{
	if (!ds.active || ds.t_moved)
		return;
	ds.t_moved = now_ms();
	acc(&ds.sum_wake, &ds.max_wake, ds.t_moved - ds.t_sample);
}

void
apj_ds_set(long nred)
{
	if (!ds.active || !ds.t_moved || ds.t_set)
		return;
	ds.t_set = now_ms();
	acc(&ds.sum_app, &ds.max_app, ds.t_set - ds.t_moved);
	acc(&ds.sum_red, &ds.max_red, nred);
}

void
apj_ds_unblock(short why)
{
	if (!ds.active)
		return;
	if (why >= 0 && why < 5)
		ds.why[why]++;
	if (ds.t_set)
	{
		acc(&ds.sum_rdrw, &ds.max_rdrw, now_ms() - ds.t_set);
		ds.t_set = 0;
	}
}

static void
ds_write(const char *s, long l)
{
	struct file *fp;
	char path[256];
	long n = strlen(C.start_path);

	if (n == 0 || n > 200)
		return;
	strcpy(path, C.start_path);
	if (path[n - 1] != '\\' && path[n - 1] != '/')
		path[n++] = '\\', path[n] = 0;
	strcat(path, "DRAGSTAT.LOG");

	fp = kernel_open(path, O_RDWR, NULL, NULL);
	if (!fp)
		fp = kernel_open(path, O_RDWR|O_CREAT|O_TRUNC, NULL, NULL);
	else
		kernel_lseek(fp, 0, SEEK_END);
	if (fp)
	{
		kernel_write(fp, s, l);
		kernel_close(fp);
	}
}

void
apj_ds_end(const char *owner)
{
	char buf[512];
	long l, n, ms;

	if (!ds.active)
		return;
	ds.active = 0;
	n = ds.n_steps ? ds.n_steps : 1;
	ms = now_ms() - ds.t_start;

	l = sprintf(buf, sizeof(buf),
		"drag %s: %ld ms, %ld steps (%ld ms/step, longest gap %ld ms), pointer packets %ld of which %ld arrived while blocked\r\n"
		"  wake  sample->WM_MOVED   avg %ld max %ld ms\r\n"
		"  app   WM_MOVED->wind_set avg %ld max %ld ms\r\n"
		"  redraw wind_set->done    avg %ld max %ld ms, WM_REDRAWs/step avg %ld max %ld\r\n"
		"  step px avg %ld max %ld  hist 1:%ld 2:%ld 3-4:%ld 5-8:%ld 9-16:%ld 17-32:%ld >32:%ld\r\n"
		"  unblocked by: cevent-only %ld, no-redraws %ld, redraws-done %ld, timeout %ld\r\n",
		owner ? owner : "?", ms, ds.n_steps, ms / n, ds.max_gap, ds.motion, ds.skipped,
		ds.sum_wake / n, ds.max_wake,
		ds.sum_app / n, ds.max_app,
		ds.sum_rdrw / n, ds.max_rdrw, ds.sum_red / n, ds.max_red,
		ds.sum_step / n, ds.max_step,
		ds.hist[0], ds.hist[1], ds.hist[2], ds.hist[3], ds.hist[4], ds.hist[5], ds.hist[6],
		ds.why[1], ds.why[2], ds.why[3], ds.why[4]);
	ds_write(buf, l);
}
