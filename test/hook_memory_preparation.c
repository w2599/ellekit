#include <assert.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void EKPrepareHookMemory(void);
extern kern_return_t (*EKHookMemoryRaw)(void *, const void *, size_t);

static const char *mode;
static atomic_int lookupCount;
static atomic_int readyCount;
static atomic_bool startWorkers;
static atomic_bool threadsPaused;
static int hookCalls;
static int protectCalls;

// 模拟完整写入后端，确认缓存后的函数指针仍可调用。
static kern_return_t hook_memory(void *target, const void *data, size_t size)
{
    hookCalls++;
    memcpy(target, data, size);
    return KERN_SUCCESS;
}

// 模拟页面权限后端，覆盖无完整写入后端时的回退和错误传播。
static kern_return_t protect_memory(mach_port_name_t task, mach_vm_address_t address,
    mach_vm_size_t size, boolean_t maximum, vm_prot_t protection)
{
    (void)task; (void)address; (void)size; (void)maximum;
    protectCalls++;
    assert(protection == (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) ||
           protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    return strcmp(mode, "protect-error") == 0 ? KERN_PROTECTION_FAILURE : KERN_SUCCESS;
}

// 替换生产代码的解析入口，禁止暂停阶段重新查询符号。
void *EKTestDlsym(void *handle, const char *symbol)
{
    assert(handle == RTLD_DEFAULT);
    assert(!atomic_load(&threadsPaused));
    atomic_fetch_add(&lookupCount, 1);
    usleep(10000);
    if (strcmp(symbol, "substrate_vm_protect") == 0) return (void *)protect_memory;
    assert(strcmp(symbol, "substrate_hook_memory") == 0);
    return strncmp(mode, "protect", 7) == 0 ? NULL : (void *)hook_memory;
}

// 提供未被本测试调用的沙箱检查符号，便于链接完整生产文件。
int sandbox_check_by_audit_token(audit_token_t token, const char *operation, int type, ...)
{
    (void)token; (void)operation; (void)type;
    assert(false);
    return -1;
}

// 并发触发首次准备，检查 dispatch_once 只解析一轮。
static void *prepare_worker(void *context)
{
    (void)context;
    atomic_fetch_add(&readyCount, 1);
    while (!atomic_load(&startWorkers)) usleep(100);
    EKPrepareHookMemory();
    return NULL;
}

// 保留外部替换公开写入入口的兼容性。
static kern_return_t override_memory(void *target, const void *data, size_t size)
{
    (void)target; (void)data; (void)size;
    return KERN_NOT_SUPPORTED;
}

// 在独立进程中验证初始化、缓存、回退与直接调用入口。
int main(int argc, char **argv)
{
    assert(argc == 2);
    mode = argv[1];
    unsigned char output[4] = {0};
    const unsigned char input[4] = {0x11, 0x22, 0x33, 0x44};

    if (strcmp(mode, "direct") == 0) {
        assert(EKHookMemoryRaw(output, input, sizeof(input)) == KERN_SUCCESS);
        assert(memcmp(output, input, sizeof(input)) == 0);
    } else {
        pthread_t workers[8];
        for (int i = 0; i < 8; i++) assert(pthread_create(&workers[i], NULL, prepare_worker, NULL) == 0);
        while (atomic_load(&readyCount) != 8) usleep(100);
        atomic_store(&startWorkers, true);
        for (int i = 0; i < 8; i++) assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(atomic_load(&lookupCount) == 2);

    atomic_store(&threadsPaused, true);
    if (strcmp(mode, "override") == 0) EKHookMemoryRaw = override_memory;
    kern_return_t status = EKHookMemoryRaw(output, input, sizeof(input));
    assert(atomic_load(&lookupCount) == 2);
    if (strcmp(mode, "override") == 0) {
        assert(status == KERN_NOT_SUPPORTED);
    } else if (strcmp(mode, "protect-error") == 0) {
        assert(status == KERN_PROTECTION_FAILURE);
        assert(protectCalls == 1 && hookCalls == 0);
        assert(output[0] == 0);
    } else {
        assert(status == KERN_SUCCESS);
        assert(memcmp(output, input, sizeof(input)) == 0);
        if (strcmp(mode, "protect") == 0) assert(protectCalls == 2 && hookCalls == 0);
        else assert(hookCalls == (strcmp(mode, "direct") == 0 ? 2 : 1));
    }
    printf("PASS %s\n", mode);
    return 0;
}
