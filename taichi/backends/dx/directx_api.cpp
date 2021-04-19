#include "directx_api.h"

#include "taichi/program/kernel.h"

TLANG_NAMESPACE_BEGIN

namespace dx {

ID3D11Device *g_device;
ID3D11DeviceContext *g_context;
ID3D11Buffer *g_args_i32_buf, *g_args_f32_buf, *g_data_i32_buf, *g_data_f32_buf;
ID3D11UnorderedAccessView *g_args_i32_uav, *g_args_f32_uav, *g_data_i32_uav,
    *g_data_f32_uav;

HRESULT CreateComputeDevice(ID3D11Device **out_device,
                            ID3D11DeviceContext **out_context,
                            bool force_ref) {
  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  UINT flags = 0;
  flags |= D3D11_CREATE_DEVICE_DEBUG;

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, levels, _countof(levels),
                                 D3D11_SDK_VERSION, &device, nullptr, &context);

  if (FAILED(hr)) {
    printf("Failed to create D3D11 device: %08X\n", hr);
    return -1;
  }

  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hwopts = {0};
    device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hwopts,
                                sizeof(hwopts));
    if (!hwopts.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) {
      device->Release();
      printf(
          "DirectCompute not supported via "
          "ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4\n");
      return -1;
    }
  }

  *out_device = device;
  *out_context = context;
  return hr;
}

HRESULT CreateStructuredBuffer(ID3D11Device *device,
                               UINT element_size,
                               UINT count,
                               void *init_data,
                               ID3D11Buffer **out_buf) {
  *out_buf = nullptr;
  D3D11_BUFFER_DESC desc = {};
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  desc.ByteWidth = element_size * count;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.StructureByteStride = element_size;
  if (init_data) {
    D3D11_SUBRESOURCE_DATA data;
    data.pSysMem = init_data;
    return device->CreateBuffer(&desc, &data, out_buf);
  } else {
    return device->CreateBuffer(&desc, nullptr, out_buf);
  }
}

HRESULT CreateBufferUAV(ID3D11Device *device,
                        ID3D11Buffer *buffer,
                        ID3D11UnorderedAccessView **out_uav) {
  D3D11_BUFFER_DESC buf_desc = {};
  buffer->GetDesc(&buf_desc);
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  if (buf_desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) {
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    uav_desc.Buffer.NumElements = buf_desc.ByteWidth / 4;
  } else if (buf_desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.Buffer.NumElements =
        buf_desc.ByteWidth / buf_desc.StructureByteStride;
  } else
    return E_INVALIDARG;
  return device->CreateUnorderedAccessView(buffer, &uav_desc, out_uav);
}

HRESULT CompileComputeShaderFromString(const std::string &source,
                                       LPCSTR entry_point,
                                       ID3D11Device *device,
                                       ID3DBlob **blob) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
  LPCSTR profile = (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0)
                       ? "cs_5_0"
                       : "cs_4_0";
  const D3D_SHADER_MACRO defines[] = {"EXAMPLE_DEFINE", "1", NULL, NULL};
  ID3DBlob *shader_blob = nullptr, *error_blob = nullptr;
  HRESULT hr =
      D3DCompile(source.data(), source.size(), nullptr, defines, nullptr,
                 entry_point, profile, flags, 0, &shader_blob, &error_blob);
  if (FAILED(hr)) {
    printf("Error in CompileComputeShaderFromString\n");
    if (error_blob) {
      printf("%s\n", (char *)error_blob->GetBufferPointer());
      error_blob->Release();
    } else
      printf("error_blob is null\n");
    if (shader_blob) {
      shader_blob->Release();
    }
    fflush(stdout);
    return hr;
  }
  *blob = shader_blob;
  return hr;
}

char* DumpBuffer(ID3D11Buffer *buf, size_t* len) {
  D3D11_BUFFER_DESC desc = {};
  buf->GetDesc(&desc);
  ID3D11Buffer *tmpbuf;
  HRESULT hr;

  D3D11_BUFFER_DESC tmp_desc = {};
  tmp_desc.ByteWidth = desc.ByteWidth;
  tmp_desc.Usage = D3D11_USAGE_STAGING;
  tmp_desc.BindFlags = 0;
  tmp_desc.MiscFlags = 0;
  tmp_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = g_device->CreateBuffer(&tmp_desc, nullptr, &tmpbuf);
  assert(SUCCEEDED(hr));
  g_context->CopyResource(tmpbuf, buf);

  D3D11_MAPPED_SUBRESOURCE mapped;
  hr = g_context->Map(tmpbuf, 0, D3D11_MAP_READ, 0, &mapped);
  assert(SUCCEEDED(hr));
  char *ret = new char[desc.ByteWidth];
  if (len) {
    *len = desc.ByteWidth;
  }
  memcpy(ret, mapped.pData, desc.ByteWidth);
  g_context->Unmap(tmpbuf, 0);
  tmpbuf->Release();
  return ret;
}

