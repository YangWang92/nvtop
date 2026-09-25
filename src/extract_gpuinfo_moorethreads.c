/*
 * Moore Threads GPU monitoring through the dynamically loaded MTML library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nvtop/extract_gpuinfo_common.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public MTML ABI, also exposed by the vendor's pymtml bindings. No MUSA SDK
 * or link-time dependency is needed. Handles are opaque, separately owned
 * library/device/GPU/memory objects (not NVML-compatible device handles). */
typedef void *mtml_handle;
struct mtml_pci_info {
  char sbdf[32];
  unsigned segment, bus, device, device_id, subsystem_id, bus_width;
  float max_speed, current_speed;
  unsigned max_width, current_width, max_gen, current_gen;
  int reserved[6];
};

static struct {
  int (*LibraryInit)(mtml_handle *);
  int (*LibraryShutDown)(mtml_handle);
  int (*LibraryCountDevice)(mtml_handle, unsigned *);
  int (*LibraryInitDeviceByIndex)(mtml_handle, unsigned, mtml_handle *);
  int (*LibraryFreeDevice)(mtml_handle);
  int (*DeviceInitGpu)(mtml_handle, mtml_handle *);
  int (*DeviceFreeGpu)(mtml_handle);
  int (*DeviceInitMemory)(mtml_handle, mtml_handle *);
  int (*DeviceFreeMemory)(mtml_handle);
  const char *(*ErrorString)(int);
  int (*DeviceGetName)(mtml_handle, char *, unsigned);
  int (*DeviceGetPciInfo)(mtml_handle, struct mtml_pci_info *);
  int (*DeviceGetPowerUsage)(mtml_handle, unsigned *);
  int (*DeviceGetFanSpeed)(mtml_handle, unsigned, unsigned *);
  int (*DeviceGetFanRpm)(mtml_handle, unsigned, unsigned *);
  int (*GpuGetUtilization)(mtml_handle, unsigned *);
  int (*GpuGetClock)(mtml_handle, unsigned *);
  int (*GpuGetMaxClock)(mtml_handle, unsigned *);
  int (*GpuGetTemperature)(mtml_handle, unsigned *);
  int (*GpuGetEnforcedPowerLimit)(mtml_handle, unsigned *);
  int (*MemoryGetTotal)(mtml_handle, uint64_t *);
  int (*MemoryGetUsed)(mtml_handle, uint64_t *);
  int (*MemoryGetClock)(mtml_handle, unsigned *);
  int (*MemoryGetMaxClock)(mtml_handle, unsigned *);
} mtml;

static void *mtml_so;
static mtml_handle library;
static char error_string[256] = "MTML is not initialized";
static LIST_HEAD(allocations);

struct gpu_info_moorethreads {
  struct gpu_info base;
  struct list_head allocation;
  mtml_handle device, gpu, memory;
};

static void free_handles(struct gpu_info_moorethreads *gpu) {
  if (gpu->memory)
    mtml.DeviceFreeMemory(gpu->memory);
  if (gpu->gpu)
    mtml.DeviceFreeGpu(gpu->gpu);
  if (gpu->device)
    mtml.LibraryFreeDevice(gpu->device);
}

static void moorethreads_shutdown(void) {
  struct gpu_info_moorethreads *gpu, *next;
  list_for_each_entry_safe(gpu, next, &allocations, allocation) {
    list_del(&gpu->allocation);
    free_handles(gpu);
    free(gpu);
  }
  if (library)
    mtml.LibraryShutDown(library);
  library = NULL;
  if (mtml_so)
    dlclose(mtml_so);
  mtml_so = NULL;
  memset(&mtml, 0, sizeof(mtml));
}

