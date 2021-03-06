/******************************************************************************
 * Copyright (c) 2017-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

// Code shared by CUDA MDL SDK examples

#ifndef EXAMPLE_CUDA_SHARED_H
#define EXAMPLE_CUDA_SHARED_H

#include <string>
#include <vector>
#include <sstream>
#include <iostream>

#include <mi/mdl_sdk.h>

#include "example_shared.h"

#include <cuda.h>
#ifdef OPENGL_INTEROP
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cudaGL.h>
#endif
#include <cuda_runtime.h>
#include <vector_functions.h>


// Structure representing an MDL texture, containing filtered and unfiltered CUDA texture
// objects and the size of the texture.
struct Texture
{
    Texture(cudaTextureObject_t  filtered_object,
            cudaTextureObject_t  unfiltered_object,
            uint3                size)
        : filtered_object(filtered_object)
        , unfiltered_object(unfiltered_object)
        , size(size)
        , inv_size(make_float3(1.0f / size.x, 1.0f / size.y, 1.0f / size.z))
    {}

    cudaTextureObject_t  filtered_object;    // uses filter mode cudaFilterModeLinear
    cudaTextureObject_t  unfiltered_object;  // uses filter mode cudaFilterModePoint
    uint3                size;               // size of the texture, needed for texel access
    float3               inv_size;           // the inverse values of the size of the texture
};

// Structure representing the resources used by the generated code of a target code.
struct Target_code_data
{
    Target_code_data(size_t num_textures, CUdeviceptr textures, CUdeviceptr ro_data_segment)
        : num_textures(num_textures)
        , textures(textures)
        , ro_data_segment(ro_data_segment)
    {}

    size_t      num_textures;      // number of elements in the textures field
    CUdeviceptr textures;          // a device pointer to a list of Texture objects, if used
    CUdeviceptr ro_data_segment;   // a device pointer to the read-only data segment, if used
};


//------------------------------------------------------------------------------
//
// Helper functions
//
//------------------------------------------------------------------------------

// Return a textual representation of the given value.
template <typename T>
std::string to_string(T val)
{
    std::ostringstream stream;
    stream << val;
    return stream.str();
}


//------------------------------------------------------------------------------
//
// CUDA helper functions
//
//------------------------------------------------------------------------------

// Helper macro. Checks whether the expression is cudaSuccess and if not prints a message and
// resets the device and exits.
#define check_cuda_success(expr) \
    do { \
        int err = (expr); \
        if (err != 0) { \
            fprintf(stderr, "CUDA error %d in file %s, line %u: \"%s\".\n", \
                err, __FILE__, __LINE__, #expr); \
            keep_console_open(); \
            cudaDeviceReset(); \
            exit(EXIT_FAILURE); \
        } \
    } while (false)


// Initialize CUDA.
CUcontext init_cuda(
#ifdef OPENGL_INTEROP
    const bool opengl_interop
#endif
    )
{
    CUdevice cu_device;
    CUcontext cu_context;

    check_cuda_success(cuInit(0));
#if defined(OPENGL_INTEROP) && !defined(__APPLE__)
    if (opengl_interop) {
        // Use first device used by OpenGL context
        unsigned int num_cu_devices;
        check_cuda_success(cuGLGetDevices(&num_cu_devices, &cu_device, 1, CU_GL_DEVICE_LIST_ALL));
    }
    else
#endif
        // Use first device
        check_cuda_success(cuDeviceGet(&cu_device, 0));

    check_cuda_success(cuCtxCreate(&cu_context, 0, cu_device));

    // For this example, increase printf CUDA buffer size to support a larger number
    // of MDL debug::print() calls per CUDA kernel launch
    cudaDeviceSetLimit(cudaLimitPrintfFifoSize, 16 * 1024 * 1024);

    return cu_context;
}

// Uninitialize CUDA.
void uninit_cuda(CUcontext cuda_context)
{
    check_cuda_success(cuCtxDestroy(cuda_context));
}

// Allocate memory on GPU and copy the given data to the allocated memory.
CUdeviceptr gpu_mem_dup(void const *data, size_t size)
{
    CUdeviceptr device_ptr;
    check_cuda_success(cuMemAlloc(&device_ptr, size));
    check_cuda_success(cuMemcpyHtoD(device_ptr, data, size));
    return device_ptr;
}

// Allocate memory on GPU and copy the given data to the allocated memory.
template<typename T>
CUdeviceptr gpu_mem_dup(std::vector<T> const &data)
{
    return gpu_mem_dup(&data[0], data.size() * sizeof(T));
}


//------------------------------------------------------------------------------
//
// Material_gpu_context class
//
//------------------------------------------------------------------------------

// Helper class responsible for making textures and read-only data available to the GPU
// by generating and managing a list of Target_code_data objects.
class Material_gpu_context
{
public:
    Material_gpu_context()
        : m_device_target_code_data_list(0)
        , m_device_target_argument_block_list(0)
    {
        // Use first entry as "not-used" block
        m_target_argument_block_list.push_back(0);
    }

    // Free all acquired resources.
    ~Material_gpu_context();

    // Prepare the needed data of the given target code.
    bool prepare_target_code_data(
        mi::neuraylib::ITransaction       *transaction,
        mi::neuraylib::IImage_api         *image_api,
        mi::neuraylib::ITarget_code const *target_code);

    // Get a device pointer to the target code data list.
    CUdeviceptr get_device_target_code_data_list();

    // Get a device pointer to the target argument block list.
    CUdeviceptr get_device_target_argument_block_list();

    // Get a device pointer to the i'th target argument block.
    CUdeviceptr get_device_target_argument_block(size_t i)
    {
        // First entry is the "not-used" block, so start at index 1.
        if (i + 1 >= m_target_argument_block_list.size()) return 0;
        return m_target_argument_block_list[i + 1];
    }

    // Get the number of target argument blocks.
    size_t get_argument_block_count() const
    {
        return m_own_arg_blocks.size();
    }

    // Get the argument block of the i'th distribution function.
    // If the distribution function has no target argument block, size_t(~0) is returned.
    size_t get_df_argument_block_index(size_t i) const
    {
        if (i >= m_df_arg_block_indices.size()) return size_t(~0);
        return m_df_arg_block_indices[i];
    }

    // Get a writable copy of the i'th target argument block.
    mi::base::Handle<mi::neuraylib::ITarget_argument_block> get_argument_block(size_t i)
    {
        if (i >= m_own_arg_blocks.size())
            return mi::base::Handle<mi::neuraylib::ITarget_argument_block>();
        return m_own_arg_blocks[i];
    }

    // Get the layout of the i'th target argument block.
    mi::base::Handle<mi::neuraylib::ITarget_value_layout const> get_argument_block_layout(size_t i)
    {
        if (i >= m_arg_block_layouts.size())
            return mi::base::Handle<mi::neuraylib::ITarget_value_layout const>();
        return m_arg_block_layouts[i];
    }

    // Update the i'th target argument block on the device with the data from the corresponding
    // block returned by get_argument_block().
    void update_device_argument_block(size_t i);
private:
    // Prepare the texture identified by the texture_index for use by the texture access functions
    // on the GPU.
    bool prepare_texture(
        mi::neuraylib::ITransaction       *transaction,
        mi::neuraylib::IImage_api         *image_api,
        mi::neuraylib::ITarget_code const *code_ptx,
        mi::Size                           texture_index,
        std::vector<Texture>              &textures);

    // The device pointer of the target code data list.
    CUdeviceptr m_device_target_code_data_list;

    // List of all target code data objects owned by this context.
    std::vector<Target_code_data> m_target_code_data_list;

    // The device pointer of the target argument block list.
    CUdeviceptr m_device_target_argument_block_list;

    // List of all target argument blocks owned by this context.
    std::vector<CUdeviceptr> m_target_argument_block_list;

    // List of all local, writable copies of the target argument blocks.
    std::vector<mi::base::Handle<mi::neuraylib::ITarget_argument_block> > m_own_arg_blocks;

    // List of argument block indices per material df.
    std::vector<size_t> m_df_arg_block_indices;

    // List of all target argument block layouts.
    std::vector<mi::base::Handle<mi::neuraylib::ITarget_value_layout const> > m_arg_block_layouts;

    // List of all Texture objects owned by this context.
    std::vector<Texture> m_all_textures;

    // List of all CUDA arrays owned by this context.
    std::vector<cudaArray_t> m_all_texture_arrays;
};

// Free all acquired resources.
Material_gpu_context::~Material_gpu_context()
{
    for (std::vector<cudaArray_t>::iterator it = m_all_texture_arrays.begin(),
            end = m_all_texture_arrays.end(); it != end; ++it) {
        check_cuda_success(cudaFreeArray(*it));
    }
    for (std::vector<Texture>::iterator it = m_all_textures.begin(),
            end = m_all_textures.end(); it != end; ++it) {
        check_cuda_success(cudaDestroyTextureObject(it->filtered_object));
        check_cuda_success(cudaDestroyTextureObject(it->unfiltered_object));
    }
    for (std::vector<Target_code_data>::iterator it = m_target_code_data_list.begin(),
            end = m_target_code_data_list.end(); it != end; ++it) {
        if (it->textures)
            check_cuda_success(cuMemFree(it->textures));
        if (it->ro_data_segment)
            check_cuda_success(cuMemFree(it->ro_data_segment));
    }
    for (std::vector<CUdeviceptr>::iterator it = m_target_argument_block_list.begin(),
            end = m_target_argument_block_list.end(); it != end; ++it) {
        if (*it != 0)
            check_cuda_success(cuMemFree(*it));
    }
    check_cuda_success(cuMemFree(m_device_target_code_data_list));
}

// Get a device pointer to the target code data list.
CUdeviceptr Material_gpu_context::get_device_target_code_data_list()
{
    if (!m_device_target_code_data_list)
        m_device_target_code_data_list = gpu_mem_dup(m_target_code_data_list);
    return m_device_target_code_data_list;
}

// Get a device pointer to the target argument block list.
CUdeviceptr Material_gpu_context::get_device_target_argument_block_list()
{
    if (!m_device_target_argument_block_list)
        m_device_target_argument_block_list = gpu_mem_dup(m_target_argument_block_list);
    return m_device_target_argument_block_list;
}

// Prepare the texture identified by the texture_index for use by the texture access functions
// on the GPU.
bool Material_gpu_context::prepare_texture(
    mi::neuraylib::ITransaction       *transaction,
    mi::neuraylib::IImage_api         *image_api,
    mi::neuraylib::ITarget_code const *code_ptx,
    mi::Size                           texture_index,
    std::vector<Texture>              &textures)
{
    // Get access to the texture data by the texture database name from the target code.
    mi::base::Handle<const mi::neuraylib::ITexture> texture(
        transaction->access<mi::neuraylib::ITexture>(code_ptx->get_texture(texture_index)));
    mi::base::Handle<const mi::neuraylib::IImage> image(
        transaction->access<mi::neuraylib::IImage>(texture->get_image()));
    mi::base::Handle<const mi::neuraylib::ICanvas> canvas(image->get_canvas());
    mi::Uint32 tex_width = canvas->get_resolution_x();
    mi::Uint32 tex_height = canvas->get_resolution_y();
    mi::Uint32 tex_layers = canvas->get_layers_size();
    char const *image_type = image->get_type();

    if (image->is_uvtile()) {
        std::cerr << "The example does not support uvtile textures!" << std::endl;
        return false;
    }

    if (canvas->get_tiles_size_x() != 1 || canvas->get_tiles_size_y() != 1) {
        std::cerr << "The example does not support tiled images!" << std::endl;
        return false;
    }

    // For simplicity, the texture access functions are only implemented for float4 and gamma
    // is pre-applied here (all images are converted to linear space).

    // Convert to linear color space if necessary
    if (texture->get_effective_gamma() != 1.0f) {
        // Copy/convert to float4 canvas and adjust gamma from "effective gamma" to 1.
        mi::base::Handle<mi::neuraylib::ICanvas> gamma_canvas(
            image_api->convert(canvas.get(), "Color"));
        gamma_canvas->set_gamma(texture->get_effective_gamma());
        image_api->adjust_gamma(gamma_canvas.get(), 1.0f);
        canvas = gamma_canvas;
    } else if (strcmp(image_type, "Color") != 0 && strcmp(image_type, "Float32<4>") != 0) {
        // Convert to expected format
        canvas = image_api->convert(canvas.get(), "Color");
    }

    // Copy image data to GPU array depending on texture shape
    cudaChannelFormatDesc channel_desc = cudaCreateChannelDesc<float4>();
    cudaArray_t device_tex_data;
    mi::neuraylib::ITarget_code::Texture_shape texture_shape =
        code_ptx->get_texture_shape(texture_index);
    if (texture_shape == mi::neuraylib::ITarget_code::Texture_shape_cube ||
        texture_shape == mi::neuraylib::ITarget_code::Texture_shape_3d) {
        // Cubemap and 3D texture objects require 3D CUDA arrays

        if (texture_shape == mi::neuraylib::ITarget_code::Texture_shape_cube &&
            tex_layers != 6) {
            std::cerr << "Invalid number of layers (" << tex_layers
                << "), cubemaps must have 6 layers!" << std::endl;
            return false;
        }

        // Allocate a 3D array on the GPU
        cudaExtent extent = make_cudaExtent(tex_width, tex_height, tex_layers);
        check_cuda_success(cudaMalloc3DArray(
            &device_tex_data, &channel_desc, extent,
            texture_shape == mi::neuraylib::ITarget_code::Texture_shape_cube ?
            cudaArrayCubemap : 0));

        // Prepare the memcpy parameter structure
        cudaMemcpy3DParms copy_params;
        memset(&copy_params, 0, sizeof(copy_params));
        copy_params.dstArray = device_tex_data;
        copy_params.extent = make_cudaExtent(tex_width, tex_height, 1);
        copy_params.kind = cudaMemcpyHostToDevice;

        // Copy the image data of all layers (the layers are not consecutive in memory)
        for (mi::Uint32 layer = 0; layer < tex_layers; ++layer) {
            mi::base::Handle<const mi::neuraylib::ITile> tile(
                canvas->get_tile(0, 0, layer));
            float const *data = static_cast<float const *>(tile->get_data());

            copy_params.srcPtr = make_cudaPitchedPtr(
                const_cast<float *>(data), tex_width * sizeof(float) * 4,
                tex_width, tex_height);
            copy_params.dstPos = make_cudaPos(0, 0, layer);

            check_cuda_success(cudaMemcpy3D(&copy_params));
        }
    } else {
        // 2D texture objects use CUDA arrays
        check_cuda_success(cudaMallocArray(
            &device_tex_data, &channel_desc, tex_width, tex_height));

        mi::base::Handle<const mi::neuraylib::ITile> tile(canvas->get_tile(0, 0));
        mi::Float32 const *data = static_cast<mi::Float32 const *>(tile->get_data());
        check_cuda_success(cudaMemcpyToArray(device_tex_data, 0, 0, data,
            tex_width * tex_height * sizeof(float) * 4, cudaMemcpyHostToDevice));
    }
    m_all_texture_arrays.push_back(device_tex_data);

    // Create filtered texture object
    cudaResourceDesc res_desc;
    memset(&res_desc, 0, sizeof(res_desc));
    res_desc.resType = cudaResourceTypeArray;
    res_desc.res.array.array = device_tex_data;

    // For cube maps we need clamped address mode to avoid artifacts in the corners
    cudaTextureAddressMode addr_mode =
        texture_shape == mi::neuraylib::ITarget_code::Texture_shape_cube
        ? cudaAddressModeClamp
        : cudaAddressModeWrap;

    cudaTextureDesc tex_desc;
    memset(&tex_desc, 0, sizeof(tex_desc));
    tex_desc.addressMode[0]   = addr_mode;
    tex_desc.addressMode[1]   = addr_mode;
    tex_desc.addressMode[2]   = addr_mode;
    tex_desc.filterMode       = cudaFilterModeLinear;
    tex_desc.readMode         = cudaReadModeElementType;
    tex_desc.normalizedCoords = 1;

    cudaTextureObject_t tex_obj = 0;
    check_cuda_success(cudaCreateTextureObject(&tex_obj, &res_desc, &tex_desc, NULL));

    // Create unfiltered texture object if necessary (cube textures have no texel functions)
    cudaTextureObject_t tex_obj_unfilt = 0;
    if (texture_shape != mi::neuraylib::ITarget_code::Texture_shape_cube) {
        // Use a black border for access outside of the texture
        tex_desc.addressMode[0]   = cudaAddressModeBorder;
        tex_desc.addressMode[1]   = cudaAddressModeBorder;
        tex_desc.addressMode[2]   = cudaAddressModeBorder;
        tex_desc.filterMode       = cudaFilterModePoint;

        check_cuda_success(cudaCreateTextureObject(
            &tex_obj_unfilt, &res_desc, &tex_desc, NULL));
    }

    // Store texture infos in result vector
    textures.push_back(Texture(
        tex_obj,
        tex_obj_unfilt,
        make_uint3(tex_width, tex_height, tex_layers)));
    m_all_textures.push_back(textures.back());

    return true;
}

// Prepare the needed target code data of the given target code.
bool Material_gpu_context::prepare_target_code_data(
    mi::neuraylib::ITransaction       *transaction,
    mi::neuraylib::IImage_api         *image_api,
    mi::neuraylib::ITarget_code const *target_code)
{
    // Target code data list may not have been retrieved already
    check_success(!m_device_target_code_data_list);

    // Handle the read-only data segments if necessary.
    // They are only created, if the "enable_ro_segment" backend option was set to "on".
    CUdeviceptr device_ro_data = 0;
    if (target_code->get_ro_data_segment_count() > 0) {
        device_ro_data = gpu_mem_dup(
            target_code->get_ro_data_segment_data(0),
            target_code->get_ro_data_segment_size(0));
    }

    // Copy textures to GPU if the code has more than just the invalid texture
    CUdeviceptr device_textures = 0;
    mi::Size num_textures = target_code->get_texture_count();
    if (num_textures > 1) {
        std::vector<Texture> textures;

        // Loop over all textures skipping the first texture,
        // which is always the invalid texture
        for (mi::Size i = 1; i < num_textures; ++i) {
            if (!prepare_texture(
                    transaction, image_api, target_code, i, textures))
                return false;
        }

        // Copy texture list to GPU
        device_textures = gpu_mem_dup(textures);
    }

    m_target_code_data_list.push_back(
        Target_code_data(num_textures, device_textures, device_ro_data));

    for (mi::Size i = 0, num = target_code->get_argument_block_count(); i < num; ++i) {
        mi::base::Handle<mi::neuraylib::ITarget_argument_block const> arg_block(
            target_code->get_argument_block(i));
        CUdeviceptr dev_block = gpu_mem_dup(arg_block->get_data(), arg_block->get_size());
        m_target_argument_block_list.push_back(dev_block);
        m_own_arg_blocks.push_back(mi::base::make_handle(arg_block->clone()));
        m_arg_block_layouts.push_back(
            mi::base::make_handle(target_code->get_argument_block_layout(i)));
    }

    // Collect all target argument block indices of the distribution functions.
    for (mi::Size i = 0, num = target_code->get_callable_function_count(); i < num; ++i) {
        mi::neuraylib::ITarget_code::Function_kind kind =
            target_code->get_callable_function_kind(i);
        if (kind != mi::neuraylib::ITarget_code::FK_DF_INIT) continue;

        m_df_arg_block_indices.push_back(
            size_t(target_code->get_callable_function_argument_block_index(i)));
    }

    return true;
}

// Update the i'th target argument block on the device with the data from the corresponding
// block returned by get_argument_block().
void Material_gpu_context::update_device_argument_block(size_t i)
{
    CUdeviceptr device_ptr = get_device_target_argument_block(i);
    if (device_ptr == 0) return;

    mi::base::Handle<mi::neuraylib::ITarget_argument_block> arg_block(get_argument_block(i));
    check_cuda_success(cuMemcpyHtoD(
        device_ptr, arg_block->get_data(), arg_block->get_size()));
}


//------------------------------------------------------------------------------
//
// MDL material compilation code
//
//------------------------------------------------------------------------------

class Material_compiler {
private:
    // Helper function to extract the module name from a fully-qualified material name.
    static std::string get_module_name(const std::string& material_name);

    // Helper function to extract the material name from a fully-qualified material name.
    static std::string get_material_name(const std::string& material_name);

    // Creates an instance of the given material.
    mi::neuraylib::IMaterial_instance* create_material_instance(
        const std::string& material_name);

    // Compiles the given material instance in the given compilation modes.
    mi::neuraylib::ICompiled_material* compile_material_instance(
        mi::neuraylib::IMaterial_instance* material_instance,
        bool class_compilation);

public:
    // Add a subexpression of a given material to the link unit.
    // path is the path of the sub-expression.
    // fname is the function name in the generated code.
    // If class_compilation is true, the material will use class compilation.
    bool add_material_subexpr(
        const std::string& material_name,
        const char* path,
        const char* fname,
        bool class_compilation=false);

    // Add a distribution function of a given material to the link unit.
    // path is the path of the sub-expression.
    // fname is the function name in the generated code.
    // If class_compilation is true, the material will use class compilation.
    bool add_material_df(
        const std::string& material_name,
        const char* path,
        const char* base_fname,
        bool class_compilation=false);

    // Generates CUDA PTX target code for the current link unit.
    mi::base::Handle<const mi::neuraylib::ITarget_code> generate_cuda_ptx();

    // Constructor.
    Material_compiler(
        mi::neuraylib::IMdl_compiler* mdl_compiler,
        mi::neuraylib::ITransaction* transaction,
        unsigned num_texture_results);

    // Get the list of used material definitions.
    // There will be one entry per add_* call.
    std::vector<mi::base::Handle<mi::neuraylib::IMaterial_definition const> >
        &get_material_defs()
    {
        return m_material_defs;
    }

    // Get the list of compiled materials.
    // There will be one entry per add_* call.
    std::vector<mi::base::Handle<mi::neuraylib::ICompiled_material> > &get_compiled_materials()
    {
        return m_compiled_materials;
    }

private:
    mi::base::Handle<mi::neuraylib::IMdl_compiler> m_mdl_compiler;
    mi::base::Handle<mi::neuraylib::IMdl_backend>  m_be_cuda_ptx;
    mi::base::Handle<mi::neuraylib::ITransaction>  m_transaction;

    mi::base::Handle<mi::neuraylib::ILink_unit> m_link_unit;

    std::vector<mi::base::Handle<mi::neuraylib::IMaterial_definition const> > m_material_defs;
    std::vector<mi::base::Handle<mi::neuraylib::ICompiled_material> > m_compiled_materials;
};

// Helper function to extract the module name from a fully-qualified material name.
std::string Material_compiler::get_module_name(const std::string& material_name)
{
    size_t p = material_name.rfind("::");
    return material_name.substr(0, p);
}

// Helper function to extract the material name from a fully-qualified material name.
std::string Material_compiler::get_material_name(const std::string& material_name)
{
    size_t p = material_name.rfind("::");
    if (p == std::string::npos)
        return material_name;
    return material_name.substr(p + 2, material_name.size() - p);
}

// Creates an instance of the given material.
mi::neuraylib::IMaterial_instance* Material_compiler::create_material_instance(
    const std::string& material_name)
{
    // Load mdl module.
    std::string module_name = get_module_name(material_name);
    check_success(m_mdl_compiler->load_module(m_transaction.get(), module_name.c_str()) >= 0);

    // Create a material instance from the material definition
    // with the default arguments.
    const char *prefix = (material_name.find("::") == 0) ? "mdl" : "mdl::";

    std::string material_db_name = prefix + material_name;
    mi::base::Handle<const mi::neuraylib::IMaterial_definition> material_definition(
        m_transaction->access<mi::neuraylib::IMaterial_definition>(
            material_db_name.c_str()));
    check_success(material_definition);

    m_material_defs.push_back(material_definition);

    mi::Sint32 result;
    mi::base::Handle<mi::neuraylib::IMaterial_instance> material_instance(
        material_definition->create_material_instance(0, &result));
    check_success(result == 0);

    material_instance->retain();
    return material_instance.get();
}

// Compiles the given material instance in the given compilation modes.
mi::neuraylib::ICompiled_material *Material_compiler::compile_material_instance(
    mi::neuraylib::IMaterial_instance* material_instance,
    bool class_compilation)
{
    mi::Sint32 result = 0;
    mi::Uint32 flags = class_compilation
        ? mi::neuraylib::IMaterial_instance::CLASS_COMPILATION
        : mi::neuraylib::IMaterial_instance::DEFAULT_OPTIONS;
    mi::base::Handle<mi::neuraylib::ICompiled_material> compiled_material(
        material_instance->create_compiled_material(flags, 1.0f, 380.0f, 780.0f, &result));
    check_success(result == 0);

    m_compiled_materials.push_back(compiled_material);

    compiled_material->retain();
    return compiled_material.get();
}

// Generates CUDA PTX target code for the current link unit.
mi::base::Handle<const mi::neuraylib::ITarget_code> Material_compiler::generate_cuda_ptx()
{
    mi::Sint32 result = -1;
    mi::base::Handle<const mi::neuraylib::ITarget_code> code_cuda_ptx(
        m_be_cuda_ptx->translate_link_unit(m_link_unit.get(), &result));
    check_success(result == 0);
    check_success(code_cuda_ptx);

#ifdef DUMP_PTX
    std::cout << "Dumping CUDA PTX code:\n\n"
        << code_cuda_ptx->get_code() << std::endl;
#endif

    return code_cuda_ptx;
}

// Add a subexpression of a given material to the link unit.
// path is the path of the sub-expression.
// fname is the function name in the generated code.
bool Material_compiler::add_material_subexpr(
    const std::string& material_name,
    const char* path,
    const char* fname,
    bool class_compilation)
{
    // Load the given module and create a material instance
    mi::base::Handle<mi::neuraylib::IMaterial_instance> material_instance(
        create_material_instance(material_name.c_str()));

    // Compile the material instance in instance compilation mode
    mi::base::Handle<mi::neuraylib::ICompiled_material> compiled_material(
        compile_material_instance(material_instance.get(), class_compilation));

    return m_link_unit->add_material_expression(compiled_material.get(), path, fname) >= 0;
}

// Add a distribution function of a given material to the link unit.
// path is the path of the sub-expression.
// fname is the function name in the generated code.
bool Material_compiler::add_material_df(
    const std::string& material_name,
    const char* path,
    const char* base_fname,
    bool class_compilation)
{
    // Load the given module and create a material instance
    mi::base::Handle<mi::neuraylib::IMaterial_instance> material_instance(
        create_material_instance(material_name.c_str()));

    // Compile the material instance in instance compilation mode
    mi::base::Handle<mi::neuraylib::ICompiled_material> compiled_material(
        compile_material_instance(material_instance.get(), class_compilation));

    return m_link_unit->add_material_df(
        compiled_material.get(), path, base_fname, /*include_geometry_normal=*/true) >= 0;
}