bool initialize_dx(bool error_tolerance = false) {
  if (g_device == nullptr || g_context == nullptr) {
    TI_TRACE("Creating D3D11 device");
    CreateComputeDevice(&g_device, &g_context, false);

    TI_TRACE("Creating D3D11 buffers");
    const int N = 1048576;
    CreateStructuredBuffer(g_device, 4, N, nullptr, &g_data_i32_buf);
    CreateStructuredBuffer(g_device, 4, N, nullptr, &g_data_f32_buf);
    CreateStructuredBuffer(g_device, 4, N, nullptr, &g_args_i32_buf);
    CreateStructuredBuffer(g_device, 4, N, nullptr, &g_args_f32_buf);

    TI_TRACE("Creating D3D11 UAVs");
    CreateBufferUAV(g_device, g_data_i32_buf, &g_data_i32_uav);
    CreateBufferUAV(g_device, g_data_f32_buf, &g_data_f32_uav);
    CreateBufferUAV(g_device, g_args_i32_buf, &g_args_i32_uav);
    CreateBufferUAV(g_device, g_args_f32_buf, &g_args_f32_uav);
  } else {
    TI_TRACE("D3D11 device has already been created.");
  }
  return true;
}

bool is_dx_api_available() {
	//if (get_environ_config("TI_ENABLE_DX", 1) == 0)
	//	return false;
    return initialize_dx();
}

// CompiledKernel, CompiledKernel::Impl

CompiledKernel::CompiledKernel(const std::string &kernel_name_,
                               const std::string &kernel_source_code,
                               std::unique_ptr<ParallelSize> ps_) :
   impl(std::make_unique<Impl>(kernel_name_, kernel_source_code,
     std::move(ps_))) {
}

void CompiledKernel::dispatch_compute(HLSLLauncher *launcher) const {
  impl->dispatch_compute(launcher);
}

CompiledKernel::Impl::Impl(const std::string &kernel_name,
                           const std::string &kernel_source_code,
                           std::unique_ptr<ParallelSize> ps_)
    : kernel_name(kernel_name), ps(std::move(ps)), compute_shader(nullptr) {
  printf("CompiledKernel::Impl ctor\n");
  printf("kernel_name: %s\n", kernel_name.c_str());
  printf("kernel_source_code: %s\n", kernel_source_code.c_str());
  // todo: add dimension limit

  // Build program here
  ID3DBlob *shader_blob;
  HRESULT hr = CompileComputeShaderFromString(kernel_source_code, "CSMain",
                                              g_device, &shader_blob);
  if (SUCCEEDED(hr)) {
    TI_TRACE("Kernel compilation OK");

    ID3D11ComputeShader *cs = nullptr;
    hr = g_device->CreateComputeShader(shader_blob->GetBufferPointer(),
                                       shader_blob->GetBufferSize(), nullptr,
                                       &cs);
    shader_blob->Release();
    compute_shader = cs;
    if (SUCCEEDED(hr)) {
      TI_TRACE("Create Compute Shader OK");
    }

  } else {
    TI_ERROR("Kernel compilation error");
  }
}

void CompiledKernel::Impl::dispatch_compute(HLSLLauncher *launcher) const {
  TI_TRACE("CompiledKernel::Impl::dispatch_compute");
  // 1. set shader

  // debug
  size_t nbytes;
  float* f32_data0 = (float *)(DumpBuffer(g_data_f32_buf, &nbytes));

  // Temporary u0=_data_i32_, u1=_data_f32_, u2=_args_i32_, u3=_args_f32_
  ID3D11UnorderedAccessView *uavs[] = {g_data_i32_uav, g_data_f32_uav,
                                       g_args_i32_uav, g_args_f32_uav};
  g_context->CSSetShader(compute_shader, nullptr, 0);
  g_context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);

  // FIXME: ps is sometimes invalid
  UINT num_grids = 1; //ps.get()->grid_dim;
  TI_TRACE("num_grids={}", num_grids);
  g_context->Dispatch(num_grids, 1, 1);
  // 2. memory barrier

  // debug
  float* f32_data1 = (float*)DumpBuffer(g_data_f32_buf, nullptr);

  printf("f32_data before vs after\n");
  int num_diff = 0;
  for (int i = 0; i < nbytes / 4; i++) {
    float b0 = f32_data0[i], b1 = f32_data1[i];
    if (b0 != b1) {
      num_diff++;
      if (num_diff <= 10) {
        printf("[%d]: %g vs %g\n", i, b0, b1);
      }
    }
  }
  printf("%d differences in total\n", num_diff);
}

