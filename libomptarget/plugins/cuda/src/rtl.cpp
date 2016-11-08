//===----RTLs/cuda/src/rtl.cpp - Target RTLs Implementation ------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// RTL for CUDA machine
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <assert.h>
#include <cstdio>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <libelf.h>
#include <list>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>

#include "omptarget.h"

#ifndef TARGET_NAME
#define TARGET_NAME Generic - 64bit
#endif

#define GETNAME2(name) #name
#define GETNAME(name) GETNAME2(name)
#define DP(...) DEBUGP("Target " GETNAME(TARGET_NAME) " RTL", __VA_ARGS__)

// Utility for retrieving and printing CUDA error string.
#ifdef CUDA_ERROR_REPORT
#define CUDA_ERR_STRING(err)                                                   \
  do {                                                                         \
    const char *errStr;                                                        \
    cuGetErrorString(err, &errStr);                                            \
    DP("CUDA error is: %s\n", errStr);                                         \
  } while (0)
#else
#define CUDA_ERR_STRING(err)                                                   \
  {}
#endif

// Implemented in libomp, they are called from within __tgt_rtl_* functions.
int omp_get_thread_limit(void) __attribute__((weak));
int omp_get_max_threads(void) __attribute__((weak));

/// Account the memory allocated per device.
struct AllocMemEntryTy {
  int64_t TotalSize;
  std::vector<std::pair<void *, int64_t>> Ptrs;

  AllocMemEntryTy() : TotalSize(0) {}
};

/// Keep entries table per device.
struct FuncOrGblEntryTy {
  __tgt_target_table Table;
  std::vector<__tgt_offload_entry> Entries;
};

enum ExecutionModeType {
  SPMD,
  GENERIC,
  NONE
};

/// Use a single entity to encode a kernel and a set of flags
struct KernelTy {
  CUfunction Func;

  // execution mode of kernel
  // 0 - SPMD mode (without master warp)
  // 1 - Generic mode (with master warp)
  int8_t ExecutionMode;

  // keep track of cuda pointer to write to it when thread_limit value
  // changes (check against last value written to ThreadLimit).
  CUdeviceptr ThreadLimitPtr;
  int ThreadLimit;

  KernelTy(CUfunction _Func, int8_t _ExecutionMode, CUdeviceptr _ThreadLimitPtr)
      : Func(_Func), ExecutionMode(_ExecutionMode),
        ThreadLimitPtr(_ThreadLimitPtr) {
    ThreadLimit = 0; // default (0) signals that it was not initialized
  };
};

/// List that contains all the kernels.
/// FIXME: we may need this to be per device and per library.
std::list<KernelTy> KernelsList;

/// Class containing all the device information.
class RTLDeviceInfoTy {
  std::vector<FuncOrGblEntryTy> FuncGblEntries;

public:
  int NumberOfDevices;
  std::vector<CUmodule> Modules;
  std::vector<CUcontext> Contexts;

  // Device properties
  std::vector<int> ThreadsPerBlock;
  std::vector<int> BlocksPerGrid;
  std::vector<int> WarpSize;

  // OpenMP properties
  std::vector<int> NumTeams;
  std::vector<int> NumThreads;

  // OpenMP Environment properties
  int EnvNumTeams;
  int EnvTeamLimit;

  //static int EnvNumThreads;
  static const int HardTeamLimit = 1<<16; // 64k
  static const int HardThreadLimit = 1024;
  static const int DefaultNumTeams = 128;
  static const int DefaultNumThreads = 1024;

