/* Minimal x86_64 KVM userspace loop: one vCPU, one anonymous page,
 * OUT DX, AL then HLT. No irqchip, so a HLT exit can come back here.
 * A normal VM that creates an in-kernel local APIC does not take that path.
 *
 * Exit status: 0 observed IO then HLT; 77 SKIP; 1 any other failure.
 * The handler only stores a flag. It does not print or touch errno.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef __x86_64__

int main(void)
{
	fprintf(stderr, "SKIP: not x86_64\n");
	return 77;
}

#else

_Static_assert(KVM_API_VERSION == 12, "KVM_API_VERSION");
_Static_assert(KVM_EXIT_IO == 2, "KVM_EXIT_IO");
_Static_assert(KVM_EXIT_HLT == 5, "KVM_EXIT_HLT");
_Static_assert(KVM_EXIT_IO_IN == 0, "KVM_EXIT_IO_IN");
_Static_assert(KVM_EXIT_IO_OUT == 1, "KVM_EXIT_IO_OUT");
_Static_assert(KVM_CAP_USER_MEMORY == 3, "KVM_CAP_USER_MEMORY");

enum {
	EXIT_OK = 0,
	EXIT_ERR = 1,
	EXIT_SKIP = 77,
	MAX_RUNS = 8,
	MAX_EINTR = 8,
	DEADLINE_MS = 2000,
	GUEST_PORT = 0x3f8,
	/* Below 4G, three pages, and not inside the guest page at GPA 0.
	 * vmx_set_tss_addr rejects an address in the last three pages.
	 */
	TSS_ADDR = 0xfffbd000
};

/* OUT DX, AL ; HLT. RIP 0, CS base 0, DX = port, AL = byte. */
static const unsigned char guest_code[] = {0xee, 0xf4};
static const unsigned char guest_byte = 'K';

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void explain_errno(const char *what)
{
	fprintf(stderr, "ERROR: %s: %s\n", what, strerror(errno));
}

static int skip_open(const char *path, int err)
{
	const char *kind = "error";

	if (err == ENOENT)
		kind = "no-device";
	else if (err == EACCES || err == EPERM)
		kind = "permission";
	else if (err == ENODEV || err == ENXIO || err == EOPNOTSUPP)
		kind = "no-virtualization";
	else
		return 0;

	fprintf(stderr, "SKIP: %s %s: %s\n", kind, path, strerror(err));
	return 1;
}

static long elapsed_ms(const struct timespec *t0)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return DEADLINE_MS + 1;
	return (now.tv_sec - t0->tv_sec) * 1000L +
	       (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

static int xioctl(int fd, unsigned long req, unsigned long arg)
{
	int i;

	for (i = 0; i < MAX_EINTR; i++) {
		int r = ioctl(fd, req, arg);

		if (r != -1 || errno != EINTR)
			return r;
		if (stop_requested)
			return -1;
	}
	errno = EINTR;
	return -1;
}

static const char *exit_name(uint32_t reason)
{
	switch (reason) {
	case KVM_EXIT_IO:
		return "KVM_EXIT_IO";
	case KVM_EXIT_HLT:
		return "KVM_EXIT_HLT";
	case KVM_EXIT_MMIO:
		return "KVM_EXIT_MMIO";
	case KVM_EXIT_INTR:
		return "KVM_EXIT_INTR";
	case KVM_EXIT_FAIL_ENTRY:
		return "KVM_EXIT_FAIL_ENTRY";
	case KVM_EXIT_INTERNAL_ERROR:
		return "KVM_EXIT_INTERNAL_ERROR";
	case KVM_EXIT_SHUTDOWN:
		return "KVM_EXIT_SHUTDOWN";
	default:
		return "KVM_EXIT_OTHER";
	}
}

static int arm_signals(void)
{
	struct sigaction sa;
	struct itimerval it;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) != 0 ||
	    sigaction(SIGINT, &sa, NULL) != 0 ||
	    sigaction(SIGTERM, &sa, NULL) != 0) {
		explain_errno("sigaction");
		return -1;
	}

	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = DEADLINE_MS / 1000;
	it.it_value.tv_usec = (DEADLINE_MS % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
		explain_errno("setitimer");
		return -1;
	}
	return 0;
}

static void disarm_timer(void)
{
	struct itimerval it;

	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
}

/* IO data lives at data_offset in the vCPU mapping, which on v6.6.160 is
 * the pio_data page and not a field inside struct kvm_run. Reading it does
 * not retire the instruction; the next KVM_RUN does that.
 */