// CompiledProgram, CompiledProgram::Impl

CompiledProgram::CompiledProgram(Kernel *kernel)
    : impl(std::make_unique<Impl>(kernel)) {
}

void CompiledProgram::Impl::add(const std::string &kernel_name,
                                const std::string &kernel_source_code,
                                std::unique_ptr<ParallelSize> ps) {
  TI_TRACE("CompiledProgram::Impl::add");
  kernels.push_back(std::make_unique<CompiledKernel>(
      kernel_name, kernel_source_code, std::move(ps)));
}

CompiledProgram::Impl::Impl(Kernel *kernel) {
  arg_count = kernel->args.size();
  ret_count = kernel->rets.size();

  for (int i = 0; i < arg_count; i++) {
    if (kernel->args[i].is_nparray) {
      TI_ERROR("ext_arr_map not implemented");
      TI_NOT_IMPLEMENTED;
    }
  }
}

// args UAVs are used both for arguments and return values
void CompiledProgram::Impl::launch(Context &ctx, HLSLLauncher *launcher) const {
  TI_TRACE("CompiledProgram launch, ctx.args: {} {} {} {} {} {} {} {}, arg_count={}, ret_count={}", 
    ctx.args[0], ctx.args[1],
           ctx.args[2], ctx.args[3], ctx.args[4], ctx.args[5], ctx.args[6],
           ctx.args[7], arg_count, ret_count);
  std::vector<char> args;
  args.resize(std::max(arg_count, ret_count) * sizeof(uint64_t));

  // Copy to the UAVs
  ID3D11Buffer *tmp_arg_buf;
  D3D11_BUFFER_DESC desc;
  g_args_f32_buf->GetDesc(&desc);
  D3D11_BUFFER_DESC tmp_desc = {};
  tmp_desc.ByteWidth = desc.ByteWidth;
  tmp_desc.BindFlags = 0;
  tmp_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
  tmp_desc.MiscFlags = 0;
  tmp_desc.Usage = D3D11_USAGE_STAGING;
  HRESULT hr = g_device->CreateBuffer(&tmp_desc, nullptr, &tmp_arg_buf);
  assert(SUCCEEDED(hr));

  // Reinterpret as ints
  int int_args[8];
  for (int i = 0; i < 8; i++) {
    int_args[i] = ctx.get_arg<int>(i);
  }
  D3D11_MAPPED_SUBRESOURCE mapped;
  hr = g_context->Map(tmp_arg_buf, 0, D3D11_MAP_WRITE, 0, &mapped);
  assert(SUCCEEDED(hr));
  memcpy(mapped.pData, int_args, sizeof(int_args));
  g_context->Unmap(tmp_arg_buf, 0);
  g_context->CopyResource(g_args_i32_buf, tmp_arg_buf);

  float float_args[8];
  for (int i = 0; i < 8; i++) {
    float_args[i] = ctx.get_arg<float>(i);
  }
  hr = g_context->Map(tmp_arg_buf, 0, D3D11_MAP_WRITE, 0, &mapped);
  assert(SUCCEEDED(hr));
  memcpy(mapped.pData, float_args, sizeof(float_args));
  g_context->Unmap(tmp_arg_buf, 0);
  g_context->CopyResource(g_args_f32_buf, tmp_arg_buf);

  for (const auto &kernel : kernels) {
    kernel->dispatch_compute(launcher);
  }

  // Process return values
  // Very crappy for now
  // TODO: figure out what the return statements mean.
  g_context->CopyResource(tmp_arg_buf, g_args_f32_buf);
  hr = g_context->Map(tmp_arg_buf, 0, D3D11_MAP_READ, 0, &mapped);
  assert(SUCCEEDED(hr));
  memcpy(float_args, mapped.pData, sizeof(float_args));
  uint64_t *ptr = (uint64_t*)(launcher->result_buffer);
  for (int i = 0; i < ret_count; i++) {
    ptr[i] = *(reinterpret_cast<uint64_t*>(&float_args[i]));
  }
}

void CompiledProgram::add(const std::string &kernel_name,
  const std::string &kernel_source_code,
  std::unique_ptr<ParallelSize> ps) {
  impl->add(kernel_name, kernel_source_code, std::move(ps));
}

void CompiledProgram::launch(Context &ctx, HLSLLauncher *launcher) const {
  impl->launch(ctx, launcher);
}

HLSLLauncher::HLSLLauncher(size_t size) {
  initialize_dx();
  TI_TRACE("HLSLLauncher ctor");
  impl = std::make_unique<HLSLLauncherImpl>();
}

void HLSLLauncher::keep(std::unique_ptr<CompiledProgram> program) {
  impl->programs.push_back(std::move(program));
}



}

TLANG_NAMESPACE_END