// Constructor.
Material_compiler::Material_compiler(
    mi::neuraylib::IMdl_compiler* mdl_compiler,
    mi::neuraylib::ITransaction* transaction,
    unsigned num_texture_results)
    : m_mdl_compiler(mi::base::make_handle_dup(mdl_compiler))
    , m_be_cuda_ptx(mdl_compiler->get_backend(mi::neuraylib::IMdl_compiler::MB_CUDA_PTX))
    , m_transaction(mi::base::make_handle_dup(transaction))
    , m_link_unit()
{
    check_success(m_be_cuda_ptx->set_option("num_texture_spaces", "1") == 0);

    // Option "enable_ro_segment": Default is disabled.
    // If you have a lot of big arrays, enabling this might speed up compilation.
    // check_success(m_be_cuda_ptx->set_option("enable_ro_segment", "on") == 0);

    // Option "tex_lookup_call_mode": Default mode is vtable mode.
    // You can switch to the slower vtable mode by commenting out the next line.
    check_success(m_be_cuda_ptx->set_option("tex_lookup_call_mode", "direct_call") == 0);

    // Option "num_texture_results": Default is 0.
    // Set the size of a renderer provided array for texture results in the MDL SDK state in number
    // of float4 elements processed by the init() function.
    check_success(m_be_cuda_ptx->set_option(
        "num_texture_results",
        to_string(num_texture_results).c_str()) == 0);

    // After we set the options, we can create the link unit
    m_link_unit = mi::base::make_handle(m_be_cuda_ptx->create_link_unit(transaction, NULL));
}

