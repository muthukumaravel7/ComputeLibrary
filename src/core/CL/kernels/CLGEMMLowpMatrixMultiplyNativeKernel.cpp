/*
 * Copyright (c) 2019-2020 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/core/CL/kernels/CLGEMMLowpMatrixMultiplyNativeKernel.h"

#include "arm_compute/core/AccessWindowStatic.h"
#include "arm_compute/core/CL/CLHelpers.h"
#include "arm_compute/core/CL/CLKernelLibrary.h"
#include "arm_compute/core/CL/ICLTensor.h"
#include "arm_compute/core/CL/OpenCL.h"
#include "arm_compute/core/Error.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Types.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/core/Window.h"
#include "arm_compute/core/utils/misc/ShapeCalculator.h"
#include "support/StringSupport.h"

#include <cstddef>
#include <cstdint>
#include <tuple>

namespace arm_compute
{
using namespace misc::shape_calculator;

namespace
{
using ElementsProcessed = Steps;

Status validate_arguments(const ITensorInfo *input0, const ITensorInfo *input1, const ITensorInfo *output, const GEMMLHSMatrixInfo &lhs_info, const GEMMRHSMatrixInfo &rhs_info,
                          const GEMMReshapeInfo &gemm_info)
{
    ARM_COMPUTE_RETURN_ERROR_ON_NULLPTR(input0, input1, output);
    ARM_COMPUTE_RETURN_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input0, 1, DataType::QASYMM8, DataType::QASYMM8_SIGNED);
    if(input0->data_type() == DataType::QASYMM8)
    {
        ARM_COMPUTE_RETURN_ERROR_ON_MISMATCHING_DATA_TYPES(input0, input1);
    }
    else
    {
        ARM_COMPUTE_RETURN_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input1, 1, DataType::QSYMM8, DataType::QASYMM8_SIGNED, DataType::QSYMM8_PER_CHANNEL);
    }
    ARM_COMPUTE_RETURN_ERROR_ON_MSG(input0->num_dimensions() > 4, "The number of dimensions for the LHS matrix must be <= 4");
    ARM_COMPUTE_RETURN_ERROR_ON_MSG(input1->num_dimensions() > 3, "The number of dimensions for the RHS matrix must be <= 3");
    ARM_COMPUTE_RETURN_ERROR_ON(lhs_info.k0 != rhs_info.k0);
    ARM_COMPUTE_RETURN_ERROR_ON_MSG(((lhs_info.k0 & (lhs_info.k0 - 1)) && lhs_info.k0 != 3), "Only 2,3,4,8,16 are supported for k0");
    ARM_COMPUTE_RETURN_ERROR_ON(lhs_info.k0 > 16);
    ARM_COMPUTE_RETURN_ERROR_ON(lhs_info.m0 < 1 || lhs_info.m0 > 8);
    ARM_COMPUTE_RETURN_ERROR_ON_MSG(((rhs_info.n0 & (rhs_info.n0 - 1)) && rhs_info.n0 != 3), "Only 2,3,4,8,16 are supported for n0");

    const int m = gemm_info.m();
    const int n = gemm_info.n();
    const int k = gemm_info.k();

    ARM_COMPUTE_UNUSED(m);
    ARM_COMPUTE_UNUSED(n);
    ARM_COMPUTE_UNUSED(k);

    ARM_COMPUTE_RETURN_ERROR_ON(input0->dimension(0) != static_cast<unsigned int>(k));
    ARM_COMPUTE_RETURN_ERROR_ON(input1->dimension(0) != static_cast<unsigned int>(n));
    ARM_COMPUTE_RETURN_ERROR_ON(input1->dimension(1) != static_cast<unsigned int>(k));
    if(gemm_info.reinterpret_input_as_3d())
    {
        ARM_COMPUTE_RETURN_ERROR_ON(input0->dimension(1) * input0->dimension(2) != static_cast<unsigned int>(m));
    }
    else
    {
        ARM_COMPUTE_RETURN_ERROR_ON(input0->dimension(1) != static_cast<unsigned int>(m));
    }

    if(output->total_size() != 0)
    {
        const TensorInfo tensor_info_output = output->clone()->set_tensor_shape(compute_mm_shape(*input0, *input1, gemm_info));
        ARM_COMPUTE_RETURN_ERROR_ON_MISMATCHING_SHAPES(output, &tensor_info_output);
        ARM_COMPUTE_RETURN_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(output, 1, DataType::S32);
    }

    return Status{};
}

std::pair<Status, Window> validate_and_configure_window(ITensorInfo *input0, ITensorInfo *input1, ITensorInfo *output, const GEMMLHSMatrixInfo &lhs_info, const GEMMRHSMatrixInfo &rhs_info,
                                                        const GEMMReshapeInfo &gemm_info, ElementsProcessed &num_elements_processed)
{
    unsigned int &num_elems_processed_per_iteration_x = num_elements_processed[0];
    unsigned int &num_elems_processed_per_iteration_y = num_elements_processed[1];
    bool          reinterpret_input_as_3d             = gemm_info.reinterpret_input_as_3d();
    bool          reinterpret_output_as_3d            = (gemm_info.depth_output_gemm3d() != 0);

    Window win{};
    Window win_out{};
    bool   window_changed = false;

    // In case both input and output have to be reinterpreted as 3D tensors,
    // force reinterpret_output_as_3d to be false.
    if(reinterpret_input_as_3d == reinterpret_output_as_3d)
    {
        reinterpret_output_as_3d = false;
    }

    // Output tensor auto initialization if not yet initialized
    auto_init_if_empty(*output, input0->clone()->set_tensor_shape(compute_mm_shape(*input0, *input1, gemm_info)).set_data_type(DataType::S32));

    TensorInfo tmp_info(*output);

    if(reinterpret_output_as_3d)
    {
        // Since the output tensor has to be reinterpreted as 3D and the execute window is based on a 2D GEMM,
        // the window needs to be constructed on the 2D collapsed version of the tensor
        TensorShape tmp_shape(output->tensor_shape());
        tmp_shape.collapse(2U, 1U);
        tmp_info.set_tensor_shape(tmp_shape);
    }

    // Configure kernel window
    num_elems_processed_per_iteration_x = rhs_info.n0;
    num_elems_processed_per_iteration_y = lhs_info.m0;

    // Note: bottom paddings are calculated manually as the output can be reinterpreted as 3D tensor
    // The only way to set properly the paddings, it is to set those explicitly through the AccessWindowStatic
    const int m          = reinterpret_output_as_3d ? gemm_info.m() : input0->dimension(1);
    const int bottom_pad = (num_elems_processed_per_iteration_y - (m % num_elems_processed_per_iteration_y)) % num_elems_processed_per_iteration_y;

    win     = calculate_max_window(tmp_info, Steps(num_elems_processed_per_iteration_x, num_elems_processed_per_iteration_y));
    win_out = calculate_max_window(*output, Steps(num_elems_processed_per_iteration_x, num_elems_processed_per_iteration_y));

    AccessWindowStatic input0_access(input0, 0, 0,
                                     input0->dimension(0),
                                     input0->dimension(1) + bottom_pad);
    AccessWindowStatic input1_access(input1, 0, 0,
                                     ceil_to_multiple(input1->dimension(0), num_elems_processed_per_iteration_x),
                                     input1->dimension(1));
    AccessWindowStatic output_access(output, 0, 0,
                                     ceil_to_multiple(output->dimension(0), num_elems_processed_per_iteration_x),
                                     output->dimension(1) + bottom_pad);

    window_changed = update_window_and_padding(win, input0_access, input1_access) || // window used by the execute_window_loop
                     update_window_and_padding(win_out, output_access);              // window used to update the padding requirements of output tensor

    output_access.set_valid_region(win_out, ValidRegion(Coordinates(), output->tensor_shape()));

    // Collapse along the Z direction
    // This collapse needs to be here in order to tune the Z dimension of LWS
    Window             collapsed             = win;
    const unsigned int dimension_to_collapse = std::min(static_cast<unsigned int>(output->num_dimensions()), 2u);
    collapsed                                = win.collapse(win, dimension_to_collapse);

    Status err = (window_changed) ? ARM_COMPUTE_CREATE_ERROR(ErrorCode::RUNTIME_ERROR, "Insufficient Padding!") : Status{};
    return std::make_pair(err, collapsed);
}
} // namespace

CLGEMMLowpMatrixMultiplyNativeKernel::CLGEMMLowpMatrixMultiplyNativeKernel()
    : _input0(nullptr), _input1(nullptr), _output(nullptr), _slide_matrix_b(true), _reinterpret_input_as_3d(false), _reinterpret_output_as_3d(false), _use_dummy_work_items(false)
{
}

void CLGEMMLowpMatrixMultiplyNativeKernel::configure(const ICLTensor *input0, const ICLTensor *input1, ICLTensor *output, const GEMMLHSMatrixInfo &lhs_info, const GEMMRHSMatrixInfo &rhs_info,
                                                     const GEMMReshapeInfo &gemm_info)
{
    configure(CLKernelLibrary::get().get_compile_context(), input0, input1, output, lhs_info, rhs_info, gemm_info);
}

void CLGEMMLowpMatrixMultiplyNativeKernel::configure(const CLCompileContext &compile_context, const ICLTensor *input0, const ICLTensor *input1, ICLTensor *output, const GEMMLHSMatrixInfo &lhs_info,
                                                     const GEMMRHSMatrixInfo &rhs_info,
                                                     const GEMMReshapeInfo   &gemm_info)
{
    ARM_COMPUTE_ERROR_ON_NULLPTR(input0, input1, output);

    ARM_COMPUTE_ERROR_THROW_ON(validate_arguments(input0->info(), input1->info(), output->info(), lhs_info, rhs_info, gemm_info));

    _input0                   = input0;
    _input1                   = input1;
    _output                   = output;
    _reinterpret_input_as_3d  = gemm_info.reinterpret_input_as_3d();
    _reinterpret_output_as_3d = (gemm_info.depth_output_gemm3d() != 0);
    _use_dummy_work_items     = preferred_dummy_work_items_support(CLKernelLibrary::get().get_device());

    // In case both input and output have to be reinterpreted as 3D tensors,
    // force reinterpret_input_as_3d and reinterpret_output_as_3d to be false.
    if(_reinterpret_input_as_3d == _reinterpret_output_as_3d)
    {
        _reinterpret_input_as_3d  = false;
        _reinterpret_output_as_3d = false;
    }

    // Check if we need to slide the matrix B
    const unsigned int num_dimensions_input0 = _input0->info()->num_dimensions();
    _slide_matrix_b                          = (_input1->info()->num_dimensions() >= num_dimensions_input0);

    ElementsProcessed num_elements_processed{};

    // Configure kernel window
    auto win_config = validate_and_configure_window(input0->info(), input1->info(), output->info(), lhs_info, rhs_info, gemm_info, num_elements_processed);
    ARM_COMPUTE_ERROR_THROW_ON(win_config.first);
    ICLKernel::configure_internal(win_config.second);

    // Create build options
    CLBuildOptions build_opts;
    build_opts.add_option_if(_reinterpret_input_as_3d, "-DREINTERPRET_INPUT_AS_3D");
    build_opts.add_option_if(_reinterpret_output_as_3d, "-DREINTERPRET_OUTPUT_AS_3D");
    build_opts.add_option_if(_reinterpret_input_as_3d || _reinterpret_output_as_3d, "-DHEIGHT_GEMM3D=" + support::cpp11::to_string(output->info()->dimension(1)));
    build_opts.add_option_if(_reinterpret_input_as_3d || _reinterpret_output_as_3d, "-DDEPTH_GEMM3D=" + support::cpp11::to_string(output->info()->dimension(2)));
    build_opts.add_option_if(!_slide_matrix_b, "-DMATRIX_B_DEPTH=" + support::cpp11::to_string(input1->info()->dimension(2)));
    build_opts.add_option_if(_use_dummy_work_items, "-DDUMMY_WORK_ITEMS");
    build_opts.add_option("-DM=" + support::cpp11::to_string(input0->info()->dimension(1)));
    build_opts.add_option("-DN=" + support::cpp11::to_string(gemm_info.n()));
    build_opts.add_option("-DK=" + support::cpp11::to_string(gemm_info.k()));
    build_opts.add_option("-DM0=" + support::cpp11::to_string(lhs_info.m0));
    build_opts.add_option("-DN0=" + support::cpp11::to_string(rhs_info.n0));
    build_opts.add_option("-DK0=" + support::cpp11::to_string(rhs_info.k0));
    build_opts.add_option("-DDATA_TYPE=" + get_cl_type_from_data_type(input0->info()->data_type()));
    build_opts.add_option("-DACC_DATA_TYPE=" + get_cl_dot8_acc_type_from_data_type(input0->info()->data_type()));

    std::string kernel_name("gemmlowp_mm_native");

    // Create kernel
    _kernel = create_kernel(compile_context, kernel_name, build_opts.options());

    // Set config_id for enabling LWS tuning
    _config_id = kernel_name;
    _config_id += "_";
    _config_id += dot8_supported(CLKernelLibrary::get().get_device()) ? "_dot8" : "";
    _config_id += "_";
    _config_id += (_reinterpret_input_as_3d ? "3di_" : "");
    _config_id += (_reinterpret_output_as_3d ? "3do_" : "");
    _config_id += support::cpp11::to_string(output->info()->dimension(1));
    _config_id += "_";
    _config_id += support::cpp11::to_string(output->info()->dimension(0));
    _config_id += "_";
    _config_id += support::cpp11::to_string(gemm_info.k());
    _config_id += "_";
    _config_id += support::cpp11::to_string(output->info()->dimension(2));
    _config_id += "_";
    _config_id += support::cpp11::to_string(lhs_info.m0);
    _config_id += "_";
    _config_id += support::cpp11::to_string(rhs_info.n0);
    _config_id += "_";
    _config_id += support::cpp11::to_string(lhs_info.k0);
}

Status CLGEMMLowpMatrixMultiplyNativeKernel::validate(const ITensorInfo *input0, const ITensorInfo *input1, const ITensorInfo *output, const GEMMLHSMatrixInfo &lhs_info,
                                                      const GEMMRHSMatrixInfo &rhs_info, const GEMMReshapeInfo &gemm_info)
{
    ElementsProcessed num_elements_processed{};
    ARM_COMPUTE_RETURN_ON_ERROR(validate_arguments(input0, input1, output, lhs_info, rhs_info, gemm_info));
    ARM_COMPUTE_RETURN_ON_ERROR(validate_and_configure_window(input0->clone().get(),
                                                              input1->clone().get(),
                                                              output->clone().get(),
                                                              lhs_info,
                                                              rhs_info,
                                                              gemm_info,
                                                              num_elements_processed)
                                .first);

    return Status{};
}

void CLGEMMLowpMatrixMultiplyNativeKernel::run(const Window &window, cl::CommandQueue &queue)
{
    ARM_COMPUTE_ERROR_ON_UNCONFIGURED_KERNEL(this);
    ARM_COMPUTE_ERROR_ON_INVALID_SUBWINDOW(ICLKernel::window(), window);

    if(_input1->info()->num_dimensions() < 3)
    {
        // The stride_z for matrix B must be zero if we do not slice
        ARM_COMPUTE_ERROR_ON(_input1->info()->strides_in_bytes()[3] != 0);
    }

    Window slice          = window.first_slice_window_3D();
    Window slice_matrix_b = slice;

    slice_matrix_b.set(Window::DimX, Window::Dimension(0, 1, 1));
    slice_matrix_b.set(Window::DimY, Window::Dimension(0, 1, 1));

    if(_reinterpret_input_as_3d)
    {
        // Pass bottom paddings to the kernel if the input has to be reinterpreted as 3D tensor
        const unsigned int idx0                  = 3 * num_arguments_per_2D_tensor() + 3;
        const unsigned int total_cross_plane_pad = _input0->info()->padding().top + _input0->info()->padding().bottom;
        _kernel.setArg<cl_uint>(idx0, static_cast<unsigned int>(total_cross_plane_pad));
    }

    if(_reinterpret_output_as_3d)
    {
        // Pass bottom paddings to the kernel if the output has to be reinterpreted as 3D tensor
        const unsigned int idx0                  = 3 * num_arguments_per_2D_tensor() + 3 + (_reinterpret_input_as_3d ? 1 : 0);
        const unsigned int total_cross_plane_pad = _output->info()->padding().top + _output->info()->padding().bottom;
        _kernel.setArg<cl_uint>(idx0, static_cast<unsigned int>(total_cross_plane_pad));
    }

    do
    {
        Window slice_b = slice;
        // Don't slice matrix B along the z dimension if matrix B has just 2 dimensions and matrix A more than 2
        // This scenario can happen when the matrix multiplication is used to perform a convolution operation
        if(!_slide_matrix_b)
        {
            slice_b = slice_matrix_b;
        }

        unsigned int idx = 0;
        add_2D_tensor_argument(idx, _input0, slice);
        add_2D_tensor_argument(idx, _input1, slice_b);
        add_2D_tensor_argument(idx, _output, slice);
        _kernel.setArg<cl_uint>(idx++, static_cast<unsigned int>(_input0->info()->strides_in_bytes()[2]));
        _kernel.setArg<cl_uint>(idx++, static_cast<unsigned int>(_input1->info()->strides_in_bytes()[2]));
        _kernel.setArg<cl_uint>(idx++, static_cast<unsigned int>(_output->info()->strides_in_bytes()[2]));
        enqueue(queue, *this, slice, lws_hint(), _use_dummy_work_items);
    }
    while(window.slide_window_slice_3D(slice));
}
} // namespace arm_compute