static bool moorethreads_init(void) {
  if (library)
    return true;
  mtml_so = dlopen("libmtml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!mtml_so)
    mtml_so = dlopen("libmtml.so", RTLD_NOW | RTLD_LOCAL);
  if (!mtml_so) {
    snprintf(error_string, sizeof(error_string), "%s", dlerror());
    return false;
  }

#define LOAD_REQUIRED(name)                                                                                            \
  do {                                                                                                                 \
    mtml.name = dlsym(mtml_so, "mtml" #name);                                                                           \
    if (!mtml.name) {                                                                                                   \
      snprintf(error_string, sizeof(error_string), "Missing MTML symbol: mtml%s", #name);                              \
      goto fail;                                                                                                       \
    }                                                                                                                  \
  } while (0)
#define LOAD_OPTIONAL(name) mtml.name = dlsym(mtml_so, "mtml" #name)
  LOAD_REQUIRED(LibraryInit);
  LOAD_REQUIRED(LibraryShutDown);
  LOAD_REQUIRED(LibraryCountDevice);
  LOAD_REQUIRED(LibraryInitDeviceByIndex);
  LOAD_REQUIRED(LibraryFreeDevice);
  LOAD_REQUIRED(DeviceInitGpu);
  LOAD_REQUIRED(DeviceFreeGpu);
  LOAD_REQUIRED(DeviceInitMemory);
  LOAD_REQUIRED(DeviceFreeMemory);
  LOAD_OPTIONAL(ErrorString);
  LOAD_OPTIONAL(DeviceGetName);
  LOAD_OPTIONAL(DeviceGetPciInfo);
  LOAD_OPTIONAL(DeviceGetPowerUsage);
  LOAD_OPTIONAL(DeviceGetFanSpeed);
  LOAD_OPTIONAL(DeviceGetFanRpm);
  LOAD_OPTIONAL(GpuGetUtilization);
  LOAD_OPTIONAL(GpuGetClock);
  LOAD_OPTIONAL(GpuGetMaxClock);
  LOAD_OPTIONAL(GpuGetTemperature);
  LOAD_OPTIONAL(GpuGetEnforcedPowerLimit);
  LOAD_OPTIONAL(MemoryGetTotal);
  LOAD_OPTIONAL(MemoryGetUsed);
  LOAD_OPTIONAL(MemoryGetClock);
  LOAD_OPTIONAL(MemoryGetMaxClock);
#undef LOAD_REQUIRED
#undef LOAD_OPTIONAL

  int status = mtml.LibraryInit(&library);
  if (status != 0) {
    snprintf(error_string, sizeof(error_string), "MTML initialization failed (%d): %s", status,
             mtml.ErrorString ? mtml.ErrorString(status) : "unknown error");
    library = NULL;
    goto fail;
  }
  return true;
fail:
  moorethreads_shutdown();
  return false;
}

static const char *moorethreads_last_error(void) { return error_string; }

extern struct gpu_vendor gpu_vendor_moorethreads;

static bool moorethreads_get_devices(struct list_head *devices, unsigned *count) {
  unsigned device_count = 0;
  *count = 0;
  if (mtml.LibraryCountDevice(library, &device_count) != 0)
    return false;

  for (unsigned i = 0; i < device_count; ++i) {
    struct gpu_info_moorethreads *gpu = calloc(1, sizeof(*gpu));
    if (!gpu)
      break;
    if (mtml.LibraryInitDeviceByIndex(library, i, &gpu->device) != 0) {
      free(gpu);
      continue;
    }
    if (mtml.DeviceInitGpu(gpu->device, &gpu->gpu) != 0)
      gpu->gpu = NULL;
    if (mtml.DeviceInitMemory(gpu->device, &gpu->memory) != 0)
      gpu->memory = NULL;
    gpu->base.vendor = &gpu_vendor_moorethreads;
    struct mtml_pci_info pci = {0};
    snprintf(gpu->base.pdev, sizeof(gpu->base.pdev), "mtml:%u", i);
    if (mtml.DeviceGetPciInfo && mtml.DeviceGetPciInfo(gpu->device, &pci) == 0) {
      unsigned domain, bus, device, function;
      pci.sbdf[sizeof(pci.sbdf) - 1] = '\0';
      /* Normalize MTML's eight-digit segment to the Linux PCI BDF form. */
      if (sscanf(pci.sbdf, "%x:%x:%x.%x", &domain, &bus, &device, &function) == 4 &&
          domain <= 0xffff && bus <= 0xff && device <= 0x1f && function <= 7)
        snprintf(gpu->base.pdev, sizeof(gpu->base.pdev), "%04x:%02x:%02x.%x", domain, bus, device, function);
    }
    list_add_tail(&gpu->allocation, &allocations);
    list_add_tail(&gpu->base.list, devices);
    ++*count;
  }
  return true;
}

static void moorethreads_static_info(struct gpu_info *base) {
  struct gpu_info_moorethreads *gpu = (struct gpu_info_moorethreads *)base;
  struct gpuinfo_static_info *info = &base->static_info;
  RESET_ALL(info->valid);
  info->integrated_graphics = false;
  info->encode_decode_shared = false;
  if (!mtml.DeviceGetName || mtml.DeviceGetName(gpu->device, info->device_name, sizeof(info->device_name)) != 0)
    snprintf(info->device_name, sizeof(info->device_name), "Moore Threads GPU");
  info->device_name[sizeof(info->device_name) - 1] = '\0';
  SET_VALID(gpuinfo_device_name_valid, info->valid);
  struct mtml_pci_info pci = {0};
  if (mtml.DeviceGetPciInfo && mtml.DeviceGetPciInfo(gpu->device, &pci) == 0) {
    if (pci.max_gen)
      SET_GPUINFO_STATIC(info, max_pcie_gen, pci.max_gen);
    if (pci.max_width)
      SET_GPUINFO_STATIC(info, max_pcie_link_width, pci.max_width);
  }
}

static void moorethreads_dynamic_info(struct gpu_info *base) {
  struct gpu_info_moorethreads *gpu = (struct gpu_info_moorethreads *)base;
  struct gpuinfo_dynamic_info *info = &base->dynamic_info;
  RESET_ALL(info->valid);
  unsigned value;
#define QUERY(handle, function, field)                                                                                 \
  do {                                                                                                                 \
    if ((handle) && mtml.function && mtml.function(handle, &value) == 0)                                                 \
      SET_GPUINFO_DYNAMIC(info, field, value);                                                                          \
  } while (0)
  QUERY(gpu->gpu, GpuGetUtilization, gpu_util_rate);
  QUERY(gpu->gpu, GpuGetClock, gpu_clock_speed);
  QUERY(gpu->gpu, GpuGetMaxClock, gpu_clock_speed_max);
  QUERY(gpu->gpu, GpuGetTemperature, gpu_temp);
  QUERY(gpu->memory, MemoryGetClock, mem_clock_speed);
  QUERY(gpu->memory, MemoryGetMaxClock, mem_clock_speed_max);
  QUERY(gpu->device, DeviceGetPowerUsage, power_draw); /* MTML reports milliwatts. */
  QUERY(gpu->gpu, GpuGetEnforcedPowerLimit, power_draw_max); /* Also milliwatts; requires the GPU sub-handle. */
#undef QUERY
  if (GPUINFO_DYNAMIC_FIELD_VALID(info, gpu_util_rate) && info->gpu_util_rate > 100)
    RESET_GPUINFO_DYNAMIC(info, gpu_util_rate);
  if (mtml.DeviceGetFanSpeed && mtml.DeviceGetFanSpeed(gpu->device, 0, &value) == 0 && value <= 100)
    SET_GPUINFO_DYNAMIC(info, fan_speed, value);
  if (mtml.DeviceGetFanRpm && mtml.DeviceGetFanRpm(gpu->device, 0, &value) == 0)
    SET_GPUINFO_DYNAMIC(info, fan_rpm, value);

  uint64_t total, used;
  if (gpu->memory && mtml.MemoryGetTotal && mtml.MemoryGetTotal(gpu->memory, &total) == 0)
    SET_GPUINFO_DYNAMIC(info, total_memory, total);
  if (gpu->memory && mtml.MemoryGetUsed && mtml.MemoryGetUsed(gpu->memory, &used) == 0)
    SET_GPUINFO_DYNAMIC(info, used_memory, used);
  if (GPUINFO_DYNAMIC_FIELD_VALID(info, total_memory) && GPUINFO_DYNAMIC_FIELD_VALID(info, used_memory)) {
    if (info->used_memory <= info->total_memory) {
      SET_GPUINFO_DYNAMIC(info, free_memory, info->total_memory - info->used_memory);
      if (info->total_memory)
        SET_GPUINFO_DYNAMIC(info, mem_util_rate, (unsigned)(100.0 * info->used_memory / info->total_memory));
    } else {
      RESET_GPUINFO_DYNAMIC(info, used_memory);
    }
  }
  struct mtml_pci_info pci = {0};
  if (mtml.DeviceGetPciInfo && mtml.DeviceGetPciInfo(gpu->device, &pci) == 0) {
    if (pci.current_gen)
      SET_GPUINFO_DYNAMIC(info, pcie_link_gen, pci.current_gen);
    if (pci.current_width)
      SET_GPUINFO_DYNAMIC(info, pcie_link_width, pci.current_width);
  }
}

static void moorethreads_processes(struct gpu_info *base) {
  /* Native MTML process APIs exist, but the installed Python bindings omit
   * their record layouts. Do not guess those layouts or use NVML's ABI. */
  base->processes_count = 0;
}

struct gpu_vendor gpu_vendor_moorethreads = {
    .init = moorethreads_init,
    .shutdown = moorethreads_shutdown,
    .last_error_string = moorethreads_last_error,
    .get_device_handles = moorethreads_get_devices,
    .populate_static_info = moorethreads_static_info,
    .refresh_dynamic_info = moorethreads_dynamic_info,
    .refresh_running_processes = moorethreads_processes,
    .name = "Moore Threads",
};

__attribute__((constructor)) static void register_moorethreads(void) {
  register_gpu_vendor(&gpu_vendor_moorethreads);
}