//------------------------------------------------------------------------------
//
// Material execution code
//
//------------------------------------------------------------------------------

// Helper function to create PTX source code for a non-empty 32-bit value array.
void print_array_u32(
    std::string &str, std::string const &name, unsigned count, std::string const &content)
{
    str += ".visible .const .align 4 .u32 " + name + "[";
    if (count == 0) {
        // PTX does not allow empty arrays, so use a dummy entry
        str += "1] = { 0 };\n";
    } else {
        str += to_string(count) + "] = { " + content + " };\n";
    }
}

// Helper function to create PTX source code for a non-empty function pointer array.
void print_array_func(
    std::string &str, std::string const &name, unsigned count, std::string const &content)
{
    str += ".visible .const .align 8 .u64 " + name + "[";
    if (count == 0) {
        // PTX does not allow empty arrays, so use a dummy entry
        str += "1] = { dummy_func };\n";
    } else {
        str += to_string(count) + "] = { " + content + " };\n";
    }
}

// Generate PTX array containing the references to all generated functions.
std::string generate_func_array_ptx(
    const std::vector<mi::base::Handle<const mi::neuraylib::ITarget_code> > &target_codes)
{
    // Create PTX header and mdl_expr_functions_count constant
    std::string src =
        ".version 4.0\n"
        ".target sm_20\n"
        ".address_size 64\n";

    // Workaround needed to let CUDA linker resolve the function pointers in the arrays.
    // Also used for "empty" function arrays.
    src += ".func dummy_func() { ret; }\n";

    const char *suffixes[5] = {"", "_init", "_evaluate", "_sample", "_pdf"};
    std::string kind_funcs[5];
    unsigned    expr_count = 0, df_count = 0;
    std::string expr_tc_indices, df_tc_indices;
    std::string expr_block_indices, df_block_indices;

    // Iterate over all target codes
    for (size_t i = 0, num = target_codes.size(); i < num; ++i) {
        mi::base::Handle<const mi::neuraylib::ITarget_code> const &target_code = target_codes[i];

        // Collect all names and prototypes of callable functions within the current target code
        for (size_t func_index = 0, func_count = target_code->get_callable_function_count();
                func_index < func_count; ++func_index) {
            unsigned kind_index;
            switch (target_code->get_callable_function_kind(func_index)) {
                case mi::neuraylib::ITarget_code::FK_LAMBDA:      kind_index = 0; break;
                case mi::neuraylib::ITarget_code::FK_DF_INIT:     kind_index = 1; break;
                case mi::neuraylib::ITarget_code::FK_DF_EVALUATE: kind_index = 2; break;
                case mi::neuraylib::ITarget_code::FK_DF_SAMPLE:   kind_index = 3; break;
                case mi::neuraylib::ITarget_code::FK_DF_PDF:      kind_index = 4; break;
                default:
                    std::cerr << "Unsupported callable function kind: "
                        << int(target_code->get_callable_function_kind(func_index)) << std::endl;
                    return "";
            }

            // Add function name to per kind list
            if (!kind_funcs[kind_index].empty())
                kind_funcs[kind_index] += ", ";
            kind_funcs[kind_index] += target_code->get_callable_function(func_index);

            // Get argument block index and translate to 1 based list index (-> 0 = not-used)
            mi::Size arg_block_index = target_code->get_callable_function_argument_block_index(
                func_index);
            if (arg_block_index == mi::Size(~0)) arg_block_index = 0;
            else ++arg_block_index;

            // Add indices to the corresponding list
            if (kind_index == 0) {
                if (!expr_tc_indices.empty()) expr_tc_indices += ", ";
                expr_tc_indices += to_string(i);

                if (!expr_block_indices.empty()) expr_block_indices += ", ";
                expr_block_indices += to_string(arg_block_index);

                ++expr_count;
            } else if (kind_index == 1) {
                if (!df_tc_indices.empty()) df_tc_indices += ", ";
                df_tc_indices += to_string(i);

                if (!df_block_indices.empty()) df_block_indices += ", ";
                df_block_indices += to_string(arg_block_index);

                ++df_count;
            }

            // Add prototype declaration
            src += target_code->get_callable_function_prototype(
                func_index, mi::neuraylib::ITarget_code::SL_PTX);
            src += '\n';
        }
    }

    // Create arrays for all function kinds
    for (unsigned kind_index = 0; kind_index < 5; ++kind_index) {
        if (kind_index == 0) {
            src += std::string(".visible .const .align 4 .u32 mdl_expr_functions_count = ")
                + to_string(expr_count) + ";\n";
            print_array_u32(src, "mdl_expr_target_code_indices", expr_count, expr_tc_indices);
            print_array_u32(src, "mdl_expr_arg_block_indices", expr_count, expr_block_indices);
        } else if (kind_index == 1) {
            src += std::string(".visible .const .align 4 .u32 mdl_df_functions_count = ")
                + to_string(df_count) + ";\n";
            print_array_u32(src, "mdl_df_target_code_indices", df_count, df_tc_indices);
            print_array_u32(src, "mdl_df_arg_block_indices", df_count, df_block_indices);
        }

        if (kind_index == 0)
            print_array_func(src, "mdl_expr_functions", expr_count, kind_funcs[0]);
        else
            print_array_func(src, std::string("mdl_df_functions") + suffixes[kind_index],
                df_count, kind_funcs[kind_index]);
    }

    return src;
}

