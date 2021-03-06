/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "sw_sync.h"

#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/signal.h>

IGT_TEST_DESCRIPTION("Check that execbuf waits for explicit fences");

#define LOCAL_EXEC_FENCE_IN (1 << 16)
#define LOCAL_EXEC_FENCE_OUT (1 << 17)

#ifndef SYNC_IOC_MERGE
struct sync_merge_data {
	char    name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t        flags;
	uint32_t        pad;
};
#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#endif

static void store(int fd, unsigned ring, int fence, uint32_t target, unsigned offset_value)
{
	const int SCRATCH = 0;
	const int BATCH = 1;
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_IN;
	execbuf.rsvd2 = fence;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = target;

	obj[BATCH].handle = gem_create(fd, 4096);
	obj[BATCH].relocs_ptr = to_user_pointer(&reloc);
	obj[BATCH].relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	i = 0;
	reloc.target_handle = obj[SCRATCH].handle;
	reloc.presumed_offset = -1;
	reloc.offset = sizeof(uint32_t) * (i + 1);
	reloc.delta = sizeof(uint32_t) * offset_value;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = reloc.delta;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = reloc.delta;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = reloc.delta;
	}
	batch[++i] = offset_value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[BATCH].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[BATCH].handle);
}

static bool fence_busy(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, 0) == 0;
}

#define HANG 0x1
#define NONBLOCK 0x2
#define WAIT 0x4

static void test_fence_busy(int fd, unsigned ring, unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct timespec tv;
	uint32_t *batch;
	int fence, i, timeout;

	gem_quiescent_gpu(fd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_OUT;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);

	obj.relocs_ptr = to_user_pointer(&reloc);
	obj.relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

	i = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc.delta = 1;
		}
	}
	i++;

	execbuf.rsvd2 = -1;
	gem_execbuf_wr(fd, &execbuf);
	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence != -1);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(fence_busy(fence));

	timeout = 120;
	if ((flags & HANG) == 0) {
		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
		timeout = 1;
	}
	munmap(batch, 4096);

	if (flags & WAIT) {
		struct pollfd pfd = { .fd = fence, .events = POLLIN };
		igt_assert(poll(&pfd, 1, timeout*1000) == 1);
	} else {
		memset(&tv, 0, sizeof(tv));
		while (fence_busy(fence))
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	}

	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert_eq(sync_fence_status(fence),
		      flags & HANG ? -EIO : SYNC_FENCE_OK);

	close(fence);
	gem_close(fd, obj.handle);

	gem_quiescent_gpu(fd);
}

static void test_fence_await(int fd, unsigned ring, unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t scratch = gem_create(fd, 4096);
	uint32_t *batch, *out;
	unsigned engine;
	int fence, i;

	igt_require(gem_can_store_dword(fd, 0));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_OUT;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);

	obj.relocs_ptr = to_user_pointer(&reloc);
	obj.relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	out = gem_mmap__wc(fd, scratch, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

	i = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc.delta = 1;
		}
	}
	i++;

	execbuf.rsvd2 = -1;
	gem_execbuf_wr(fd, &execbuf);
	gem_close(fd, obj.handle);
	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence != -1);

	i = 0;
	for_each_engine(fd, engine) {
		if (!gem_can_store_dword(fd, engine))
			continue;

		if (flags & NONBLOCK) {
			store(fd, engine, fence, scratch, i);
		} else {
			igt_fork(child, 1)
				store(fd, engine, fence, scratch, i);
		}

		i++;
	}
	close(fence);

	sleep(1);

	/* Check for invalidly completing the task early */
	for (int n = 0; n < i; n++)
		igt_assert_eq_u32(out[n], 0);

	if ((flags & HANG) == 0) {
		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
	}
	munmap(batch, 4096);

	igt_waitchildren();

	gem_set_domain(fd, scratch, I915_GEM_DOMAIN_GTT, 0);
	while (i--)
		igt_assert_eq_u32(out[i], i);
	munmap(out, 4096);
	gem_close(fd, scratch);
}

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static void plug(int fd, struct cork *c)
{
	struct vgem_bo bo;
	int dmabuf;

	c->device = drm_open_driver(DRIVER_VGEM);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c->device, &bo);
	c->fence = vgem_fence_attach(c->device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c->device, bo.handle);
	c->handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);
}

static void unplug(struct cork *c)
{
	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	return ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf);
}

static unsigned int measure_ring_size(int fd)
{
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int count, last;
	struct itimerval itv;
	struct cork c;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	plug(fd, &c);
	obj[0].handle = c.handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 100;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 1000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	unplug(&c);
	gem_close(fd, obj[1].handle);

	return count;
}