static int check_io(const struct kvm_run *run, size_t map_size, unsigned char *out)
{
	uint64_t off = run->io.data_offset;
	uint32_t size = run->io.size;
	uint32_t count = run->io.count;
	uint64_t nbytes;
	const unsigned char *p;

	if (run->io.direction != KVM_EXIT_IO_IN &&
	    run->io.direction != KVM_EXIT_IO_OUT) {
		fprintf(stderr, "ERROR: io.direction %u is not IN or OUT\n",
			run->io.direction);
		return -1;
	}
	if (size != 1 && size != 2 && size != 4) {
		fprintf(stderr, "ERROR: io.size %u is not 1, 2, or 4\n", size);
		return -1;
	}
	if (count == 0 || count > 4096) {
		fprintf(stderr, "ERROR: io.count %u is outside 1..4096\n", count);
		return -1;
	}
	if ((uint64_t)count > UINT64_MAX / size) {
		fprintf(stderr, "ERROR: io size*count overflows\n");
		return -1;
	}
	nbytes = (uint64_t)size * count;
	if (off > map_size || nbytes > map_size - off) {
		fprintf(stderr,
			"ERROR: io data [%llu, +%llu) outside mmap %zu\n",
			(unsigned long long)off, (unsigned long long)nbytes,
			map_size);
		return -1;
	}

	p = (const unsigned char *)run + off;
	*out = p[0];
	printf("exit=%s direction=%s(%u) port=%u size=%u count=%u data_offset=%llu byte=0x%02x\n",
	       exit_name(KVM_EXIT_IO),
	       run->io.direction == KVM_EXIT_IO_OUT ? "KVM_EXIT_IO_OUT"
						    : "KVM_EXIT_IO_IN",
	       run->io.direction, run->io.port, size, count,
	       (unsigned long long)off, *out);

	if (run->io.direction != KVM_EXIT_IO_OUT || size != 1 || count != 1 ||
	    run->io.port != GUEST_PORT || *out != guest_byte) {
		fprintf(stderr,
			"ERROR: IO exit fields do not match the guest program\n");
		return -1;
	}
	return 0;
}

static int kvm_run_once(int vcpu_fd, struct kvm_run *run, const struct timespec *t0,
			uint32_t *reason)
{
	int i;

	for (i = 0; i < MAX_EINTR; i++) {
		int r;

		if (stop_requested || elapsed_ms(t0) > DEADLINE_MS) {
			fprintf(stderr, "ERROR: bounded runtime reached before KVM_RUN returned\n");
			return -1;
		}
		r = ioctl(vcpu_fd, KVM_RUN, 0);
		if (r == 0) {
			*reason = run->exit_reason;
			return 0;
		}
		if (errno != EINTR) {
			explain_errno("KVM_RUN");
			return -1;
		}
		if (stop_requested || elapsed_ms(t0) > DEADLINE_MS) {
			fprintf(stderr, "ERROR: KVM_RUN interrupted by the deadline or a signal\n");
			return -1;
		}
	}
	fprintf(stderr, "ERROR: KVM_RUN returned EINTR too many times\n");
	return -1;
}

