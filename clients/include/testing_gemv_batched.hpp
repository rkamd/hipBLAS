/* ************************************************************************
 * Copyright (C) 2016-2022 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ************************************************************************ */

#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <vector>

#include "testing_common.hpp"

/* ============================================================================================ */

template <typename T>
hipblasStatus_t testing_gemv_batched(const Arguments& argus)
{
    bool FORTRAN = argus.fortran;
    auto hipblasGemvBatchedFn
        = FORTRAN ? hipblasGemvBatched<T, true> : hipblasGemvBatched<T, false>;

    int M    = argus.M;
    int N    = argus.N;
    int lda  = argus.lda;
    int incx = argus.incx;
    int incy = argus.incy;

    size_t A_size = size_t(lda) * N;
    size_t dim_x;
    size_t dim_y;

    int batch_count = argus.batch_count;

    hipblasOperation_t transA = char2hipblas_operation(argus.transA_option);

    if(transA == HIPBLAS_OP_N)
    {
        dim_x = N;
        dim_y = M;
    }
    else
    {
        dim_x = M;
        dim_y = N;
    }

    hipblasLocalHandle handle(argus);

    // argument sanity check, quick return if input parameters are invalid before allocating invalid
    // memory
    bool invalid_size = M < 0 || N < 0 || lda < M || lda < 1 || !incx || !incy || batch_count < 0;
    if(invalid_size || !M || !N || !batch_count)
    {
        hipblasStatus_t actual = hipblasGemvBatchedFn(handle,
                                                      transA,
                                                      M,
                                                      N,
                                                      nullptr,
                                                      nullptr,
                                                      lda,
                                                      nullptr,
                                                      incx,
                                                      nullptr,
                                                      nullptr,
                                                      incy,
                                                      batch_count);
        EXPECT_HIPBLAS_STATUS(
            actual, (invalid_size ? HIPBLAS_STATUS_INVALID_VALUE : HIPBLAS_STATUS_SUCCESS));
        return actual;
    }

    int abs_incy = incy >= 0 ? incy : -incy;

    double gpu_time_used, hipblas_error_host, hipblas_error_device;

    T h_alpha = argus.get_alpha<T>();
    T h_beta  = argus.get_beta<T>();

    // arrays of pointers-to-host on host
    host_batch_vector<T> hA(A_size, 1, batch_count);
    host_batch_vector<T> hx(dim_x, incx, batch_count);
    host_batch_vector<T> hy(dim_y, incy, batch_count);
    host_batch_vector<T> hy_cpu(dim_y, incy, batch_count);
    host_batch_vector<T> hy_host(dim_y, incy, batch_count);
    host_batch_vector<T> hy_device(dim_y, incy, batch_count);

    // device pointers
    device_batch_vector<T> dA(A_size, 1, batch_count);
    device_batch_vector<T> dx(dim_x, incx, batch_count);
    device_batch_vector<T> dy(dim_y, incy, batch_count);
    device_vector<T>       d_alpha(1);
    device_vector<T>       d_beta(1);

    CHECK_HIP_ERROR(dA.memcheck());
    CHECK_HIP_ERROR(dx.memcheck());
    CHECK_HIP_ERROR(dy.memcheck());

    // Initial Data on CPU
    hipblas_init_vector(hA, argus, hipblas_client_alpha_sets_nan, true);
    hipblas_init_vector(hx, argus, hipblas_client_alpha_sets_nan, false, true);
    hipblas_init_vector(hy, argus, hipblas_client_beta_sets_nan);

    hy_cpu.copy_from(hy);

    CHECK_HIP_ERROR(dA.transfer_from(hA));
    CHECK_HIP_ERROR(dx.transfer_from(hx));
    CHECK_HIP_ERROR(dy.transfer_from(hy));
    CHECK_HIP_ERROR(hipMemcpy(d_alpha, &h_alpha, sizeof(T), hipMemcpyHostToDevice));
    CHECK_HIP_ERROR(hipMemcpy(d_beta, &h_beta, sizeof(T), hipMemcpyHostToDevice));

    /* =====================================================================
           HIPBLAS
    =================================================================== */
    if(argus.unit_check || argus.norm_check)
    {
        CHECK_HIPBLAS_ERROR(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_HOST));
        CHECK_HIPBLAS_ERROR(hipblasGemvBatchedFn(handle,
                                                 transA,
                                                 M,
                                                 N,
                                                 (T*)&h_alpha,
                                                 dA.ptr_on_device(),
                                                 lda,
                                                 dx.ptr_on_device(),
                                                 incx,
                                                 (T*)&h_beta,
                                                 dy.ptr_on_device(),
                                                 incy,
                                                 batch_count));

        CHECK_HIP_ERROR(hy_host.transfer_from(dy));
        CHECK_HIP_ERROR(dy.transfer_from(hy));

        CHECK_HIPBLAS_ERROR(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));
        CHECK_HIPBLAS_ERROR(hipblasGemvBatchedFn(handle,
                                                 transA,
                                                 M,
                                                 N,
                                                 d_alpha,
                                                 dA.ptr_on_device(),
                                                 lda,
                                                 dx.ptr_on_device(),
                                                 incx,
                                                 d_beta,
                                                 dy.ptr_on_device(),
                                                 incy,
                                                 batch_count));

        CHECK_HIP_ERROR(hy_device.transfer_from(dy));

        /* =====================================================================
           CPU BLAS
        =================================================================== */

        for(int b = 0; b < batch_count; b++)
        {
            cblas_gemv<T>(transA, M, N, h_alpha, hA[b], lda, hx[b], incx, h_beta, hy_cpu[b], incy);
        }

        // enable unit check, notice unit check is not invasive, but norm check is,
        // unit check and norm check can not be interchanged their order
        if(argus.unit_check)
        {
            unit_check_general<T>(1, dim_y, batch_count, abs_incy, hy_cpu, hy_host);
            unit_check_general<T>(1, dim_y, batch_count, abs_incy, hy_cpu, hy_device);
        }
        if(argus.norm_check)
        {
            hipblas_error_host
                = norm_check_general<T>('F', 1, dim_y, abs_incy, hy_cpu, hy_host, batch_count);
            hipblas_error_device
                = norm_check_general<T>('F', 1, dim_y, abs_incy, hy_cpu, hy_device, batch_count);
        }
    }

    if(argus.timing)
    {
        CHECK_HIPBLAS_ERROR(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));
        CHECK_HIP_ERROR(dy.transfer_from(hy));
        hipStream_t stream;
        CHECK_HIPBLAS_ERROR(hipblasGetStream(handle, &stream));

        int runs = argus.cold_iters + argus.iters;
        for(int iter = 0; iter < runs; iter++)
        {
            if(iter == argus.cold_iters)
            {
                gpu_time_used = get_time_us_sync(stream);
            }
            CHECK_HIPBLAS_ERROR(hipblasGemvBatchedFn(handle,
                                                     transA,
                                                     M,
                                                     N,
                                                     d_alpha,
                                                     dA.ptr_on_device(),
                                                     lda,
                                                     dx.ptr_on_device(),
                                                     incx,
                                                     d_beta,
                                                     dy.ptr_on_device(),
                                                     incy,
                                                     batch_count));
        }
        gpu_time_used = get_time_us_sync(stream) - gpu_time_used;

        ArgumentModel<e_transA_option,
                      e_M,
                      e_N,
                      e_alpha,
                      e_lda,
                      e_incx,
                      e_beta,
                      e_incy,
                      e_batch_count>{}
            .log_args<T>(std::cout,
                         argus,
                         gpu_time_used,
                         gemv_gflop_count<T>(transA, M, N),
                         gemv_gbyte_count<T>(transA, M, N),
                         hipblas_error_host,
                         hipblas_error_device);
    }

    return HIPBLAS_STATUS_SUCCESS;
}
