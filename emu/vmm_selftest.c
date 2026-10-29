/* vmm_selftest.c : a stand-alone check of the VMM mechanism (vmm_kvm.c).
 *
 * Loads the flat guest blob, runs it under the VMM, services its write and exit
 * doorbell syscalls, and confirms the console bytes and exit code arrived.
 * Exits 42 on success; exits 0 with a note when /dev/kvm is unavailable. */

#include "vmm.h"
#include "skj_hv_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define GUEST_ENTRY 0x100000u    /* 1 MiB: above the page tables */

int
main(int argc, char **argv)
{
    const char *blob_path = argc > 1 ? argv[1] : "build/vmm_selftest_guest.bin";
    FILE *f;
    long blob_len;
    uint8_t *blob;
    uint8_t *ram;
    vmm *vm;
    vmm_vcpu *vcpu;
    int exited = 0, exit_code = -1;
    char console[256];
    size_t console_len = 0;

    if (!vmm_available()) {
        fprintf(stderr, "vmm selftest: /dev/kvm unavailable, skipping\n");
        return 0;
    }

    f = fopen(blob_path, "rb");
    if (!f) {
        fprintf(stderr, "vmm selftest: cannot open %s\n", blob_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    blob_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (blob_len <= 0 || blob_len > 0x1000) {
        fprintf(stderr, "vmm selftest: bad blob size %ld\n", blob_len);
        fclose(f);
        return 1;
    }
    blob = malloc((size_t)blob_len);
    if (fread(blob, 1, (size_t)blob_len, f) != (size_t)blob_len) {
        fprintf(stderr, "vmm selftest: short read\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    ram = mmap(NULL, SKJ_HV_RAM_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap ram");
        return 1;
    }
    memcpy(ram + GUEST_ENTRY, blob, (size_t)blob_len);
    free(blob);

    vm = vmm_new();
    if (!vm || vmm_map(vm, ram, 0, SKJ_HV_RAM_SIZE) != 0) {
        fprintf(stderr, "vmm selftest: VM setup failed\n");
        return 1;
    }
    vcpu = vmm_vcpu_new(vm);
    if (!vcpu) {
        fprintf(stderr, "vmm selftest: vcpu setup failed\n");
        return 1;
    }
    vmm_vcpu_set_pc(vcpu, GUEST_ENTRY);
    vmm_vcpu_set_sp(vcpu, SKJ_HV_RAM_SIZE - 16);

    for (;;) {
        struct vmm_exit ex;
        uint64_t nr, args[6];

        vmm_vcpu_run(vcpu, &ex);
        if (ex.kind == VMM_X_DOORBELL) {
            vmm_vcpu_syscall(vcpu, &nr, args);
            if (nr == 1) {              /* write(fd, buf, len) */
                uint64_t buf = args[1], len = args[2];
                if (buf + len <= SKJ_HV_RAM_SIZE && console_len + len < sizeof(console)) {
                    memcpy(console + console_len, ram + buf, len);
                    console_len += len;
                }
                vmm_vcpu_syscall_ret(&ex, len);
            } else if (nr == 60) {      /* exit(code) */
                exited = 1;
                exit_code = (int)args[0];
                break;
            } else {
                vmm_vcpu_syscall_ret(&ex, (uint64_t)-1);
            }
            continue;
        }
        if (ex.kind == VMM_X_INTR)
            continue;
        if (ex.kind == VMM_X_HALT)
            break;
        fprintf(stderr, "vmm selftest: unexpected exit kind=%d addr=0x%llx pc=0x%llx\n",
                ex.kind, (unsigned long long)ex.mmio_addr,
                (unsigned long long)vmm_vcpu_pc(vcpu));
        return 1;
    }

    vmm_vcpu_free(vcpu);
    vmm_free(vm);

    console[console_len] = '\0';
    if (console_len != 10 || memcmp(console, "hello vmm\n", 10) != 0) {
        fprintf(stderr, "vmm selftest: console mismatch (%zu bytes): %s\n",
                console_len, console);
        return 1;
    }
    if (!exited || exit_code != 42) {
        fprintf(stderr, "vmm selftest: exit mismatch (exited=%d code=%d)\n",
                exited, exit_code);
        return 1;
    }
    fprintf(stderr, "vmm selftest: ok (console + exit via doorbell)\n");
    return 42;
}