  // Record entry point associated with device
  void addOffloadEntry(int32_t device_id, __tgt_offload_entry entry) {
    assert(device_id < (int32_t)FuncGblEntries.size() &&
           "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    E.Entries.push_back(entry);
  }

  // Return true if the entry is associated with device
  bool findOffloadEntry(int32_t device_id, void *addr) {
    assert(device_id < (int32_t)FuncGblEntries.size() &&
           "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    for (unsigned i = 0; i < E.Entries.size(); ++i) {
      if (E.Entries[i].addr == addr)
        return true;
    }

    return false;
  }

  // Return the pointer to the target entries table
  __tgt_target_table *getOffloadEntriesTable(int32_t device_id) {
    assert(device_id < (int32_t)FuncGblEntries.size() &&
           "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    int32_t size = E.Entries.size();

    // Table is empty
    if (!size)
      return 0;

    __tgt_offload_entry *begin = &E.Entries[0];
    __tgt_offload_entry *end = &E.Entries[size - 1];

    // Update table info according to the entries and return the pointer
    E.Table.EntriesBegin = begin;
    E.Table.EntriesEnd = ++end;

    return &E.Table;
  }

  // Clear entries table for a device
  void clearOffloadEntriesTable(int32_t device_id) {
    assert(device_id < (int32_t)FuncGblEntries.size() &&
           "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];
    E.Entries.clear();
    E.Table.EntriesBegin = E.Table.EntriesEnd = 0;
  }

  RTLDeviceInfoTy() {
    DP("Start initializing CUDA\n");

    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
      DP("Error when initializing CUDA\n");
      CUDA_ERR_STRING(err);
      return;
    }

    NumberOfDevices = 0;

    err = cuDeviceGetCount(&NumberOfDevices);
    if (err != CUDA_SUCCESS) {
      DP("Error when getting CUDA device count\n");
      CUDA_ERR_STRING(err);
      return;
    }

    if (NumberOfDevices == 0) {
      DP("There are no devices supporting CUDA.\n");
      return;
    }

    FuncGblEntries.resize(NumberOfDevices);
    Contexts.resize(NumberOfDevices);
    ThreadsPerBlock.resize(NumberOfDevices);
    BlocksPerGrid.resize(NumberOfDevices);
    WarpSize.resize(NumberOfDevices);
    NumTeams.resize(NumberOfDevices);
    NumThreads.resize(NumberOfDevices);

    // Get environment variables regarding teams
    char *envStr = getenv("OMP_TEAM_LIMIT");
    if (envStr) {
      // OMP_TEAM_LIMIT has been set
      EnvTeamLimit = std::stoi(envStr);
      DP("Parsed OMP_TEAM_LIMIT=%d\n", EnvTeamLimit);
    } else {
      EnvTeamLimit = -1;
    }
    envStr = getenv("OMP_NUM_TEAMS");
    if (envStr) {
      // OMP_NUM_TEAMS has been set
      EnvNumTeams = std::stoi(envStr);
      DP("Parsed OMP_NUM_TEAMS=%d\n", EnvNumTeams);
    } else {
      EnvNumTeams = -1;
    }
  }

  ~RTLDeviceInfoTy() {
    // Close modules
    for (unsigned i = 0; i < Modules.size(); ++i)
      if (Modules[i]) {
        CUresult err = cuModuleUnload(Modules[i]);
        if (err != CUDA_SUCCESS) {
          DP("Error when unloading CUDA module\n");
          CUDA_ERR_STRING(err);
        }
      }

    // Destroy contexts
    for (unsigned i = 0; i < Contexts.size(); ++i)
      if (Contexts[i]) {
        CUresult err = cuCtxDestroy(Contexts[i]);
        if (err != CUDA_SUCCESS) {
          DP("Error when destroying CUDA context\n");
          CUDA_ERR_STRING(err);
        }
      }
  }
};

static RTLDeviceInfoTy DeviceInfo;

#ifdef __cplusplus
extern "C" {
#endif

int32_t __tgt_rtl_is_valid_binary(__tgt_device_image *image) {

  // Is the library version incompatible with the header file?
  if (elf_version(EV_CURRENT) == EV_NONE) {
    DP("Incompatible ELF library!\n");
    return 0;
  }

  char *img_begin = (char *)image->ImageStart;
  char *img_end = (char *)image->ImageEnd;
  size_t img_size = img_end - img_begin;

  // Obtain elf handler
  Elf *e = elf_memory(img_begin, img_size);
  if (!e) {
    DP("Unable to get ELF handle: %s!\n", elf_errmsg(-1));
    return 0;
  }

  // Check if ELF is the right kind.
  if (elf_kind(e) != ELF_K_ELF) {
    DP("Unexpected ELF type!\n");
    return 0;
  }
  Elf64_Ehdr *eh64 = elf64_getehdr(e);
  Elf32_Ehdr *eh32 = elf32_getehdr(e);

  if (!eh64 && !eh32) {
    DP("Unable to get machine ID from ELF file!\n");
    elf_end(e);
    return 0;
  }

  uint16_t MachineID;
  if (eh64 && !eh32)
    MachineID = eh64->e_machine;
  else if (eh32 && !eh64)
    MachineID = eh32->e_machine;
  else {
    DP("Ambiguous ELF header!\n");
    elf_end(e);
    return 0;
  }

  elf_end(e);
  return MachineID == 190; // EM_CUDA = 190.
}

int32_t __tgt_rtl_number_of_devices() { return DeviceInfo.NumberOfDevices; }

int32_t __tgt_rtl_init_device(int32_t device_id) {

  CUdevice cuDevice;
  DP("Getting device %d\n", device_id);
  CUresult err = cuDeviceGet(&cuDevice, device_id);
  if (err != CUDA_SUCCESS) {
    DP("Error when getting CUDA device with id = %d\n", device_id);
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  // Create the context and save it to use whenever this device is selected.
  err = cuCtxCreate(&DeviceInfo.Contexts[device_id], CU_CTX_SCHED_BLOCKING_SYNC,
                    cuDevice);
  if (err != CUDA_SUCCESS) {
    DP("Error when creating a CUDA context\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  // scan properties to determine number of threads/block and blocks/grid.
  struct cudaDeviceProp Properties;
  cudaError_t error = cudaGetDeviceProperties(&Properties, device_id);
  if (error != cudaSuccess) {
    DP("Error getting device Properties, use defaults\n");
    DeviceInfo.BlocksPerGrid[device_id] = RTLDeviceInfoTy::DefaultNumTeams;
    DeviceInfo.ThreadsPerBlock[device_id] = RTLDeviceInfoTy::DefaultNumThreads;
    DeviceInfo.WarpSize[device_id] = 32;
  } else {
    // Get blocks per grid
    if (Properties.maxGridSize[0] <= RTLDeviceInfoTy::HardTeamLimit) {
      DeviceInfo.BlocksPerGrid[device_id] = Properties.maxGridSize[0];
      DP("Using %d CUDA blocks per grid\n", Properties.maxGridSize[0]);
    } else {
      DeviceInfo.BlocksPerGrid[device_id] = RTLDeviceInfoTy::HardTeamLimit;
      DP("Max CUDA blocks per grid %d exceeds the hard team limit %d, capping "
          "at the hard limit\n", Properties.maxGridSize[0],
          RTLDeviceInfoTy::HardTeamLimit);
    }

    // Get threads per block, exploit threads only along x axis
    if (Properties.maxThreadsDim[0] <= RTLDeviceInfoTy::HardThreadLimit) {
      DeviceInfo.ThreadsPerBlock[device_id] = Properties.maxThreadsDim[0];
      DP("Using %d CUDA threads per block\n", Properties.maxThreadsDim[0]);
      if (Properties.maxThreadsDim[0] < Properties.maxThreadsPerBlock) {
        DP("(fewer than max per block along all xyz dims %d)\n",
            Properties.maxThreadsPerBlock);
      }
    } else {
      DeviceInfo.ThreadsPerBlock[device_id] = RTLDeviceInfoTy::HardThreadLimit;
      DP("Max CUDA threads per block %d exceeds the hard thread limit %d, "
          "capping at the hard limt\n", Properties.maxThreadsDim[0],
          RTLDeviceInfoTy::HardThreadLimit);
    }

    // Get warp size
    DeviceInfo.WarpSize[device_id] = Properties.warpSize;
  }

  // Adjust teams to the env variables
  if (DeviceInfo.EnvTeamLimit > 0 &&
      DeviceInfo.BlocksPerGrid[device_id] > DeviceInfo.EnvTeamLimit) {
    DeviceInfo.BlocksPerGrid[device_id] = DeviceInfo.EnvTeamLimit;
    DP("Capping max CUDA blocks per grid to OMP_TEAM_LIMIT=%d\n",
        DeviceInfo.EnvTeamLimit);
  }

  DP("Max number of CUDA blocks %d, threads %d & warp size %d\n",
     DeviceInfo.BlocksPerGrid[device_id], DeviceInfo.ThreadsPerBlock[device_id],
     DeviceInfo.WarpSize[device_id]);

  // Set default number of teams
  if (DeviceInfo.EnvNumTeams > 0) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.EnvNumTeams;
    DP("Default number of teams set according to environment %d\n",
        DeviceInfo.EnvNumTeams);
  } else {
    DeviceInfo.NumTeams[device_id] = RTLDeviceInfoTy::DefaultNumTeams;
    DP("Default number of teams set according to library's default %d\n",
        RTLDeviceInfoTy::DefaultNumTeams);
  }
  if (DeviceInfo.NumTeams[device_id] > DeviceInfo.BlocksPerGrid[device_id]) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.BlocksPerGrid[device_id];
    DP("Default number of teams exceeds device limit, capping at %d\n",
        DeviceInfo.BlocksPerGrid[device_id]);
  }

  // Set default number of threads
  DeviceInfo.NumThreads[device_id] = RTLDeviceInfoTy::DefaultNumThreads;
  DP("Default number of threads set according to library's default %d\n",
          RTLDeviceInfoTy::DefaultNumThreads);
  if (DeviceInfo.NumThreads[device_id] >
      DeviceInfo.ThreadsPerBlock[device_id]) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.ThreadsPerBlock[device_id];
    DP("Default number of threads exceeds device limit, capping at %d\n",
        DeviceInfo.ThreadsPerBlock[device_id]);
  }

  return OFFLOAD_SUCCESS;
}

__tgt_target_table *__tgt_rtl_load_binary(int32_t device_id,
                                          __tgt_device_image *image) {

  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error when setting a CUDA context for device %d\n", device_id);
    CUDA_ERR_STRING(err);
    return NULL;
  }

  // Clear the offload table as we are going to create a new one.
  DeviceInfo.clearOffloadEntriesTable(device_id);

  // Create the module and extract the function pointers.

  CUmodule cumod;
  DP("load data from image %llx\n", (unsigned long long)image->ImageStart);
  err = cuModuleLoadDataEx(&cumod, image->ImageStart, 0, NULL, NULL);
  if (err != CUDA_SUCCESS) {
    DP("Error when loading CUDA module\n");
    CUDA_ERR_STRING(err);
    return NULL;
  }

  DP("CUDA module successfully loaded!\n");
  DeviceInfo.Modules.push_back(cumod);

  // Find the symbols in the module by name.
  __tgt_offload_entry *HostBegin = image->EntriesBegin;
  __tgt_offload_entry *HostEnd = image->EntriesEnd;

  for (__tgt_offload_entry *e = HostBegin; e != HostEnd; ++e) {

    if (!e->addr) {
      // FIXME: Probably we should fail when something like this happens, the
      // host should have always something in the address to uniquely identify
      // the target region.
      DP("Analyzing host entry '<null>' (size = %lld)...\n",
         (unsigned long long)e->size);

      __tgt_offload_entry entry = *e;
      DeviceInfo.addOffloadEntry(device_id, entry);
      continue;
    }

    if (e->size) {

      __tgt_offload_entry entry = *e;

      CUdeviceptr cuptr;
      size_t cusize;
      err = cuModuleGetGlobal(&cuptr, &cusize, cumod, e->name);

      if (err != CUDA_SUCCESS) {
        DP("loading global '%s' (Failed)\n", e->name);
        CUDA_ERR_STRING(err);
        return NULL;
      }

      if (cusize != e->size) {
        DP("loading global '%s' - size mismatch (%lld != %lld)\n", e->name,
           (unsigned long long)cusize, (unsigned long long)e->size);
        CUDA_ERR_STRING(err);
        return NULL;
      }

      DP("Entry point %ld maps to global %s (%016lx)\n", e - HostBegin, e->name,
         (long)cuptr);
      entry.addr = (void *)cuptr;

      DeviceInfo.addOffloadEntry(device_id, entry);

      continue;
    }

    CUfunction fun;
    err = cuModuleGetFunction(&fun, cumod, e->name);

    if (err != CUDA_SUCCESS) {
      DP("loading '%s' (Failed)\n", e->name);
      CUDA_ERR_STRING(err);
      return NULL;
    }

    DP("Entry point %ld maps to %s (%016lx)\n", e - HostBegin, e->name,
       (Elf64_Addr)fun);

    // default value GENERIC (in case symbol is missing from cubin file)
    int8_t ExecModeVal = ExecutionModeType::GENERIC;
    const char suffix[] = "_exec_mode";
    int32_t ExecModeLen = (strlen(e->name) + strlen(suffix) + 1) * sizeof(char);
    char * ExecModeName = (char *) malloc(ExecModeLen);
    snprintf(ExecModeName, ExecModeLen, "%s%s", e->name, suffix);

    CUdeviceptr ExecModePtr;
    size_t cusize;
    err = cuModuleGetGlobal(&ExecModePtr, &cusize, cumod, ExecModeName);
    if (err == CUDA_SUCCESS) {
      if ((size_t)cusize != sizeof(int8_t)) {
        DP("loading global exec_mode '%s' - size mismatch (%lld != %lld)\n",
           ExecModeName, (unsigned long long)cusize,
           (unsigned long long)sizeof(int8_t));
        CUDA_ERR_STRING(err);
        return NULL;
      }

      err = cuMemcpyDtoH(&ExecModeVal, (CUdeviceptr)ExecModePtr, cusize);
      if (err != CUDA_SUCCESS) {
        DP("Error when copying data from device to host. Pointers: "
           "host = 0x%016lx, device = 0x%016lx, size = %lld\n",
           (Elf64_Addr)&ExecModeVal, (Elf64_Addr)ExecModePtr,
           (unsigned long long)cusize);
        CUDA_ERR_STRING(err);
        return NULL;
      }

      if (ExecModeVal < 0 || ExecModeVal > 1) {
        DP("Error wrong exec_mode value specified in cubin file: %d\n",
           ExecModeVal);
        return NULL;
      }
    } else {
      DP("loading global exec_mode '%s' - symbol missing, "
         "using default value GENERIC (1)\n",
         ExecModeName);
      CUDA_ERR_STRING(err);
    }

    KernelsList.push_back(KernelTy(fun, ExecModeVal,
        /*ThreadLimitPtr=*/CUdeviceptr()));

    __tgt_offload_entry entry = *e;
    entry.addr = (void *)&KernelsList.back();
    DeviceInfo.addOffloadEntry(device_id, entry);
  }

  return DeviceInfo.getOffloadEntriesTable(device_id);
}

void *__tgt_rtl_data_alloc(int32_t device_id, int64_t size) {

  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error while trying to set CUDA current context\n");
    CUDA_ERR_STRING(err);
    return NULL;
  }

  CUdeviceptr ptr;
  err = cuMemAlloc(&ptr, size);
  if (err != CUDA_SUCCESS) {
    DP("Error while trying to allocate %d\n", err);
    CUDA_ERR_STRING(err);
    return NULL;
  }

  void *vptr = (void *)ptr;
  return vptr;
}

int32_t __tgt_rtl_data_submit(int32_t device_id, void *tgt_ptr, void *hst_ptr,
                              int64_t size) {
  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error when setting CUDA context\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  err = cuMemcpyHtoD((CUdeviceptr)tgt_ptr, hst_ptr, size);
  if (err != CUDA_SUCCESS) {
    DP("Error when copying data from host to device. Pointers: "
       "host = 0x%016lx, device = 0x%016lx, size = %lld\n",
       (Elf64_Addr)hst_ptr, (Elf64_Addr)tgt_ptr, (unsigned long long)size);
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_retrieve(int32_t device_id, void *hst_ptr, void *tgt_ptr,
                                int64_t size) {
  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error when setting CUDA context\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  err = cuMemcpyDtoH(hst_ptr, (CUdeviceptr)tgt_ptr, size);
  if (err != CUDA_SUCCESS) {
    DP("Error when copying data from device to host. Pointers: "
       "host = 0x%016lx, device = 0x%016lx, size = %lld\n",
       (Elf64_Addr)hst_ptr, (Elf64_Addr)tgt_ptr, (unsigned long long)size);
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_delete(int32_t device_id, void *tgt_ptr) {
  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error when setting CUDA context\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  err = cuMemFree((CUdeviceptr)tgt_ptr);
  if (err != CUDA_SUCCESS) {
    DP("Error when freeing CUDA memory\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_team_region(int32_t device_id, void *tgt_entry_ptr,
    void **tgt_args, int32_t arg_num, int32_t team_num, int32_t thread_limit,
    uint64_t loop_tripcount) {
  // Set the context we are using.
  CUresult err = cuCtxSetCurrent(DeviceInfo.Contexts[device_id]);
  if (err != CUDA_SUCCESS) {
    DP("Error when setting CUDA context\n");
    CUDA_ERR_STRING(err);
    return OFFLOAD_FAIL;
  }

  // All args are references.
  std::vector<void *> args(arg_num);

  for (int32_t i = 0; i < arg_num; ++i)
    args[i] = &tgt_args[i];

  KernelTy *KernelInfo = (KernelTy *)tgt_entry_ptr;

  int cudaThreadsPerBlock;

  if (thread_limit > 0) {
    cudaThreadsPerBlock = thread_limit;
    DP("Set CUDA threads per block to requested %d\n", thread_limit);
  } else {
    cudaThreadsPerBlock = DeviceInfo.NumThreads[device_id];
    DP("Set CUDA threads per block to default %d\n",
        DeviceInfo.NumThreads[device_id]);
  }

  // Add master warp if necessary
  if (KernelInfo->ExecutionMode == GENERIC) {
    cudaThreadsPerBlock += DeviceInfo.WarpSize[device_id];
    DP("Adding master warp: +%d threads\n", DeviceInfo.WarpSize[device_id]);
  }

  if (cudaThreadsPerBlock > DeviceInfo.ThreadsPerBlock[device_id]) {
    cudaThreadsPerBlock = DeviceInfo.ThreadsPerBlock[device_id];
    DP("Threads per block capped at device limit %d\n",
        DeviceInfo.ThreadsPerBlock[device_id]);
  }

  int kernel_limit;
  err = cuFuncGetAttribute(&kernel_limit,
      CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, KernelInfo->Func);
  if (err == CUDA_SUCCESS) {
    if (kernel_limit < cudaThreadsPerBlock) {
      cudaThreadsPerBlock = kernel_limit;
      DP("Threads per block capped at kernel limit %d\n", kernel_limit);
    }
  }

  int cudaBlocksPerGrid;
  if (team_num <= 0) {
    if (loop_tripcount > 0 && DeviceInfo.EnvNumTeams < 0) {
      // round up to the nearest integer
      cudaBlocksPerGrid = ((loop_tripcount - 1) / cudaThreadsPerBlock) + 1;
      DP("Using %d teams due to loop trip count %lu and number of threads per "
          "block %d\n", cudaBlocksPerGrid, loop_tripcount, cudaThreadsPerBlock);
    } else {
      cudaBlocksPerGrid = DeviceInfo.NumTeams[device_id];
      DP("Using default number of teams %d\n", DeviceInfo.NumTeams[device_id]);
    }
  } else if (team_num > DeviceInfo.BlocksPerGrid[device_id]) {
    cudaBlocksPerGrid = DeviceInfo.BlocksPerGrid[device_id];
    DP("Capping number of teams to team limit %d\n",
        DeviceInfo.BlocksPerGrid[device_id]);
  } else {
    cudaBlocksPerGrid = team_num;
    DP("Using requested number of teams %d\n", team_num);
  }

  int nshared = 0;

  // Run on the device.
  DP("launch kernel with %d blocks and %d threads\n", cudaBlocksPerGrid,
     cudaThreadsPerBlock);

  err = cuLaunchKernel(KernelInfo->Func, cudaBlocksPerGrid, 1, 1,
                       cudaThreadsPerBlock, 1, 1, nshared, 0, &args[0], 0);
  if (err != CUDA_SUCCESS) {
    DP("Device kernel launching failed!\n");
    CUDA_ERR_STRING(err);
    assert(err == CUDA_SUCCESS && "Unable to launch target execution!");
    return OFFLOAD_FAIL;
  }

  DP("Execution of entry point at %016lx successful!\n",
     (Elf64_Addr)tgt_entry_ptr);
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_region(int32_t device_id, void *tgt_entry_ptr,
                                    void **tgt_args, int32_t arg_num) {
  // use one team and one thread.
  // fix thread num.
  int32_t team_num = 1;
  int32_t thread_limit = 0; // use default.
  return __tgt_rtl_run_target_team_region(device_id, tgt_entry_ptr, tgt_args,
                                          arg_num, team_num, thread_limit, 0);
}

#ifdef __cplusplus
}
#endif