#define EXPIRED 0x10000
static void test_long_history(int fd, long ring_size, unsigned flags)
{
	const uint32_t sz = 1 << 20;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned int engines[16], engine;
	unsigned int nengine, n, s;
	unsigned long limit;
	int all_fences;
	struct cork c;

	limit = -1;
	if (!gem_uses_full_ppgtt(fd))
		limit = ring_size / 3;

	nengine = 0;
	for_each_engine(fd, engine) {
		if (engine == 0)
			continue;

		if (engine == I915_EXEC_BSD)
			continue;

		engines[nengine++] = engine;
	}
	igt_require(nengine);

	gem_quiescent_gpu(fd);

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, sz);
	gem_write(fd, obj[1].handle, sz - sizeof(bbe), &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.flags = LOCAL_EXEC_FENCE_OUT;

	gem_execbuf_wr(fd, &execbuf);
	all_fences = execbuf.rsvd2 >> 32;

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	plug(fd, &c);
	obj[0].handle = c.handle;

	igt_until_timeout(5) {
		execbuf.rsvd1 = gem_context_create(fd);

		for (n = 0; n < nengine; n++) {
			struct sync_merge_data merge;

			execbuf.flags = engines[n] | LOCAL_EXEC_FENCE_OUT;
			if (__gem_execbuf_wr(fd, &execbuf))
				continue;

			memset(&merge, 0, sizeof(merge));
			merge.fd2 = execbuf.rsvd2 >> 32;
			strcpy(merge.name, "igt");

			do_ioctl(all_fences, SYNC_IOC_MERGE, &merge);

			close(all_fences);
			close(merge.fd2);

			all_fences = merge.fence;
		}

		gem_context_destroy(fd, execbuf.rsvd1);
		if (!--limit)
			break;
	}
	unplug(&c);

	igt_info("History depth = %d\n", sync_fence_count(all_fences));

	if (flags & EXPIRED)
		gem_sync(fd, obj[1].handle);

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.rsvd2 = all_fences;
	execbuf.rsvd1 = 0;

	for (s = 0; s < ring_size; s++) {
		for (n = 0; n < nengine; n++) {
			execbuf.flags = engines[n] | LOCAL_EXEC_FENCE_IN;
			if (__gem_execbuf_wr(fd, &execbuf))
				continue;
		}
	}

	close(all_fences);

	gem_sync(fd, obj[1].handle);
	gem_close(fd, obj[1].handle);
}

static void test_fence_flip(int i915)
{
	igt_skip_on_f(1, "no fence-in for atomic flips\n");
}

#define HAVE_EXECLISTS 0x1
static unsigned int print_welcome(int fd)
{
	unsigned int result = 0;
	bool active;
	int dir;

	dir = igt_sysfs_open_parameters(fd);
	if (dir < 0)
		return 0;

	active = igt_sysfs_get_boolean(dir, "enable_guc_submission");
	if (active) {
		igt_info("Using GuC submission\n");
		result |= HAVE_EXECLISTS;
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "enable_execlists");
	if (active) {
		igt_info("Using Execlists submission\n");
		result |= HAVE_EXECLISTS;
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "semaphores");
	igt_info("Using Legacy submission%s\n",
		 active ? ", with semaphores" : "");

out:
	close(dir);
	return result;
}

igt_main
{
	const struct intel_execution_engine *e;
	unsigned int caps = 0;
	int i915 = -1;

	igt_skip_on_simulation();

	igt_fixture {
		i915 = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_require(gem_has_exec_fence(i915));
		gem_require_mmap_wc(i915);

		caps = print_welcome(i915);
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_group {
			igt_fixture {
				igt_require(gem_has_ring(i915, e->exec_id | e->flags));
				igt_require(gem_can_store_dword(i915, e->exec_id | e->flags));
			}

			igt_subtest_group {
				igt_fixture {
					igt_fork_hang_detector(i915);
				}

				igt_subtest_f("%sbusy-%s",
						e->exec_id == 0 ? "basic-" : "",
						e->name)
					test_fence_busy(i915, e->exec_id | e->flags, 0);
				igt_subtest_f("%swait-%s",
						e->exec_id == 0 ? "basic-" : "",
						e->name)
					test_fence_busy(i915, e->exec_id | e->flags, WAIT);
				igt_subtest_f("%sawait-%s",
						e->exec_id == 0 ? "basic-" : "",
						e->name)
					test_fence_await(i915, e->exec_id | e->flags, 0);
				igt_subtest_f("nb-await-%s", e->name)
					test_fence_await(i915, e->exec_id | e->flags, NONBLOCK);

				igt_fixture {
					igt_stop_hang_detector();
				}
			}

			igt_subtest_group {
				igt_hang_t hang;

				igt_fixture {
					hang = igt_allow_hang(i915, 0, 0);
				}

				igt_subtest_f("busy-hang-%s", e->name)
					test_fence_busy(i915, e->exec_id | e->flags, HANG);
				igt_subtest_f("wait-hang-%s", e->name)
					test_fence_busy(i915, e->exec_id | e->flags, HANG | WAIT);
				igt_subtest_f("await-hang-%s", e->name)
					test_fence_await(i915, e->exec_id | e->flags, HANG);
				igt_subtest_f("nb-await-hang-%s", e->name)
					test_fence_await(i915, e->exec_id | e->flags, NONBLOCK | HANG);
				igt_fixture {
					igt_disallow_hang(i915, hang);
				}
			}
		}
	}

	igt_subtest("long-history") {
		long ring_size = measure_ring_size(i915) - 1;

		igt_info("Ring size: %ld batches\n", ring_size);
		igt_require(ring_size);

		test_long_history(i915, ring_size, caps);
	}

	igt_subtest("expired-history") {
		long ring_size = measure_ring_size(i915) - 1;

		igt_info("Ring size: %ld batches\n", ring_size);
		igt_require(ring_size);

		test_long_history(i915, ring_size, caps | EXPIRED);
	}

	igt_subtest("flip") {
		gem_quiescent_gpu(i915);
		test_fence_flip(i915);
	}

	igt_fixture {
		close(i915);
	}
}