// Build a linked CUDA kernel containing our kernel and all the generated code, making it
// available to the kernel via an added "mdl_expr_functions" array.
CUmodule build_linked_kernel(
    std::vector<mi::base::Handle<const mi::neuraylib::ITarget_code> > const &target_codes,
    const char *ptx_file,
    const char *kernel_function_name,
    CUfunction *out_kernel_function)
{
    // Generate PTX array containing the references to all generated functions.
    // The linker will resolve them to addresses.

    std::string ptx_func_array_src = generate_func_array_ptx(target_codes);
#ifdef DUMP_PTX
    std::cout << "Dumping CUDA PTX code for the \"mdl_expr_functions\" array:\n\n"
        << ptx_func_array_src << std::endl;
#endif

    // Link all generated code, our generated PTX array and our kernel together

    CUlinkState   cuda_link_state;
    CUmodule      cuda_module;
    void         *linked_cubin;
    size_t        linked_cubin_size;
    char          error_log[8192], info_log[8192];
    CUjit_option  options[4];
    void         *optionVals[4];

    // Setup the linker

    // Pass a buffer for info messages
    options[0] = CU_JIT_INFO_LOG_BUFFER;
    optionVals[0] = info_log;
    // Pass the size of the info buffer
    options[1] = CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES;
    optionVals[1] = reinterpret_cast<void *>(uintptr_t(sizeof(info_log)));
    // Pass a buffer for error messages
    options[2] = CU_JIT_ERROR_LOG_BUFFER;
    optionVals[2] = error_log;
    // Pass the size of the error buffer
    options[3] = CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES;
    optionVals[3] = reinterpret_cast<void *>(uintptr_t(sizeof(error_log)));

    check_cuda_success(cuLinkCreate(4, options, optionVals, &cuda_link_state));

    CUresult link_result = CUDA_SUCCESS;
    do {
        // Add all code generated by the MDL PTX backend
        for (size_t i = 0, num_target_codes = target_codes.size(); i < num_target_codes; ++i) {
            link_result = cuLinkAddData(
                cuda_link_state, CU_JIT_INPUT_PTX,
                const_cast<char *>(target_codes[i]->get_code()),
                target_codes[i]->get_code_size(),
                NULL, 0, NULL, NULL);
            if (link_result != CUDA_SUCCESS) break;
        }
        if (link_result != CUDA_SUCCESS) break;

        // Add the "mdl_expr_functions" array PTX module
        link_result = cuLinkAddData(
            cuda_link_state, CU_JIT_INPUT_PTX,
            const_cast<char *>(ptx_func_array_src.c_str()),
            ptx_func_array_src.size(),
            NULL, 0, NULL, NULL);
        if (link_result != CUDA_SUCCESS) break;

        // Add our kernel
        link_result = cuLinkAddFile(
            cuda_link_state, CU_JIT_INPUT_PTX,
            ptx_file, 0, NULL, NULL);
        if (link_result != CUDA_SUCCESS) break;

        // Link everything to a cubin
        link_result = cuLinkComplete(cuda_link_state, &linked_cubin, &linked_cubin_size);
    } while (false);
    if (link_result != CUDA_SUCCESS) {
        std::cerr << "PTX linker error:\n" << error_log << std::endl;
        check_cuda_success(link_result);
    }

    std::cout << "CUDA link completed. Linker output:\n" << info_log << std::endl;

    // Load the result and get the entrypoint of our kernel
    check_cuda_success(cuModuleLoadData(&cuda_module, linked_cubin));
    check_cuda_success(cuModuleGetFunction(
        out_kernel_function, cuda_module, kernel_function_name));

    int regs = 0;
    check_cuda_success(
        cuFuncGetAttribute(&regs, CU_FUNC_ATTRIBUTE_NUM_REGS, *out_kernel_function));
    int lmem = 0;
    check_cuda_success(
        cuFuncGetAttribute(&lmem, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, *out_kernel_function));
    std::cout << "kernel uses " << regs << " registers and " << lmem << " lmem" << std::endl;

    // Cleanup
    check_cuda_success(cuLinkDestroy(cuda_link_state));

    return cuda_module;
}

#endif // EXAMPLE_CUDA_SHARED_H

