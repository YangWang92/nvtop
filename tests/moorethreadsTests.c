/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Keep tests independent of proprietary headers, libraries and hardware. */
#include "../src/extract_gpuinfo_moorethreads.c"
void register_gpu_vendor(struct gpu_vendor *vendor) { (void)vendor; }

static int mock_uint(mtml_handle handle, unsigned *value) {
  (void)handle;
  *value = 42;
  return 0;
}
static int mock_fail(mtml_handle handle, unsigned *value) {
  (void)handle;
  (void)value;
  return 3;
}
static uint64_t mock_total = UINT64_C(80) << 30;
static uint64_t mock_used = UINT64_C(20) << 30;
static int get_total(mtml_handle handle, uint64_t *value) {
  (void)handle;
  *value = mock_total;
  return 0;
}
static int get_used(mtml_handle handle, uint64_t *value) {
  (void)handle;
  *value = mock_used;
  return 0;
}
static int mock_pci(mtml_handle handle, struct mtml_pci_info *pci) {
  (void)handle;
  *pci = (struct mtml_pci_info){.max_gen = 5, .current_gen = 4, .max_width = 16, .current_width = 8};
  return 0;
}

static int live_test(void) {
  LIST_HEAD(devices);
  unsigned count;
  if (!moorethreads_init()) {
    fprintf(stderr, "%s\n", moorethreads_last_error());
    return 1;
  }
  assert(moorethreads_get_devices(&devices, &count));
  assert(count > 0);
  struct gpu_info *base, *next;
  list_for_each_entry(base, &devices, list) {
    moorethreads_static_info(base);
    moorethreads_dynamic_info(base);
    assert(GPUINFO_DYNAMIC_FIELD_VALID(&base->dynamic_info, total_memory));
    printf("%s %s memory=%llu used=%llu util=%u temp=%u clock=%u power_mW=%u PCIe=%ux%u\n",
           base->pdev, base->static_info.device_name, base->dynamic_info.total_memory,
           base->dynamic_info.used_memory, base->dynamic_info.gpu_util_rate, base->dynamic_info.gpu_temp,
           base->dynamic_info.gpu_clock_speed, base->dynamic_info.power_draw,
           base->dynamic_info.pcie_link_gen, base->dynamic_info.pcie_link_width);
    if (GPUINFO_DYNAMIC_FIELD_VALID(&base->dynamic_info, power_draw_max))
      printf("  enforced_power_limit_mW=%u\n", base->dynamic_info.power_draw_max);
    for (unsigned i = 0; i < 3; ++i)
      moorethreads_dynamic_info(base);
  }
  list_for_each_entry_safe(base, next, &devices, list) { list_del(&base->list); }
  moorethreads_shutdown();
  moorethreads_shutdown();
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--live") == 0)
    return live_test();
  struct gpu_info_moorethreads gpu = {0};
  gpu.device = gpu.gpu = gpu.memory = &gpu;
  mtml.GpuGetUtilization = mock_uint;
  mtml.GpuGetTemperature = mock_uint;
  mtml.GpuGetMaxClock = mock_fail;
  mtml.MemoryGetTotal = get_total;
  mtml.MemoryGetUsed = get_used;
  mtml.DeviceGetPciInfo = mock_pci;
  moorethreads_static_info(&gpu.base);
  assert(strcmp(gpu.base.static_info.device_name, "Moore Threads GPU") == 0);
  assert(gpu.base.static_info.max_pcie_gen == 5);
  moorethreads_dynamic_info(&gpu.base);
  struct gpuinfo_dynamic_info *info = &gpu.base.dynamic_info;
  assert(info->gpu_util_rate == 42);
  assert(info->total_memory == (UINT64_C(80) << 30));
  assert(info->free_memory == (UINT64_C(60) << 30));
  assert(info->mem_util_rate == 25);
  assert(info->pcie_link_gen == 4 && info->pcie_link_width == 8);
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, gpu_clock_speed_max));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, power_draw));
  mtml.GpuGetEnforcedPowerLimit = mock_uint;
  moorethreads_dynamic_info(&gpu.base);
  assert(GPUINFO_DYNAMIC_FIELD_VALID(info, power_draw_max));
  assert(info->power_draw_max == 42); /* Preserve milliwatts, without conversion. */
  mtml.GpuGetEnforcedPowerLimit = mock_fail;
  mtml.GpuGetUtilization = mock_fail;
  mock_used = mock_total + 1;
  moorethreads_dynamic_info(&gpu.base);
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, gpu_util_rate));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, power_draw_max));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, used_memory));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, free_memory));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, mem_util_rate));
  mock_total = mock_used = 0;
  moorethreads_dynamic_info(&gpu.base);
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, mem_util_rate));
  gpu.gpu = gpu.memory = NULL;
  moorethreads_dynamic_info(&gpu.base);
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, gpu_temp));
  assert(!GPUINFO_DYNAMIC_FIELD_VALID(info, total_memory));
  moorethreads_shutdown();
  moorethreads_shutdown();
  puts("Moore Threads telemetry tests passed");
  return 0;
}