int main(void)
{
	const char *path = getenv("KN_KVM_DEVICE");
	long page;
	int kvm_fd = -1, vm_fd = -1, vcpu_fd = -1;
	int api, cap, map_ret, rc = EXIT_ERR;
	void *guest = MAP_FAILED;
	struct kvm_run *run = MAP_FAILED;
	size_t guest_size = 0, run_size = 0;
	struct kvm_userspace_memory_region region;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct timespec t0;
	unsigned runs;
	int saw_io = 0;

	if (!path || !path[0])
		path = "/dev/kvm";

	page = sysconf(_SC_PAGESIZE);
	if (page != 4096) {
		fprintf(stderr, "ERROR: page size %ld is not 4096\n", page);
		return EXIT_ERR;
	}
	guest_size = (size_t)page;

	kvm_fd = open(path, O_RDWR | O_CLOEXEC);
	if (kvm_fd < 0) {
		if (skip_open(path, errno))
			return EXIT_SKIP;
		explain_errno("open");
		return EXIT_ERR;
	}

	api = xioctl(kvm_fd, KVM_GET_API_VERSION, 0);
	if (api < 0) {
		if (errno == ENOTTY || errno == ENODEV || errno == EINVAL) {
			fprintf(stderr, "ERROR: %s does not speak KVM ioctl: %s\n",
				path, strerror(errno));
		} else {
			explain_errno("KVM_GET_API_VERSION");
		}
		goto out;
	}
	if (api != KVM_API_VERSION) {
		fprintf(stderr, "ERROR: KVM_GET_API_VERSION returned %d\n", api);
		goto out;
	}

	cap = xioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
	if (cap < 0) {
		explain_errno("KVM_CHECK_EXTENSION KVM_CAP_USER_MEMORY");
		goto out;
	}
	if (cap == 0) {
		fprintf(stderr, "SKIP: no-virtualization KVM_CAP_USER_MEMORY is 0\n");
		rc = EXIT_SKIP;
		goto out;
	}

	vm_fd = xioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0) {
		if (errno == ENODEV || errno == ENXIO || errno == EOPNOTSUPP) {
			fprintf(stderr, "SKIP: no-virtualization KVM_CREATE_VM: %s\n",
				strerror(errno));
			rc = EXIT_SKIP;
		} else {
			explain_errno("KVM_CREATE_VM");
		}
		goto out;
	}

	/* Intel without unrestricted guest needs this. AMD's optional
	 * set_tss_addr returns 0. Unrestricted VMX also returns 0.
	 */
	if (xioctl(vm_fd, KVM_SET_TSS_ADDR, TSS_ADDR) < 0) {
		explain_errno("KVM_SET_TSS_ADDR");
		goto out;
	}

	guest = mmap(NULL, guest_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guest == MAP_FAILED) {
		explain_errno("mmap guest");
		goto out;
	}
	memset(guest, 0, guest_size);
	memcpy(guest, guest_code, sizeof(guest_code));

	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = 0;
	region.memory_size = guest_size;
	region.userspace_addr = (uint64_t)(uintptr_t)guest;
	if (xioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, (unsigned long)&region) < 0) {
		explain_errno("KVM_SET_USER_MEMORY_REGION");
		goto out;
	}

	vcpu_fd = xioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) {
		explain_errno("KVM_CREATE_VCPU");
		goto out;
	}

	map_ret = xioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (map_ret <= 0) {
		if (map_ret < 0)
			explain_errno("KVM_GET_VCPU_MMAP_SIZE");
		else
			fprintf(stderr, "ERROR: KVM_GET_VCPU_MMAP_SIZE returned 0\n");
		goto out;
	}
	run_size = (size_t)map_ret;
	run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
	if (run == MAP_FAILED) {
		explain_errno("mmap kvm_run");
		goto out;
	}

	if (xioctl(vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs) < 0) {
		explain_errno("KVM_GET_SREGS");
		goto out;
	}
	sregs.cs.selector = 0;
	sregs.cs.base = 0;
	sregs.cr0 &= ~((1ULL << 0) | (1ULL << 31));
	if (xioctl(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs) < 0) {
		explain_errno("KVM_SET_SREGS");
		goto out;
	}

	memset(&regs, 0, sizeof(regs));
	regs.rax = guest_byte;
	regs.rdx = GUEST_PORT;
	regs.rip = 0;
	regs.rflags = 2;
	if (xioctl(vcpu_fd, KVM_SET_REGS, (unsigned long)&regs) < 0) {
		explain_errno("KVM_SET_REGS");
		goto out;
	}

	if (arm_signals() != 0)
		goto out;
	if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
		explain_errno("clock_gettime");
		goto out;
	}

	printf("api_version=%d\n", api);
	printf("cap_user_memory=%d\n", cap);
	printf("vcpu_mmap_size=%zu\n", run_size);
	printf("tss_addr=0x%x\n", TSS_ADDR);
	printf("guest_port=%u guest_byte=0x%02x\n", GUEST_PORT, guest_byte);

	for (runs = 0; runs < MAX_RUNS; runs++) {
		uint32_t reason = 0;
		unsigned char byte = 0;

		if (kvm_run_once(vcpu_fd, run, &t0, &reason) != 0)
			goto out;

		if (reason == KVM_EXIT_IO) {
			if (saw_io) {
				fprintf(stderr, "ERROR: a second KVM_EXIT_IO arrived\n");
				goto out;
			}
			if (check_io(run, run_size, &byte) != 0)
				goto out;
			saw_io = 1;
			/* The OUT is retired by this next KVM_RUN, which should
			 * then execute HLT. Do not treat the IO print as done.
			 */
			continue;
		}

		if (reason == KVM_EXIT_HLT) {
			if (!saw_io) {
				fprintf(stderr, "ERROR: KVM_EXIT_HLT before KVM_EXIT_IO\n");
				goto out;
			}
			printf("exit=%s\n", exit_name(reason));
			printf("result=ok\n");
			rc = EXIT_OK;
			goto out;
		}

		if (reason == KVM_EXIT_INTR &&
		    (stop_requested || elapsed_ms(&t0) > DEADLINE_MS)) {
			fprintf(stderr, "ERROR: KVM_EXIT_INTR at the deadline\n");
			goto out;
		}

		fprintf(stderr, "ERROR: unexpected exit %s (%u)\n",
			exit_name(reason), reason);
		goto out;
	}

	fprintf(stderr, "ERROR: exit count reached %d\n", MAX_RUNS);

out:
	disarm_timer();
	if (run != MAP_FAILED)
		munmap(run, run_size);
	if (guest != MAP_FAILED)
		munmap(guest, guest_size);
	if (vcpu_fd >= 0)
		close(vcpu_fd);
	if (vm_fd >= 0)
		close(vm_fd);
	if (kvm_fd >= 0)
		close(kvm_fd);
	return rc;
}

#endif
